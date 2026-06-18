#ifndef KLEE_CONSTRAINT_CANONICALIZER_H
#define KLEE_CONSTRAINT_CANONICALIZER_H

#include "klee/Expr/Expr.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace klee {

class Array;

namespace detail {
struct ExprPtrPairHash {
  std::size_t
  operator()(const std::pair<const Expr *, const Expr *> &p) const {
    // Mix the two pointers; std::hash<const void*> is identity on most libc++,
    // so rotate one half to avoid trivially-colliding (a,b)/(b,a) buckets.
    std::size_t h1 = std::hash<const void *>()(p.first);
    std::size_t h2 = std::hash<const void *>()(p.second);
    return h1 ^ (h2 * 1099511628211ULL + 0x9e3779b97f4a7c15ULL);
  }
};
/// Memo of three-way (a,b) comparisons keyed by raw Expr* pair. Valid only
/// while every cached pointer is live — i.e. within a single sort over a fixed
/// set of expressions (no Expr is freed/reallocated mid-sort).
using ExprCompareMemo =
    std::unordered_map<std::pair<const Expr *, const Expr *>, int,
                       ExprPtrPairHash>;
} // namespace detail

/// Total structural order on expressions, independent of pointer addresses.
/// Orders primarily by the cheap cached structural hash; the structural memo is
/// only consulted on a hash collision, so it is allocated LAZILY on first use
/// (eagerly allocating one per comparator — i.e. per commutative node in
/// flattenAndRebuildAssoc — was wasted work once ordering went hash-first).
/// The memo is shared via shared_ptr so any std::sort comparator copies that do
/// hit a collision share the cache. Construct a FRESH ExprCanonicalOrder per
/// sort: the memo keys are raw pointers, only valid while no expression in the
/// sorted range is freed.
struct ExprCanonicalOrder {
  ExprCanonicalOrder() = default;
  bool operator()(const klee::ref<Expr> &a,
                  const klee::ref<Expr> &b) const;

private:
  mutable std::shared_ptr<detail::ExprCompareMemo> memo_;
};

/// Canonicalization result.
struct CanonicalizationResult {
  std::vector<klee::ref<Expr>> constraints;
  std::unordered_map<const Array *, const Array *> forwardArrayMap;
  std::unordered_map<const Array *, const Array *> inverseArrayMap;
  /// Canonical array -> its small-integer index (the `N` in name "AN").
  /// Carried so serialisation reads the index directly instead of re-parsing
  /// it back out of the name string via stoi(name.substr(1)).
  std::unordered_map<const Array *, std::uint8_t> canonIndex;
};

klee::ref<Expr> canonicalizeExprTree(klee::ref<Expr> e);

/// Canonicalize the expression tree of each constraint (commutative/associative
/// operand ordering) WITHOUT alpha-renaming arrays. Because array identities are
/// preserved, an Assignment computed for the original constraints stays valid
/// for the canonicalized ones — unlike buildConstraintDiskKey, which renames
/// arrays for cross-run disk keys. Used to measure the in-memory
/// counterexample-cache hit-rate effect of canonicalization
/// (the --canonicalize-cex-key flag).
std::vector<klee::ref<Expr>>
canonicalizeExprTreesOnly(const std::vector<klee::ref<Expr>> &constraints);

/// Takes `constraints` by value so callers holding an rvalue (e.g. the disk
/// lookup path, which has just materialised the key set into a vector) can move
/// it straight into the result instead of paying a second copy (audit P2).
CanonicalizationResult
canonicalizeConstraintSet(std::vector<klee::ref<Expr>> constraints,
                          ArrayCache &arrayCache);

/// Single authoritative disk-key construction shared by the read and write
/// paths; returns the printed canonical constraint strings — **sorted and
/// deduplicated** (the disk trie requires a sorted set of distinct keys) as a
/// vector rather than a std::set, to avoid a per-element red-black-tree node
/// allocation on every lookup (audit O4) — plus the CanonicalizationResult
/// that produced them.
std::pair<std::vector<std::string>, CanonicalizationResult>
buildConstraintDiskKey(std::vector<klee::ref<Expr>> constraints,
                       ArrayCache &arrayCache);

} // namespace klee

#endif
