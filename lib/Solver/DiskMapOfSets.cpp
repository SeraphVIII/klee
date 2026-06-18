#include "klee/Solver/DiskMapOfSets.h"
#include "klee/Support/ErrorHandling.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// Multi-byte integers are read via raw memcpy; LE-only until routed through
// explicit byte-order helpers.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "MapOfSets file format requires a little-endian host");
#endif

using namespace klee::mapofsets;

DiskMapOfSets::DiskMapOfSets(const std::string &filename, size_t max_cache_size)
    // A bound of 0 would make get_chunk evict from an empty LRU list (UB);
    // clamp to at least one resident chunk.
    : max_cache_size_(max_cache_size ? max_cache_size : 1) {
  auto fail = [&](const char *msg) {
    klee_warning("%s: %s", msg, filename.c_str());
    if (mmap_base_ && mmap_base_ != MAP_FAILED) {
      munmap(mmap_base_, file_size_);
      mmap_base_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  };

  fd_ = open(filename.c_str(), O_RDONLY);
  if (fd_ < 0) { fail("DiskMapOfSets: cannot open"); return; }

  struct stat st;
  if (fstat(fd_, &st) != 0) { fail("DiskMapOfSets: fstat failed on"); return; }
  file_size_ = st.st_size;
  if (file_size_ == 0) { fail("DiskMapOfSets: file is empty"); return; }

  mmap_base_ = mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (mmap_base_ == MAP_FAILED) { fail("DiskMapOfSets: mmap failed"); return; }

  // Mapping keeps the inode alive after close.
  close(fd_);
  fd_ = -1;

  // Layout: [u64 magic][u64 header_size][Header proto][directory][values][string table][chunks]
  static constexpr uint64_t kRawMagic = 0x4d41504f53455453ULL; // "MAPOSETS"

  if (file_size_ < 16) { fail("DiskMapOfSets: file too small (<16 bytes)"); return; }

  uint64_t magic = 0, header_size = 0;
  memcpy(&magic,       mmap_base_, 8);
  memcpy(&header_size, static_cast<const char *>(mmap_base_) + 8, 8);

  if (magic != kRawMagic) { fail("DiskMapOfSets: bad magic in"); return; }
  if (16ULL + header_size > (uint64_t)file_size_) {
    fail("DiskMapOfSets: header_size out of range in"); return;
  }

  google::protobuf::io::ArrayInputStream stream(
      static_cast<const char *>(mmap_base_) + 16, (int)header_size);
  if (!header_file_.ParseFromZeroCopyStream(&stream)) {
    fail("DiskMapOfSets: failed to parse header protobuf in"); return;
  }

  uint32_t version = header_file_.header().version();
  if (version != 1) {
    fail("DiskMapOfSets: unsupported format version (delete and re-run) in");
    return;
  }

  // chunk_size==0 would SIGFPE in chunk_id arithmetic.
  if (header_file_.header().chunk_size() == 0) {
    fail("DiskMapOfSets: chunk_size is zero in"); return;
  }

  // Stale-key files (older canonicalization) must be rebuilt rather than
  // silently producing all misses. Keep in lockstep with kCanonVersion in
  // MapOfSetsDiskBuilder.cpp (v3: hash-ordered canonical operand sort).
  static constexpr uint32_t kExpectedCanonVersion = 3;
  uint32_t canon_version = header_file_.header().canonicalization_version();
  if (canon_version != kExpectedCanonVersion) {
    fail("DiskMapOfSets: canonicalization version mismatch (delete and re-run) in");
    return;
  }

  uint64_t dir_off = header_file_.header().directory_offset();
  uint64_t dir_sz  = header_file_.header().directory_size();
  if (dir_off < 16ULL + header_size || dir_off + dir_sz > (uint64_t)file_size_) {
    fail("DiskMapOfSets: directory range invalid in"); return;
  }

  uint64_t val_off = header_file_.header().values_offset();
  uint64_t val_sz  = header_file_.header().values_size();
  if (val_sz > 0 &&
      (val_off < 16ULL + header_size || val_off + val_sz > (uint64_t)file_size_)) {
    fail("DiskMapOfSets: values region invalid in"); return;
  }

  uint64_t str_off = header_file_.header().string_table_offset();
  uint64_t str_sz  = header_file_.header().string_table_size();
  if (str_sz < 4 ||
      str_off < 16ULL + header_size || str_off + str_sz > (uint64_t)file_size_) {
    fail("DiskMapOfSets: string table range invalid in"); return;
  }

  const char *strBlob = static_cast<const char *>(mmap_base_) + str_off;
  uint32_t strCount = 0;
  memcpy(&strCount, strBlob, 4);
  const char *p = strBlob + 4;
  const char *end = strBlob + str_sz;
  string_table_.reserve(strCount);
  for (uint32_t i = 0; i < strCount; ++i) {
    if (p + 4 > end) { fail("DiskMapOfSets: string table truncated in"); return; }
    uint32_t len = 0;
    memcpy(&len, p, 4); p += 4;
    if (p + len > end) { fail("DiskMapOfSets: string table entry overrun in"); return; }
    string_table_.emplace_back(p, len);
    p += len;
  }

  valid_ = true;
}

DiskMapOfSets::~DiskMapOfSets() {
  if (mmap_base_)
    munmap(mmap_base_, file_size_);
  if (fd_ >= 0)
    close(fd_);
}

NodeChunk *DiskMapOfSets::get_chunk(uint32_t chunk_id) {
  auto it = chunk_cache_.find(chunk_id);
  if (it != chunk_cache_.end()) {
    lru_order_.splice(lru_order_.begin(), lru_order_, it->second.lru_it);
    return it->second.parsed.get();
  }

  if (chunk_cache_.size() >= max_cache_size_) {
    uint32_t evict_id = lru_order_.back();
    lru_order_.pop_back();
    chunk_cache_.erase(evict_id);
  }

  uint64_t dir_off = header_file_.header().directory_offset();
  uint64_t chunk_entry_off = dir_off + (uint64_t)chunk_id * 12ULL;

  // Bad-file paths below soft-fail to nullptr; get_node degrades to a "miss".
  if (chunk_entry_off + 12ULL > (uint64_t)file_size_) {
    klee_warning("DiskMapOfSets: chunk directory entry out of range "
                 "(chunk_id=%u); marking cache invalid", chunk_id);
    valid_ = false;
    return nullptr;
  }

  Chunk chunk;
  memcpy(&chunk.offset, static_cast<const char *>(mmap_base_) + chunk_entry_off, 8);
  memcpy(&chunk.size,   static_cast<const char *>(mmap_base_) + chunk_entry_off + 8, 4);

  if (chunk.offset > (uint64_t)file_size_) {
    klee_warning("DiskMapOfSets: chunk %u offset past EOF; marking cache invalid",
                 chunk_id);
    valid_ = false;
    return nullptr;
  }
  if ((uint64_t)chunk.size > (uint64_t)file_size_ - chunk.offset) {
    klee_warning("DiskMapOfSets: chunk %u size exceeds EOF; marking cache invalid",
                 chunk_id);
    valid_ = false;
    return nullptr;
  }

  chunk.parsed = std::make_unique<NodeChunk>();
  google::protobuf::io::ArrayInputStream chunk_stream(
      static_cast<const char *>(mmap_base_) + chunk.offset, chunk.size);
  if (!chunk.parsed->ParseFromZeroCopyStream(&chunk_stream)) {
    klee_warning("DiskMapOfSets: failed to parse chunk %u; marking cache invalid",
                 chunk_id);
    valid_ = false;
    return nullptr;
  }

  NodeChunk *raw = chunk.parsed.get();
  lru_order_.push_front(chunk_id);
  chunk.lru_it = lru_order_.begin();
  chunk_cache_[chunk_id] = std::move(chunk);
  return raw;
}

const Node &DiskMapOfSets::get_node(uint32_t node_id) {
  // Default-initialised: not end-of-set, no children — degrades to "miss".
  static const Node kEmptyNode;
  uint32_t cid = chunk_id(node_id);
  uint32_t lid = local_id(node_id);
  NodeChunk *chunk = get_chunk(cid);
  if (!chunk) return kEmptyNode;
  if (lid >= static_cast<uint32_t>(chunk->nodes_size())) {
    klee_warning("DiskMapOfSets: node %u out of chunk bounds; "
                 "marking cache invalid", node_id);
    valid_ = false;
    return kEmptyNode;
  }
  return chunk->nodes(lid);
}

DiskMapOfSets::NodeView DiskMapOfSets::get_node_view(uint32_t node_id) {
  const Node &node = get_node(node_id);
  NodeView view;
  view.is_end_of_set = node.is_end_of_set();
  view.value_offset = node.value_offset();
  view.children.reserve(node.children().size());
  for (const auto &c : node.children())
    view.children.push_back({c.key_index(), c.child_id()});
  return view;
}

const std::string &DiskMapOfSets::key_str(uint32_t index) const {
  if (index >= string_table_.size()) {
    klee_warning("DiskMapOfSets: key_index %u out of range (table size %zu); "
                 "marking cache invalid", index, string_table_.size());
    valid_ = false;
    static const std::string kEmpty;
    return kEmpty;
  }
  return string_table_[index];
}

std::string DiskMapOfSets::read_value(uint64_t offset) const {
  if (offset == 0) return "";
  uint64_t real    = offset - 1;
  uint64_t val_off = header_file_.header().values_offset();
  uint64_t val_sz  = header_file_.header().values_size();

  // Reject a wild offset up front so the arithmetic below cannot wrap uint64.
  if (real >= val_sz) return "";
  // `real + 4 == val_sz` is a valid edge case (length field exactly fills the
  // region); do NOT change `>` to `>=`.
  if (real + 4ULL > val_sz) return "";
  const char *blob = static_cast<const char *>(mmap_base_) + val_off + real;
  uint32_t len = 0;
  memcpy(&len, blob, 4);
  if (real + 4ULL + (uint64_t)len > val_sz) return "";
  return std::string(blob + 4, len);
}

std::vector<uint32_t>
DiskMapOfSets::resolveQueryIndices(const std::vector<std::string> &query_set,
                                   bool &allPresent) const {
  allPresent = true;
  std::vector<uint32_t> indices;
  indices.reserve(query_set.size());
  // string_table_ is sorted ascending (the builder sorts it). query_set is a
  // sorted vector (the canonical key is sorted), so iterating it yields indices
  // in ascending order without an extra sort.
  for (const auto &s : query_set) {
    auto it = std::lower_bound(string_table_.begin(), string_table_.end(), s);
    if (it != string_table_.end() && *it == s)
      indices.push_back(static_cast<uint32_t>(it - string_table_.begin()));
    else
      allPresent = false;
  }
  return indices;
}

std::optional<std::string>
DiskMapOfSets::lookup(const std::vector<std::string> &query_set) {
  if (!valid_) return std::nullopt;
  bool allPresent = false;
  std::vector<uint32_t> q = resolveQueryIndices(query_set, allPresent);
  // Exact lookup needs every query element to match a child; a string absent
  // from the table can never match, so the whole lookup misses.
  if (!allPresent) return std::nullopt;
  return lookup_rec(header_file_.header().root_id(),
                    q.data(), q.data() + q.size());
}

std::optional<std::string> DiskMapOfSets::lookup_rec(
    uint32_t node_id, const uint32_t *q_begin, const uint32_t *q_end) {
  if (q_begin == q_end) {
    const auto &node = get_node(node_id);
    if (node.is_end_of_set())
      return read_value(node.value_offset());
    return std::nullopt;
  }

  const auto &node = get_node(node_id);
  const uint32_t target = *q_begin;

  auto it = std::lower_bound(
      node.children().begin(), node.children().end(), target,
      [](const Child &child, uint32_t t) {
        return child.key_index() < t;
      });

  if (it == node.children().end() || it->key_index() != target)
    return std::nullopt;

  return lookup_rec(it->child_id(), q_begin + 1, q_end);
}

std::vector<std::string>
DiskMapOfSets::subsets(const std::vector<std::string> &query_set) {
  if (!valid_) return {};
  bool allPresent = false;
  // For subset search, query strings absent from the table can be dropped: no
  // stored set contains them, so descending on them would find nothing.
  std::vector<uint32_t> q = resolveQueryIndices(query_set, allPresent);
  std::vector<std::string> results;
  find_subsets(header_file_.header().root_id(),
               q.data(), q.data() + q.size(), results);
  return results;
}

void DiskMapOfSets::find_subsets(
    uint32_t node_id,
    const uint32_t *q_begin,
    const uint32_t *q_end,
    std::vector<std::string> &results) {
  // Snapshot before recursing: a recursive call can evict this node's chunk.
  NodeView node = get_node_view(node_id);

  if (node.is_end_of_set)
    results.push_back(read_value(node.value_offset));

  for (auto q_it = q_begin; q_it != q_end; ++q_it) {
    const uint32_t elt = *q_it;

    auto child_it = std::lower_bound(
        node.children.begin(), node.children.end(), elt,
        [](const ChildRef &c, uint32_t t) {
          return c.key_index < t;
        });

    if (child_it != node.children.end() && child_it->key_index == elt) {
      find_subsets(child_it->child_id, q_it + 1, q_end, results);
    }
  }
}

std::vector<std::string>
DiskMapOfSets::supersets(const std::vector<std::string> &query_set) {
  if (!valid_) return {};
  bool allPresent = false;
  std::vector<uint32_t> q = resolveQueryIndices(query_set, allPresent);
  // A stored superset must contain every query element; if any query string is
  // absent from the table, no stored set can contain it, so there are none.
  if (!allPresent) return {};
  std::vector<std::string> results;
  // Epoch-stamped cycle guard, reused across calls (audit P1). Size once; bump
  // the epoch to clear it in O(1). On wraparound reset stamps and skip epoch 0
  // (an unwritten stamp reads 0 and must not look "visited").
  uint32_t totalNodes = header_file_.header().total_nodes();
  if (supersetVisit_.size() < totalNodes)
    supersetVisit_.assign(totalNodes, 0);
  if (++supersetVisitEpoch_ == 0) {
    std::fill(supersetVisit_.begin(), supersetVisit_.end(), 0);
    supersetVisitEpoch_ = 1;
  }
  find_supersets(header_file_.header().root_id(),
                 q.data(), q.data() + q.size(), results);
  return results;
}

void DiskMapOfSets::find_supersets(
    uint32_t node_id,
    const uint32_t *q_begin,
    const uint32_t *q_end,
    std::vector<std::string> &results) {
  // Cycle guard for malformed files: the branches below can recurse without
  // shrinking the query, so a corrupt child_id cycle would not self-terminate.
  if (node_id < supersetVisit_.size()) {
    if (supersetVisit_[node_id] == supersetVisitEpoch_) {
      klee_warning("DiskMapOfSets: cycle detected at node %u during superset "
                   "search; marking cache invalid", node_id);
      valid_ = false;
      return;
    }
    supersetVisit_[node_id] = supersetVisitEpoch_;
  }

  // Snapshot before recursing: a recursive call can evict this node's chunk.
  NodeView node = get_node_view(node_id);

  if (q_begin == q_end) {
    if (node.is_end_of_set)
      results.push_back(read_value(node.value_offset));

    for (const auto &child : node.children)
      find_supersets(child.child_id, q_begin, q_end, results);
  } else {
    const uint32_t elt = *q_begin;
    const uint32_t *next_q = q_begin + 1;

    for (const auto &child : node.children) {
      if (child.key_index == elt) {
        find_supersets(child.child_id, next_q, q_end, results);
      } else if (child.key_index < elt) {
        // Extra superset element; follow without advancing the query.
        find_supersets(child.child_id, q_begin, q_end, results);
      } else {
        break; // child_key > elt: remaining sorted children all overshoot.
      }
    }
  }
}

void DiskMapOfSets::enumerate_all(
    uint32_t node_id,
    std::set<std::string> &accum,
    std::vector<bool> &visited,
    const std::function<void(const std::set<std::string>&,
                             const std::string&)> &cb) {
  // Cycle guard for malformed files: query traversals self-terminate by
  // shrinking the query, but forEach has no such bound.
  if (node_id < visited.size()) {
    if (visited[node_id]) {
      klee_warning("DiskMapOfSets: cycle detected at node %u during forEach; "
                   "marking cache invalid", node_id);
      valid_ = false;
      return;
    }
    visited[node_id] = true;
  }
  // Snapshot before recursing: a recursive call can evict this node's chunk.
  NodeView node = get_node_view(node_id);
  if (node.is_end_of_set)
    cb(accum, read_value(node.value_offset));

  for (const auto &child : node.children) {
    const std::string &key = key_str(child.key_index);
    accum.insert(key);
    enumerate_all(child.child_id, accum, visited, cb);
    accum.erase(key);
  }
}

void DiskMapOfSets::forEach(
    std::function<void(const std::set<std::string>&, const std::string&)> cb) {
  if (!valid_) return;
  std::set<std::string> accum;
  std::vector<bool> visited(header_file_.header().total_nodes(), false);
  enumerate_all(header_file_.header().root_id(), accum, visited, cb);
}

std::vector<DiskMapOfSets::Entry> DiskMapOfSets::allEntries() {
  if (!valid_) return {};
  std::vector<Entry> results;
  forEach([&](const std::set<std::string> &ks, const std::string &v) {
    results.push_back({ks, v});
  });
  return results;
}
