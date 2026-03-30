// DiskCexCache.cpp

#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Expr/ExprPPrinter.h"

#include "llvm/Support/raw_ostream.h"

using namespace klee;
using mapofsets::DiskMapOfSets;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DiskCexCache::DiskCexCache(const std::string &filename)
    : disk_(filename), builder_(createDefaultExprBuilder()) {}

// ---------------------------------------------------------------------------
// Serialization (public static — used by the write path and tests)
// ---------------------------------------------------------------------------

std::string DiskCexCache::serializeAssignment(
    const Assignment *a,
    const std::map<const Array *, const Array *> &forwardArrayMap) {
  std::string out = "SAT_DATA:";
  for (const auto &[orig, canon] : forwardArrayMap) {
    out += canon->name;
    out += '=';
    auto it = a->bindings.find(orig);
    if (it != a->bindings.end()) {
      for (unsigned char b : it->second) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 0xf];
      }
    }
    // An empty hex string is valid and decodes to an empty byte vector.
    out += ';';
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
  if (val == "UNSAT")
    return {ValueKind::Unsat, ""};
  if (val.size() > 9 && val.compare(0, 9, "SAT_DATA:") == 0)
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

  // Parse "SAT_DATA:A0=deadbeef;A1=0102;" entry by entry.
  std::vector<const Array *> objects;
  std::vector<std::vector<unsigned char>> values;

  size_t pos = 9; // skip "SAT_DATA:"
  while (pos < data.size()) {
    size_t eq = data.find('=', pos);
    if (eq == std::string::npos)
      break;
    std::string name = data.substr(pos, eq - pos);

    size_t semi = data.find(';', eq + 1);
    if (semi == std::string::npos)
      semi = data.size();
    const std::string hexStr = data.substr(eq + 1, semi - eq - 1);

    auto nameIt = nameToOrig.find(name);
    if (nameIt != nameToOrig.end()) {
      std::vector<unsigned char> bytes;
      bytes.reserve(hexStr.size() / 2);
      for (size_t i = 0; i + 1 < hexStr.size(); i += 2) {
        int hi = hexNibble(hexStr[i]);
        int lo = hexNibble(hexStr[i + 1]);
        if (hi < 0 || lo < 0)
          return nullptr; // malformed hex
        bytes.push_back(static_cast<unsigned char>((hi << 4) | lo));
      }
      objects.push_back(nameIt->second);
      values.push_back(std::move(bytes));
    }
    pos = semi + 1;
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
