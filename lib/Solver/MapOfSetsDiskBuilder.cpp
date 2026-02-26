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
  // 1. Extract nodes in a stable order (DFS assigns IDs)
  std::vector<BuildNode> nodes;
  nodes.reserve(1024);
  const auto *root = &tree.root;
  dfsAssign(root, nodes);
  uint32_t totalNodes = static_cast<uint32_t>(nodes.size());

  klee_message("BUILDER: %u nodes\n", totalNodes);

  // 2. Build values blob and record per-node offsets.
  //    We store offset+1 so 0 can mean "no value" (proto convention).
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
  klee_message("BUILDER: valuesBlob=%zu bytes\n", valuesBlob.size());

  // 3. Build the header protobuf (MapOfSetsFile)
  mapofsets::MapOfSetsFile file;
  mapofsets::Header *hdr = file.mutable_header();
  hdr->set_magic(0x4d41504f53455453ULL); // "MAPOSETS" (also stored in raw preamble)
  hdr->set_version(1);
  hdr->set_root_id(0);
  hdr->set_total_nodes(totalNodes);
  hdr->set_chunk_size(chunkSize);
  hdr->set_values_blob_size(valuesBlob.size());

  // values_blob must be set before serializing the header protobuf
  *file.mutable_values_blob() = std::move(valuesBlob);

  // 4. Directory sizing
  uint32_t numChunks = (totalNodes + chunkSize - 1) / chunkSize;
  uint64_t dirSize = numChunks * 12ULL; // uint64 offset + uint32 size
  hdr->set_directory_size(dirSize);

  // -------------------------------------------------------------------------
  // On-disk layout:
  //   [u64 magic][u64 header_size][header protobuf bytes][directory][chunks...]
  // The reader parses ONLY the header protobuf region.
  // -------------------------------------------------------------------------
  static constexpr uint64_t kRawMagic = 0x4d41504f53455453ULL; // "MAPOSETS"
  const uint64_t kPreambleSize = 16;

  // directory_offset depends on the serialized header size, which itself can
  // change when directory_offset changes (varint length). Compute fixed point.
  std::string finalHeaderBlob;
  uint64_t lastSize = 0;
  for (int it = 0; it < 6; ++it) {
    std::string tmp;
    file.SerializeToString(&tmp);
    uint64_t sz = tmp.size();

    hdr->set_directory_offset(kPreambleSize + sz);

    if (sz == lastSize) {
      finalHeaderBlob = std::move(tmp);
      break;
    }
    lastSize = sz;
    finalHeaderBlob = std::move(tmp);
  }

  uint64_t headerSize = finalHeaderBlob.size();
  uint64_t dirOffset = kPreambleSize + headerSize;

  // 5. Build chunk protobufs
  uint64_t chunkOffset = dirOffset + dirSize;
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

    std::string buf;
    chunkMsg.SerializeToString(&buf);
    chunkBuffers[c] = std::move(buf);
  }

  // 6. Write file
  std::ofstream out(filename, std::ios::binary);
  if (!out) {
    klee_message("BUILDER: cannot create %s\n", filename.c_str());
    return;
  }

  // Raw preamble: magic + header_size
  out.write(reinterpret_cast<const char *>(&kRawMagic), sizeof(uint64_t));
  out.write(reinterpret_cast<const char *>(&headerSize), sizeof(uint64_t));

  // Header protobuf (MapOfSetsFile)
  out.write(finalHeaderBlob.data(), finalHeaderBlob.size());

  // Directory: entries are (u64 offset, u32 size) for each chunk
  uint64_t curOffset = chunkOffset;
  for (const auto &buf : chunkBuffers) {
    out.write(reinterpret_cast<const char *>(&curOffset), sizeof(uint64_t));
    uint32_t sz = static_cast<uint32_t>(buf.size());
    out.write(reinterpret_cast<const char *>(&sz), sizeof(uint32_t));
    curOffset += sz;
  }

  // Chunk protobufs
  for (const auto &buf : chunkBuffers) {
    out.write(buf.data(), buf.size());
  }

  klee_message("BUILDER: wrote %zu bytes OK\n", (size_t)curOffset);
}
