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

#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace klee;
using namespace klee::expr;

namespace klee_pcache_opt {

// Synthetic array size used when reconstructing canonical-key arrays.  The
// concrete size is irrelevant for symbolic-array satisfiability — Z3 models
// arrays without a size constraint — but the parser requires a literal here.
static constexpr unsigned kSyntheticArraySize = 4096;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Canonical-key parsing
// ---------------------------------------------------------------------------

// Scan a constraint string for canonical array-name references (A0, A1, ...).
// The canonicalizer only ever uses the form A<digits>, so a hand-rolled scan
// over identifier-character runs is sufficient and cheap.
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

ParsedKey OfflineEngine::parseKey(const std::set<std::string> &key) {
  ParsedKey result;

  if (key.empty()) {
    result.ok = true;
    return result;
  }

  // 1. Discover all array names referenced anywhere in the key.
  std::set<std::string> namesSet;
  for (const auto &s : key)
    collectArrayNames(s, namesSet);

  // Sort by canonical index (A0 < A1 < ... < A10) so that the array order in
  // the result matches the canonicalisation order.
  std::vector<std::string> names(namesSet.begin(), namesSet.end());
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b) {
              unsigned ai = std::strtoul(a.c_str() + 1, nullptr, 10);
              unsigned bi = std::strtoul(b.c_str() + 1, nullptr, 10);
              return ai < bi;
            });

  // 2. Synthesise a KQuery source: array decls + (query [...] false).
  std::ostringstream src;
  for (const auto &name : names) {
    src << "array " << name << '[' << kSyntheticArraySize
        << "] : w32 -> w8 = symbolic\n";
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

  // 3. Walk decls to recover arrays (in declaration order) and the query.
  for (const auto &d : decls) {
    if (auto *AD = llvm::dyn_cast<ArrayDecl>(d.get())) {
      result.arrays.push_back(AD->Root);
    } else if (auto *QC = llvm::dyn_cast<QueryCommand>(d.get())) {
      result.constraints.assign(QC->Constraints.begin(), QC->Constraints.end());
    }
  }

  // The Arrays in `result.arrays` and the ReadExprs inside `result.constraints`
  // hold raw pointers into P's ArrayCache.  Keep both alive on the result.
  result._decls = std::move(decls);
  result._parser = std::move(P);
  result.ok = true;
  return result;
}

// ---------------------------------------------------------------------------
// Solver wrappers
// ---------------------------------------------------------------------------

bool OfflineEngine::isUnsat(const std::vector<ref<Expr>> &constraints,
                            bool &timedOut) {
  timedOut = false;
  // Solver::mustBeTrue / mayBeTrue short-circuit when the query expression is
  // a constant (e.g., `false`), so we cannot use them to test "is this
  // constraint set UNSAT?".  Drop into the impl directly: an empty objects
  // vector makes computeInitialValues report SAT/UNSAT without serialising
  // any model.
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

  // Precondition: the full set must be UNSAT (under symbolic-array
  // reconstruction).  Otherwise the canonical key relied on concrete-array
  // contents we cannot reconstruct, and shrinking would be unsound.
  bool timedOut = false;
  ++res.solverCalls;
  if (!isUnsat(constraints, timedOut)) {
    if (timedOut) res.timedOut = true;
    else          res.notUnsatPrecondition = true;
    return res;
  }

  // Single-element delta-debugging: try removing one constraint at a time.
  // If the residue is still UNSAT, accept the removal and re-scan from the
  // current position (the removed element's "slot").  Otherwise advance.
  std::vector<ref<Expr>> sub;
  sub.reserve(n);
  size_t i = 0;
  while (i < res.keptIndices.size()) {
    if (res.keptIndices.size() == 1) break; // can't shrink past 1

    sub.clear();
    for (size_t j = 0; j < res.keptIndices.size(); ++j)
      if (j != i)
        sub.push_back(constraints[res.keptIndices[j]]);

    bool subTimedOut = false;
    ++res.solverCalls;
    bool subUnsat = isUnsat(sub, subTimedOut);

    if (subTimedOut) {
      // Treat timeout as "cannot remove" and advance — minimisation may still
      // make progress on other indices.
      res.timedOut = true;
      ++i;
      continue;
    }
    if (subUnsat) {
      res.keptIndices.erase(res.keptIndices.begin() + i);
      // Re-examine the same index (which now points at the next constraint).
    } else {
      ++i;
    }
  }
  return res;
}

// ---------------------------------------------------------------------------
// Byte-level dependency components (Pass 2)
// ---------------------------------------------------------------------------

namespace {

// Per-constraint footprint: which arrays it touches, and at which bytes.
// `symbolic` means a non-constant index — the constraint depends on every
// byte of that array.
struct ConstraintFootprint {
  // arrayName -> set of constant byte offsets (empty if `symbolicArrays`
  // contains the same name).
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

  // Owners: for each (array, offset) pair and for each "any byte of array",
  // remember the first constraint that touched it; subsequent touchers union
  // with that owner.
  std::unordered_map<std::string, std::unordered_map<std::uint32_t, int>>
      offsetOwner;
  std::unordered_map<std::string, int> wildcardOwner;

  for (std::size_t i = 0; i < n; ++i) {
    const auto &fp = fps[i];

    // Symbolic-index arrays: this constraint depends on every byte of each
    // such array, so unite with every prior owner of that array.
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

    // Concrete offsets: unite with prior owner of (array, offset), and with
    // any wildcard owner of array.
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

  // Group constraint indices by component root, accumulating array coverage.
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
      // else: already nullopt (symbolic) → leave as-is
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
