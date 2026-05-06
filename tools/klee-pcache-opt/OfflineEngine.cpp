//===-- OfflineEngine.cpp -------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "OfflineEngine.h"

#include "klee/Expr/Constraints.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Expr/Parser/Parser.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Solver/SolverImpl.h"
#include "klee/System/Time.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

// Direct Z3Builder access (via lib/Solver/ on the include path) lets us
// drive a parallel Z3 context for unsat-core extraction.
#include "Z3Builder.h"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

using namespace klee;
using namespace klee::expr;

namespace klee_pcache_opt {

// Z3 models arrays without a size; the parser still requires a literal.
static constexpr unsigned kSyntheticArraySize = 4096;

OfflineEngine::OfflineEngine(unsigned coreSolverTimeoutSeconds)
    : builder_(createDefaultExprBuilder()),
      timeoutSec_(coreSolverTimeoutSeconds) {
  solver_ = createCoreSolver(CoreSolverToUse);
  if (!solver_) {
    fprintf(stderr,
            "OfflineEngine: failed to create core solver (build with Z3 or STP)\n");
    std::abort();
  }
  if (timeoutSec_ > 0)
    solver_->setCoreSolverTimeout(time::seconds(timeoutSec_));
}

OfflineEngine::~OfflineEngine() = default;

OfflineEngine::UnsatCoreResult
OfflineEngine::getUnsatCore(const std::vector<ref<Expr>> &constraints) {
  UnsatCoreResult res;
  if (constraints.empty())
    return res;

  if (!z3Builder_)
    z3Builder_.reset(
        new Z3Builder(/*autoClearConstructCache=*/false,
                      /*z3LogInteractionFile=*/nullptr));

  Z3_context ctx = z3Builder_->ctx;
  Z3_solver solver = Z3_mk_solver(ctx);
  Z3_solver_inc_ref(ctx, solver);

  Z3_params params = Z3_mk_params(ctx);
  Z3_params_inc_ref(ctx, params);
  Z3_params_set_bool(ctx, params,
                     Z3_mk_string_symbol(ctx, "unsat_core"), true);
  if (timeoutSec_ > 0)
    Z3_params_set_uint(ctx, params,
                       Z3_mk_string_symbol(ctx, "timeout"),
                       timeoutSec_ * 1000u);
  Z3_solver_set_params(ctx, solver, params);
  Z3_params_dec_ref(ctx, params);

  // Track each constraint by a fresh literal so the unsat core identifies
  // which inputs participated.
  Z3_sort boolSort = Z3_mk_bool_sort(ctx);
  std::vector<Z3_ast> trackers;
  trackers.reserve(constraints.size());
  std::unordered_map<unsigned, std::size_t> idToIndex;

  for (std::size_t i = 0; i < constraints.size(); ++i) {
    Z3_ast tracker = Z3_mk_fresh_const(ctx, "c", boolSort);
    Z3_inc_ref(ctx, tracker);
    trackers.push_back(tracker);
    idToIndex[Z3_get_ast_id(ctx, tracker)] = i;

    Z3ASTHandle ast = z3Builder_->construct(constraints[i]);
    Z3_solver_assert_and_track(ctx, solver, ast, tracker);
  }

  Z3_lbool sat = Z3_solver_check(ctx, solver);

  auto cleanup = [&]() {
    for (Z3_ast t : trackers) Z3_dec_ref(ctx, t);
    Z3_solver_dec_ref(ctx, solver);
    z3Builder_->clearConstructCache();
  };

  if (sat == Z3_L_UNDEF) {
    // Treat unknown as timeout; delta-debug fallback will simply not shrink.
    res.timedOut = true;
    cleanup();
    return res;
  }
  if (sat == Z3_L_TRUE) {
    cleanup();
    return res;
  }

  Z3_ast_vector core = Z3_solver_get_unsat_core(ctx, solver);
  Z3_ast_vector_inc_ref(ctx, core);
  unsigned n = Z3_ast_vector_size(ctx, core);
  res.coreIndices.reserve(n);
  for (unsigned k = 0; k < n; ++k) {
    Z3_ast t = Z3_ast_vector_get(ctx, core, k);
    auto it = idToIndex.find(Z3_get_ast_id(ctx, t));
    if (it != idToIndex.end())
      res.coreIndices.push_back(it->second);
  }
  Z3_ast_vector_dec_ref(ctx, core);
  std::sort(res.coreIndices.begin(), res.coreIndices.end());

  res.ok = true;
  cleanup();
  return res;
}

// Hand-rolled scan for A<digits> identifiers; the canonicalizer never emits
// any other shape so this is sufficient.
void OfflineEngine::collectArrayNames(const std::string &s,
                                      std::set<std::string> &out) {
  size_t i = 0, n = s.size();
  while (i < n) {
    char c = s[i];
    bool atBoundary = (i == 0) || !(std::isalnum((unsigned char)s[i - 1]) ||
                                    s[i - 1] == '_');
    if (atBoundary && c == 'A' && i + 1 < n &&
        std::isdigit((unsigned char)s[i + 1])) {
      size_t j = i + 1;
      while (j < n && std::isdigit((unsigned char)s[j]))
        ++j;
      if (j == n || !(std::isalnum((unsigned char)s[j]) || s[j] == '_')) {
        out.insert(s.substr(i, j - i));
        i = j;
        continue;
      }
      i = j;
      continue;
    }
    ++i;
  }
}

ParsedKey OfflineEngine::parseKey(
    const std::set<std::string> &key,
    const std::vector<std::pair<uint8_t, std::vector<unsigned char>>>
        &concreteArrays) {
  ParsedKey result;

  if (key.empty()) {
    result.ok = true;
    return result;
  }

  std::map<uint8_t, const std::vector<unsigned char> *> concreteMap;
  for (const auto &p : concreteArrays)
    concreteMap[p.first] = &p.second;

  std::set<std::string> namesSet;
  for (const auto &s : key)
    collectArrayNames(s, namesSet);

  // Numeric sort by canonical index (so A2 < A10) matches canonicalisation order.
  std::vector<std::string> names(namesSet.begin(), namesSet.end());
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b) {
              unsigned ai = std::strtoul(a.c_str() + 1, nullptr, 10);
              unsigned bi = std::strtoul(b.c_str() + 1, nullptr, 10);
              return ai < bi;
            });

  // Synthesise a KQuery: declare each array (concrete bytes if available,
  // else symbolic), then a (query [...] false).
  std::ostringstream src;
  for (const auto &name : names) {
    unsigned nameIdx =
        static_cast<unsigned>(std::strtoul(name.c_str() + 1, nullptr, 10));
    auto it = concreteMap.find(static_cast<uint8_t>(nameIdx));
    if (it != concreteMap.end()) {
      const auto &bytes = *it->second;
      src << "array " << name << '[' << bytes.size()
          << "] : w32 -> w8 = [";
      for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) src << ' ';
        src << static_cast<unsigned>(bytes[i]);
      }
      src << "]\n";
    } else {
      src << "array " << name << '[' << kSyntheticArraySize
          << "] : w32 -> w8 = symbolic\n";
    }
  }
  src << "(query [";
  bool first = true;
  for (const auto &c : key) {
    if (!first)
      src << ' ';
    src << c;
    first = false;
  }
  src << "] false)\n";

  std::string text = src.str();

  auto buf = llvm::MemoryBuffer::getMemBufferCopy(
      llvm::StringRef(text), "<canonical-key>");

  std::unique_ptr<Parser> P(Parser::Create("<canonical-key>", buf.get(),
                                           builder_.get(),
                                           /*ClearArrayAfterQuery=*/false));
  P->SetMaxErrors(5);

  std::vector<std::unique_ptr<Decl>> decls;
  while (Decl *D = P->ParseTopLevelDecl()) {
    decls.emplace_back(D);
  }
  if (P->GetNumErrors() > 0) {
    result.error = "parser reported " + std::to_string(P->GetNumErrors()) +
                   " errors on synthesised KQuery";
    return result;
  }

  for (const auto &d : decls) {
    if (auto *AD = llvm::dyn_cast<ArrayDecl>(d.get())) {
      result.arrays.push_back(AD->Root);
    } else if (auto *QC = llvm::dyn_cast<QueryCommand>(d.get())) {
      result.constraints.assign(QC->Constraints.begin(), QC->Constraints.end());
    }
  }

  // result.arrays and the ReadExprs in result.constraints alias P's
  // ArrayCache; keep P alive for the lifetime of the result.
  result._decls = std::move(decls);
  result._parser = std::move(P);
  result.ok = true;
  return result;
}

bool OfflineEngine::isUnsat(const std::vector<ref<Expr>> &constraints,
                            bool &timedOut) {
  timedOut = false;
  // mustBeTrue / mayBeTrue short-circuit on a constant query expression, so
  // drop into impl directly with an empty objects vector.
  ConstraintSet cs(constraints);
  Query q(cs, ConstantExpr::alloc(0, Expr::Bool));
  std::vector<std::vector<unsigned char>> dummy;
  bool hasSolution = false;
  bool ok = solver_->impl->computeInitialValues(q, /*objects=*/{}, dummy,
                                                hasSolution);
  if (!ok) {
    if (solver_->impl->getOperationStatusCode() ==
        SolverImpl::SOLVER_RUN_STATUS_TIMEOUT)
      timedOut = true;
    return false;
  }
  return !hasSolution;
}

OfflineEngine::MinimizationResult
OfflineEngine::minimizeUnsatCore(const std::vector<ref<Expr>> &constraints) {
  MinimizationResult res;
  const size_t n = constraints.size();
  res.keptIndices.reserve(n);
  for (size_t i = 0; i < n; ++i)
    res.keptIndices.push_back(i);

  if (n <= 1)
    return res;

  // Z3's unsat-core check shrinks N constraints to a typically small subset
  // in one call (and serves as the UNSAT precondition check); delta-debug
  // afterwards because the core is not guaranteed minimal.
  ++res.solverCalls;
  UnsatCoreResult core = getUnsatCore(constraints);
  if (core.timedOut) {
    res.timedOut = true;
    return res;
  }
  if (!core.ok) {
    // SAT under symbolic-array reconstruction: the canonical key depends on
    // concrete bytes we can't recover, so shrinking would be unsound.
    res.notUnsatPrecondition = true;
    return res;
  }
  res.keptIndices = std::move(core.coreIndices);
  if (res.keptIndices.size() <= 1)
    return res;

  std::vector<ref<Expr>> sub;
  sub.reserve(n);
  size_t i = 0;
  while (i < res.keptIndices.size()) {
    if (res.keptIndices.size() == 1) break;

    sub.clear();
    for (size_t j = 0; j < res.keptIndices.size(); ++j)
      if (j != i)
        sub.push_back(constraints[res.keptIndices[j]]);

    bool subTimedOut = false;
    ++res.solverCalls;
    bool subUnsat = isUnsat(sub, subTimedOut);

    if (subTimedOut) {
      res.timedOut = true;
      ++i;
      continue;
    }
    if (subUnsat) {
      res.keptIndices.erase(res.keptIndices.begin() + i);
      // Don't advance: i now points at the next surviving constraint.
    } else {
      ++i;
    }
  }
  return res;
}

namespace {

// Per-constraint footprint: which arrays are touched, at which byte offsets.
// `symbolicArrays` membership means a non-constant index, i.e. the constraint
// depends on every byte of that array.
struct ConstraintFootprint {
  std::map<std::string, std::set<std::uint32_t>> concreteOffsets;
  std::set<std::string> symbolicArrays;
};

ConstraintFootprint footprintOf(const ref<Expr> &c) {
  ConstraintFootprint fp;
  std::vector<ref<ReadExpr>> reads;
  findReads(c, /*visitUpdates=*/true, reads);
  for (const auto &re : reads) {
    if (!re->updates.root) continue;
    const std::string &name = re->updates.root->name;
    if (auto *ce = dyn_cast<ConstantExpr>(re->index)) {
      fp.concreteOffsets[name].insert(
          static_cast<std::uint32_t>(ce->getZExtValue()));
    } else {
      fp.symbolicArrays.insert(name);
    }
  }
  return fp;
}

// Disjoint-set union over indices [0, n).
struct DSU {
  std::vector<int> parent;
  explicit DSU(std::size_t n) : parent(n) {
    for (std::size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i);
  }
  int find(int x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  }
  void unite(int a, int b) {
    int ra = find(a), rb = find(b);
    if (ra != rb) parent[ra] = rb;
  }
};

} // namespace

std::vector<OfflineEngine::ByteComponent>
OfflineEngine::computeByteComponents(
    const std::vector<ref<Expr>> &constraints) {
  const std::size_t n = constraints.size();

  std::vector<ConstraintFootprint> fps;
  fps.reserve(n);
  for (const auto &c : constraints)
    fps.push_back(footprintOf(c));

  DSU dsu(n);

  // First-toucher owner per (array, offset) and per "any byte of array".
  std::unordered_map<std::string, std::unordered_map<std::uint32_t, int>>
      offsetOwner;
  std::unordered_map<std::string, int> wildcardOwner;

  for (std::size_t i = 0; i < n; ++i) {
    const auto &fp = fps[i];

    // Symbolic-index reads depend on every byte: unite with all prior owners.
    for (const auto &name : fp.symbolicArrays) {
      auto wIt = wildcardOwner.find(name);
      if (wIt != wildcardOwner.end())
        dsu.unite(static_cast<int>(i), wIt->second);
      else
        wildcardOwner[name] = static_cast<int>(i);

      auto offIt = offsetOwner.find(name);
      if (offIt != offsetOwner.end()) {
        for (auto &kv : offIt->second)
          dsu.unite(static_cast<int>(i), kv.second);
      }
    }

    for (const auto &[name, offs] : fp.concreteOffsets) {
      auto wIt = wildcardOwner.find(name);
      if (wIt != wildcardOwner.end())
        dsu.unite(static_cast<int>(i), wIt->second);

      auto &offMap = offsetOwner[name];
      for (auto off : offs) {
        auto it = offMap.find(off);
        if (it != offMap.end())
          dsu.unite(static_cast<int>(i), it->second);
        else
          offMap[off] = static_cast<int>(i);
      }
    }
  }

  std::unordered_map<int, ByteComponent> byRoot;
  for (std::size_t i = 0; i < n; ++i) {
    int r = dsu.find(static_cast<int>(i));
    auto &comp = byRoot[r];
    comp.constraintIndices.push_back(i);
    const auto &fp = fps[i];

    auto markSymbolic = [&](const std::string &name) {
      comp.arrayNames.insert(name);
      comp.arrayCoverage[name] = std::nullopt;
    };
    for (const auto &name : fp.symbolicArrays)
      markSymbolic(name);

    for (const auto &[name, offs] : fp.concreteOffsets) {
      comp.arrayNames.insert(name);
      auto it = comp.arrayCoverage.find(name);
      if (it == comp.arrayCoverage.end()) {
        std::uint32_t mx = 0;
        for (auto o : offs) if (o + 1 > mx) mx = o + 1;
        comp.arrayCoverage[name] = mx;
      } else if (it->second.has_value()) {
        std::uint32_t cur = *it->second;
        for (auto o : offs) if (o + 1 > cur) cur = o + 1;
        it->second = cur;
      }
      // nullopt (symbolic) is preserved.
    }
  }

  std::vector<ByteComponent> out;
  out.reserve(byRoot.size());
  for (auto &kv : byRoot)
    out.push_back(std::move(kv.second));
  return out;
}

bool OfflineEngine::getInitialValues(
    const std::vector<ref<Expr>> &constraints,
    const std::vector<const Array *> &arrays,
    std::vector<std::vector<unsigned char>> &assignment, bool &timedOut) {
  timedOut = false;
  ConstraintSet cs(constraints);
  Query q(cs, ConstantExpr::alloc(0, Expr::Bool));
  bool hasSolution = false;
  bool ok = solver_->impl->computeInitialValues(q, arrays, assignment,
                                                hasSolution);
  if (!ok) {
    if (solver_->impl->getOperationStatusCode() ==
        SolverImpl::SOLVER_RUN_STATUS_TIMEOUT)
      timedOut = true;
    return false;
  }
  return hasSolution;
}

} // namespace klee_pcache_opt
