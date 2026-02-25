// DiskCexCache.h

#ifndef DISK_CEX_CACHE_H
#define DISK_CEX_CACHE_H

#include "DiskMapOfSets.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"

#include <memory>
#include <unordered_map>
#include <string>
#include <set>

namespace klee {

/// Simple interface for a read-only CEX cache backed by DiskMapOfSets.
/// This is *not* trying to be the full CexCachingSolver, just a helper.
class DiskCexCache {
public:
  /// How values are encoded on disk right now
  enum class ValueKind {
    Unsat,          ///< represents an UNSAT sentinel
    AssignmentId,   ///< represents a SAT assignment with an ID
    Unknown         ///< parse failure, treat as miss
  };

  struct ParsedValue {
    ValueKind kind;
    unsigned assignmentId; // valid iff kind == AssignmentId
  };

public:
  /// Construct from an existing disk file and an assignment table.
  /// assignmentTable maps IDs -> Assignment*. ID 0 is reserved for UNSAT.
  DiskCexCache(const std::string &filename,
               const std::vector<Assignment *> &assignmentTable);

  /// Try to find a cached SAT assignment for a superset of `constraints`.
  /// Returns true on cache hit; outAssignment is:
  ///   - non-nullptr for SAT,
  ///   - nullptr for UNSAT sentinel.
  bool findSuperset(const std::set<ref<Expr>> &constraints,
                    Assignment *&outAssignment);

  /// Try to find a cached SAT assignment for a subset of `constraints`.
  bool findSubset(const std::set<ref<Expr>> &constraints,
                  Assignment *&outAssignment);

private:
  mapofsets::DiskMapOfSets disk_;
  const std::vector<Assignment *> &assignmentTable_;

  /// Canonicalize a constraint expression into a stable string key.
  std::string canonConstraint(ref<Expr> e) const;

  /// Convert a set<ref<Expr>> to set<string> using canonConstraint.
  std::set<std::string>
  buildDiskKey(const std::set<ref<Expr>> &constraints) const;

  /// Parse the disk value string into a ParsedValue.
  ParsedValue parseValue(const std::string &val) const;

  /// Helper: pick *one* entry out of the vector<DiskMapOfSets::Entry>
  /// and convert its value to an Assignment*.
  bool pickEntry(const std::vector<mapofsets::DiskMapOfSets::Entry> &entries,
                 Assignment *&outAssignment);
};

} // namespace klee

#endif
