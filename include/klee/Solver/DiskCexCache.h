// DiskCexCache.h

#ifndef DISK_CEX_CACHE_H
#define DISK_CEX_CACHE_H

#include "DiskMapOfSets.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprBuilder.h"
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

  /// Open a disk cache file for reading.
  /// `current` describes the running KLEE instance; if the file was written
  /// with different metadata a warning is emitted (the cache is still used
  /// since SAT assignments are re-verified and UNSAT results are solver-agnostic).
  explicit DiskCexCache(const std::string &filename,
                        const CacheMetadata &current = {});

  /// Try to find a cached result for `constraints`, checking supersets first
  /// (if trySuperset is true) then subsets, with a single canonicalization.
  /// Returns true on hit; outAssignment is non-nullptr for SAT, nullptr for UNSAT.
  bool find(const std::set<ref<Expr>> &constraints,
            bool trySuperset,
            Assignment *&outAssignment);

  /// Try to find a cached SAT assignment for a superset of `constraints`.
  /// Returns true on hit; outAssignment is non-nullptr for SAT, nullptr for UNSAT.
  bool findSuperset(const std::set<ref<Expr>> &constraints,
                    Assignment *&outAssignment);

  /// Try to find a cached SAT assignment for a subset of `constraints`.
  bool findSubset(const std::set<ref<Expr>> &constraints,
                  Assignment *&outAssignment);

private:
  mapofsets::DiskMapOfSets disk_;
  std::unique_ptr<ExprBuilder> builder_;
  mutable ArrayCache arrayCache_;
  /// Owns Assignment objects reconstructed from disk. Raw pointers returned
  /// by findSuperset/findSubset remain valid for the lifetime of this object.
  std::vector<std::unique_ptr<Assignment>> ownedAssignments_;

  enum class ValueKind {
    Unsat,          ///< empty value: UNSAT sentinel
    AssignmentData, ///< starts with 0x01: inline binary assignment
    Unknown         ///< unrecognised format, treat as miss
  };

  struct ParsedValue {
    ValueKind kind;
    std::string satData; // valid iff kind == AssignmentData
  };

  ParsedValue parseValue(const std::string &val) const;

  /// Returns nullptr on parse failure; on success owned by ownedAssignments_.
  Assignment *parseAssignmentData(
      const std::string &data,
      const std::map<std::string, const Array *> &nameToOrig);

  // unsatValid: true when values come from subset queries (an UNSAT subset
  // proves the full set UNSAT); false for superset queries (an UNSAT superset
  // says nothing about the subset — only SAT superset assignments are usable).
  bool pickEntry(const std::vector<std::string> &values,
                 const CanonicalizationResult &canon,
                 const std::set<ref<Expr>> &originalConstraints,
                 bool unsatValid,
                 Assignment *&outAssignment);
};

} // namespace klee

#endif
