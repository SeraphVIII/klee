#ifndef DISK_MAPOFSETS_H
#define DISK_MAPOFSETS_H

#include "mapofsets.pb.h"
#include "llvm/ADT/SmallVector.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>
#include <cstdint>
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
    std::set<std::string> key_set;
    std::string value;
  };

  explicit DiskMapOfSets(const std::string& filename,
                         size_t max_cache_size = 100);
  ~DiskMapOfSets();

  bool isValid() const { return valid_; }

  std::string solverBackend() const { return header_file_.header().solver_backend(); }
  std::string kleeVersion()   const { return header_file_.header().klee_version(); }

  // query_set must be sorted and de-duplicated (the canonical disk key already
  // is — see buildConstraintDiskKey). A vector rather than std::set avoids a
  // per-element RB-tree node allocation on every lookup (audit O4).
  std::optional<std::string> lookup(const std::vector<std::string>& query_set);

  // Values only — callers don't need the matching key sets.
  std::vector<std::string> subsets(const std::vector<std::string>& query_set);
  std::vector<std::string> supersets(const std::vector<std::string>& query_set);

  // Streams (key_set, value) pairs without materialising the full set.
  void forEach(std::function<void(const std::set<std::string>&, const std::string&)> cb);

  std::vector<Entry> allEntries();

private:
  // mutable: const accessors (key_str) clear it on corruption.
  mutable bool valid_ = false;
  int fd_ = -1;
  void* mmap_base_ = nullptr;
  size_t file_size_ = 0;
  MapOfSetsFile header_file_;
  std::vector<std::string> string_table_;

  struct Chunk {
    uint64_t offset;
    uint32_t size;
    std::unique_ptr<NodeChunk> parsed;
    std::list<uint32_t>::iterator lru_it;
  };
  std::unordered_map<uint32_t, Chunk> chunk_cache_;
  std::list<uint32_t> lru_order_; // front = most recently used
  size_t max_cache_size_;

  // Reusable cycle-guard for supersets(): a node is "visited this call" iff
  // supersetVisit_[node] == supersetVisitEpoch_. Bumping the epoch clears the
  // whole guard in O(1); the buffer is sized once to total_nodes (audit P1).
  std::vector<uint32_t> supersetVisit_;
  uint32_t supersetVisitEpoch_ = 0;

  uint32_t chunk_id(uint32_t node_id) const { return node_id / header_file_.header().chunk_size(); }
  uint32_t local_id(uint32_t node_id) const { return node_id % header_file_.header().chunk_size(); }

  NodeChunk* get_chunk(uint32_t chunk_id);
  const Node& get_node(uint32_t node_id);

  // A node's data copied out of its (evictable) chunk. Traversals that recurse
  // must snapshot via get_node_view first: a recursive get_node() can evict the
  // chunk backing a still-live get_node() reference.
  struct ChildRef { uint32_t key_index; uint32_t child_id; };
  struct NodeView {
    bool is_end_of_set = false;
    uint64_t value_offset = 0;
    // Inline storage for the common small-fan-out node so the per-node snapshot
    // (taken for eviction safety during recursion) needs no heap allocation
    // (audit P4); large nodes still spill to the heap.
    llvm::SmallVector<ChildRef, 8> children; // sorted by key, same as on disk
  };
  NodeView get_node_view(uint32_t node_id);

  const std::string& key_str(uint32_t index) const;
  std::string read_value(uint64_t offset) const;

  // Resolve query strings to their string-table indices once per lookup, so
  // the traversal compares small integers instead of (potentially long)
  // canonical-constraint strings at every trie node (audit F6). The string
  // table is sorted, so an index < index comparison reproduces string order;
  // children are stored in that same order. Returns indices in ascending order
  // (absent strings are omitted) and sets allPresent=false if any query string
  // is missing from the table.
  std::vector<uint32_t> resolveQueryIndices(const std::vector<std::string>& query_set,
                                            bool& allPresent) const;

  std::optional<std::string> lookup_rec(uint32_t node_id,
                                        const uint32_t* q_begin,
                                        const uint32_t* q_end);

  void find_subsets(uint32_t node_id,
                    const uint32_t* q_begin,
                    const uint32_t* q_end,
                    std::vector<std::string>& results);

  // Guards against circular child_id references in malformed files: the
  // empty-query and "extra element" branches recurse without shrinking the
  // query, so they cannot self-terminate on a cycle. Uses the epoch-stamped
  // supersetVisit_ buffer (a node is visited-this-call iff its stamp equals
  // supersetVisitEpoch_) so each call costs O(nodes visited) instead of
  // allocating+zeroing an O(total_nodes) bitmap (audit P1).
  void find_supersets(uint32_t node_id,
                      const uint32_t* q_begin,
                      const uint32_t* q_end,
                      std::vector<std::string>& results);

  // visited guards against circular child_id references in malformed files.
  void enumerate_all(uint32_t node_id, std::set<std::string> &accum,
                     std::vector<bool> &visited,
                     const std::function<void(const std::set<std::string>&,
                                              const std::string&)> &cb);
};

}} // namespace klee::mapofsets

#endif
