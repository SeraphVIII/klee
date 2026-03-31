// DiskCexCache.h

#ifndef DISK_CEX_CACHE_H
#define DISK_CEX_CACHE_H

#include "DiskMapOfSets.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
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
///
/// Values on disk use binary v2 format (see DiskCexCache::serializeAssignment):
///   SAT:   [u8 0x01][u8 n_arrays]([u8 name_idx][u32 data_len][bytes])×n
///   UNSAT: empty string (zero bytes)
///
/// Keys are produced by canonicalizeConstraintSet() followed by
/// ExprPPrinter::printSingleExpr() (no trailing newline) for each constraint.
class DiskCexCache {
public:
  /// Serialize a SAT assignment to the "SAT_DATA:..." disk format.
  /// forwardArrayMap maps original Array* -> canonical Array* (A0, A1, ...).
  /// This is the inverse of what is needed for reading; it is exposed here
  /// so that the future write path in CexCachingSolver can call it directly.
  static std::string
  serializeAssignment(const Assignment *a,
                      const std::map<const Array *, const Array *> &forwardArrayMap);

  /// Open a disk cache file for reading.
  explicit DiskCexCache(const std::string &filename);

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

  /// Deserialize a binary assignment from `data` into an Assignment whose
  /// bindings reference the original arrays via `nameToOrig`
  /// (canonical name -> original Array*).
  /// Returns nullptr on parse failure. The returned pointer is owned by
  /// ownedAssignments_ and remains valid for the lifetime of this object.
  Assignment *parseAssignmentData(
      const std::string &data,
      const std::map<std::string, const Array *> &nameToOrig);

  bool pickEntry(const std::vector<mapofsets::DiskMapOfSets::Entry> &entries,
                 const CanonicalizationResult &canon,
                 const std::set<ref<Expr>> &originalConstraints,
                 Assignment *&outAssignment);
};

} // namespace klee

#endif
