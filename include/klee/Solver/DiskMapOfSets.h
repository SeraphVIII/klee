#ifndef DISK_MAPOFSETS_H
#define DISK_MAPOFSETS_H

#include "mapofsets.pb.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>
#include <functional>
#include <list>
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

  explicit DiskMapOfSets(const std::string& filename,
                         size_t max_cache_size = 100);
  ~DiskMapOfSets();

  /// Returns false if the file could not be opened or parsed.
  bool isValid() const { return valid_; }

  /// Metadata stored in the file header (empty strings if not present).
  std::string solverBackend() const { return header_file_.header().solver_backend(); }
  std::string kleeVersion()   const { return header_file_.header().klee_version(); }

  // Exact lookup
  std::optional<std::string> lookup(const std::set<std::string>& query_set);
  
  // Values of all subsets of query_set (key sets not returned; callers only
  // need the stored values, avoiding the cost of rebuilding key sets).
  std::vector<std::string> subsets(const std::set<std::string>& query_set);

  // Values of all supersets of query_set (same rationale as subsets above).
  std::vector<std::string> supersets(const std::set<std::string>& query_set);

  // Stream every entry in the trie without materialising them all at once.
  // The callback receives (key_set, value) for each stored entry.
  // Used for merge-on-write; avoids a peak allocation proportional to cache size.
  void forEach(std::function<void(const std::set<std::string>&, const std::string&)> cb);

  // All entries in the trie (full traversal). Used for merge-on-write.
  std::vector<Entry> allEntries();

private:
  bool valid_ = false;
  int fd_ = -1;
  void* mmap_base_ = nullptr;
  size_t file_size_ = 0;
  MapOfSetsFile header_file_;
  std::vector<std::string> string_table_; // loaded once at open; indexed by key_index
  
  struct Chunk {
    uint64_t offset;
    uint32_t size;
    std::unique_ptr<NodeChunk> parsed;
    std::list<uint32_t>::iterator lru_it; // position in lru_order_
  };
  std::unordered_map<uint32_t, Chunk> chunk_cache_; // chunk_id -> Chunk
  std::list<uint32_t> lru_order_; // front = most recently used
  size_t max_cache_size_;

  uint32_t chunk_id(uint32_t node_id) const { return node_id / header_file_.header().chunk_size(); }
  uint32_t local_id(uint32_t node_id) const { return node_id % header_file_.header().chunk_size(); }
  
  NodeChunk* get_chunk(uint32_t chunk_id);
  const Node& get_node(uint32_t node_id);
  const std::string& key_str(uint32_t index) const; // look up string table
  std::string read_value(uint64_t offset) const;
  
  std::optional<std::string> lookup_rec(uint32_t node_id, 
                                        std::set<std::string>::const_iterator q_begin,
                                        std::set<std::string>::const_iterator q_end);
  
  // Query traversals: no accum — callers only need values, not key sets.
  void find_subsets(uint32_t node_id,
                    std::set<std::string>::const_iterator q_begin,
                    std::set<std::string>::const_iterator q_end,
                    std::vector<std::string>& results);

  void find_supersets(uint32_t node_id,
                      std::set<std::string>::const_iterator q_begin,
                      std::set<std::string>::const_iterator q_end,
                      std::vector<std::string>& results);

  // Full traversal with callback; accum tracks the current path for key_set.
  void enumerate_all(uint32_t node_id, std::set<std::string> &accum,
                     const std::function<void(const std::set<std::string>&,
                                              const std::string&)> &cb);
};

}} // namespace klee::mapofsets

#endif
