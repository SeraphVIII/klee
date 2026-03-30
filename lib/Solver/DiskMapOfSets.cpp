#include "klee/Solver/DiskMapOfSets.h"
#include "klee/Support/ErrorHandling.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace klee::mapofsets;

DiskMapOfSets::DiskMapOfSets(const std::string &filename, size_t max_cache_size)
    : max_cache_size_(max_cache_size) {
  // Helper: log a warning, clean up partially-acquired resources, and return
  // with valid_ == false so the caller can detect a soft failure.
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
    // valid_ stays false
  };

  fd_ = open(filename.c_str(), O_RDONLY);
  if (fd_ < 0) { fail("DiskMapOfSets: cannot open"); return; }

  struct stat st;
  fstat(fd_, &st);
  file_size_ = st.st_size;
  if (file_size_ == 0) { fail("DiskMapOfSets: file is empty"); return; }

  mmap_base_ = mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (mmap_base_ == MAP_FAILED) { fail("DiskMapOfSets: mmap failed"); return; }

  // -------------------------------------------------------------------------
  // On-disk layout written by MapOfSetsDiskBuilder::build():
  //   [u64 magic][u64 header_size][header protobuf bytes][directory][chunks...]
  // We must parse ONLY the header protobuf bytes (not the whole file).
  // -------------------------------------------------------------------------
  static constexpr uint64_t kRawMagic = 0x4d41504f53455453ULL; // "MAPOSETS"

  if (file_size_ < 16) { fail("DiskMapOfSets: file too small (<16 bytes)"); return; }

  uint64_t magic = 0;
  uint64_t header_size = 0;
  memcpy(&magic, mmap_base_, 8);
  memcpy(&header_size, static_cast<const char *>(mmap_base_) + 8, 8);

  if (magic != kRawMagic) { fail("DiskMapOfSets: bad magic in"); return; }
  if (16ULL + header_size > (uint64_t)file_size_) { fail("DiskMapOfSets: header_size out of range in"); return; }

  google::protobuf::io::ArrayInputStream stream(
      static_cast<const char *>(mmap_base_) + 16, (int)header_size);
  if (!header_file_.ParseFromZeroCopyStream(&stream)) {
    fail("DiskMapOfSets: failed to parse header protobuf in");
    return;
  }

  // Sanity-check directory and values regions.
  uint64_t dir_off = header_file_.header().directory_offset();
  uint64_t dir_sz  = header_file_.header().directory_size();
  if (dir_off < 16ULL + header_size || dir_off + dir_sz > (uint64_t)file_size_) {
    fail("DiskMapOfSets: directory range invalid in");
    return;
  }

  uint64_t val_off = header_file_.header().values_offset();
  uint64_t val_sz  = header_file_.header().values_size();
  if (val_sz > 0 &&
      (val_off < 16ULL + header_size || val_off + val_sz > (uint64_t)file_size_)) {
    fail("DiskMapOfSets: values region invalid in");
    return;
  }

  valid_ = true;
}

DiskMapOfSets::~DiskMapOfSets() {
  if (mmap_base_)
    munmap(mmap_base_, file_size_);
  if (fd_ >= 0)
    close(fd_);
  // Chunks auto-destruct via map
}

NodeChunk *DiskMapOfSets::get_chunk(uint32_t chunk_id) {
  auto it = chunk_cache_.find(chunk_id);
  if (it != chunk_cache_.end()) {
    // Move to front of LRU list (most recently used).
    lru_order_.splice(lru_order_.begin(), lru_order_, it->second.lru_it);
    return it->second.parsed.get();
  }

  if (chunk_cache_.size() >= max_cache_size_) {
    // Evict least recently used (back of list).
    uint32_t evict_id = lru_order_.back();
    lru_order_.pop_back();
    chunk_cache_.erase(evict_id);
  }

  // Directory is a simple array:
  //   entry[chunk_id] = (u64 offset, u32 size) => 12 bytes
  uint64_t dir_off = header_file_.header().directory_offset();
  uint64_t chunk_entry_off = dir_off + (uint64_t)chunk_id * 12ULL;

  if (chunk_entry_off + 12ULL > (uint64_t)file_size_) {
    klee_message("DiskMapOfSets: chunk directory entry out of range "
                 "(chunk_id=%u, entry_off=%llu)\n",
                 chunk_id, (unsigned long long)chunk_entry_off);
    abort();
  }

  Chunk chunk;
  memcpy(&chunk.offset,
         static_cast<const char *>(mmap_base_) + chunk_entry_off, 8);
  memcpy(&chunk.size,
         static_cast<const char *>(mmap_base_) + chunk_entry_off + 8, 4);

  if (chunk.offset > (uint64_t)file_size_) {
    klee_message("DiskMapOfSets: chunk out of range (offset past EOF)\n");
    abort();
  }

  if ((uint64_t)chunk.size > (uint64_t)file_size_ - chunk.offset) {
    klee_message("DiskMapOfSets: chunk out of range (size exceeds EOF)\n"
                 "(chunk_id=%u, off=%llu, size=%u, file=%zu, diff=%zu)\n",
                 chunk_id, (unsigned long long)chunk.offset, chunk.size,
                 file_size_, (uint64_t)file_size_ - chunk.offset);
    abort();
  }
  chunk.parsed = std::make_unique<NodeChunk>();
  google::protobuf::io::ArrayInputStream chunk_stream(
      static_cast<const char *>(mmap_base_) + chunk.offset, chunk.size);

  if (!chunk.parsed->ParseFromZeroCopyStream(&chunk_stream)) {
    klee_error("DiskMapOfSets: failed to parse chunk %u", chunk_id);
  }

  NodeChunk *raw = chunk.parsed.get();
  lru_order_.push_front(chunk_id);
  chunk.lru_it = lru_order_.begin();
  chunk_cache_[chunk_id] = std::move(chunk);
  return raw;
}

const Node &DiskMapOfSets::get_node(uint32_t node_id) {
  uint32_t cid = chunk_id(node_id);
  uint32_t lid = local_id(node_id);
  NodeChunk *chunk = get_chunk(cid);
  return chunk->nodes(lid);
}

std::string DiskMapOfSets::deserialize_key(const std::string &key_bytes) const {
  return key_bytes; // Simple string K; extend for varint/etc.
}

std::string DiskMapOfSets::read_value(uint64_t offset) const {
  // Builder stores real offsets shifted by +1 so that 0 can mean "no value".
  if (offset == 0)
    return "";

  uint64_t real     = offset - 1;
  uint64_t val_off  = header_file_.header().values_offset();
  uint64_t val_sz   = header_file_.header().values_size();

  if (real + 4ULL > val_sz)
    return "";

  // Read directly from the mmap'd file region — no heap allocation for the
  // lookup itself, only for the returned string copy.
  const char *blob = static_cast<const char *>(mmap_base_) + val_off + real;
  uint32_t len = 0;
  memcpy(&len, blob, 4); // 4-byte little-endian length prefix

  if (real + 4ULL + (uint64_t)len > val_sz)
    return "";

  return std::string(blob + 4, len);
}

std::optional<std::string>
DiskMapOfSets::lookup(const std::set<std::string> &query_set) {
  if (!valid_) return std::nullopt;
  return lookup_rec(header_file_.header().root_id(), query_set.begin(),
                    query_set.end());
}

std::optional<std::string> DiskMapOfSets::lookup_rec(
    uint32_t node_id, std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end) {
  if (q_begin == q_end) {
    const auto &node = get_node(node_id);
    if (node.is_end_of_set()) {
      return read_value(node.value_offset());
    }
    return std::nullopt;
  }

  const auto &node = get_node(node_id);
  const std::string &target = *q_begin;

  // Binary search children (assumed sorted by key)
  auto it = std::lower_bound(
      node.children().begin(), node.children().end(), target,
      [this](const Child &child, const std::string &t) {
        return deserialize_key(child.key()) < t;
      });

  if (it == node.children().end() || deserialize_key(it->key()) != target) {
    return std::nullopt;
  }

  return lookup_rec(it->child_id(), std::next(q_begin), q_end);
}

std::vector<DiskMapOfSets::Entry>
DiskMapOfSets::subsets(const std::set<std::string> &query_set) {
  if (!valid_) return {};
  std::vector<Entry> results;
  std::set<std::string> accum;
  find_subsets(header_file_.header().root_id(), accum,
               query_set.begin(), query_set.end(), results);
  return results;
}

void DiskMapOfSets::find_subsets(
    uint32_t node_id, std::set<std::string> &accum,
    std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end,
    std::vector<Entry> &results) {
  const auto &node = get_node(node_id);

  if (node.is_end_of_set()) {
    std::string v = read_value(node.value_offset());
    results.push_back(Entry{accum, std::move(v)});
  }

  for (auto q_it = q_begin; q_it != q_end; ++q_it) {
    const std::string &elt = *q_it;
    const auto &node2 = get_node(node_id);

    auto child_it = std::lower_bound(
        node2.children().begin(), node2.children().end(), elt,
        [this](const Child &c, const std::string &t) {
          return deserialize_key(c.key()) < t;
        });

    if (child_it != node2.children().end() &&
        deserialize_key(child_it->key()) == elt) {
      accum.insert(elt);
      find_subsets(child_it->child_id(), accum,
                   std::next(q_it), q_end, results);
      accum.erase(elt); // backtrack
    }
  }
}

std::vector<DiskMapOfSets::Entry>
DiskMapOfSets::supersets(const std::set<std::string> &query_set) {
  if (!valid_) return {};
  std::vector<Entry> results;
  std::set<std::string> accum;
  find_supersets(header_file_.header().root_id(), accum,
                 query_set.begin(), query_set.end(), results);
  return results;
}

void DiskMapOfSets::enumerate_all(uint32_t node_id,
                                  std::set<std::string> &accum,
                                  std::vector<Entry> &results) {
  const auto &node = get_node(node_id);
  if (node.is_end_of_set()) {
    std::string v = read_value(node.value_offset());
    results.push_back(Entry{accum, std::move(v)});
  }
  for (const auto &child : node.children()) {
    std::string key = deserialize_key(child.key());
    accum.insert(key);
    enumerate_all(child.child_id(), accum, results);
    accum.erase(key);
  }
}

std::vector<DiskMapOfSets::Entry> DiskMapOfSets::allEntries() {
  if (!valid_) return {};
  std::vector<Entry> results;
  std::set<std::string> accum;
  enumerate_all(header_file_.header().root_id(), accum, results);
  return results;
}

void DiskMapOfSets::find_supersets(
    uint32_t node_id, std::set<std::string> &accum,
    std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end,
    std::vector<Entry> &results) {
  const auto &node = get_node(node_id);

  if (q_begin == q_end) {
    if (node.is_end_of_set())
      results.push_back(Entry{accum, read_value(node.value_offset())});

    for (const auto &child : node.children()) {
      const std::string child_key = deserialize_key(child.key());
      accum.insert(child_key);
      find_supersets(child.child_id(), accum, q_begin, q_end, results);
      accum.erase(child_key);  // erase by key, not by position
    }
  } else {
    const std::string &elt = *q_begin;
    auto next_q = std::next(q_begin);

    for (const auto &child : node.children()) {
      const std::string child_key = deserialize_key(child.key());
      accum.insert(child_key);

      if (child_key == elt) {
        find_supersets(child.child_id(), accum, next_q, q_end, results);
      } else if (child_key < elt) {
        find_supersets(child.child_id(), accum, q_begin, q_end, results);
      } else {
        accum.erase(child_key);  // must erase before break
        break;
      }

      accum.erase(child_key);  // erase by key, not by position
    }
  }
}
