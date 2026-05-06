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
    : max_cache_size_(max_cache_size) {
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
  // silently producing all misses.
  static constexpr uint32_t kExpectedCanonVersion = 1;
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

const std::string &DiskMapOfSets::key_str(uint32_t index) const {
  if (index >= string_table_.size()) {
    klee_error("DiskMapOfSets: key_index %u out of range (table size %zu)",
               index, string_table_.size());
  }
  return string_table_[index];
}

std::string DiskMapOfSets::read_value(uint64_t offset) const {
  if (offset == 0) return "";
  uint64_t real    = offset - 1;
  uint64_t val_off = header_file_.header().values_offset();
  uint64_t val_sz  = header_file_.header().values_size();

  // `real + 4 == val_sz` is a valid edge case (length field exactly fills the
  // region); do NOT change `>` to `>=`.
  if (real + 4ULL > val_sz) return "";
  const char *blob = static_cast<const char *>(mmap_base_) + val_off + real;
  uint32_t len = 0;
  memcpy(&len, blob, 4);
  if (real + 4ULL + (uint64_t)len > val_sz) return "";
  return std::string(blob + 4, len);
}

std::optional<std::string>
DiskMapOfSets::lookup(const std::set<std::string> &query_set) {
  if (!valid_) return std::nullopt;
  return lookup_rec(header_file_.header().root_id(),
                    query_set.begin(), query_set.end());
}

std::optional<std::string> DiskMapOfSets::lookup_rec(
    uint32_t node_id, std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end) {
  if (q_begin == q_end) {
    const auto &node = get_node(node_id);
    if (node.is_end_of_set())
      return read_value(node.value_offset());
    return std::nullopt;
  }

  const auto &node = get_node(node_id);
  const std::string &target = *q_begin;

  auto it = std::lower_bound(
      node.children().begin(), node.children().end(), target,
      [this](const Child &child, const std::string &t) {
        return key_str(child.key_index()) < t;
      });

  if (it == node.children().end() || key_str(it->key_index()) != target)
    return std::nullopt;

  return lookup_rec(it->child_id(), std::next(q_begin), q_end);
}

std::vector<std::string>
DiskMapOfSets::subsets(const std::set<std::string> &query_set) {
  if (!valid_) return {};
  std::vector<std::string> results;
  find_subsets(header_file_.header().root_id(),
               query_set.begin(), query_set.end(), results);
  return results;
}

void DiskMapOfSets::find_subsets(
    uint32_t node_id,
    std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end,
    std::vector<std::string> &results) {
  const auto &node = get_node(node_id);

  if (node.is_end_of_set())
    results.push_back(read_value(node.value_offset()));

  for (auto q_it = q_begin; q_it != q_end; ++q_it) {
    const std::string &elt = *q_it;

    auto child_it = std::lower_bound(
        node.children().begin(), node.children().end(), elt,
        [this](const Child &c, const std::string &t) {
          return key_str(c.key_index()) < t;
        });

    if (child_it != node.children().end() &&
        key_str(child_it->key_index()) == elt) {
      find_subsets(child_it->child_id(), std::next(q_it), q_end, results);
    }
  }
}

std::vector<std::string>
DiskMapOfSets::supersets(const std::set<std::string> &query_set) {
  if (!valid_) return {};
  std::vector<std::string> results;
  find_supersets(header_file_.header().root_id(),
                 query_set.begin(), query_set.end(), results);
  return results;
}

void DiskMapOfSets::find_supersets(
    uint32_t node_id,
    std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end,
    std::vector<std::string> &results) {
  const auto &node = get_node(node_id);

  if (q_begin == q_end) {
    if (node.is_end_of_set())
      results.push_back(read_value(node.value_offset()));

    for (const auto &child : node.children())
      find_supersets(child.child_id(), q_begin, q_end, results);
  } else {
    const std::string &elt = *q_begin;
    auto next_q = std::next(q_begin);

    for (const auto &child : node.children()) {
      const std::string &child_key = key_str(child.key_index());

      if (child_key == elt) {
        find_supersets(child.child_id(), next_q, q_end, results);
      } else if (child_key < elt) {
        // Extra superset element; follow without advancing the query.
        find_supersets(child.child_id(), q_begin, q_end, results);
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
  const auto &node = get_node(node_id);
  if (node.is_end_of_set())
    cb(accum, read_value(node.value_offset()));

  for (const auto &child : node.children()) {
    const std::string &key = key_str(child.key_index());
    accum.insert(key);
    enumerate_all(child.child_id(), accum, visited, cb);
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
