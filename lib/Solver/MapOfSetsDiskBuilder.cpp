// MapOfSetsDiskBuilder.cpp

#include "MapOfSetsDiskBuilder.h"
#include <queue>
#include <cstring>

using namespace klee::mapofsets;

// Internal representation of a node during building
struct BuildNode {
  uint32_t id;
  bool isEndOfSet;
  V value;
  std::vector<std::pair<K, uint32_t>> children; // key → child_id
};

namespace {

// DFS over in-memory UBTree assigning dense IDs and capturing structure.
// We assume access to MapOfSets<K,V>::Node; if needed, make this a friend.
template <class K, class V>
void dfsAssign(const typename MapOfSets<K,V>::Node *src,
               std::vector<BuildNode> &outNodes) {
  // ID will be index in outNodes
  uint32_t id = outNodes.size();
  outNodes.push_back(BuildNode{});
  BuildNode &bn = outNodes.back();
  bn.id = id;
  bn.isEndOfSet = src->isEndOfSet;
  if (src->isEndOfSet)
    bn.value = src->value;

  // Children: assign IDs recursively, but we need to ensure each source node
  // is visited once. For UBTree (tree, no sharing), this simple DFS is fine.
  for (auto it = src->children.begin(), ie = src->children.end(); it != ie; ++it) {
    const K &key = it->first;
    const auto *childSrc = &it->second;
    uint32_t childId = outNodes.size();
    dfsAssign<K,V>(childSrc, outNodes);
    bn.children.emplace_back(key, childId);
  }
}

} // anonymous namespace

void MapOfSetsDiskBuilder::build(const UBTree &tree,
                                 const std::string &filename,
                                 uint32_t chunkSize) {
  // 1. Extract UBTree nodes into a flat vector<BuildNode>
  std::vector<BuildNode> nodes;
  nodes.reserve(1024);
  // Access the root; if MapOfSets::root is private, add a getter or friend.
  const auto *root = &tree.root; // adjust if needed
  dfsAssign<K,V>(root, nodes);

  uint32_t totalNodes = static_cast<uint32_t>(nodes.size());

  // 2. Prepare protobuf MapOfSetsFile
  MapOfSetsFile file;
  Header *hdr = file.mutable_header();
  hdr->set_magic(0x4d41504f53455453ULL); // "MAPOSETS"
  hdr->set_version(1);
  hdr->set_root_id(0);
  hdr->set_total_nodes(totalNodes);
  hdr->set_chunk_size(chunkSize);

  // 3. Build values_blob: each V = std::string, length-prefixed with uint32
  std::string &valuesBlob = *file.mutable_values_blob();
  valuesBlob.clear();
  valuesBlob.reserve(totalNodes * 8);

  // Map node_id -> value_offset
  std::vector<uint64_t> valueOffsets(totalNodes, 0);

  for (const auto &bn : nodes) {
    if (!bn.isEndOfSet)
      continue;
    const V &val = bn.value;
    uint64_t offset = valuesBlob.size();
    uint32_t len = static_cast<uint32_t>(val.size());
    valuesBlob.append(reinterpret_cast<const char *>(&len), sizeof(len));
    valuesBlob.append(val.data(), val.size());
    valueOffsets[bn.id] = offset;
  }

  hdr->set_values_blob_size(valuesBlob.size());

  // 4. Lay out chunks and directory in the output file
  // We write in this order:
  //   [Header + values_blob serialized by protobuf]
  //   [Directory: (offset,size) pairs for chunks]
  //   [Chunks: NodeChunk messages]
  //
  // So we need to:
  //   a) Compute nodes -> chunks grouping
  //   b) Serialize header+values_blob to an in-memory buffer to know its size
  //   c) Compute directory offsets and chunk offsets
  //   d) Write everything to disk in one pass

  // a) Group nodes into chunks
  uint32_t numChunks = (totalNodes + chunkSize - 1) / chunkSize;

  struct DirEntry {
    uint64_t offset;
    uint32_t size;
  };
  std::vector<DirEntry> directory(numChunks);

  // b) Serialize header+values_blob to a buffer
  std::string headerAndBlob;
  file.SerializeToString(&headerAndBlob);
  uint64_t headerAndBlobSize = headerAndBlob.size();

  // c) Compute directory and chunk offsets
  uint64_t directoryOffset = headerAndBlobSize;
  uint64_t directorySize = numChunks * (sizeof(uint64_t) + sizeof(uint32_t)); // 12 bytes/entry
  hdr->set_directory_offset(directoryOffset);
  hdr->set_directory_size(directorySize);

  // Chunk contents are protobuf NodeChunk messages; we don't yet know the exact
  // byte size of each, so we will serialize them to temporary buffers and then
  // fill the directory.
  std::vector<std::string> chunkBuffers(numChunks);
  uint64_t chunkBaseOffset = directoryOffset + directorySize;

  // Build each chunk's NodeChunk protobuf and buffer
  for (uint32_t c = 0; c < numChunks; ++c) {
    uint32_t first = c * chunkSize;
    uint32_t count = std::min(chunkSize, totalNodes - first);

    NodeChunk chunkMsg;
    chunkMsg.set_first_node_id(first);
    chunkMsg.set_node_count(count);

    for (uint32_t i = 0; i < count; ++i) {
      const BuildNode &bn = nodes[first + i];
      Node *n = chunkMsg.add_nodes();
      n->set_is_end_of_set(bn.isEndOfSet);
      n->set_value_offset(valueOffsets[bn.id]);

      // children
      for (const auto &kv : bn.children) {
        Child *ch = n->add_children();
        ch->set_key(kv.first);         // serialize K=string directly
        ch->set_child_id(kv.second);   // child node_id
      }
    }

    std::string buf;
    chunkMsg.SerializeToString(&buf);
    chunkBuffers[c] = std::move(buf);
  }

  // Now fill directory entries with offsets and sizes
  uint64_t currentChunkOffset = chunkBaseOffset;
  for (uint32_t c = 0; c < numChunks; ++c) {
    directory[c].offset = currentChunkOffset;
    directory[c].size = static_cast<uint32_t>(chunkBuffers[c].size());
    currentChunkOffset += chunkBuffers[c].size();
  }

  // d) Rewrite header with correct directory info, then write full file
  file.mutable_header()->CopyFrom(*hdr);
  file.set_values_blob(valuesBlob); // ensure in sync

  // Re-serialize header+blob (header changed directory_* fields)
  headerAndBlob.clear();
  file.SerializeToString(&headerAndBlob);
  headerAndBlobSize = headerAndBlob.size();
  // directoryOffset may have shifted slightly if header size changed,
  // but DiskMapOfSets only uses header.directory_offset and header.directory_size;
  // chunks' offsets are absolute from file start, so that's OK.

  std::ofstream out(filename, std::ios::binary);
  if (!out)
    throw std::runtime_error("Failed to open output file for MapOfSets");

  // write header+values_blob protobuf
  out.write(headerAndBlob.data(), headerAndBlob.size());

  // write directory (offset,size) entries
  for (const auto &de : directory) {
    out.write(reinterpret_cast<const char *>(&de.offset), sizeof(de.offset));
    out.write(reinterpret_cast<const char *>(&de.size), sizeof(de.size));
  }

  // write chunk buffers
  for (const auto &buf : chunkBuffers) {
    out.write(buf.data(), buf.size());
  }

  out.close();
}
