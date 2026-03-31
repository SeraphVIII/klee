// DiskCexCache.cpp

#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Expr/ExprPPrinter.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>

using namespace klee;
using mapofsets::DiskMapOfSets;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DiskCexCache::DiskCexCache(const std::string &filename)
    : disk_(filename), builder_(createDefaultExprBuilder()) {}

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
  struct Entry { uint8_t idx; const std::vector<unsigned char> *bytes; };
  std::vector<Entry> entries;
  entries.reserve(forwardArrayMap.size());

  for (const auto &[orig, canon] : forwardArrayMap) {
    uint8_t idx = static_cast<uint8_t>(std::stoi(canon->name.substr(1)));
    auto it = a->bindings.find(orig);
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
// Key construction
// ---------------------------------------------------------------------------

std::pair<std::set<std::string>, CanonicalizationResult>
DiskCexCache::buildDiskKeyAndCanon(
    const std::set<ref<Expr>> &constraints) const {
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  CanonicalizationResult canon =
      klee::canonicalizeConstraintSet(vec, *builder_, arrayCache_);

  // Serialize each canonicalized constraint to a string key using
  // printSingleExpr with no trailing newline, for an unambiguous format.
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
DiskCexCache::parseAssignmentData(const std::string &data,
                                  const CanonicalizationResult &canon) {
  // Build canonical_name -> original Array* from the forward map.
  std::map<std::string, const Array *> nameToOrig;
  for (const auto &[orig, can] : canon.forwardArrayMap)
    nameToOrig[can->name] = orig;

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
  for (const auto &e : entries) {
    ParsedValue pv = parseValue(e.value);
    switch (pv.kind) {
    case ValueKind::Unsat:
      // Any UNSAT subset proves the full set UNSAT.
      outAssignment = nullptr;
      return true;
    case ValueKind::AssignmentData: {
      Assignment *a = parseAssignmentData(pv.satData, canon);
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
  auto [diskKey, canon] = buildDiskKeyAndCanon(constraints);
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
  auto [diskKey, canon] = buildDiskKeyAndCanon(constraints);
  auto supers = disk_.supersets(diskKey);
  return pickEntry(supers, canon, constraints, outAssignment);
}

bool DiskCexCache::findSubset(const std::set<ref<Expr>> &constraints,
                              Assignment *&outAssignment) {
  if (!disk_.isValid())
    return false;
  auto [diskKey, canon] = buildDiskKeyAndCanon(constraints);
  auto subs = disk_.subsets(diskKey);
  return pickEntry(subs, canon, constraints, outAssignment);
}
