#include "klee/Solver/DiskCexCache.h"
#include "klee/Solver/ConstraintCanonicalizer.h"
#include "klee/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace klee;
using mapofsets::DiskMapOfSets;

namespace {
// W6 ablation probe: when KLEE_DISK_CEX_ABLATE is set, classify every disk
// lookup on the SAME query stream as a single trie run, so the trie-vs-flat
// comparison is free of cross-run exploration drift. trieHit is the actual
// result; flatHit is whether an exact-key (flat-hash) match would have been
// usable; trieOnly = hits the subset/superset trie provides that a flat hash
// structurally cannot.
struct CexAblationProbe {
  bool on;
  unsigned long lookups = 0, trieHit = 0, flatHit = 0, trieOnly = 0;
  CexAblationProbe() : on(getenv("KLEE_DISK_CEX_ABLATE") != nullptr) {}
  ~CexAblationProbe() {
    if (on)
      fprintf(stderr,
              "CEX_ABLATE lookups=%lu trieHit=%lu flatHit=%lu trieOnly=%lu\n",
              lookups, trieHit, flatHit, trieOnly);
  }
};
CexAblationProbe g_ablate;
} // namespace

DiskCexCache::DiskCexCache(const std::string &filename,
                           const CacheMetadata &current,
                           size_t lruCacheSize)
    : disk_(filename, lruCacheSize) {
  if (!disk_.isValid())
    return;

  const std::string &storedSolver  = disk_.solverBackend();
  const std::string &storedVersion = disk_.kleeVersion();

  // Solver/version mismatch is non-fatal: SAT hits are re-verified by
  // Assignment::satisfies(), UNSAT hits are solver-agnostic.
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

// SAT v2: [u8 0x01][u8 n_arrays]([u8 idx][u32 len LE][bytes])×n
// UNSAT:  empty string
std::string DiskCexCache::serializeAssignment(
    const Assignment *a,
    const std::map<const Array *, const Array *> &forwardArrayMap) {
  // Returning "" aliases the UNSAT sentinel, so production callers must
  // pre-check; soft-fail here is defence in depth against direct callers.
  if (forwardArrayMap.size() > 255) {
    klee_warning_once(nullptr,
                      "serializeAssignment: too many symbolic arrays (%zu) "
                      "for binary format (max 255); returning empty record",
                      forwardArrayMap.size());
    return "";
  }

  struct Entry { uint8_t idx; const std::vector<unsigned char> *bytes; };
  std::vector<Entry> entries;
  entries.reserve(forwardArrayMap.size());

  for (const auto &[orig, canon] : forwardArrayMap) {
    uint8_t idx = static_cast<uint8_t>(std::stoi(canon->name.substr(1)));
    auto it = a->bindings.find(orig);
    // An unbound array gets data_len=0; distinct from UNSAT (whole record empty).
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

DiskCexCache::ParsedValue
DiskCexCache::parseValue(const std::string &val) const {
  if (val.empty())
    return {ValueKind::Unsat, ""};
  if (static_cast<unsigned char>(val[0]) == 0x01)
    return {ValueKind::AssignmentData, val};
  return {ValueKind::Unknown, ""};
}

std::unique_ptr<Assignment>
DiskCexCache::parseAssignmentData(
    const std::string &data,
    const std::map<std::string, const Array *> &nameToOrig) {
  if (data.size() < 2) return nullptr;
  uint8_t n_arrays = static_cast<uint8_t>(data[1]);

  std::vector<const Array *> objects;
  std::vector<std::vector<unsigned char>> values;
  objects.reserve(n_arrays);
  values.reserve(n_arrays);

  size_t pos = 2;
  for (uint8_t i = 0; i < n_arrays; ++i) {
    if (pos + 5 > data.size()) return nullptr;
    uint8_t name_index = static_cast<uint8_t>(data[pos++]);
    uint32_t data_len = 0;
    memcpy(&data_len, &data[pos], 4); pos += 4;
    if (pos + data_len > data.size()) return nullptr;

    std::string name = "A" + std::to_string(name_index);
    auto nameIt = nameToOrig.find(name);
    if (nameIt != nameToOrig.end()) {
      // The canonical key doesn't encode array size, so the cached witness
      // may not match the live array's size. Resize it (zero-pad/truncate)
      // rather than reject: pickEntry's satisfies() re-verifies, and an
      // exact-sized witness is required or IndependentSolver asserts.
      const Array *orig = nameIt->second;
      const unsigned char *src =
          reinterpret_cast<const unsigned char *>(&data[pos]);
      std::vector<unsigned char> witness(orig->size, 0);
      std::memcpy(witness.data(), src,
                  std::min<size_t>(data_len, orig->size));
      objects.push_back(orig);
      values.push_back(std::move(witness));
    }
    pos += data_len;
  }
  if (pos != data.size()) return nullptr;

  return std::make_unique<Assignment>(objects, values);
}

bool DiskCexCache::pickEntry(
    const std::vector<std::string> &values,
    const CanonicalizationResult &canon,
    const std::set<ref<Expr>> &originalConstraints,
    bool unsatValid,
    Assignment *&outAssignment) {
  std::map<std::string, const Array *> nameToOrig;
  for (const auto &[orig, can] : canon.forwardArrayMap)
    nameToOrig[can->name] = orig;

  for (const auto &val : values) {
    ParsedValue pv = parseValue(val);
    switch (pv.kind) {
    case ValueKind::Unsat:
      // UNSAT is monotonic on supersets but not subsets, so only valid for
      // subset queries (current set ⊇ stored set).
      if (unsatValid) {
        outAssignment = nullptr;
        return true;
      }
      break;
    case ValueKind::AssignmentData: {
      // Retain only on success — rejected candidates die at end of scope to
      // bound heap growth across long-running queries.
      auto a = parseAssignmentData(pv.satData, nameToOrig);
      if (a && a->satisfies(originalConstraints.begin(),
                            originalConstraints.end())) {
        outAssignment = a.get();
        ownedAssignments_.push_back(std::move(a));
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

bool DiskCexCache::find(const std::set<ref<Expr>> &constraints,
                        bool trySuperset,
                        Assignment *&outAssignment) {
  if (!disk_.isValid())
    return false;
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  auto [diskKey, canon] = klee::buildConstraintDiskKey(vec, arrayCache_);

  // W6 ablation: KLEE_DISK_CEX_FLAT emulates a flat canonical-key->result hash
  // map by matching on the exact key only (no subset/superset trie traversal).
  // Running cat warm with and without this isolates what the trie buys.
  static const bool flatMode = getenv("KLEE_DISK_CEX_FLAT") != nullptr;
  if (flatMode) {
    auto exact = disk_.lookup(diskKey);
    if (!exact)
      return false;
    std::vector<std::string> v{*exact};
    return pickEntry(v, canon, constraints, /*unsatValid=*/true, outAssignment);
  }

  bool trieResult = false;
  if (trySuperset) {
    auto supers = disk_.supersets(diskKey);
    if (pickEntry(supers, canon, constraints, /*unsatValid=*/false, outAssignment))
      trieResult = true;
  }
  if (!trieResult) {
    auto subs = disk_.subsets(diskKey);
    trieResult = pickEntry(subs, canon, constraints, /*unsatValid=*/true, outAssignment);
  }

  if (g_ablate.on) {
    // Would a flat exact-key hash have produced a usable hit on THIS query?
    auto flatUsable = [&]() -> bool {
      auto exact = disk_.lookup(diskKey);
      if (!exact)
        return false;
      ParsedValue pv = parseValue(*exact);
      if (pv.kind == ValueKind::Unsat)
        return true; // exact key: live set == stored set, UNSAT is valid
      if (pv.kind == ValueKind::AssignmentData) {
        std::map<std::string, const Array *> nameToOrig;
        for (const auto &[orig, can] : canon.forwardArrayMap)
          nameToOrig[can->name] = orig;
        auto a = parseAssignmentData(pv.satData, nameToOrig);
        return a && a->satisfies(constraints.begin(), constraints.end());
      }
      return false;
    };
    bool flat = flatUsable();
    ++g_ablate.lookups;
    if (trieResult)          ++g_ablate.trieHit;
    if (flat)                ++g_ablate.flatHit;
    if (trieResult && !flat) ++g_ablate.trieOnly;
  }
  return trieResult;
}

bool DiskCexCache::findSuperset(const std::set<ref<Expr>> &constraints,
                                Assignment *&outAssignment) {
  if (!disk_.isValid())
    return false;
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  auto [diskKey, canon] = klee::buildConstraintDiskKey(vec, arrayCache_);
  auto supers = disk_.supersets(diskKey);
  return pickEntry(supers, canon, constraints, /*unsatValid=*/false, outAssignment);
}

bool DiskCexCache::findSubset(const std::set<ref<Expr>> &constraints,
                              Assignment *&outAssignment) {
  if (!disk_.isValid())
    return false;
  std::vector<ref<Expr>> vec(constraints.begin(), constraints.end());
  auto [diskKey, canon] = klee::buildConstraintDiskKey(vec, arrayCache_);
  auto subs = disk_.subsets(diskKey);
  return pickEntry(subs, canon, constraints, /*unsatValid=*/true, outAssignment);
}
