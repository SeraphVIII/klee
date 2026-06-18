#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/Support/ErrorHandling.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

// Multi-byte integers are written via raw memcpy; LE-only until routed
// through explicit byte-order helpers.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "MapOfSets file format requires a little-endian host");
#endif

using namespace klee;

void MapOfSetsDiskBuilder::dfsAssign(const MapOfSetsDiskBuilder::UBTree::Node *src,
               std::vector<BuildNode> &outNodes) {
  uint32_t id = outNodes.size();
  outNodes.emplace_back();
  outNodes[id].id = id;
  outNodes[id].isEndOfSet = src->isEndOfSet;
  if (src->isEndOfSet)
    outNodes[id].value = src->value;

  for (const auto& kv : src->children) {
    const MapOfSetsDiskBuilder::K &key = kv.first;
    const auto *childSrc = &kv.second;
    uint32_t childId = outNodes.size();
    dfsAssign(childSrc, outNodes);
    outNodes[id].children.emplace_back(key, childId);
  }
}

void MapOfSetsDiskBuilder::build(const UBTree &tree,
                                 const std::string &filename,
                                 const CacheMetadata &metadata,
                                 uint32_t chunkSize) {
  static constexpr uint64_t kRawMagic = 0x4d41504f53455453ULL; // "MAPOSETS"
  const uint64_t kPreambleSize = 16; // magic + header_size

  std::vector<BuildNode> nodes;
  nodes.reserve(1024);
  dfsAssign(&tree.root, nodes);
  uint32_t totalNodes = static_cast<uint32_t>(nodes.size());

  // Collect distinct keys with O(1) inserts, then sort into the string table.
  // The table MUST stay sorted: the reader binary-searches children by key and
  // relies on key_index numeric order matching string order (audit N1/F6).
  std::unordered_set<std::string> uniqueKeys;
  for (const auto &bn : nodes)
    for (const auto &kv : bn.children)
      uniqueKeys.insert(kv.first);

  std::vector<std::string> strings(uniqueKeys.begin(), uniqueKeys.end());
  std::sort(strings.begin(), strings.end());

  std::unordered_map<std::string, uint32_t> stringIndex;
  stringIndex.reserve(strings.size() * 2);
  for (uint32_t i = 0; i < static_cast<uint32_t>(strings.size()); ++i)
    stringIndex[strings[i]] = i;

  std::string stringTableBlob;
  uint32_t strCount = static_cast<uint32_t>(strings.size());
  stringTableBlob.append(reinterpret_cast<const char *>(&strCount), 4);
  for (const auto &s : strings) {
    uint32_t len = static_cast<uint32_t>(s.size());
    stringTableBlob.append(reinterpret_cast<const char *>(&len), 4);
    stringTableBlob.append(s);
  }

  // Values stored as offset+1 so 0 doubles as "no value"; len=0 means UNSAT.
  std::string valuesBlob;
  std::vector<uint64_t> valueOffsets(totalNodes, 0);
  for (const auto &bn : nodes) {
    if (!bn.isEndOfSet)
      continue;
    uint64_t offset = valuesBlob.size();
    uint32_t len = static_cast<uint32_t>(bn.value.size());
    valuesBlob.append(reinterpret_cast<const char *>(&len), sizeof(len));
    valuesBlob.append(bn.value.data(), bn.value.size());
    valueOffsets[bn.id] = offset + 1;
  }

  uint32_t numChunks = (totalNodes + chunkSize - 1) / chunkSize;
  uint64_t dirSize = numChunks * 12ULL; // u64 offset + u32 size per entry

  std::vector<std::string> chunkBuffers(numChunks);
  for (uint32_t c = 0; c < numChunks; ++c) {
    uint32_t first = c * chunkSize;
    uint32_t count = std::min(chunkSize, totalNodes - first);

    mapofsets::NodeChunk chunkMsg;
    chunkMsg.set_first_node_id(first);
    chunkMsg.set_node_count(count);

    for (uint32_t i = 0; i < count; ++i) {
      const BuildNode &bn = nodes[first + i];
      mapofsets::Node *n = chunkMsg.add_nodes();
      n->set_is_end_of_set(bn.isEndOfSet);
      n->set_value_offset(valueOffsets[bn.id]);
      for (const auto &kv : bn.children) {
        mapofsets::Child *ch = n->add_children();
        ch->set_key_index(stringIndex.at(kv.first));
        ch->set_child_id(kv.second);
      }
    }

    chunkMsg.SerializeToString(&chunkBuffers[c]);
  }

  mapofsets::MapOfSetsFile file;
  mapofsets::Header *hdr = file.mutable_header();
  hdr->set_magic(kRawMagic);
  hdr->set_version(1);
  // Bump on any canonicalization or key-serialization change so readers
  // reject stale cache files.
  // v2: canonicalizer DFS walks converted from recursion to explicit-stack
  // iteration (deep KLEE expressions overflowed the C++ stack); the traversal
  // change can perturb canonical key bytes, so v1 caches must not be reused.
  // v3: ExprCanonicalOrder now orders by cached structural hash (with a
  // structural tiebreak on collision) instead of a pure structural walk; this
  // changes commutative operand order and thus the canonical key bytes.
  // CAVEAT: as of v3 the canonical operand order depends on Expr::computeHash().
  // That is deterministic within a build (so cold/warm match), but if KLEE's
  // expression hashing ever changes the canonical key bytes shift silently.
  // The failure mode is benign (old caches all-miss, never a wrong hit — the
  // key strings simply differ), but BUMP THIS VERSION if computeHash changes so
  // the mismatch is reported instead of degrading to silent misses.
  static constexpr uint32_t kCanonVersion = 3;
  hdr->set_canonicalization_version(kCanonVersion);
  if (!metadata.solverBackend.empty())
    hdr->set_solver_backend(metadata.solverBackend);
  if (!metadata.kleeVersion.empty())
    hdr->set_klee_version(metadata.kleeVersion);
  hdr->set_root_id(0);
  hdr->set_total_nodes(totalNodes);
  hdr->set_chunk_size(chunkSize);
  hdr->set_directory_size(dirSize);
  hdr->set_values_size(valuesBlob.size());
  hdr->set_string_table_size(stringTableBlob.size());

  // Header offsets feed back into the header's own serialised size via varint
  // encoding; iterate until size is stable (1–2 iterations on real inputs).
  std::string finalHeaderBlob;
  uint64_t lastSize = 0;
  bool converged = false;
  for (int iter = 0; iter < 8; ++iter) {
    std::string tmp;
    file.SerializeToString(&tmp);
    uint64_t sz = tmp.size();

    uint64_t dirOffset = kPreambleSize + sz;
    uint64_t valOffset = dirOffset + dirSize;
    uint64_t strOffset = valOffset + valuesBlob.size();
    hdr->set_directory_offset(dirOffset);
    hdr->set_values_offset(valOffset);
    hdr->set_string_table_offset(strOffset);

    if (sz == lastSize) {
      finalHeaderBlob = std::move(tmp);
      converged = true;
      break;
    }
    lastSize = sz;
    finalHeaderBlob = std::move(tmp);
  }
  if (!converged) {
    // A non-converged header would point at the wrong byte offsets — refuse
    // to write rather than emit a silently-corrupt file.
    klee_warning("MapOfSetsDiskBuilder: header offset fixed-point did not "
                 "converge; aborting write to '%s'", filename.c_str());
    return;
  }

  uint64_t headerSize = finalHeaderBlob.size();
  uint64_t dirOffset  = kPreambleSize + headerSize;
  uint64_t valOffset  = dirOffset + dirSize;
  uint64_t strOffset  = valOffset + valuesBlob.size();
  uint64_t chunkStart = strOffset + stringTableBlob.size();

  // Atomic publish via temp+rename (POSIX rename is atomic).
  std::string tmpPath = filename + ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out) {
      klee_warning("MapOfSetsDiskBuilder: cannot create '%s'", tmpPath.c_str());
      return;
    }

    out.write(reinterpret_cast<const char *>(&kRawMagic), sizeof(uint64_t));
    out.write(reinterpret_cast<const char *>(&headerSize), sizeof(uint64_t));
    out.write(finalHeaderBlob.data(), finalHeaderBlob.size());

    uint64_t curChunkOffset = chunkStart;
    for (const auto &buf : chunkBuffers) {
      out.write(reinterpret_cast<const char *>(&curChunkOffset), sizeof(uint64_t));
      uint32_t sz = static_cast<uint32_t>(buf.size());
      out.write(reinterpret_cast<const char *>(&sz), sizeof(uint32_t));
      curChunkOffset += sz;
    }

    out.write(valuesBlob.data(), valuesBlob.size());
    out.write(stringTableBlob.data(), stringTableBlob.size());
    for (const auto &buf : chunkBuffers)
      out.write(buf.data(), buf.size());

    if (out.fail()) {
      klee_warning("MapOfSetsDiskBuilder: write error for '%s' (disk full?)",
                   tmpPath.c_str());
      out.close();
      std::remove(tmpPath.c_str());
      return;
    }
  }

  if (std::rename(tmpPath.c_str(), filename.c_str()) != 0) {
    klee_warning("MapOfSetsDiskBuilder: rename '%s' -> '%s' failed",
                 tmpPath.c_str(), filename.c_str());
    std::remove(tmpPath.c_str());
  }
}
