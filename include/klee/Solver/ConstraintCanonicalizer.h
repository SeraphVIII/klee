#ifndef KLEE_CONSTRAINT_CANONICALIZER_H
#define KLEE_CONSTRAINT_CANONICALIZER_H

#include "klee/Expr/Expr.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace klee {

class Array;
class ExprBuilder;

/// Total structural order on expressions, independent of pointer addresses.
struct ExprCanonicalOrder {
  bool operator()(const klee::ref<Expr> &a,
                  const klee::ref<Expr> &b) const;
};

/// Canonicalization result.
struct CanonicalizationResult {
  std::vector<klee::ref<Expr>> constraints;
  std::map<const Array *, const Array *> forwardArrayMap;
  std::map<const Array *, const Array *> inverseArrayMap;
};

klee::ref<Expr> canonicalizeExprTree(klee::ref<Expr> e);

CanonicalizationResult
canonicalizeConstraintSet(const std::vector<klee::ref<Expr>> &constraints,
                          ExprBuilder &builder,
                          ArrayCache &arrayCache);

/// Single authoritative disk-key construction shared by the read and write
/// paths; returns the set of printed canonical constraint strings plus the
/// CanonicalizationResult that produced them.
std::pair<std::set<std::string>, CanonicalizationResult>
buildConstraintDiskKey(const std::vector<klee::ref<Expr>> &constraints,
                       ExprBuilder &builder,
                       ArrayCache &arrayCache);

} // namespace klee

#endif
