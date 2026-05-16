#include "klee/Solver/ConstraintCanonicalizer.h"

#include "klee/Expr/ArrayExprVisitor.h"
#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/ExprPPrinter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

using namespace klee;

namespace {

// Three-way compare matching ExprCanonicalOrder's order, memoised on (a,b)
// pairs: Expr operands are shared DAGs, so an unmemoised recursion is
// exponential in the sharing factor. Returns <0 / 0 / >0.
int compareExpr(const ref<Expr> &a, const ref<Expr> &b,
                std::map<std::pair<const Expr *, const Expr *>, int> &memo) {
  if (a.get() == b.get())
    return 0;

  std::pair<const Expr *, const Expr *> key(a.get(), b.get());
  auto it = memo.find(key);
  if (it != memo.end())
    return it->second;

  int result;
  if (a->getKind() != b->getKind()) {
    result = a->getKind() < b->getKind() ? -1 : 1;
  } else if (a->getWidth() != b->getWidth()) {
    result = a->getWidth() < b->getWidth() ? -1 : 1;
  } else if (a->getKind() == Expr::Constant) {
    const ConstantExpr *ca = cast<ConstantExpr>(a);
    const ConstantExpr *cb = cast<ConstantExpr>(b);
    if (ca->getAPValue().ult(cb->getAPValue()))
      result = -1;
    else if (cb->getAPValue().ult(ca->getAPValue()))
      result = 1;
    else
      result = 0;
  } else if (a->getKind() == Expr::Read) {
    const ReadExpr *ra = cast<ReadExpr>(a);
    const ReadExpr *rb = cast<ReadExpr>(b);
    int cmp = ra->updates.root->name.compare(rb->updates.root->name);
    if (cmp != 0) {
      result = cmp < 0 ? -1 : 1;
    } else {
      result = compareExpr(ra->index, rb->index, memo);
      if (result == 0) {
        // Indices equivalent — also order by the update list so this is a
        // *total* order. A non-total comparator leaves std::sort's tie order
        // input-dependent, which would make DFS array naming vary across runs.
        ref<UpdateNode> ua = ra->updates.head;
        ref<UpdateNode> ub = rb->updates.head;
        while (ua && ub && result == 0) {
          result = compareExpr(ua->index, ub->index, memo);
          if (result == 0)
            result = compareExpr(ua->value, ub->value, memo);
          ua = ua->next;
          ub = ub->next;
        }
        if (result == 0) {
          if (ua)
            result = 1; // ra's update list is longer
          else if (ub)
            result = -1; // rb's update list is longer
        }
      }
    }
  } else {
    unsigned ak = a->getNumKids();
    unsigned bk = b->getNumKids();
    if (ak != bk) {
      result = ak < bk ? -1 : 1;
    } else {
      result = 0;
      for (unsigned i = 0; i < ak && result == 0; ++i)
        result = compareExpr(a->getKid(i), b->getKid(i), memo);
    }
  }

  memo.emplace(key, result);
  return result;
}

} // namespace

bool klee::ExprCanonicalOrder::operator()(const ref<Expr> &a,
                                          const ref<Expr> &b) const {
  std::map<std::pair<const Expr *, const Expr *>, int> memo;
  return compareExpr(a, b, memo) < 0;
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
  case Expr::Not:  return NotExpr::create(kids[0]);
  case Expr::NotOptimized: return NotOptimizedExpr::create(kids[0]);
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
    // Update list normalised one level up in canonicalizeExprTreeMemo.
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

// Memoised tree canonicalisation: same exponential-on-shared-DAGs problem as
// compareExpr. Keying on the original Expr* is sound — canonicalisation is a
// pure function of node structure.
static ref<Expr>
canonicalizeExprTreeMemo(const ref<Expr> &e,
                         std::unordered_map<const Expr *, ref<Expr>> &memo) {
  if (e.isNull())
    return e;

  unsigned n = e->getNumKids();
  if (n == 0)
    return e;

  auto it = memo.find(e.get());
  if (it != memo.end())
    return it->second;

  std::vector<ref<Expr>> kids;
  kids.reserve(n);
  for (unsigned i = 0; i < n; ++i)
    kids.push_back(canonicalizeExprTreeMemo(e->getKid(i), memo));

  ref<Expr> result;
  if (e->getKind() == Expr::Read) {
    // ArraySubstitutionVisitor renames arrays in writes but leaves commutative
    // subtrees unsorted; canonicalise un->index / un->value here too so
    // equivalent constraints hash to the same disk key.
    const ReadExpr *re = cast<ReadExpr>(e.get());
    const UpdateList &ul = re->updates;
    if (!ul.head) {
      result = ReadExpr::create(ul, kids[0]);
    } else {
      std::vector<ref<UpdateNode>> nodes;
      for (ref<UpdateNode> un = ul.head; un; un = un->next)
        nodes.push_back(un);
      UpdateList newUL(ul.root, nullptr);
      // extend() prepends; replay oldest-first to preserve head=newest.
      for (auto rit = nodes.rbegin(); rit != nodes.rend(); ++rit) {
        ref<Expr> newIdx = canonicalizeExprTreeMemo((*rit)->index, memo);
        ref<Expr> newVal = canonicalizeExprTreeMemo((*rit)->value, memo);
        newUL.extend(newIdx, newVal);
      }
      result = ReadExpr::create(newUL, kids[0]);
    }
  } else if (isCommutativeKind(e->getKind())) {
    ref<Expr> tmp = rebuildWithKids(e, kids);
    result = flattenAndRebuildAssoc(tmp);
  } else {
    result = rebuildWithKids(e, kids);
  }

  memo.emplace(e.get(), result);
  return result;
}

namespace klee {

ref<Expr> canonicalizeExprTree(ref<Expr> e) {
  std::unordered_map<const Expr *, ref<Expr>> memo;
  return canonicalizeExprTreeMemo(e, memo);
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

  // Shared memo: constraints share sub-DAGs after substitution.
  std::unordered_map<const Expr *, ref<Expr>> treeMemo;
  for (auto &e : res.constraints)
    e = canonicalizeExprTreeMemo(e, treeMemo);

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