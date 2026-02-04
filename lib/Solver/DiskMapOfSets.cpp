#include "DiskMapOfSets.h"
#include <algorithm>
#include <cstring>

using namespace klee::mapofsets;

DiskMapOfSets::DiskMapOfSets(const std::string& filename) 
  : fd_(-1), mmap_base_(nullptr), file_size_(0) {
  fd_ = open(filename.c_str(), O_RDONLY);
  if (fd_ < 0) throw std::runtime_error("Cannot open file");

  struct stat st;
  fstat(fd_, &st);
  file_size_ = st.st_size;
  
  mmap_base_ = mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (mmap_base_ == MAP_FAILED) throw std::runtime_error("mmap failed");

  // Parse header + values_blob (small, always in memory)
  google::protobuf::io::ArrayInputStream stream(
    static_cast<const char*>(mmap_base_), file_size_);
  header_file_.ParseFromZeroCopyStream(&stream);
  
  if (header_file_.header().magic() != 0x4d41504f53455453ULL)
    throw std::runtime_error("Invalid magic");
}

DiskMapOfSets::~DiskMapOfSets() {
  if (mmap_base_) munmap(mmap_base_, file_size_);
  if (fd_ >= 0) close(fd_);
  // Chunks auto-destruct via map
}

NodeChunk* DiskMapOfSets::get_chunk(uint32_t chunk_id) {
  auto it = chunk_cache_.find(chunk_id);
  if (it != chunk_cache_.end()) return it->second.parsed;

  if (chunk_cache_.size() >= max_cache_size_) {
    // Simple LRU: erase first
    chunk_cache_.erase(chunk_cache_.begin());
  }

  // Directory is after header+blob, simple array format
  uint64_t dir_off = header_file_.header().directory_offset();
  uint32_t chunk_size = header_file_.header().chunk_size();
  uint64_t chunk_entry_off = dir_off + chunk_id * 12; // 8+4 bytes per entry
  
  Chunk chunk;
  memcpy(&chunk.offset, static_cast<char*>(mmap_base_) + chunk_entry_off, 8);
  memcpy(&chunk.size, static_cast<char*>(mmap_base_) + chunk_entry_off + 8, 4);
  
  chunk.parsed = new NodeChunk();
  google::protobuf::io::ArrayInputStream chunk_stream(
    static_cast<const char*>(mmap_base_) + chunk.offset, chunk.size);
  chunk.parsed->ParseFromZeroCopyStream(&chunk_stream);
  
  chunk_cache_[chunk_id] = std::move(chunk);
  return chunk_cache_[chunk_id].parsed;
}

const Node& DiskMapOfSets::get_node(uint32_t node_id) {
  uint32_t cid = chunk_id(node_id);
  uint32_t lid = local_id(node_id);
  NodeChunk* chunk = get_chunk(cid);
  if (lid >= chunk->nodes_size()) 
    throw std::runtime_error("Node ID out of chunk bounds");
  return chunk->nodes(lid);
}

std::string DiskMapOfSets::deserialize_key(const std::string& key_bytes) const {
  return key_bytes;  // Simple string K; extend for varint/etc.
}

std::string DiskMapOfSets::read_value(uint64_t offset) const {
  if (offset == 0 || offset >= header_file_.values_blob().size()) 
    return "";
  
  const char* blob = header_file_.values_blob().data() + offset;
  uint32_t len;
  memcpy(&len, blob, 4);  // Assume 4-byte length prefix
  return std::string(blob + 4, len);
}

std::optional<std::string> DiskMapOfSets::lookup(const std::set<std::string>& query_set) {
  return lookup_rec(header_file_.header().root_id(), query_set.begin(), query_set.end());
}

std::optional<std::string> DiskMapOfSets::lookup_rec(uint32_t node_id,
                                                     std::set<std::string>::const_iterator q_begin,
                                                     std::set<std::string>::const_iterator q_end) {
  if (q_begin == q_end) {
    const auto& node = get_node(node_id);
    if (node.is_end_of_set()) {
      return read_value(node.value_offset());
    }
    return std::nullopt;
  }
  
  const auto& node = get_node(node_id);
  const std::string& target = *q_begin;
  
  // Binary search children
  auto it = std::lower_bound(node.children().begin(), node.children().end(), target,
    [](const Child& child, const std::string& t) {
      return deserialize_key(child.key()) < t;
    });
  
  if (it == node.children().end() || deserialize_key(it->key()) != target) {
    return std::nullopt;
  }
  
  return lookup_rec(it->child_id(), std::next(q_begin), q_end);
}

std::vector<Entry> DiskMapOfSets::subsets(const std::set<std::string>& query_set) {
  std::vector<Entry> results;
  std::set<std::string> accum;
  find_subsets(header_file_.header().root_id(), accum, 
               query_set.begin(), query_set.end(), results);
  return results;
}

void DiskMapOfSets::find_subsets(uint32_t node_id, std::set<std::string> accum,
                                 std::set<std::string>::const_iterator q_begin,
                                 std::set<std::string>::const_iterator q_end,
                                 std::vector<Entry>& results) {
  const auto& node = get_node(node_id);
  if (node.is_end_of_set()) {
    Entry e{accum, read_value(node.value_offset())};
    results.push_back(e);
  }
  
  for (auto q_it = q_begin; q_it != q_end; ++q_it) {
    const std::string& elt = *q_it;
    const auto& node = get_node(node_id);
    
    auto child_it = std::lower_bound(node.children().begin(), node.children().end(), elt,
      [](const Child& c, const std::string& t){ return deserialize_key(c.key()) < t; });
    
    if (child_it != node.children().end() && deserialize_key(child_it->key()) == elt) {
      accum.insert(elt);
      find_subsets(child_it->child_id(), std::move(accum), std::next(q_it), q_end, results);
      accum.erase(--accum.end());  // backtrack
    }
  }
}

std::vector<Entry> DiskMapOfSets::supersets(const std::set<std::string>& query_set) {
  std::vector<Entry> results;
  std::set<std::string> accum;
  find_supersets(header_file_.header().root_id(), std::move(accum), 
                 query_set.begin(), query_set.end(), results);
  return results;
}

void DiskMapOfSets::find_supersets(uint32_t node_id, std::set<std::string> accum,
                                   std::set<std::string>::const_iterator q_begin,
                                   std::set<std::string>::const_iterator q_end,
                                   std::vector<Entry>& results) {
  const auto& node = get_node(node_id);
  
  if (q_begin == q_end) {
    // All query elements matched; any end-of-set here or deeper is a superset
    if (node.is_end_of_set()) {
      Entry e{accum, read_value(node.value_offset())};
      results.push_back(e);
    }
    // Continue into all children (adding extra elements makes bigger supersets)
    for (const auto& child : node.children()) {
      accum.insert(deserialize_key(child.key()));
      find_supersets(child.child_id(), std::move(accum), q_begin, q_end, results);
      accum.erase(std::prev(accum.end()));  // backtrack
    }
  } else {
    // Still need to match remaining query elements
    const std::string& elt = *q_begin;
    auto next_q = std::next(q_begin);
    
    // Scan all children in order (they're sorted)
    for (const auto& child : node.children()) {
      const std::string child_key = deserialize_key(child.key());
      accum.insert(child_key);
      
      if (child_key == elt) {
        // Exact match: recurse with next query element
        find_supersets(child.child_id(), std::move(accum), next_q, q_end, results);
      } else if (child_key < elt) {
        // Extra element before required elt: recurse still needing elt
        find_supersets(child.child_id(), std::move(accum), q_begin, q_end, results);
      } else {
        // child_key > elt: cannot match elt in this subtree, skip rest
        break;
      }
      
      accum.erase(std::prev(accum.end()));  // backtrack
    }
  }
}
