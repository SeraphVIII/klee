#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include "klee/Support/ErrorHandling.h"
#include <algorithm>
#include <fstream>

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
                                 uint32_t chunkSize) {
  // -------------------------------------------------------------------------
  // On-disk layout:
  //   [u64 magic][u64 header_size][Header proto bytes]
  //   [directory: numChunks * 12 bytes]
  //   [values blob]
  //   [chunk protobufs...]
  //
  // All offsets recorded in Header are byte positions from the start of file.
  // -------------------------------------------------------------------------
  static constexpr uint64_t kRawMagic = 0x4d41504f53455453ULL; // "MAPOSETS"
  const uint64_t kPreambleSize = 16; // magic + header_size

  // 1. Extract nodes in stable DFS order.
  std::vector<BuildNode> nodes;
  nodes.reserve(1024);
  dfsAssign(&tree.root, nodes);
  uint32_t totalNodes = static_cast<uint32_t>(nodes.size());


  // 2. Build values blob and record per-node offsets.
  //    Stored as offset+1 so 0 means "no value".
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

  // 3. Compute directory size (fixed regardless of header size).
  uint32_t numChunks = (totalNodes + chunkSize - 1) / chunkSize;
  uint64_t dirSize = numChunks * 12ULL; // uint64 offset + uint32 size per entry

  // 4. Build the header protobuf. directory_offset and values_offset both
  //    depend on the serialized header size, which depends on those fields
  //    (varint encoding). Converge with a fixed-point loop.
  mapofsets::MapOfSetsFile file;
  mapofsets::Header *hdr = file.mutable_header();
  hdr->set_magic(0x4d41504f53455453ULL);
  hdr->set_version(1);
  hdr->set_root_id(0);
  hdr->set_total_nodes(totalNodes);
  hdr->set_chunk_size(chunkSize);
  hdr->set_directory_size(dirSize);
  hdr->set_values_size(valuesBlob.size());

  std::string finalHeaderBlob;
  uint64_t lastSize = 0;
  for (int iter = 0; iter < 8; ++iter) {
    std::string tmp;
    file.SerializeToString(&tmp);
    uint64_t sz = tmp.size();

    uint64_t dirOffset  = kPreambleSize + sz;
    uint64_t valOffset  = dirOffset + dirSize;
    hdr->set_directory_offset(dirOffset);
    hdr->set_values_offset(valOffset);

    if (sz == lastSize) {
      finalHeaderBlob = std::move(tmp);
      break;
    }
    lastSize = sz;
    finalHeaderBlob = std::move(tmp);
  }

  uint64_t headerSize  = finalHeaderBlob.size();
  uint64_t dirOffset   = kPreambleSize + headerSize;
  uint64_t valOffset   = dirOffset + dirSize;
  uint64_t chunkStart  = valOffset + valuesBlob.size();

  // 5. Build chunk protobufs.
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
        ch->set_key(kv.first);
        ch->set_child_id(kv.second);
      }
    }

    chunkMsg.SerializeToString(&chunkBuffers[c]);
  }

  // 6. Write file.
  std::ofstream out(filename, std::ios::binary);
  if (!out) {
    klee_warning("MapOfSetsDiskBuilder: cannot create '%s'", filename.c_str());
    return;
  }

  // Preamble: raw magic + header_size
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

  // Values blob (raw bytes, accessed via mmap in the reader)
  out.write(valuesBlob.data(), valuesBlob.size());

  // Chunk protobufs
  for (const auto &buf : chunkBuffers)
    out.write(buf.data(), buf.size());

}
