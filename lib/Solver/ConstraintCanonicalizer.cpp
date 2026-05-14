#include "klee/Solver/ConstraintCanonicalizer.h"

#include "klee/Expr/ArrayExprVisitor.h"
#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/ExprPPrinter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <unordered_map>

using namespace klee;

bool klee::ExprCanonicalOrder::operator()(const ref<Expr> &a,
                                          const ref<Expr> &b) const {
  if (a.get() == b.get()) return false;
  if (a->getKind() != b->getKind()) return a->getKind() < b->getKind();
  if (a->getWidth() != b->getWidth()) return a->getWidth() < b->getWidth();

  if (a->getKind() == Expr::Constant) {
    const ConstantExpr *ca = cast<ConstantExpr>(a);
    const ConstantExpr *cb = cast<ConstantExpr>(b);
    return ca->getAPValue().ult(cb->getAPValue());
  }

  if (a->getKind() == Expr::Read) {
    const ReadExpr *ra = cast<ReadExpr>(a);
    const ReadExpr *rb = cast<ReadExpr>(b);
    int cmp = ra->updates.root->name.compare(rb->updates.root->name);
    if (cmp != 0) return cmp < 0;
    if (operator()(ra->index, rb->index)) return true;
    if (operator()(rb->index, ra->index)) return false;
    // Indices equivalent — also order by the update list so this is a *total*
    // order. A non-total comparator leaves std::sort's tie order input-
    // dependent, which would make DFS array naming vary across runs.
    ref<UpdateNode> ua = ra->updates.head;
    ref<UpdateNode> ub = rb->updates.head;
    while (ua && ub) {
      if (operator()(ua->index, ub->index)) return true;
      if (operator()(ub->index, ua->index)) return false;
      if (operator()(ua->value, ub->value)) return true;
      if (operator()(ub->value, ua->value)) return false;
      ua = ua->next;
      ub = ub->next;
    }
    if (ua) return false; // ra's update list is longer
    if (ub) return true;  // rb's update list is longer
    return false;         // fully equivalent
  }

  unsigned ak = a->getNumKids();
  unsigned bk = b->getNumKids();
  if (ak != bk) return ak < bk;

  for (unsigned i = 0; i < ak; ++i) {
    if (operator()(a->getKid(i), b->getKid(i))) return true;
    if (operator()(b->getKid(i), a->getKid(i))) return false;
  }
  return false;
}

namespace {

// Assigns each Array a DFS first-appearance index, giving canonical names
// A0, A1, ... independent of allocation order.
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

// Rewrites ReadExprs onto canonical arrays, walking UpdateList chains so
// symbolic writes are substituted too.
class ArraySubstitutionVisitor : public ExprVisitor {
  const std::map<const Array *, const Array *> &subst_;

public:
  explicit ArraySubstitutionVisitor(
      const std::map<const Array *, const Array *> &subst)
      : ExprVisitor(/*recursive=*/true), subst_(subst) {}

  Action visitRead(const ReadExpr &re) override {
    const UpdateList &ul = re.updates;
    const Array *root = ul.root;

    auto it = subst_.find(root);
    if (it == subst_.end() && !ul.head)
      return Action::doChildren();

    const Array *newRoot = (it != subst_.end()) ? it->second : root;

    // ul.head is the newest update; reverse so extend() replays oldest-first.
    std::vector<ref<UpdateNode>> nodes;
    for (ref<UpdateNode> un = ul.head; un; un = un->next)
      nodes.push_back(un);

    UpdateList newUL(newRoot, nullptr);
    for (auto rit = nodes.rbegin(); rit != nodes.rend(); ++rit) {
      ref<UpdateNode> un = *rit;
      ref<Expr> newIdx = visit(un->index);
      ref<Expr> newVal = visit(un->value);
      newUL.extend(newIdx, newVal);
    }

    ref<Expr> newIndex = visit(re.index);
    return Action::changeTo(ReadExpr::create(newUL, newIndex));
  }
};

} // anonymous namespace

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
  case Expr::Add:  return AddExpr::create(kids[0], kids[1]);
  case Expr::And:  return AndExpr::create(kids[0], kids[1]);
  case Expr::Or:   return OrExpr::create(kids[0], kids[1]);
  case Expr::Xor:  return XorExpr::create(kids[0], kids[1]);
  case Expr::Mul:  return MulExpr::create(kids[0], kids[1]);
  case Expr::Eq:   return EqExpr::create(kids[0], kids[1]);
  case Expr::Ne:   return NeExpr::create(kids[0], kids[1]);
  case Expr::Ult:  return UltExpr::create(kids[0], kids[1]);
  case Expr::Ule:  return UleExpr::create(kids[0], kids[1]);
  case Expr::Slt:  return SltExpr::create(kids[0], kids[1]);
  case Expr::Sle:  return SleExpr::create(kids[0], kids[1]);
  case Expr::Sub:  return SubExpr::create(kids[0], kids[1]);
  case Expr::UDiv: return UDivExpr::create(kids[0], kids[1]);
  case Expr::SDiv: return SDivExpr::create(kids[0], kids[1]);
  case Expr::URem: return URemExpr::create(kids[0], kids[1]);
  case Expr::SRem: return SRemExpr::create(kids[0], kids[1]);
  case Expr::Shl:  return ShlExpr::create(kids[0], kids[1]);
  case Expr::LShr: return LShrExpr::create(kids[0], kids[1]);
  case Expr::AShr: return AShrExpr::create(kids[0], kids[1]);
  case Expr::ZExt: return ZExtExpr::create(kids[0], orig->getWidth());
  case Expr::SExt: return SExtExpr::create(kids[0], orig->getWidth());
  case Expr::Select:
    return SelectExpr::create(kids[0], kids[1], kids[2]);
  case Expr::Concat:
    return ConcatExpr::create(kids[0], kids[1]);
  case Expr::Extract: {
    // Extract carries offset/width as metadata, not kids.
    const ExtractExpr *ee = cast<ExtractExpr>(orig);
    return ExtractExpr::create(kids[0], ee->offset, ee->width);
  }
  case Expr::Constant:
    return orig;
  case Expr::Read: {
    // Update list is already on canonical arrays (ArraySubstitutionVisitor);
    // rebuild with the canonicalized index rather than dropping it.
    const ReadExpr *re = cast<ReadExpr>(orig);
    return ReadExpr::create(re->updates, kids[0]);
  }
  default:
    llvm_unreachable("rebuildWithKids: unhandled expression kind");
  }
}

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

  // Eq/Ne are commutative but not associative — flattening across them
  // would mix widths. Just sort the two children.
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
                          ArrayCache &arrayCache) {
  CanonicalizationResult res;
  res.constraints = constraints;

  ExprCanonicalOrder cmp;
  // Sort first so the DFS encounter order — which drives canonical name
  // assignment — is itself pointer-independent.
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

  // Renaming and tree canonicalization can perturb the relative order; re-sort.
  std::sort(res.constraints.begin(), res.constraints.end(), cmp);

  return res;
}

std::pair<std::set<std::string>, CanonicalizationResult>
buildConstraintDiskKey(const std::vector<ref<Expr>> &constraints,
                       ArrayCache &arrayCache) {
  CanonicalizationResult canon =
      canonicalizeConstraintSet(constraints, arrayCache);

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