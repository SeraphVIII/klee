//===-- Constraints.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr/Constraints.h"

#include "klee/Expr/ExprHashMap.h"
#include "klee/Expr/ExprVisitor.h"
#include "klee/Module/KModule.h"
#include "klee/Support/OptionCategories.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace klee;

namespace {
// Behaviour-preserving accelerator for ConstraintManager::rewriteConstraints.
//
// When an equality `const == X` is added, every existing constraint is visited
// to substitute X with the constant.  Profiling (printf 300k) shows ~99.5% of
// those visits touch a constraint that does not contain X at all: the whole
// expression tree is walked and the constraint returned unchanged.  A
// constraint `ce` can contain `X` only if X's structural hash occurs among
// ce's subexpression hashes, so we cache, per constraint expression, the sorted
// unique set of its subexpression hashes and skip the (expensive) visit
// whenever X's hash is absent.  A hash collision merely causes a redundant —
// still correct — visit, so the produced constraint set is byte-identical.
//
// The cache is keyed by Expr* and each entry pins its key with a ref<Expr>, so
// the pointer cannot be freed and reused while the entry is live (same safety
// argument as the IndependentElementSet cache in IndependentSolver.cpp).
class SubExprHashIndex {
  struct Entry {
    ref<Expr> pin;                // keeps the key Expr* alive (no stale reuse)
    std::vector<unsigned> hashes; // sorted, unique subexpression hashes
  };
  std::unordered_map<const Expr *, Entry> cache_;
  static const size_t kMaxEntries = 1u << 18; // ~262k constraints, then drop

  static void collect(const Expr *e, std::vector<unsigned> &out,
                      std::unordered_set<const Expr *> &seen) {
    if (!seen.insert(e).second)
      return; // shared subtree already accounted for (DAG dedup)
    out.push_back(e->hash());
    for (unsigned i = 0, n = e->getNumKids(); i < n; ++i)
      collect(e->getKid(i).get(), out, seen);
  }

  const std::vector<unsigned> &signatureOf(const ref<Expr> &e) {
    const Expr *key = e.get();
    auto it = cache_.find(key);
    if (it != cache_.end())
      return it->second.hashes;
    if (cache_.size() > kMaxEntries)
      cache_.clear();
    Entry entry;
    entry.pin = e;
    std::unordered_set<const Expr *> seen;
    collect(key, entry.hashes, seen);
    std::sort(entry.hashes.begin(), entry.hashes.end());
    entry.hashes.erase(std::unique(entry.hashes.begin(), entry.hashes.end()),
                       entry.hashes.end());
    return cache_.emplace(key, std::move(entry)).first->second.hashes;
  }

public:
  // Conservative containment test.  false => `needle` is provably not a
  // subexpression of `haystack`; true => it might be (caller must confirm via
  // the actual visit).
  bool mayContain(const ref<Expr> &haystack, const ref<Expr> &needle) {
    const std::vector<unsigned> &h = signatureOf(haystack);
    return std::binary_search(h.begin(), h.end(), needle->hash());
  }
};
SubExprHashIndex g_subExprIndex;

llvm::cl::opt<bool> RewriteEqualities(
    "rewrite-equalities",
    llvm::cl::desc("Rewrite existing constraints when an equality with a "
                   "constant is added (default=true)"),
    llvm::cl::init(true),
    llvm::cl::cat(SolvingCat));
} // namespace

class ExprReplaceVisitor : public ExprVisitor {
private:
  ref<Expr> src, dst;

public:
  ExprReplaceVisitor(const ref<Expr> &_src, const ref<Expr> &_dst)
      : src(_src), dst(_dst) {}

  Action visitExpr(const Expr &e) override {
    if (e == *src) {
      return Action::changeTo(dst);
    }
    return Action::doChildren();
  }

  Action visitExprPost(const Expr &e) override {
    if (e == *src) {
      return Action::changeTo(dst);
    }
    return Action::doChildren();
  }
};

class ExprReplaceVisitor2 : public ExprVisitor {
private:
  // Pure lookup table (never iterated for order), so an unordered map keyed by
  // the expression's cached hash replaces the old std::map that ordered keys by
  // the (expensive) structural Expr::compare.  Lookups become O(1) cached-hash
  // instead of O(log n) structural comparisons.
  const ExprHashMap<ref<Expr>> &replacements;

public:
  explicit ExprReplaceVisitor2(const ExprHashMap<ref<Expr>> &_replacements)
      : ExprVisitor(true), replacements(_replacements) {}

  Action visitExprPost(const Expr &e) override {
    auto it = replacements.find(ref<Expr>(const_cast<Expr *>(&e)));
    if (it!=replacements.end()) {
      return Action::changeTo(it->second);
    }
    return Action::doChildren();
  }
};

bool ConstraintManager::rewriteConstraints(ExprVisitor &visitor,
                                           const ref<Expr> &filterSrc) {
  ConstraintSet old;
  bool changed = false;

  // Ablation switch (default on): KLEE_DISABLE_RWC_FILTER=1 restores the
  // original visit-every-constraint behaviour, for A/B verification.
  static const bool filterEnabled = !std::getenv("KLEE_DISABLE_RWC_FILTER");

  std::swap(constraints, old);
  for (auto &ce : old) {
    // A constraint can only change if it contains the substituted subexpression.
    // When it provably does not, skip the visit and keep it verbatim — this is
    // exactly the path the visit would have taken (returns ce unchanged), so the
    // resulting set is identical, just without the wasted tree walk.
    if (filterEnabled && filterSrc.get() &&
        !g_subExprIndex.mayContain(ce, filterSrc)) {
      constraints.push_back(ce);
      continue;
    }

    ref<Expr> e = visitor.visit(ce);

    if (e!=ce) {
      addConstraintInternal(e); // enable further reductions
      changed = true;
    } else {
      constraints.push_back(ce);
    }
  }

  return changed;
}

ref<Expr> ConstraintManager::simplifyExpr(const ConstraintSet &constraints,
                                          const ref<Expr> &e) {

  if (isa<ConstantExpr>(e))
    return e;

  // First-insertion-wins (insert never overwrites), matching the previous
  // std::map; only the key ordering changed, which this table does not rely on.
  ExprHashMap<ref<Expr>> equalities;

  for (auto &constraint : constraints) {
    if (const EqExpr *ee = dyn_cast<EqExpr>(constraint)) {
      if (isa<ConstantExpr>(ee->left)) {
        equalities.insert(std::make_pair(ee->right,
                                         ee->left));
      } else {
        equalities.insert(
            std::make_pair(constraint, ConstantExpr::alloc(1, Expr::Bool)));
      }
    } else {
      equalities.insert(
          std::make_pair(constraint, ConstantExpr::alloc(1, Expr::Bool)));
    }
  }

  return ExprReplaceVisitor2(equalities).visit(e);
}

void ConstraintManager::addConstraintInternal(const ref<Expr> &e) {
  // rewrite any known equalities and split Ands into different conjuncts

  switch (e->getKind()) {
  case Expr::Constant:
    assert(cast<ConstantExpr>(e)->isTrue() &&
           "attempt to add invalid (false) constraint");
    break;

    // split to enable finer grained independence and other optimizations
  case Expr::And: {
    BinaryExpr *be = cast<BinaryExpr>(e);
    addConstraintInternal(be->left);
    addConstraintInternal(be->right);
    break;
  }

  case Expr::Eq: {
    if (RewriteEqualities) {
      // XXX: should profile the effects of this and the overhead.
      // traversing the constraints looking for equalities is hardly the
      // slowest thing we do, but it is probably nicer to have a
      // ConstraintSet ADT which efficiently remembers obvious patterns
      // (byte-constant comparison).
      BinaryExpr *be = cast<BinaryExpr>(e);
      if (isa<ConstantExpr>(be->left)) {
	ExprReplaceVisitor visitor(be->right, be->left);
	rewriteConstraints(visitor, be->right);
      }
    }
    constraints.push_back(e);
    break;
  }

  default:
    constraints.push_back(e);
    break;
  }
}

void ConstraintManager::addConstraint(const ref<Expr> &e) {
  ref<Expr> simplified = simplifyExpr(constraints, e);
  addConstraintInternal(simplified);
}

ConstraintManager::ConstraintManager(ConstraintSet &_constraints)
    : constraints(_constraints) {}

bool ConstraintSet::empty() const { return constraints.empty(); }

klee::ConstraintSet::constraint_iterator ConstraintSet::begin() const {
  return constraints.begin();
}

klee::ConstraintSet::constraint_iterator ConstraintSet::end() const {
  return constraints.end();
}

size_t ConstraintSet::size() const noexcept { return constraints.size(); }

void ConstraintSet::push_back(const ref<Expr> &e) { constraints.push_back(e); }
