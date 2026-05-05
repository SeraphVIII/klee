//===-- klee-pcache-opt/main.cpp --------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// klee-pcache-opt: Offline optimiser for KLEE persistent solver caches.
//
// Reads a cache file (MapOfSets v2 format) and/or one or more log files
// produced by KLEE's --write-disk-cex-cache / --persistent-cex-cache flags,
// applies solver-free optimisations, and writes a compacted output cache file.
//
// Log file format (produced by CexCachingSolver::appendToCacheLog):
//   record₁ record₂ ... recordₙ
//   Each record:
//     [u32 num_keys]
//       ([u32 key_len][key_bytes]) × num_keys
//     [u32 val_len]
//     [val_bytes]
//
// Optimisations (all enabled by default):
//
//   UNSAT dominance pruning
//     For two UNSAT entries A and B where A.key_set ⊂ B.key_set, entry B is
//     redundant: any lookup whose constraint set is a superset of B.key_set is
//     also a superset of A.key_set, so A will always be found first.  B can
//     therefore be removed without affecting cache behaviour.
//     Algorithm: process entries in ascending key_set-size order; build an
//     incremental UNSAT trie; skip any entry whose key_set already has a
//     stored UNSAT subset in the trie.
//
//   Deduplication
//     Multiple entries with the same key_set are collapsed to one.  For
//     SAT/UNSAT conflicts at the same key_set (cache corruption), UNSAT wins.
//
// SAT entries are not pruned: verifying whether a stored assignment still
// satisfies a different query requires the original constraint objects and is
// therefore outside the scope of solver-free processing.
//
//===----------------------------------------------------------------------===//

#include "OfflineEngine.h"

#include "klee/Solver/DiskMapOfSets.h"
#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/ADT/MapOfSets.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace klee;
using namespace klee::mapofsets;
using klee_pcache_opt::OfflineEngine;
using klee_pcache_opt::ParsedKey;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isUnsat(const std::string &value) { return value.empty(); }

// ---------------------------------------------------------------------------
// SAT value-blob helpers (format mirrors DiskCexCache::serializeAssignment)
//
// SAT v2 blob: [u8 0x01][u8 n_arrays]
//                ([u8 name_index][u32 data_len LE][data_len bytes]) × n_arrays
// ---------------------------------------------------------------------------

struct ValueArray {
  std::uint8_t nameIndex;
  std::vector<unsigned char> bytes;
};

static bool parseSatValue(const std::string &v,
                          std::vector<ValueArray> &out) {
  if (v.size() < 2) return false;
  if (static_cast<unsigned char>(v[0]) != 0x01) return false;
  std::uint8_t n = static_cast<std::uint8_t>(v[1]);
  std::size_t pos = 2;
  out.clear();
  out.reserve(n);
  for (std::uint8_t i = 0; i < n; ++i) {
    if (pos + 5 > v.size()) return false;
    ValueArray va;
    va.nameIndex = static_cast<std::uint8_t>(v[pos++]);
    std::uint32_t len = 0;
    std::memcpy(&len, &v[pos], 4); pos += 4;
    if (pos + len > v.size()) return false;
    va.bytes.assign(reinterpret_cast<const unsigned char *>(&v[pos]),
                    reinterpret_cast<const unsigned char *>(&v[pos]) + len);
    pos += len;
    out.push_back(std::move(va));
  }
  return true;
}

static std::string serializeSatValue(const std::vector<ValueArray> &arrays) {
  std::string out;
  out.push_back('\x01');
  out.push_back(static_cast<char>(arrays.size()));
  for (const auto &va : arrays) {
    out.push_back(static_cast<char>(va.nameIndex));
    std::uint32_t len = static_cast<std::uint32_t>(va.bytes.size());
    out.append(reinterpret_cast<const char *>(&len), 4);
    out.append(reinterpret_cast<const char *>(va.bytes.data()), len);
  }
  return out;
}

static int arrayNameIndex(const std::string &name) {
  if (name.size() < 2 || name[0] != 'A') return -1;
  for (std::size_t i = 1; i < name.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return -1;
  return std::atoi(name.c_str() + 1);
}

static uint64_t hashKeySet(const std::set<std::string> &ks) {
  uint64_t h = 14695981039346656037ULL;
  for (const auto &s : ks) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    h ^= '\0'; h *= 1099511628211ULL;
  }
  return h;
}

struct Conflict {
  std::set<std::string> key_set;
  std::string satSource;
  std::string unsatSource;
};

struct LogEntry {
  std::set<std::string> key_set;
  std::string value;
  // Concrete canonical arrays: (name_index, byte_values).
  // Populated from the log record's concrete-arrays section; empty if all
  // arrays in the entry were symbolic.
  std::vector<std::pair<uint8_t, std::vector<unsigned char>>> concreteArrays;
};

/// Read all records from a CEX cache log file.
/// Returns the number of records read; partial trailing records (from a crash)
/// are silently discarded.
static size_t readLogFile(const std::string &path,
                          std::vector<LogEntry> &out) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "error: cannot open log file: %s\n", path.c_str());
    return 0;
  }

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size == 0) {
    close(fd);
    return 0;
  }

  std::vector<char> buf(st.st_size);
  ssize_t n = read(fd, buf.data(), buf.size());
  close(fd);
  if (n <= 0)
    return 0;

  size_t pos = 0;
  size_t fileSize = static_cast<size_t>(n);
  size_t count = 0;

  while (pos + 4 <= fileSize) {
    // Read num_keys
    uint32_t numKeys = 0;
    memcpy(&numKeys, &buf[pos], 4);
    pos += 4;

    std::set<std::string> keySet;
    bool truncated = false;
    for (uint32_t i = 0; i < numKeys; ++i) {
      if (pos + 4 > fileSize) { truncated = true; break; }
      uint32_t klen = 0;
      memcpy(&klen, &buf[pos], 4);
      pos += 4;
      if (pos + klen > fileSize) { truncated = true; break; }
      keySet.emplace(&buf[pos], klen);
      pos += klen;
    }
    if (truncated) break;

    if (pos + 4 > fileSize) break;
    uint32_t vlen = 0;
    memcpy(&vlen, &buf[pos], 4);
    pos += 4;
    if (pos + vlen > fileSize) break;

    std::string value(&buf[pos], vlen);
    pos += vlen;

    // Read concrete arrays section.
    if (pos + 4 > fileSize) break;
    uint32_t numConcrete = 0;
    memcpy(&numConcrete, &buf[pos], 4);
    pos += 4;

    std::vector<std::pair<uint8_t, std::vector<unsigned char>>> concretes;
    bool concreteTruncated = false;
    for (uint32_t i = 0; i < numConcrete; ++i) {
      if (pos + 5 > fileSize) { concreteTruncated = true; break; }
      uint8_t nameIdx = static_cast<uint8_t>(buf[pos++]);
      uint32_t clen = 0;
      memcpy(&clen, &buf[pos], 4); pos += 4;
      if (pos + clen > fileSize) { concreteTruncated = true; break; }
      std::vector<unsigned char> bytes(
          reinterpret_cast<const unsigned char *>(&buf[pos]),
          reinterpret_cast<const unsigned char *>(&buf[pos]) + clen);
      pos += clen;
      concretes.emplace_back(nameIdx, std::move(bytes));
    }
    if (concreteTruncated) break;

    out.push_back({std::move(keySet), std::move(value), std::move(concretes)});
    ++count;
  }

  return count;
}

static void printUsage(const char *prog) {
  fprintf(stderr,
      "Usage: %s [options] [CACHE_FILE] [--log LOG_FILE ...] -o OUTPUT\n"
      "\n"
      "Optimise a KLEE persistent solver cache.\n"
      "\n"
      "Inputs (at least one required):\n"
      "  CACHE_FILE             Existing cache file (MapOfSets v2 format)\n"
      "  --log LOG_FILE         Log file(s) produced by --write-disk-cex-cache\n"
      "                         or --persistent-cex-cache (may be repeated)\n"
      "\n"
      "Options:\n"
      "  -o OUTPUT              Output cache file path (required unless --stats)\n"
      "  --no-prune-unsat       Disable UNSAT dominance pruning\n"
      "  --no-dedup             Disable deduplication of identical entries\n"
      "  --minimize-unsat       Shrink each UNSAT entry to a minimal UNSAT core\n"
      "                         via iterative delta-debugging (calls solver)\n"
      "  --split-independent    Split SAT entries whose constraints touch\n"
      "                         disjoint byte ranges into separate sub-entries\n"
      "  --compact-witnesses    Truncate each SAT value blob to the byte range\n"
      "                         its constraints actually reference\n"
      "  --discover-unsat-pairs Try every pair of cache constraints sharing an\n"
      "                         array; SAT-side pairs are ignored, UNSAT pairs\n"
      "                         become new 2-element entries\n"
      "  --max-pair-calls N     Cap on solver calls for pair discovery (default 10000)\n"
      "  --stats                Print statistics only; do not write output\n"
      "  --verify               Roundtrip every entry through the solver to\n"
      "                         confirm parse + UNSAT/SAT classification (slow)\n"
      "  --solver-timeout SECS  Per-query solver timeout (default 30, 0 = none)\n"
      "  -h, --help             Show this help\n",
      prog);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  std::string inputPath;
  std::string outputPath;
  std::vector<std::string> logPaths;
  bool pruneUnsat = true;
  bool dedup      = true;
  bool statsOnly  = false;
  bool verify     = false;
  bool minimizeUnsat = false;
  bool splitIndependent = false;
  bool compactWitnesses = false;
  bool discoverUnsatPairs = false;
  size_t maxPairCalls = 10000;
  unsigned solverTimeoutSec = 30;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      outputPath = argv[++i];
    } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
      logPaths.push_back(argv[++i]);
    } else if (strcmp(argv[i], "--no-prune-unsat") == 0) {
      pruneUnsat = false;
    } else if (strcmp(argv[i], "--no-dedup") == 0) {
      dedup = false;
    } else if (strcmp(argv[i], "--stats") == 0) {
      statsOnly = true;
    } else if (strcmp(argv[i], "--verify") == 0) {
      verify = true;
    } else if (strcmp(argv[i], "--minimize-unsat") == 0) {
      minimizeUnsat = true;
    } else if (strcmp(argv[i], "--split-independent") == 0) {
      splitIndependent = true;
    } else if (strcmp(argv[i], "--compact-witnesses") == 0) {
      compactWitnesses = true;
    } else if (strcmp(argv[i], "--discover-unsat-pairs") == 0) {
      discoverUnsatPairs = true;
    } else if (strcmp(argv[i], "--max-pair-calls") == 0 && i + 1 < argc) {
      maxPairCalls = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (strcmp(argv[i], "--solver-timeout") == 0 && i + 1 < argc) {
      solverTimeoutSec = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(argv[i], "-h") == 0 ||
               strcmp(argv[i], "--help") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
      printUsage(argv[0]);
      return 1;
    } else {
      if (!inputPath.empty()) {
        fprintf(stderr, "Multiple cache input files specified.\n\n");
        printUsage(argv[0]);
        return 1;
      }
      inputPath = argv[i];
    }
  }

  if (inputPath.empty() && logPaths.empty()) {
    fprintf(stderr, "No input files specified.\n\n");
    printUsage(argv[0]);
    return 1;
  }
  if (!statsOnly && outputPath.empty()) {
    fprintf(stderr, "No output file specified (use -o OUTPUT or --stats).\n\n");
    printUsage(argv[0]);
    return 1;
  }

  // -------------------------------------------------------------------------
  // Collect all entries from cache file and/or log files
  // -------------------------------------------------------------------------

  struct Entry {
    std::set<std::string> key_set;
    std::string value;
    std::vector<std::pair<uint8_t, std::vector<unsigned char>>> concreteArrays;
    std::string source; // "cache:<path>" or "log:<path>"
  };

  std::vector<Entry> unsatEntries, satEntries;
  std::string storedSolver, storedVersion;

  // Read existing cache file (if provided)
  if (!inputPath.empty()) {
    DiskMapOfSets disk(inputPath, /*max_cache_size=*/200);
    if (!disk.isValid()) {
      fprintf(stderr, "error: could not open or parse cache file: %s\n",
              inputPath.c_str());
      return 1;
    }
    storedSolver  = disk.solverBackend();
    storedVersion = disk.kleeVersion();

    std::string cacheSource = "cache:" + inputPath;
    disk.forEach([&](const std::set<std::string> &ks, const std::string &v) {
      if (isUnsat(v))
        unsatEntries.push_back({ks, v, {}, cacheSource});
      else
        satEntries.push_back({ks, v, {}, cacheSource});
    });
    fprintf(stdout, "Cache file   : %s  (%zu entries)\n",
            inputPath.c_str(), unsatEntries.size() + satEntries.size());
  }

  // Read log files
  for (const auto &logPath : logPaths) {
    std::vector<LogEntry> logEntries;
    size_t n = readLogFile(logPath, logEntries);
    std::string logSource = "log:" + logPath;
    for (auto &le : logEntries) {
      if (isUnsat(le.value))
        unsatEntries.push_back({std::move(le.key_set), std::move(le.value),
                                std::move(le.concreteArrays), logSource});
      else
        satEntries.push_back({std::move(le.key_set), std::move(le.value),
                              std::move(le.concreteArrays), logSource});
    }
    fprintf(stdout, "Log file     : %s  (%zu records)\n", logPath.c_str(), n);
  }

  // Snapshot the input counts before any pass mutates the vectors.
  const size_t inputUnsat = unsatEntries.size();
  const size_t inputSat   = satEntries.size();
  const size_t inputTotal = inputUnsat + inputSat;
  // `unsatIn` / `satIn` track the pre-insert sizes (i.e. after passes 1-4)
  // so the dominance/dedup percentages downstream are correct.
  size_t unsatIn = inputUnsat;
  size_t satIn   = inputSat;

  // -------------------------------------------------------------------------
  // UNSAT core minimisation (Pass 1) — optional, requires solver
  // -------------------------------------------------------------------------

  size_t minimizedEntries        = 0;
  size_t totalConstraintsRemoved = 0;
  size_t minimizeSkippedNotUnsat = 0;
  size_t minimizeSkippedTimeout  = 0;
  size_t minimizeSolverCalls     = 0;

  if (minimizeUnsat && !unsatEntries.empty()) {
    fprintf(stdout, "\nMinimising %zu UNSAT entries (timeout=%us)...\n",
            unsatEntries.size(), solverTimeoutSec);
    OfflineEngine engine(solverTimeoutSec);

    size_t reportEvery = std::max<size_t>(unsatEntries.size() / 20, 1);

    for (size_t i = 0; i < unsatEntries.size(); ++i) {
      auto &e = unsatEntries[i];
      if (e.key_set.size() <= 1) continue;

      // Snapshot the canonical key strings in their lex (set iteration) order;
      // this matches the order parseKey emits constraints in.
      std::vector<std::string> orderedKeys(e.key_set.begin(), e.key_set.end());

      ParsedKey pk = engine.parseKey(e.key_set, e.concreteArrays);
      if (!pk.ok) continue;
      if (pk.constraints.size() != orderedKeys.size()) continue;

      auto mr = engine.minimizeUnsatCore(pk.constraints);
      minimizeSolverCalls += mr.solverCalls;
      if (mr.notUnsatPrecondition) { ++minimizeSkippedNotUnsat; continue; }
      if (mr.timedOut)             { ++minimizeSkippedTimeout;  /* fall through */ }
      if (mr.keptIndices.size() == orderedKeys.size()) continue; // already minimal

      std::set<std::string> shrunk;
      for (size_t k : mr.keptIndices)
        shrunk.insert(orderedKeys[k]);

      totalConstraintsRemoved += orderedKeys.size() - shrunk.size();
      ++minimizedEntries;
      e.key_set = std::move(shrunk);

      if ((i + 1) % reportEvery == 0)
        fprintf(stderr, "  minimised %zu/%zu (shrunk=%zu, removed_total=%zu)\n",
                i + 1, unsatEntries.size(), minimizedEntries,
                totalConstraintsRemoved);
    }

    fprintf(stdout, "  UNSAT entries shrunk      : %zu / %zu\n",
            minimizedEntries, unsatIn);
    fprintf(stdout, "  Constraints removed (total): %zu\n", totalConstraintsRemoved);
    fprintf(stdout, "  Skipped (not unsat)       : %zu\n", minimizeSkippedNotUnsat);
    fprintf(stdout, "  Affected by timeout       : %zu\n", minimizeSkippedTimeout);
    fprintf(stdout, "  Solver calls              : %zu\n", minimizeSolverCalls);
  }

  // -------------------------------------------------------------------------
  // SAT independence splitting (Pass 2) — optional, parser-only, no solver
  // -------------------------------------------------------------------------

  size_t splitInputEntries  = 0;
  size_t splitOutputEntries = 0;
  size_t splitSkippedParse  = 0;
  size_t splitSkippedBlob   = 0;
  size_t splitSkippedShape  = 0;

  if (splitIndependent && !satEntries.empty()) {
    fprintf(stdout, "\nSplitting independent SAT entries...\n");
    OfflineEngine engine(0); // parser only, no solver invoked

    std::vector<Entry> newSat;
    newSat.reserve(satEntries.size());

    for (auto &e : satEntries) {
      if (e.key_set.size() <= 1) { newSat.push_back(std::move(e)); continue; }

      // Snapshot key strings in iteration order (matches parser output order).
      std::vector<std::string> orderedKeys(e.key_set.begin(), e.key_set.end());

      ParsedKey pk = engine.parseKey(e.key_set, e.concreteArrays);
      if (!pk.ok || pk.constraints.size() != orderedKeys.size()) {
        ++splitSkippedParse;
        newSat.push_back(std::move(e));
        continue;
      }

      auto comps = engine.computeByteComponents(pk.constraints);
      if (comps.size() <= 1) { newSat.push_back(std::move(e)); continue; }

      // Parse the stored value blob into per-array byte vectors.
      std::vector<ValueArray> origArrays;
      if (!parseSatValue(e.value, origArrays)) {
        ++splitSkippedBlob;
        newSat.push_back(std::move(e));
        continue;
      }
      // name_index → ptr for fast lookup
      std::map<std::uint8_t, const ValueArray *> byIdx;
      for (const auto &va : origArrays) byIdx[va.nameIndex] = &va;

      // Validate: every component-required array must be present in the blob.
      bool ok = true;
      for (const auto &comp : comps) {
        for (const auto &name : comp.arrayNames) {
          int idx = arrayNameIndex(name);
          if (idx < 0 || idx > 255 || !byIdx.count(static_cast<std::uint8_t>(idx))) {
            ok = false; break;
          }
        }
        if (!ok) break;
      }
      if (!ok) {
        ++splitSkippedShape;
        newSat.push_back(std::move(e));
        continue;
      }

      ++splitInputEntries;
      for (const auto &comp : comps) {
        Entry sub;
        for (auto i : comp.constraintIndices)
          sub.key_set.insert(orderedKeys[i]);

        std::vector<ValueArray> subArrays;
        subArrays.reserve(comp.arrayNames.size());
        for (const auto &name : comp.arrayNames) {
          int idx = arrayNameIndex(name);
          const ValueArray *src = byIdx[static_cast<std::uint8_t>(idx)];
          // KLEE's IndependentSolver asserts that two assignments for the
          // same array produce the same byte count.  Truncating to the
          // component's coverage breaks that invariant when the value is
          // later mixed with a fresh full-size Z3 result.  So duplicate the
          // full original bytes into each sub-blob; the only saving from
          // splitting is on the *key* side, not the value side.
          ValueArray va;
          va.nameIndex = static_cast<std::uint8_t>(idx);
          va.bytes = src->bytes;
          subArrays.push_back(std::move(va));
        }
        std::sort(subArrays.begin(), subArrays.end(),
                  [](const ValueArray &a, const ValueArray &b) {
                    return a.nameIndex < b.nameIndex;
                  });
        sub.value = serializeSatValue(subArrays);
        newSat.push_back(std::move(sub));
        ++splitOutputEntries;
      }
    }
    satEntries = std::move(newSat);

    fprintf(stdout, "  Entries split             : %zu (→ %zu sub-entries)\n",
            splitInputEntries, splitOutputEntries);
    fprintf(stdout, "  Skipped (parse failure)   : %zu\n", splitSkippedParse);
    fprintf(stdout, "  Skipped (bad value blob)  : %zu\n", splitSkippedBlob);
    fprintf(stdout, "  Skipped (missing arrays)  : %zu\n", splitSkippedShape);
    satIn = satEntries.size(); // reflect new entries in subsequent stats
  }

  // -------------------------------------------------------------------------
  // SAT witness compaction (Pass 3) — parser-only, no solver
  //
  // The original plan was to truncate each array's byte count down to the
  // entry's referenced byte coverage.  That turned out to be UNSOUND for
  // KLEE consumption: IndependentSolver::computeInitialValues asserts that
  // two assignments for the same array have matching byte counts, and a
  // truncated cache hit mixed with a fresh full-size Z3 result trips the
  // assert.  The only safe compaction is to *drop arrays the entry's
  // constraints don't reference at all* — those bindings come from the
  // original witness containing extra arrays and are pure bloat for this
  // entry's lookups.  We do NOT truncate bytes of referenced arrays.
  // -------------------------------------------------------------------------

  size_t compactScanned     = 0;
  size_t compactShrunk      = 0;
  size_t compactBytesSaved  = 0;
  size_t compactSkippedParse = 0;
  size_t compactSkippedBlob = 0;

  if (compactWitnesses && !satEntries.empty()) {
    fprintf(stdout, "\nCompacting SAT witnesses...\n");
    OfflineEngine engine(0);

    for (auto &e : satEntries) {
      if (e.key_set.empty()) continue;
      ++compactScanned;

      ParsedKey pk = engine.parseKey(e.key_set, e.concreteArrays);
      if (!pk.ok) { ++compactSkippedParse; continue; }

      // Collect every array name that any constraint in the key references.
      auto comps = engine.computeByteComponents(pk.constraints);
      std::set<std::string> referenced;
      for (const auto &comp : comps)
        for (const auto &name : comp.arrayNames)
          referenced.insert(name);

      std::vector<ValueArray> arrays;
      if (!parseSatValue(e.value, arrays)) {
        ++compactSkippedBlob;
        continue;
      }

      std::vector<ValueArray> kept;
      kept.reserve(arrays.size());
      std::size_t before = e.value.size();
      for (auto &va : arrays) {
        std::string name = "A" + std::to_string(va.nameIndex);
        if (referenced.count(name))
          kept.push_back(std::move(va));
        // else: drop unreferenced array binding entirely.
      }
      if (kept.size() == arrays.size()) continue; // no array dropped

      e.value = serializeSatValue(kept);
      ++compactShrunk;
      compactBytesSaved += (before - e.value.size());
    }

    fprintf(stdout, "  Witnesses scanned         : %zu\n", compactScanned);
    fprintf(stdout, "  Witnesses shrunk          : %zu\n", compactShrunk);
    fprintf(stdout, "  Bytes saved               : %zu\n", compactBytesSaved);
    if (compactSkippedParse > 0)
      fprintf(stdout, "  Skipped (parse failure)   : %zu\n", compactSkippedParse);
    if (compactSkippedBlob > 0)
      fprintf(stdout, "  Skipped (bad value blob)  : %zu\n", compactSkippedBlob);
  }

  // -------------------------------------------------------------------------
  // UNSAT pair discovery (Pass 4) — solver-heavy, optional
  //
  // Across the entire cache, look at all unique constraint strings.  Cluster
  // them by shared canonical array names — only constraints that touch the
  // same arrays can possibly contradict.  For each within-cluster pair, ask
  // the solver whether the pair is UNSAT.  New UNSAT pairs are added as
  // fresh 2-element entries, which after dominance pruning will subsume any
  // larger UNSAT entries containing both.
  // -------------------------------------------------------------------------

  size_t pairsTried = 0, pairsSkippedNoOverlap = 0, pairsAlreadyKnown = 0;
  size_t pairsUnsat = 0, pairsTimeout = 0, pairsParseFail = 0;

  if (discoverUnsatPairs) {
    fprintf(stdout, "\nDiscovering UNSAT pairs (timeout=%us, cap=%zu calls)...\n",
            solverTimeoutSec, maxPairCalls);
    OfflineEngine engine(solverTimeoutSec);

    // 1. Gather unique constraint strings + their array sets.
    std::map<std::string, std::set<std::string>> stringArrays;
    auto collectFrom = [&](const std::vector<Entry> &es) {
      for (const auto &e : es)
        for (const auto &c : e.key_set) {
          auto it = stringArrays.find(c);
          if (it == stringArrays.end()) {
            std::set<std::string> arrs;
            OfflineEngine::collectArrayNames(c, arrs);
            stringArrays.emplace(c, std::move(arrs));
          }
        }
    };
    collectFrom(unsatEntries);
    collectFrom(satEntries);

    // 2. Index existing UNSAT keys so we don't re-emit known pairs.
    std::set<std::set<std::string>> knownUnsatKeys;
    for (const auto &e : unsatEntries) knownUnsatKeys.insert(e.key_set);

    // 3. Materialise pair candidates sorted by descending array-overlap
    //    weight (the size of the intersection of their canonical-array sets),
    //    with lexicographic tiebreak on the (uniq[i], uniq[j]) pair.
    //
    //    Why descending overlap: under a tight --max-pair-calls budget, pairs
    //    with more shared arrays are a priori more likely to interact.  A
    //    plain lexicographic iteration (the previous behaviour, accidental
    //    from std::map ordering) wastes the budget on alphabetically early
    //    constraints regardless of solver-relevance.
    //
    //    Why lex tiebreak: makes the candidate sequence — and therefore which
    //    pairs are explored under a budget cap, and the byte content of any
    //    new UNSAT entries inserted into the result — deterministic across
    //    runs.  Without it, ties resolve by std::sort's implementation-
    //    defined choices.
    std::vector<std::string> uniq;
    uniq.reserve(stringArrays.size());
    for (auto &kv : stringArrays) uniq.push_back(kv.first);

    fprintf(stdout, "  Constraint universe       : %zu unique strings\n",
            uniq.size());

    struct PairCandidate {
      size_t i, j;            // indices into uniq, with i < j
      uint32_t overlapWeight; // |arrays(uniq[i]) ∩ arrays(uniq[j])|
    };
    std::vector<PairCandidate> candidates;
    for (size_t i = 0; i < uniq.size(); ++i) {
      const auto &arrsA = stringArrays[uniq[i]];
      for (size_t j = i + 1; j < uniq.size(); ++j) {
        const auto &arrsB = stringArrays[uniq[j]];
        uint32_t overlap = 0;
        for (const auto &s : arrsA) if (arrsB.count(s)) ++overlap;
        if (overlap == 0) { ++pairsSkippedNoOverlap; continue; }
        candidates.push_back({i, j, overlap});
      }
    }

    std::sort(candidates.begin(), candidates.end(),
              [&](const PairCandidate &a, const PairCandidate &b) {
                if (a.overlapWeight != b.overlapWeight)
                  return a.overlapWeight > b.overlapWeight; // descending
                int ci = uniq[a.i].compare(uniq[b.i]);
                if (ci != 0) return ci < 0;
                return uniq[a.j] < uniq[b.j];
              });

    for (const auto &cand : candidates) {
      if (pairsTried >= maxPairCalls) break;

      std::set<std::string> key{uniq[cand.i], uniq[cand.j]};
      if (knownUnsatKeys.count(key)) { ++pairsAlreadyKnown; continue; }

      ParsedKey pk = engine.parseKey(key);
      if (!pk.ok) { ++pairsParseFail; continue; }

      bool to = false;
      ++pairsTried;
      bool unsat = engine.isUnsat(pk.constraints, to);
      if (to) {
        ++pairsTimeout;
      } else if (unsat) {
        ++pairsUnsat;
        Entry e;
        e.key_set = key;
        e.value = ""; // UNSAT sentinel
        unsatEntries.push_back(std::move(e));
        knownUnsatKeys.insert(std::move(key));
      }
    }

    fprintf(stdout, "  Pairs solver-checked      : %zu\n", pairsTried);
    fprintf(stdout, "  Pairs skipped (no overlap): %zu\n", pairsSkippedNoOverlap);
    fprintf(stdout, "  Pairs already in cache    : %zu\n", pairsAlreadyKnown);
    fprintf(stdout, "  New UNSAT pairs found     : %zu\n", pairsUnsat);
    fprintf(stdout, "  Pairs timed out           : %zu\n", pairsTimeout);
    if (pairsParseFail > 0)
      fprintf(stdout, "  Pairs parse failed        : %zu\n", pairsParseFail);
    unsatIn = unsatEntries.size(); // include newly-discovered pairs
  }

  // -------------------------------------------------------------------------
  // Build optimised output tree
  // -------------------------------------------------------------------------

  MapOfSets<std::string, std::string> result;

  size_t unsatDuplicates = 0;
  size_t unsatDominated  = 0;
  size_t satDuplicates   = 0;
  size_t satConflicts    = 0; // key_set present as UNSAT; SAT overwritten

  std::vector<Conflict> conflicts;

  // --- SAT entries (inserted first so UNSAT can overwrite on conflict) ------

  // Track seen SAT key_sets for deduplication reporting.
  std::set<std::set<std::string>> satSeen;
  // Map key_set → source so we can report the SAT side of any conflict.
  std::map<std::set<std::string>, std::string> satKeyToSource;

  for (const auto &e : satEntries) {
    if (dedup && !satSeen.insert(e.key_set).second) {
      ++satDuplicates;
      continue;
    }
    satKeyToSource[e.key_set] = e.source;
    result.insert(e.key_set, e.value);
  }

  // --- UNSAT entries ---------------------------------------------------------

  // Sort ascending by key_set size so that smaller (dominating) sets are
  // processed before larger (dominated) sets.  Lexicographic tiebreak on
  // the key_set itself makes the iteration order — and therefore the output
  // file's byte content — deterministic across runs (std::sort is unstable).
  std::sort(unsatEntries.begin(), unsatEntries.end(),
            [](const Entry &a, const Entry &b) {
              if (a.key_set.size() != b.key_set.size())
                return a.key_set.size() < b.key_set.size();
              return a.key_set < b.key_set;
            });

  // Separate incremental trie used only for subset queries during pruning.
  // We do not re-use `result` for this because `result` contains SAT entries
  // whose key_sets must not affect UNSAT dominance decisions.
  MapOfSets<std::string, std::string> unsatTrie;

  std::set<std::set<std::string>> unsatSeen;

  for (const auto &e : unsatEntries) {
    // Deduplication check (exact key_set seen before).
    if (dedup && !unsatSeen.insert(e.key_set).second) {
      ++unsatDuplicates;
      continue;
    }

    if (pruneUnsat) {
      // At this point e.key_set is not yet in unsatTrie (we only insert on
      // acceptance), so any result from subsets() is a strict subset.
      std::vector<std::pair<std::set<std::string>, std::string>> subs;
      unsatTrie.subsets(e.key_set, subs);
      if (!subs.empty()) {
        ++unsatDominated;
        continue;
      }
      unsatTrie.insert(e.key_set, e.value);
    }

    // Check for a conflicting SAT entry at the same key_set.
    if (result.lookup(e.key_set) != nullptr) {
      ++satConflicts;
      auto it = satKeyToSource.find(e.key_set);
      std::string satSrc =
          (it != satKeyToSource.end()) ? it->second : "<unknown>";
      conflicts.push_back({e.key_set, satSrc, e.source});
    }

    // Insert UNSAT into result, overwriting any conflicting SAT value.
    result.insert(e.key_set, e.value);
  }

  // -------------------------------------------------------------------------
  // Statistics
  // -------------------------------------------------------------------------

  const size_t unsatOut = unsatIn - unsatDuplicates - unsatDominated;
  const size_t satOut   = satIn   - satDuplicates   - satConflicts;
  const size_t totalOut = unsatOut + satOut;
  // (totalRemoved is computed inline in the print block; underflow-safe there)

  if (!storedSolver.empty())
    fprintf(stdout, "Solver       : %s\n", storedSolver.c_str());
  if (!storedVersion.empty())
    fprintf(stdout, "KLEE version : %s\n", storedVersion.c_str());
  fprintf(stdout, "\n");

  fprintf(stdout, "Input entries  : %zu  (UNSAT: %zu, SAT: %zu)\n",
          inputTotal, inputUnsat, inputSat);
  if (unsatEntries.size() != inputUnsat || satEntries.size() != inputSat)
    fprintf(stdout, "  After passes  : UNSAT=%zu  SAT=%zu\n",
            unsatEntries.size(), satEntries.size());

  if (dedup) {
    if (unsatDuplicates > 0)
      fprintf(stdout, "  Duplicates removed (UNSAT): %zu\n", unsatDuplicates);
    if (satDuplicates > 0)
      fprintf(stdout, "  Duplicates removed (SAT)  : %zu\n", satDuplicates);
  }
  if (pruneUnsat && unsatDominated > 0)
    fprintf(stdout, "  UNSAT dominated pruned    : %zu  (%.1f%% of UNSAT)\n",
            unsatDominated,
            unsatIn > 0 ? 100.0 * unsatDominated / unsatIn : 0.0);
  if (satConflicts > 0)
    fprintf(stdout, "  SAT/UNSAT conflicts       : %zu  (SAT overwritten)\n",
            satConflicts);

  fprintf(stdout, "\n");
  fprintf(stdout, "Output entries : %zu  (UNSAT: %zu, SAT: %zu)\n",
          totalOut, unsatOut, satOut);
  if (inputTotal > 0) {
    if (totalOut <= inputTotal)
      fprintf(stdout, "Reduction      : %zu entries removed (%.1f%%)\n",
              inputTotal - totalOut,
              100.0 * (inputTotal - totalOut) / inputTotal);
    else
      fprintf(stdout, "Expansion      : %zu entries added (net, after split)\n",
              totalOut - inputTotal);
  }

  // -------------------------------------------------------------------------
  // Report SAT/UNSAT inconsistencies
  // -------------------------------------------------------------------------

  if (!conflicts.empty()) {
    for (const auto &c : conflicts) {
      fprintf(stderr,
              "INCONSISTENCY key_hash=%016" PRIx64 " key_size=%zu"
              " sat_from=%s unsat_from=%s\n",
              hashKeySet(c.key_set), c.key_set.size(),
              c.satSource.c_str(), c.unsatSource.c_str());
    }
    fprintf(stderr,
            "warning: %zu SAT/UNSAT inconsistenc%s detected; "
            "UNSAT entries were kept\n",
            conflicts.size(), conflicts.size() == 1 ? "y" : "ies");
  }

  // -------------------------------------------------------------------------
  // Verification pass: roundtrip every entry through parser + solver
  // -------------------------------------------------------------------------

  if (verify) {
    fprintf(stdout, "\nVerifying entries against solver (timeout=%us)...\n",
            solverTimeoutSec);
    OfflineEngine engine(solverTimeoutSec);
    size_t parseFail = 0, unsatConfirmed = 0, unsatRefuted = 0,
           unsatTimeout = 0, satParsed = 0, emptyKey = 0;

    auto verifyVec = [&](const std::vector<Entry> &entries, bool expectUnsat) {
      for (size_t i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        if (e.key_set.empty()) { ++emptyKey; continue; }
        ParsedKey pk = engine.parseKey(e.key_set, e.concreteArrays);
        if (!pk.ok) {
          ++parseFail;
          if (parseFail <= 3)
            fprintf(stderr, "  parse failure (%s entry %zu): %s\n",
                    expectUnsat ? "UNSAT" : "SAT", i, pk.error.c_str());
          continue;
        }
        if (!expectUnsat) { ++satParsed; continue; }
        bool timedOut = false;
        bool isU = engine.isUnsat(pk.constraints, timedOut);
        if (timedOut)  ++unsatTimeout;
        else if (isU)  ++unsatConfirmed;
        else           ++unsatRefuted;
      }
    };
    verifyVec(unsatEntries, /*expectUnsat=*/true);
    verifyVec(satEntries,   /*expectUnsat=*/false);

    fprintf(stdout, "\nVerification:\n");
    fprintf(stdout, "  Parse failures        : %zu\n", parseFail);
    fprintf(stdout, "  Empty key sets        : %zu\n", emptyKey);
    fprintf(stdout, "  SAT parsed OK         : %zu / %zu\n", satParsed, satIn);
    fprintf(stdout, "  UNSAT confirmed       : %zu / %zu\n", unsatConfirmed, unsatIn);
    fprintf(stdout, "  UNSAT refuted (!)     : %zu\n", unsatRefuted);
    fprintf(stdout, "  UNSAT solver timeout  : %zu\n", unsatTimeout);
    if (unsatRefuted > 0)
      fprintf(stderr, "warning: %zu stored UNSAT entries did not re-verify; "
                      "this likely indicates a concrete-array reference whose "
                      "contents the canonical key cannot reconstruct.\n",
              unsatRefuted);
  }

  if (statsOnly) {
    fprintf(stdout, "\n(--stats: no output written)\n");
    return conflicts.empty() ? 0 : 2;
  }

  // -------------------------------------------------------------------------
  // Write output
  // -------------------------------------------------------------------------

  CacheMetadata meta;
  meta.solverBackend = storedSolver;
  meta.kleeVersion   = storedVersion;

  MapOfSetsDiskBuilder::build(result, outputPath, meta);

  fprintf(stdout, "\nWritten to   : %s\n", outputPath.c_str());
  return conflicts.empty() ? 0 : 2;
}
