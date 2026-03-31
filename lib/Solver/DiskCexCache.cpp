// DiskCexCache.cpp

#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstring>

using namespace klee;
using mapofsets::DiskMapOfSets;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DiskCexCache::DiskCexCache(const std::string &filename,
                           const CacheMetadata &current)
    : disk_(filename), builder_(createDefaultExprBuilder()) {
  if (!disk_.isValid())
    return;

  const std::string &storedSolver  = disk_.solverBackend();
  const std::string &storedVersion = disk_.kleeVersion();

  // Warn if the cache was written with a different solver backend.  SAT
  // assignments are re-verified by Assignment::satisfies() so a stale hit is
  // caught; UNSAT results are logically solver-agnostic.  We warn rather than
  // reject so that a solver upgrade doesn't silently discard a large cache.
  if (!current.solverBackend.empty() && !storedSolver.empty() &&
      current.solverBackend != storedSolver)
    klee_warning("DiskCexCache: cache was written with solver '%s' "
                 "but current solver is '%s'; results will be re-verified",
                 storedSolver.c_str(), current.solverBackend.c_str());

  if (!current.kleeVersion.empty() && !storedVersion.empty() &&
      current.kleeVersion != storedVersion)
    klee_warning("DiskCexCache: cache was written with %s, "
                 "current build is %s",
                 storedVersion.c_str(), current.kleeVersion.c_str());
}

// ---------------------------------------------------------------------------
// Serialization (public static — used by the write path and tests)
//
// Binary value format (v2):
//   SAT:  [u8 0x01][u8 n_arrays]
//           ([u8 name_index][u32 data_len LE][data_len bytes]) × n_arrays
//   UNSAT: empty string (zero-byte entry in the values blob)
//
// name_index is the integer suffix of canonical array name "A{n}".
// ---------------------------------------------------------------------------

std::string DiskCexCache::serializeAssignment(
    const Assignment *a,
    const std::map<const Array *, const Array *> &forwardArrayMap) {
  assert(forwardArrayMap.size() <= 255 &&
         "serializeAssignment: too many symbolic arrays for binary format (max 255)");

  struct Entry { uint8_t idx; const std::vector<unsigned char> *bytes; };
  std::vector<Entry> entries;
  entries.reserve(forwardArrayMap.size());

  for (const auto &[orig, canon] : forwardArrayMap) {
    uint8_t idx = static_cast<uint8_t>(std::stoi(canon->name.substr(1)));
    auto it = a->bindings.find(orig);
    // An array that appears in the constraint set but has no binding in the
    // assignment is written with data_len=0.  This is distinct from the UNSAT
    // sentinel (the entire value blob being absent): here we are inside a SAT
    // record (marker byte 0x01 is present) and len=0 simply means the array
    // was unconstrained in this particular assignment.
    static const std::vector<unsigned char> empty;
    entries.push_back({idx, it != a->bindings.end() ? &it->second : &empty});
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry &x, const Entry &y) { return x.idx < y.idx; });

  std::string out;
  out.push_back('\x01');
  out.push_back(static_cast<char>(entries.size()));
  for (const auto &e : entries) {
    out.push_back(static_cast<char>(e.idx));
    uint32_t len = static_cast<uint32_t>(e.bytes->size());
    out.append(reinterpret_cast<const char *>(&len), 4);
    out.append(reinterpret_cast<const char *>(e.bytes->data()), len);
  }
  return out;
}


// ---------------------------------------------------------------------------
// Value parsing
// ---------------------------------------------------------------------------

DiskCexCache::ParsedValue
DiskCexCache::parseValue(const std::string &val) const {
  if (val.empty())
    return {ValueKind::Unsat, ""};
  if (static_cast<unsigned char>(val[0]) == 0x01)
    return {ValueKind::AssignmentData, val};
  return {ValueKind::Unknown, ""};
}

// ---------------------------------------------------------------------------
// Assignment deserialization
// ---------------------------------------------------------------------------

Assignment *
DiskCexCache::parseAssignmentData(
    const std::string &data,
    const std::map<std::string, const Array *> &nameToOrig) {
  // Binary format: [0x01][n_arrays]([name_index][u32 data_len LE][bytes])×n
  if (data.size() < 2) return nullptr;
  uint8_t n_arrays = static_cast<uint8_t>(data[1]);

  std::vector<const Array *> objects;
  std::vector<std::vector<unsigned char>> values;
  objects.reserve(n_arrays);
  values.reserve(n_arrays);

  size_t pos = 2;
  for (uint8_t i = 0; i < n_arrays; ++i) {
    if (pos + 5 > data.size()) return nullptr; // need idx(1) + len(4)
    uint8_t name_index = static_cast<uint8_t>(data[pos++]);
    uint32_t data_len = 0;
    memcpy(&data_len, &data[pos], 4); pos += 4;
    if (pos + data_len > data.size()) return nullptr;

    std::string name = "A" + std::to_string(name_index);
    auto nameIt = nameToOrig.find(name);
    if (nameIt != nameToOrig.end()) {
      objects.push_back(nameIt->second);
      values.emplace_back(
          reinterpret_cast<const unsigned char *>(&data[pos]),
          reinterpret_cast<const unsigned char *>(&data[pos]) + data_len);
    }
    pos += data_len;
  }

  auto *a = new Assignment(objects, values);
  ownedAssignments_.emplace_back(a);
  return a;
}

// ---------------------------------------------------------------------------
// Entry selection
// ---------------------------------------------------------------------------

bool DiskCexCache::pickEntry(
    const std::vector<DiskMapOfSets::Entry> &entries,
    const CanonicalizationResult &canon,
    const std::set<ref<Expr>> &originalConstraints,
    Assignment *&outAssignment) {
  // Build canonical_name -> original Array* once for all entries in this call;
  // all entries share the same CanonicalizationResult.
  std::map<std::string, const Array *> nameToOrig;
  for (const auto &[orig, can] : canon.forwardArrayMap)
    nameToOrig[can->name] = orig;

  for (const auto &e : entries) {
    ParsedValue pv = parseValue(e.value);
    switch (pv.kind) {
    case ValueKind::Unsat:
      // Any UNSAT subset proves the full set UNSAT.
      outAssignment = nullptr;
      return true;
    case ValueKind::AssignmentData: {
      Assignment *a = parseAssignmentData(pv.satData, nameToOrig);
      if (a && a->satisfies(originalConstraints.begin(),
                            originalConstraints.end())) {
        outAssignment = a;
        return true;
      }
      break;
    }
    case ValueKind::Unknown:
    default:
      break;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool DiskCexCache::find(const std::set<ref<Expr>> &constraints,
                        bool trySuperset,
                        Assignment *&outAssignment) {
  if (!disk_.isValid())
    return false;
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  auto [diskKey, canon] = klee::buildConstraintDiskKey(vec, *builder_, arrayCache_);
  if (trySuperset) {
    auto supers = disk_.supersets(diskKey);
    if (pickEntry(supers, canon, constraints, outAssignment))
      return true;
  }
  auto subs = disk_.subsets(diskKey);
  return pickEntry(subs, canon, constraints, outAssignment);
}

bool DiskCexCache::findSuperset(const std::set<ref<Expr>> &constraints,
                                Assignment *&outAssignment) {
  if (!disk_.isValid())
    return false;
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  auto [diskKey, canon] = klee::buildConstraintDiskKey(vec, *builder_, arrayCache_);
  auto supers = disk_.supersets(diskKey);
  return pickEntry(supers, canon, constraints, outAssignment);
}

bool DiskCexCache::findSubset(const std::set<ref<Expr>> &constraints,
                              Assignment *&outAssignment) {
  if (!disk_.isValid())
    return false;
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  auto [diskKey, canon] = klee::buildConstraintDiskKey(vec, *builder_, arrayCache_);
  auto subs = disk_.subsets(diskKey);
  return pickEntry(subs, canon, constraints, outAssignment);
}
