#ifndef DISK_CEX_CACHE_H
#define DISK_CEX_CACHE_H

#include "DiskMapOfSets.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ArrayCache.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace klee {

/// Read-only CEX cache backed by DiskMapOfSets.
class DiskCexCache {
public:
  /// Serialize a SAT assignment to the binary v2 disk format.
  /// forwardArrayMap maps original Array* -> canonical Array* (A0, A1, ...).
  static std::string
  serializeAssignment(const Assignment *a,
                      const std::map<const Array *, const Array *> &forwardArrayMap);

  /// `current` describes the running KLEE; metadata mismatch is non-fatal
  /// (warns only — SAT is re-verified, UNSAT is solver-agnostic).
  /// `lruCacheSize` bounds the number of decoded chunks held in memory.
  explicit DiskCexCache(const std::string &filename,
                        const CacheMetadata &current = {},
                        size_t lruCacheSize = 100);

  /// Look up a cached result, checking supersets first (when trySuperset)
  /// then subsets.  outAssignment is non-null for SAT, null for UNSAT.
  bool find(const std::set<ref<Expr>> &constraints,
            bool trySuperset,
            Assignment *&outAssignment);

  bool findSuperset(const std::set<ref<Expr>> &constraints,
                    Assignment *&outAssignment);

  bool findSubset(const std::set<ref<Expr>> &constraints,
                  Assignment *&outAssignment);

private:
  mapofsets::DiskMapOfSets disk_;
  mutable ArrayCache arrayCache_;
  // Owns reconstructed Assignments; raw pointers returned via outAssignment
  // alias entries here and stay valid for the lifetime of this object.
  std::vector<std::unique_ptr<Assignment>> ownedAssignments_;

  enum class ValueKind { Unsat, AssignmentData, Unknown };

  struct ParsedValue {
    ValueKind kind;
    std::string satData;
  };

  ParsedValue parseValue(const std::string &val) const;

  // Caller chooses ownership: retain by moving into ownedAssignments_, else
  // the unique_ptr dies at end of scope. pickEntry retains only on a hit.
  std::unique_ptr<Assignment> parseAssignmentData(
      const std::string &data,
      const std::map<std::string, const Array *> &nameToOrig);

  // unsatValid is true for subset queries (UNSAT subset ⇒ UNSAT) and false
  // for superset queries (UNSAT superset says nothing about the subset).
  bool pickEntry(const std::vector<std::string> &values,
                 const CanonicalizationResult &canon,
                 const std::set<ref<Expr>> &originalConstraints,
                 bool unsatValid,
                 Assignment *&outAssignment);
};

} // namespace klee

#endif
