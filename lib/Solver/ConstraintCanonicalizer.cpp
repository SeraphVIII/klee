#include "klee/Solver/ConstraintCanonicalizer.h"

#include "klee/Expr/ArrayExprVisitor.h"
#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/ExprPPrinter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SHA1.h"

#include <algorithm>
#include <unordered_map>

using namespace klee;

///===----------------------------------------------------------------------===///
/// ExprCanonicalOrder
///===----------------------------------------------------------------------===///

bool klee::ExprCanonicalOrder::operator()(const ref<Expr> &a,
                                          const ref<Expr> &b) const {
  if (a.get() == b.get()) return false;
  if (a->getKind() != b->getKind()) return a->getKind() < b->getKind();
  if (a->getWidth() != b->getWidth()) return a->getWidth() < b->getWidth();

  // For constants, compare the value directly — fully deterministic.
  if (a->getKind() == Expr::Constant) {
    const ConstantExpr *ca = cast<ConstantExpr>(a);
    const ConstantExpr *cb = cast<ConstantExpr>(b);
    return ca->getAPValue().ult(cb->getAPValue());
  }

  // For reads, compare array names then the index.
  if (a->getKind() == Expr::Read) {
    const ReadExpr *ra = cast<ReadExpr>(a);
    const ReadExpr *rb = cast<ReadExpr>(b);
    int cmp = ra->updates.root->name.compare(rb->updates.root->name);
    if (cmp != 0) return cmp < 0;
    return operator()(ra->index, rb->index);
  }

  unsigned ak = a->getNumKids();
  unsigned bk = b->getNumKids();
  if (ak != bk) return ak < bk;

  for (unsigned i = 0; i < ak; ++i) {
    if (operator()(a->getKid(i), b->getKid(i))) return true;
    if (operator()(b->getKid(i), a->getKid(i))) return false;
  }

  // Truly structurally identical — not less-than.
  return false;
}

///===----------------------------------------------------------------------===///
/// Alpha-renaming support
///===----------------------------------------------------------------------===///

namespace {

///===----------------------------------------------------------------------===//
/// Collects Arrays in deterministic DFS order, assigning each a position
/// index based on first appearance. This order is used to generate stable
/// canonical names (A0, A1, A2, ...) independent of original array names
/// or allocation order.
///===----------------------------------------------------------------------===//
class ArrayOrderCollector : public ExprVisitor {
  std::unordered_map<const Array *, unsigned> &order_;
  unsigned &nextIndex_;

public:
  ArrayOrderCollector(std::unordered_map<const Array *, unsigned> &order,
                      unsigned &nextIndex)
      : ExprVisitor(/*recursive=*/true), order_(order), nextIndex_(nextIndex) {}

  Action visitRead(const ReadExpr &re) override {
    const Array *root = re.updates.root;
    if (root && !order_.count(root))
      order_[root] = nextIndex_++;

    for (ref<UpdateNode> un = re.updates.head; un; un = un->next) {
      visit(un->index);
      visit(un->value);
    }

    return ExprVisitor::visitRead(re);
  }
};

///===----------------------------------------------------------------------===//
/// Rewrites every ReadExpr whose root Array appears in subst_, replacing it
/// with the corresponding canonical Array. Also walks the full UpdateList
/// chain so that symbolic writes (array updates) are substituted correctly.
///
/// subst_ is the forwardArrayMap from CanonicalizationResult:
///   original Array*  -->  canonical Array*  (e.g. arr_foo --> A2)
///===----------------------------------------------------------------------===//
class ArraySubstitutionVisitor : public ExprVisitor {
  const std::map<const Array *, const Array *> &subst_;

public:
  explicit ArraySubstitutionVisitor(
      const std::map<const Array *, const Array *> &subst)
      : ExprVisitor(/*recursive=*/true), subst_(subst) {}

  Action visitRead(const ReadExpr &re) override {
    const UpdateList &ul = re.updates;
    const Array *root = ul.root;

    // Look up the root array. If it isn't in subst_ we still need to
    // rebuild if the update chain contains expressions that reference
    // other arrays that *are* in subst_.
    auto it = subst_.find(root);
    const Array *newRoot = (it != subst_.end()) ? it->second : root;

    // Rebuild the update chain oldest-first (head is most recent write,
    // so we collect into a vector and replay in reverse).
    //
    // Example: if the chain is  [write idx2 val2] -> [write idx1 val1] -> nil
    // we want to replay write idx1 first, then write idx2, so the rebuilt
    // chain has the same logical meaning.
    std::vector<ref<UpdateNode>> nodes;
    for (ref<UpdateNode> un = ul.head; un; un = un->next)
      nodes.push_back(un);

    UpdateList newUL(newRoot, nullptr);
    for (auto rit = nodes.rbegin(); rit != nodes.rend(); ++rit) {
      ref<UpdateNode> un = *rit;
      // visit() recursively applies this substitution to sub-expressions,
      // so any nested array reads inside the index/value are also renamed.
      ref<Expr> newIdx = visit(un->index);
      ref<Expr> newVal = visit(un->value);
      newUL.extend(newIdx, newVal);
    }

    // Substitute inside the read index as well.
    ref<Expr> newIndex = visit(re.index);

    return Action::changeTo(ReadExpr::create(newUL, newIndex));
  }
};

} // anonymous namespace

///===----------------------------------------------------------------------===///
/// Expression tree canonicalization (static helpers)
///===----------------------------------------------------------------------===///

static bool isCommutativeKind(Expr::Kind k) {
  switch (k) {
  case Expr::Add:
  case Expr::And:
  case Expr::Or:
  case Expr::Xor:
  case Expr::Mul:
  case Expr::Eq:
  case Expr::Ne:
    return true;
  default:
    return false;
  }
}

static ref<Expr> rebuildWithKids(const ref<Expr> &orig,
                                 const std::vector<ref<Expr>> &kids) {
  switch (orig->getKind()) {
  // --- commutative / associative ---
  case Expr::Add:  return AddExpr::create(kids[0], kids[1]);
  case Expr::And:  return AndExpr::create(kids[0], kids[1]);
  case Expr::Or:   return OrExpr::create(kids[0], kids[1]);
  case Expr::Xor:  return XorExpr::create(kids[0], kids[1]);
  case Expr::Mul:  return MulExpr::create(kids[0], kids[1]);
  case Expr::Eq:   return EqExpr::create(kids[0], kids[1]);

  // --- comparisons ---
  case Expr::Ne:   return NeExpr::create(kids[0], kids[1]);
  case Expr::Ult:  return UltExpr::create(kids[0], kids[1]);
  case Expr::Ule:  return UleExpr::create(kids[0], kids[1]);
  case Expr::Slt:  return SltExpr::create(kids[0], kids[1]);
  case Expr::Sle:  return SleExpr::create(kids[0], kids[1]);

  // --- arithmetic ---
  case Expr::Sub:  return SubExpr::create(kids[0], kids[1]);
  case Expr::UDiv: return UDivExpr::create(kids[0], kids[1]);
  case Expr::SDiv: return SDivExpr::create(kids[0], kids[1]);
  case Expr::URem: return URemExpr::create(kids[0], kids[1]);
  case Expr::SRem: return SRemExpr::create(kids[0], kids[1]);

  // --- shifts ---
  case Expr::Shl:  return ShlExpr::create(kids[0], kids[1]);
  case Expr::LShr: return LShrExpr::create(kids[0], kids[1]);
  case Expr::AShr: return AShrExpr::create(kids[0], kids[1]);

  // --- casts ---
  case Expr::ZExt: return ZExtExpr::create(kids[0], orig->getWidth());
  case Expr::SExt: return SExtExpr::create(kids[0], orig->getWidth());

  // --- misc ---
  case Expr::Select:
    return SelectExpr::create(kids[0], kids[1], kids[2]);
  case Expr::Concat:
    return ConcatExpr::create(kids[0], kids[1]);
  case Expr::Extract: {
    // Extract carries offset and width as metadata, not kids.
    const ExtractExpr *ee = cast<ExtractExpr>(orig);
    return ExtractExpr::create(kids[0], ee->offset, ee->width);
  }

  // --- leaves (should not reach here since getNumKids()==0) ---
  case Expr::Constant:
  case Expr::Read:
    // ReadExpr kids are only the index; update list is handled separately
    // by ArraySubstitutionVisitor — do not rebuild here.
    return orig;

  default:
    llvm_unreachable("rebuildWithKids: unhandled expression kind");
  }
}

// Only called for commutative/associative kinds, so the list is short.
static ref<Expr> rebuildWithKind(Expr::Kind k,
                                 const ref<Expr> &a,
                                 const ref<Expr> &b) {
  switch (k) {
  case Expr::Add:  return AddExpr::create(a, b);
  case Expr::And:  return AndExpr::create(a, b);
  case Expr::Or:   return OrExpr::create(a, b);
  case Expr::Xor:  return XorExpr::create(a, b);
  case Expr::Mul:  return MulExpr::create(a, b);
  case Expr::Eq:   return EqExpr::create(a, b);
  case Expr::Ne:   return NeExpr::create(a, b);
  default:
    llvm_unreachable("rebuildWithKind: not a binary commutative kind");
  }
}

static ref<Expr> flattenAndRebuildAssoc(ref<Expr> e) {
  if (!isCommutativeKind(e->getKind()))
    return e;

  Expr::Kind k = e->getKind();
  ExprCanonicalOrder cmp;

  // Eq and Ne are commutative but NOT associative: flattening
  // Eq(Eq(a,b), c) into {a,b,c} would mix expression widths.
  // Just sort the two children for a canonical argument order.
  if (k == Expr::Eq || k == Expr::Ne) {
    assert(e->getNumKids() == 2);
    ref<Expr> a = e->getKid(0), b = e->getKid(1);
    if (cmp(b, a)) std::swap(a, b);
    std::vector<ref<Expr>> sorted = {a, b};
    return rebuildWithKids(e, sorted);
  }

  std::vector<ref<Expr>> elems;
  std::vector<ref<Expr>> stack;
  stack.push_back(e);

  while (!stack.empty()) {
    ref<Expr> cur = stack.back();
    stack.pop_back();
    if (cur->getKind() == k && cur->getNumKids() == 2) {
      stack.push_back(cur->getKid(0));
      stack.push_back(cur->getKid(1));
    } else {
      elems.push_back(cur);
    }
  }

  std::sort(elems.begin(), elems.end(), cmp);

  while (elems.size() > 1) {
    std::vector<ref<Expr>> next;
    for (size_t i = 0; i + 1 < elems.size(); i += 2)
      next.push_back(rebuildWithKind(k, elems[i], elems[i + 1]));
    if (elems.size() & 1)
      next.push_back(elems.back());
    elems.swap(next);
  }

  return elems[0];
}

///===----------------------------------------------------------------------===///
/// Public API — all inside namespace klee so symbols match the header
///===----------------------------------------------------------------------===///

namespace klee {

ref<Expr> canonicalizeExprTree(ref<Expr> e) {
  if (e.isNull())
    return e;

  unsigned n = e->getNumKids();
  if (n == 0)
    return e;

  std::vector<ref<Expr>> kids;
  kids.reserve(n);
  for (unsigned i = 0; i < n; ++i)
    kids.push_back(canonicalizeExprTree(e->getKid(i)));

  if (isCommutativeKind(e->getKind())) {
    ref<Expr> tmp = rebuildWithKids(e, kids);
    return flattenAndRebuildAssoc(tmp);
  }

  return rebuildWithKids(e, kids);
}

CanonicalizationResult
canonicalizeConstraintSet(const std::vector<ref<Expr>> &constraints,
                          ExprBuilder &builder,
                          ArrayCache &arrayCache) {
  CanonicalizationResult res;
  res.constraints = constraints;

  ExprCanonicalOrder cmp;
  // First sort: establish a deterministic DFS encounter order for arrays so
  // that the first-seen array gets canonical name A0, the second A1, etc.
  // The ordering of constraints here drives which array gets which index.
  std::sort(res.constraints.begin(), res.constraints.end(), cmp);

  std::unordered_map<const Array *, unsigned> order;
  unsigned nextIndex = 0;
  ArrayOrderCollector collector(order, nextIndex);
  for (auto &e : res.constraints)
    collector.visit(e);

  for (auto &kv : order) {
    const Array *orig = kv.first;
    unsigned pos = kv.second;
    std::string canonName = "A" + llvm::utostr(pos);

    const ref<ConstantExpr> *cbegin = nullptr;
    const ref<ConstantExpr> *cend   = nullptr;
    if (!orig->constantValues.empty()) {
      cbegin = &orig->constantValues[0];
      cend   = cbegin + orig->constantValues.size();
    }

    const Array *canon =
        arrayCache.CreateArray(canonName, orig->size,
                               cbegin, cend,
                               orig->domain, orig->range);

    res.forwardArrayMap[orig] = canon;
    res.inverseArrayMap[canon] = orig;
  }

  ArraySubstitutionVisitor subst(res.forwardArrayMap);
  for (auto &e : res.constraints)
    e = subst.visit(e);

  for (auto &e : res.constraints)
    e = canonicalizeExprTree(e);

  // Second sort: array renaming (A0, A1, ...) and expression-tree
  // canonicalization can both change expression structure, which may alter
  // their relative order under ExprCanonicalOrder.  Re-sort to restore the
  // canonical ordering before the constraints are serialized into disk keys.
  std::sort(res.constraints.begin(), res.constraints.end(), cmp);

  return res;
}

std::string serializeCanonicalConstraints(
    const std::vector<ref<Expr>> &canonConstraints) {
  std::string out;
  llvm::raw_string_ostream os(out);

  for (auto &e : canonConstraints) {
    ExprPPrinter::printSingleExpr(os, e);
    os << '\n';
  }
  os.flush();
  return out;
}

std::string computeCanonicalKey(
    const std::vector<ref<Expr>> &canonConstraints) {
  std::string buf = serializeCanonicalConstraints(canonConstraints);

  llvm::SHA1 hash;
  hash.update(buf);
  auto digest = hash.final();

  std::string hex;
  hex.reserve(digest.size() * 2);
  for (uint8_t b : digest) {
    hex.push_back("0123456789abcdef"[b >> 4]);
    hex.push_back("0123456789abcdef"[b & 0xf]);
  }
  return hex;
}

std::pair<std::set<std::string>, CanonicalizationResult>
buildConstraintDiskKey(const std::vector<ref<Expr>> &constraints,
                       ExprBuilder &builder,
                       ArrayCache &arrayCache) {
  CanonicalizationResult canon =
      canonicalizeConstraintSet(constraints, builder, arrayCache);

  std::set<std::string> key;
  for (const auto &e : canon.constraints) {
    std::string s;
    llvm::raw_string_ostream os(s);
    ExprPPrinter::printSingleExpr(os, e);
    os.flush();
    key.insert(s);
  }
  return {std::move(key), std::move(canon)};
}

} // namespace klee