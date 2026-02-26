#ifndef DISK_MAPOFSETS_H
#define DISK_MAPOFSETS_H

#include "mapofsets.pb.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>
#include <unordered_map>
#include <vector>
#include <set>
#include <optional>

namespace klee { namespace mapofsets {

class DiskMapOfSets {
public:
  struct Entry {
    std::set<std::string> key_set;  // for K=string
    std::string value;              // for V=string
  };

  DiskMapOfSets(const std::string& filename);
  ~DiskMapOfSets();

  // Exact lookup
  std::optional<std::string> lookup(const std::set<std::string>& query_set);
  
  // All subsets of query_set
  std::vector<Entry> subsets(const std::set<std::string>& query_set);
  
  // All supersets of query_set  
  std::vector<Entry> supersets(const std::set<std::string>& query_set);

private:
  int fd_;
  void* mmap_base_;
  size_t file_size_;
  MapOfSetsFile header_file_;
  
  struct Chunk {
    uint64_t offset;
    uint32_t size;
    NodeChunk* parsed;
  };
  std::unordered_map<uint32_t, Chunk> chunk_cache_; // chunk_id -> Chunk
  size_t max_cache_size_ = 100; // chunks

  uint32_t chunk_id(uint32_t node_id) const { return node_id / header_file_.header().chunk_size(); }
  uint32_t local_id(uint32_t node_id) const { return node_id % header_file_.header().chunk_size(); }
  
  NodeChunk* get_chunk(uint32_t chunk_id);
  const Node& get_node(uint32_t node_id);
  std::string deserialize_key(const std::string& key_bytes) const;
  std::string read_value(uint64_t offset) const;
  
  std::optional<std::string> lookup_rec(uint32_t node_id, 
                                        std::set<std::string>::const_iterator q_begin,
                                        std::set<std::string>::const_iterator q_end);
  
  void find_subsets(uint32_t node_id, std::set<std::string> accum,
                    std::set<std::string>::const_iterator q_begin,
                    std::set<std::string>::const_iterator q_end,
                    std::vector<Entry>& results);
  
  void find_supersets(uint32_t node_id, std::set<std::string> accum,
                      std::set<std::string>::const_iterator q_begin,
                      std::set<std::string>::const_iterator q_end,
                      std::vector<Entry>& results);
};

}} // namespace klee::mapofsets

#endif
