#include "klee/Solver/ConstraintCanonicalizer.h"

#include "klee/Expr/ArrayExprVisitor.h"
#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/ExprPPrinter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace klee;

namespace {

// ---------------------------------------------------------------------------
// All DAG traversals below are explicit-stack iterative rather than recursive.
// KLEE expression trees can be thousands of levels deep on long-running states
// (e.g. coreutils at multi-million instruction budgets), and a recursive DFS
// would recurse to the full tree depth and overflow the C++ stack. Memoisation
// bounds total *work* but not stack *depth*, so the recursion has to go.
// ---------------------------------------------------------------------------

// Shallow (no-descent) part of the structural comparison. Returns true and
// sets `out` when (a,b) can be ordered without comparing children; returns
// false when the caller must compare children (Read index + update list, or
// the kids of an internal node).
bool compareShallow(const Expr *a, const Expr *b, int &out) {
  if (a == b) {
    out = 0;
    return true;
  }
  if (a->getKind() != b->getKind()) {
    out = a->getKind() < b->getKind() ? -1 : 1;
    return true;
  }
  if (a->getWidth() != b->getWidth()) {
    out = a->getWidth() < b->getWidth() ? -1 : 1;
    return true;
  }
  if (a->getKind() == Expr::Constant) {
    const ConstantExpr *ca = cast<ConstantExpr>(a);
    const ConstantExpr *cb = cast<ConstantExpr>(b);
    if (ca->getAPValue().ult(cb->getAPValue()))
      out = -1;
    else if (cb->getAPValue().ult(ca->getAPValue()))
      out = 1;
    else
      out = 0;
    return true;
  }
  if (a->getKind() == Expr::Read) {
    const ReadExpr *ra = cast<ReadExpr>(a);
    const ReadExpr *rb = cast<ReadExpr>(b);
    int cmp = ra->updates.root->name.compare(rb->updates.root->name);
    if (cmp != 0) {
      out = cmp < 0 ? -1 : 1;
      return true;
    }
    return false; // names equal: descend into index + update list
  }
  if (a->getNumKids() != b->getNumKids()) {
    out = a->getNumKids() < b->getNumKids() ? -1 : 1;
    return true;
  }
  return false; // descend into kids
}

// Ordered list of child (a,b) sub-comparisons for a node that needs descent,
// in the exact order the result depends on them:
//   Read     -> [(index,index)] then per paired update node [(idx,idx),(val,val)]
//   internal -> [(kid0,kid0), ..., (kid_{n-1},kid_{n-1})]
// The result is the first non-zero of these (then, for Read, an update-list
// length tie-break). This reproduces the original recursive comparator exactly.
std::vector<std::pair<ref<Expr>, ref<Expr>>>
compareChildPairs(const ref<Expr> &a, const ref<Expr> &b) {
  std::vector<std::pair<ref<Expr>, ref<Expr>>> v;
  if (a->getKind() == Expr::Read) {
    const ReadExpr *ra = cast<ReadExpr>(a.get());
    const ReadExpr *rb = cast<ReadExpr>(b.get());
    v.emplace_back(ra->index, rb->index);
    ref<UpdateNode> ua = ra->updates.head;
    ref<UpdateNode> ub = rb->updates.head;
    while (ua && ub) {
      v.emplace_back(ua->index, ub->index);
      v.emplace_back(ua->value, ub->value);
      ua = ua->next;
      ub = ub->next;
    }
  } else {
    unsigned n = a->getNumKids();
    for (unsigned i = 0; i < n; ++i)
      v.emplace_back(a->getKid(i), b->getKid(i));
  }
  return v;
}

// -1/0/1 by update-list length (longer list ranks greater); meaningful only
// when both Reads share a name and all paired index/value comparisons tied.
int updateListLenCmp(const ref<Expr> &a, const ref<Expr> &b) {
  ref<UpdateNode> ua = cast<ReadExpr>(a.get())->updates.head;
  ref<UpdateNode> ub = cast<ReadExpr>(b.get())->updates.head;
  while (ua && ub) {
    ua = ua->next;
    ub = ub->next;
  }
  if (ua)
    return 1;
  if (ub)
    return -1;
  return 0;
}

// Iterative three-way compare matching ExprCanonicalOrder. Memoised on (a,b)
// pairs (shared DAGs would otherwise be exponential). Returns <0 / 0 / >0.
int compareExpr(const ref<Expr> &a0, const ref<Expr> &b0,
                std::map<std::pair<const Expr *, const Expr *>, int> &memo) {
  struct Frame {
    ref<Expr> a, b;
    bool resolve;
  };
  std::vector<Frame> stack;
  stack.push_back({a0, b0, false});

  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();
    const Expr *ka = f.a.get();
    const Expr *kb = f.b.get();
    if (ka == kb)
      continue; // compareChildPairs/lookups treat a==b as 0 directly
    std::pair<const Expr *, const Expr *> key(ka, kb);

    if (!f.resolve) {
      if (memo.count(key))
        continue;
      int shallow;
      if (compareShallow(ka, kb, shallow)) {
        memo.emplace(key, shallow);
        continue;
      }
      // Needs children: schedule the combine, then the child pairs above it so
      // they are computed first (LIFO).
      stack.push_back({f.a, f.b, true});
      for (auto &cp : compareChildPairs(f.a, f.b))
        stack.push_back({cp.first, cp.second, false});
    } else {
      if (memo.count(key))
        continue;
      int result = 0;
      for (auto &cp : compareChildPairs(f.a, f.b)) {
        const Expr *ca = cp.first.get();
        const Expr *cb = cp.second.get();
        if (ca == cb)
          continue;
        int r = memo[{ca, cb}];
        if (r != 0) {
          result = r;
          break;
        }
      }
      if (result == 0 && f.a->getKind() == Expr::Read)
        result = updateListLenCmp(f.a, f.b);
      memo.emplace(key, result);
    }
  }

  if (a0.get() == b0.get())
    return 0;
  return memo[{a0.get(), b0.get()}];
}

} // namespace

bool klee::ExprCanonicalOrder::operator()(const ref<Expr> &a,
                                          const ref<Expr> &b) const {
  std::map<std::pair<const Expr *, const Expr *>, int> memo;
  return compareExpr(a, b, memo) < 0;
}

namespace {

// Iterative DFS assigning each Array a first-appearance index, giving canonical
// names A0, A1, ... independent of allocation order. Reproduces the previous
// recursive ExprVisitor pre-order exactly: for a Read, the update-list nodes
// (index then value, head-first) are visited before the read index, and shared
// subexpressions are visited once (first encounter wins).
void collectArrayOrder(const ref<Expr> &root,
                       std::unordered_map<const Array *, unsigned> &order,
                       unsigned &nextIndex,
                       std::unordered_set<const Expr *> &visited) {
  std::vector<ref<Expr>> stack;
  stack.push_back(root);

  while (!stack.empty()) {
    ref<Expr> e = stack.back();
    stack.pop_back();
    if (e.isNull() || isa<ConstantExpr>(e))
      continue;
    const Expr *k = e.get();
    if (!visited.insert(k).second)
      continue;

    // children in visitation order
    std::vector<ref<Expr>> children;
    if (e->getKind() == Expr::Read) {
      const ReadExpr *re = cast<ReadExpr>(k);
      const Array *rootA = re->updates.root;
      if (rootA && !order.count(rootA))
        order[rootA] = nextIndex++;
      for (ref<UpdateNode> un = re->updates.head; un; un = un->next) {
        children.push_back(un->index);
        children.push_back(un->value);
      }
      children.push_back(re->index);
    } else {
      unsigned n = e->getNumKids();
      for (unsigned i = 0; i < n; ++i)
        children.push_back(e->getKid(i));
    }
    // push reversed so children pop in visitation order
    for (auto it = children.rbegin(); it != children.rend(); ++it)
      stack.push_back(*it);
  }
}

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
    // Update list normalised by the caller (canonicalizeExprTree).
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

namespace {

// Helper for the post-order rebuild passes below: the canonicalised/substituted
// form of a child. Leaves (constants, zero-kid nodes) map to themselves; every
// node with kids has been resolved into `memo` before its parent's resolve
// runs (LIFO post-order), so the lookup is always populated.
static ref<Expr>
resolvedChild(const ref<Expr> &x,
              const std::unordered_map<const Expr *, ref<Expr>> &memo) {
  if (x.isNull() || x->getNumKids() == 0)
    return x;
  return memo.at(x.get());
}

// Iterative post-order rewrite of every ReadExpr onto its canonical array,
// walking UpdateList chains so symbolic writes are substituted too. Pure
// structural rewrite, so traversal order does not affect the output.
ref<Expr> substituteArrays(const ref<Expr> &root,
                           const std::map<const Array *, const Array *> &subst,
                           std::unordered_map<const Expr *, ref<Expr>> &memo) {
  struct Frame {
    ref<Expr> e;
    bool resolve;
  };
  std::vector<Frame> stack;
  stack.push_back({root, false});

  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();
    if (f.e.isNull() || f.e->getNumKids() == 0)
      continue;
    const Expr *k = f.e.get();

    if (!f.resolve) {
      if (memo.count(k))
        continue;
      stack.push_back({f.e, true});
      unsigned n = f.e->getNumKids();
      for (unsigned i = 0; i < n; ++i)
        stack.push_back({f.e->getKid(i), false});
      if (f.e->getKind() == Expr::Read) {
        const ReadExpr *re = cast<ReadExpr>(k);
        for (ref<UpdateNode> un = re->updates.head; un; un = un->next) {
          stack.push_back({un->index, false});
          stack.push_back({un->value, false});
        }
      }
    } else {
      if (memo.count(k))
        continue;
      unsigned n = f.e->getNumKids();
      std::vector<ref<Expr>> kids;
      kids.reserve(n);
      for (unsigned i = 0; i < n; ++i)
        kids.push_back(resolvedChild(f.e->getKid(i), memo));

      ref<Expr> result;
      if (f.e->getKind() == Expr::Read) {
        const ReadExpr *re = cast<ReadExpr>(k);
        const UpdateList &ul = re->updates;
        const Array *r = ul.root;
        auto it = subst.find(r);
        if (it == subst.end() && !ul.head) {
          // Root unchanged and no writes: only the index may have changed.
          result = ReadExpr::create(ul, kids[0]);
        } else {
          const Array *newRoot = (it != subst.end()) ? it->second : r;
          // ul.head is newest; replay oldest-first so head stays newest.
          std::vector<ref<UpdateNode>> nodes;
          for (ref<UpdateNode> un = ul.head; un; un = un->next)
            nodes.push_back(un);
          UpdateList newUL(newRoot, nullptr);
          for (auto rit = nodes.rbegin(); rit != nodes.rend(); ++rit)
            newUL.extend(resolvedChild((*rit)->index, memo),
                         resolvedChild((*rit)->value, memo));
          result = ReadExpr::create(newUL, kids[0]);
        }
      } else {
        result = rebuildWithKids(f.e, kids);
      }
      memo.emplace(k, result);
    }
  }

  if (root.isNull() || root->getNumKids() == 0)
    return root;
  return memo.at(root.get());
}

// Iterative post-order tree canonicalisation. Memoised on the original Expr*
// (sound: canonicalisation is a pure function of node structure). Sorts
// commutative children and rebuilds associative chains into a balanced shape;
// also canonicalises read update-list index/value subtrees.
ref<Expr>
canonicalizeExprTreeMemo(const ref<Expr> &root,
                         std::unordered_map<const Expr *, ref<Expr>> &memo) {
  struct Frame {
    ref<Expr> e;
    bool resolve;
  };
  std::vector<Frame> stack;
  stack.push_back({root, false});

  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();
    if (f.e.isNull() || f.e->getNumKids() == 0)
      continue;
    const Expr *k = f.e.get();

    if (!f.resolve) {
      if (memo.count(k))
        continue;
      stack.push_back({f.e, true});
      unsigned n = f.e->getNumKids();
      for (unsigned i = 0; i < n; ++i)
        stack.push_back({f.e->getKid(i), false});
      if (f.e->getKind() == Expr::Read) {
        const ReadExpr *re = cast<ReadExpr>(k);
        for (ref<UpdateNode> un = re->updates.head; un; un = un->next) {
          stack.push_back({un->index, false});
          stack.push_back({un->value, false});
        }
      }
    } else {
      if (memo.count(k))
        continue;
      unsigned n = f.e->getNumKids();
      std::vector<ref<Expr>> kids;
      kids.reserve(n);
      for (unsigned i = 0; i < n; ++i)
        kids.push_back(resolvedChild(f.e->getKid(i), memo));

      ref<Expr> result;
      if (f.e->getKind() == Expr::Read) {
        const ReadExpr *re = cast<ReadExpr>(k);
        const UpdateList &ul = re->updates;
        if (!ul.head) {
          result = ReadExpr::create(ul, kids[0]);
        } else {
          std::vector<ref<UpdateNode>> nodes;
          for (ref<UpdateNode> un = ul.head; un; un = un->next)
            nodes.push_back(un);
          UpdateList newUL(ul.root, nullptr);
          // extend() prepends; replay oldest-first to preserve head=newest.
          for (auto rit = nodes.rbegin(); rit != nodes.rend(); ++rit)
            newUL.extend(resolvedChild((*rit)->index, memo),
                         resolvedChild((*rit)->value, memo));
          result = ReadExpr::create(newUL, kids[0]);
        }
      } else if (isCommutativeKind(f.e->getKind())) {
        ref<Expr> tmp = rebuildWithKids(f.e, kids);
        result = flattenAndRebuildAssoc(tmp);
      } else {
        result = rebuildWithKids(f.e, kids);
      }
      memo.emplace(k, result);
    }
  }

  if (root.isNull() || root->getNumKids() == 0)
    return root;
  return memo.at(root.get());
}

} // anonymous namespace

namespace klee {

ref<Expr> canonicalizeExprTree(ref<Expr> e) {
  std::unordered_map<const Expr *, ref<Expr>> memo;
  return canonicalizeExprTreeMemo(e, memo);
}

std::vector<ref<Expr>>
canonicalizeExprTreesOnly(const std::vector<ref<Expr>> &constraints) {
  std::vector<ref<Expr>> out = constraints;
  // Constraints share sub-DAGs, so canonicalize them under one memo to keep the
  // work linear in distinct subexpressions. Arrays are deliberately not renamed
  // (see header): every ReadExpr keeps its original Array root, so a cached
  // Assignment remains usable and findSymbolicObjects recovers the same objects.
  std::unordered_map<const Expr *, ref<Expr>> treeMemo;
  for (auto &e : out)
    e = canonicalizeExprTreeMemo(e, treeMemo);
  return out;
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
  std::unordered_set<const Expr *> visited;
  for (auto &e : res.constraints)
    collectArrayOrder(e, order, nextIndex, visited);

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

  std::unordered_map<const Expr *, ref<Expr>> substMemo;
  for (auto &e : res.constraints)
    e = substituteArrays(e, res.forwardArrayMap, substMemo);

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
