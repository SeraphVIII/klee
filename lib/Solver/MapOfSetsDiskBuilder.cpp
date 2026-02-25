#include "klee/Solver/MapOfSetsDiskBuilder.h"
#include <algorithm>
#include <fstream>

using namespace klee;

void MapOfSetsDiskBuilder::dfsAssign(const MapOfSetsDiskBuilder::UBTree::Node *src,
               std::vector<BuildNode> &outNodes) {
  uint32_t id = outNodes.size();
  outNodes.emplace_back();
  BuildNode &bn = outNodes.back();
  bn.id = id;
  bn.isEndOfSet = src->isEndOfSet;
  if (src->isEndOfSet)
    bn.value = src->value;

  for (const auto& kv : src->children) {
    const MapOfSetsDiskBuilder::K &key = kv.first;
    const auto *childSrc = &kv.second;
    uint32_t childId = outNodes.size();
    dfsAssign(childSrc, outNodes);
    bn.children.emplace_back(key, childId);
  }
}

void MapOfSetsDiskBuilder::build(const UBTree &tree,
                                 const std::string &filename,
                                 uint32_t chunkSize) {
  // 1. Extract nodes
  std::vector<BuildNode> nodes;
  nodes.reserve(1024);
  const auto *root = &tree.root;
  dfsAssign(root, nodes);

  uint32_t totalNodes = static_cast<uint32_t>(nodes.size());

  // 2. Header
  mapofsets::MapOfSetsFile file;
  mapofsets::Header *hdr = file.mutable_header();
  hdr->set_magic(0x4d41504f53455453ULL);
  hdr->set_version(1);
  hdr->set_root_id(0);
  hdr->set_total_nodes(totalNodes);
  hdr->set_chunk_size(chunkSize);

  // 3. values_blob
  std::string &valuesBlob = *file.mutable_values_blob();
  std::vector<uint64_t> valueOffsets(totalNodes, 0);

  for (const auto &bn : nodes) {
    if (!bn.isEndOfSet) continue;
    uint64_t offset = valuesBlob.size();
    uint32_t len = static_cast<uint32_t>(bn.value.size());
    valuesBlob.append(reinterpret_cast<const char*>(&len), sizeof(len));
    valuesBlob.append(bn.value.data(), bn.value.size());
    valueOffsets[bn.id] = offset;
  }
  hdr->set_values_blob_size(valuesBlob.size());

  // 4. Serialize header to measure size
  std::string headerAndBlob;
  file.SerializeToString(&headerAndBlob);
  uint64_t headerSize = headerAndBlob.size();

  // 5. Directory + chunks
  uint32_t numChunks = (totalNodes + chunkSize - 1) / chunkSize;
  uint64_t dirOffset = headerSize;
  uint64_t dirSize = numChunks * 12; // uint64 + uint32
  hdr->set_directory_offset(dirOffset);
  hdr->set_directory_size(dirSize);

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
  out.write(headerAndBlob.data(), headerSize);

  // Directory
  uint64_t curOffset = chunkOffset;
  for (const auto &buf : chunkBuffers) {
    out.write(reinterpret_cast<const char*>(&curOffset), sizeof(uint64_t));
    uint32_t sz = buf.size();
    out.write(reinterpret_cast<const char*>(&sz), sizeof(uint32_t));
    curOffset += sz;
  }

  // Chunks
  for (const auto &buf : chunkBuffers) {
    out.write(buf.data(), buf.size());
  }
}
