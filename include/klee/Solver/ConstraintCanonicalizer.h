#ifndef KLEE_CONSTRAINT_CANONICALIZER_H
#define KLEE_CONSTRAINT_CANONICALIZER_H

#include "klee/Expr/Expr.h"

#include <map>
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

/// Canonicalize a single expression tree.
klee::ref<Expr> canonicalizeExprTree(klee::ref<Expr> e);

/// Canonicalize a whole constraint set.
CanonicalizationResult
canonicalizeConstraintSet(const std::vector<klee::ref<Expr>> &constraints,
                          ExprBuilder &builder);

/// Deterministic textual serialization of canonicalized constraints.
std::string serializeCanonicalConstraints(
    const std::vector<klee::ref<Expr>> &canonConstraints);

/// Compute a stable key (e.g. SHA-1 hex) for a canonical constraint list.
std::string computeCanonicalKey(
    const std::vector<klee::ref<Expr>> &canonConstraints);

} // namespace klee

#endif