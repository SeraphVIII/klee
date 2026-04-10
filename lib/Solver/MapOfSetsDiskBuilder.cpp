#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/Support/ErrorHandling.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

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
  // -------------------------------------------------------------------------
  // On-disk layout (v2):
  //   [u64 magic][u64 header_size][Header proto bytes]
  //   [directory: numChunks * 12 bytes]
  //   [values blob]
  //   [string table blob]
  //   [chunk protobufs...]
  //
  // String table: [u32 count]([u32 len][bytes])×count  (sorted strings).
  // Child.key_index is an index into this table.
  // Values blob entries: [u32 len][bytes]; len=0 means UNSAT.
  // -------------------------------------------------------------------------
  static constexpr uint64_t kRawMagic = 0x4d41504f53455453ULL; // "MAPOSETS"
  const uint64_t kPreambleSize = 16; // magic + header_size

  // 1. Extract nodes in stable DFS order.
  std::vector<BuildNode> nodes;
  nodes.reserve(1024);
  dfsAssign(&tree.root, nodes);
  uint32_t totalNodes = static_cast<uint32_t>(nodes.size());

  // 2. Build string table: collect all unique edge keys, sort, assign indices.
  std::set<std::string> uniqueKeys;
  for (const auto &bn : nodes)
    for (const auto &kv : bn.children)
      uniqueKeys.insert(kv.first);

  // std::set iteration is sorted, so this vector is sorted.
  std::vector<std::string> strings(uniqueKeys.begin(), uniqueKeys.end());

  std::map<std::string, uint32_t> stringIndex;
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

  // 3. Build values blob and record per-node offsets.
  //    Stored as offset+1 so that 0 means "no value".
  //    An empty value (len=0) represents UNSAT.
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

  // 4. Compute directory size (fixed regardless of header size).
  uint32_t numChunks = (totalNodes + chunkSize - 1) / chunkSize;
  uint64_t dirSize = numChunks * 12ULL; // uint64 offset + uint32 size per entry

  // 5. Build chunk protobufs using key_index instead of raw key bytes.
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

  // 6. Build the header protobuf. All offsets depend on the serialised header
  //    size (varint encoding), so converge with a fixed-point loop.
  mapofsets::MapOfSetsFile file;
  mapofsets::Header *hdr = file.mutable_header();
  hdr->set_magic(kRawMagic);
  hdr->set_version(2);
  // Increment kCanonVersion whenever the canonicalization algorithm or key
  // serialization format changes so that readers can reject stale cache files.
  static constexpr uint32_t kCanonVersion = 1;
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

  // The serialized header size affects its own field offsets (protobuf uses
  // varint encoding, so a larger offset value can grow the blob).  We iterate
  // until the serialized size stabilises — in practice this converges in 1-2
  // iterations because varint growth is bounded and the offsets only grow once.
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
  if (!converged)
    klee_warning("MapOfSetsDiskBuilder: header offset fixed-point did not "
                 "converge; output file '%s' may be corrupt", filename.c_str());

  uint64_t headerSize = finalHeaderBlob.size();
  uint64_t dirOffset  = kPreambleSize + headerSize;
  uint64_t valOffset  = dirOffset + dirSize;
  uint64_t strOffset  = valOffset + valuesBlob.size();
  uint64_t chunkStart = strOffset + stringTableBlob.size();

  // 7. Write file atomically: write to a temp path, then rename into place.
  // rename() is atomic on POSIX — the destination is never partially written.
  std::string tmpPath = filename + ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out) {
      klee_warning("MapOfSetsDiskBuilder: cannot create '%s'", tmpPath.c_str());
      return;
    }

    // Preamble
    out.write(reinterpret_cast<const char *>(&kRawMagic), sizeof(uint64_t));
    out.write(reinterpret_cast<const char *>(&headerSize), sizeof(uint64_t));
    // Header protobuf
    out.write(finalHeaderBlob.data(), finalHeaderBlob.size());
    // Directory: (u64 offset, u32 size) per chunk
    uint64_t curChunkOffset = chunkStart;
    for (const auto &buf : chunkBuffers) {
      out.write(reinterpret_cast<const char *>(&curChunkOffset), sizeof(uint64_t));
      uint32_t sz = static_cast<uint32_t>(buf.size());
      out.write(reinterpret_cast<const char *>(&sz), sizeof(uint32_t));
      curChunkOffset += sz;
    }
    // Values blob
    out.write(valuesBlob.data(), valuesBlob.size());
    // String table
    out.write(stringTableBlob.data(), stringTableBlob.size());
    // Chunk protobufs
    for (const auto &buf : chunkBuffers)
      out.write(buf.data(), buf.size());

    if (out.fail()) {
      klee_warning("MapOfSetsDiskBuilder: write error for '%s' (disk full?)",
                   tmpPath.c_str());
      out.close();
      std::remove(tmpPath.c_str());
      return;
    }
  } // ofstream flushed and closed by destructor

  if (std::rename(tmpPath.c_str(), filename.c_str()) != 0) {
    klee_warning("MapOfSetsDiskBuilder: rename '%s' -> '%s' failed",
                 tmpPath.c_str(), filename.c_str());
    std::remove(tmpPath.c_str());
  }
}
