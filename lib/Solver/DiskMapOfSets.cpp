#include "klee/Solver/DiskMapOfSets.h"
#include "klee/Support/ErrorHandling.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace klee::mapofsets;

DiskMapOfSets::DiskMapOfSets(const std::string &filename) {
  klee_message("ENTER DiskMapOfSets('%s')\n", filename.c_str());

  fd_ = open(filename.c_str(), O_RDONLY);
  klee_message("open() returned fd=%d\n", fd_);
  assert(fd_ >= 0);

  struct stat st;
  fstat(fd_, &st);
  file_size_ = st.st_size;
  klee_message("file_size=%zu\n", file_size_);
  assert(file_size_ > 0);

  mmap_base_ = mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd_, 0);
  klee_message("mmap_base=%p\n", mmap_base_);
  assert(mmap_base_ != MAP_FAILED);

  // -------------------------------------------------------------------------
  // On-disk layout written by MapOfSetsDiskBuilder::build():
  //   [u64 magic][u64 header_size][header protobuf bytes][directory][chunks...]
  // We must parse ONLY the header protobuf bytes (not the whole file).
  // -------------------------------------------------------------------------
  static constexpr uint64_t kRawMagic = 0x4d41504f53455453ULL; // "MAPOSETS"

  if (file_size_ < 16) {
    klee_message("DiskMapOfSets: file too small (<16 bytes)\n");
    abort();
  }

  uint64_t magic = 0;
  uint64_t header_size = 0;
  memcpy(&magic, mmap_base_, 8);
  memcpy(&header_size, static_cast<const char *>(mmap_base_) + 8, 8);

  klee_message("magic=0x%016llx\n", (unsigned long long)magic);
  klee_message("header_size=%llu\n", (unsigned long long)header_size);

  if (magic != kRawMagic) {
    klee_message("DiskMapOfSets: bad magic (expected 0x%016llx)\n",
                 (unsigned long long)kRawMagic);
    abort();
  }

  if (16ULL + header_size > (uint64_t)file_size_) {
    klee_message("DiskMapOfSets: header_size out of range\n");
    abort();
  }

  klee_message("About to parse protobuf header...\n");
  google::protobuf::io::ArrayInputStream stream(
      static_cast<const char *>(mmap_base_) + 16, (int)header_size);
  bool ok = header_file_.ParseFromZeroCopyStream(&stream);
  klee_message("parse ok=%d\n", ok);

  if (!ok) {
    klee_message("DiskMapOfSets: failed to parse header protobuf\n");
    abort();
  }

  // Optional sanity checks (helps catch builder/reader mismatches early)
  uint64_t dir_off = header_file_.header().directory_offset();
  uint64_t dir_sz = header_file_.header().directory_size();
  if (dir_off < 16ULL + header_size || dir_off + dir_sz > (uint64_t)file_size_) {
    klee_message("DiskMapOfSets: directory range invalid "
                 "(dir_off=%llu dir_sz=%llu file_size=%zu)\n",
                 (unsigned long long)dir_off, (unsigned long long)dir_sz,
                 file_size_);
    abort();
  }

  klee_message("DiskMapOfSets OK\n");
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
  if (it != chunk_cache_.end())
    return it->second.parsed;

  if (chunk_cache_.size() >= max_cache_size_) {
    // Simple LRU: erase first
    chunk_cache_.erase(chunk_cache_.begin());
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
  chunk.parsed = new NodeChunk();
  google::protobuf::io::ArrayInputStream chunk_stream(
      static_cast<const char *>(mmap_base_) + chunk.offset, chunk.size);

  if (!chunk.parsed->ParseFromZeroCopyStream(&chunk_stream)) {
    klee_message("DiskMapOfSets: failed to parse chunk %u at off=%llu size=%u\n",
                 chunk_id, (unsigned long long)chunk.offset, chunk.size);
    abort();
  }

  chunk_cache_[chunk_id] = std::move(chunk);
  return chunk_cache_[chunk_id].parsed;
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

  const auto &vb = header_file_.values_blob();
  uint64_t real = offset - 1;

  if (real + 4ULL > (uint64_t)vb.size())
    return "";

  const char *blob = vb.data() + real;
  uint32_t len = 0;
  memcpy(&len, blob, 4); // 4-byte length prefix

  if (real + 4ULL + (uint64_t)len > (uint64_t)vb.size())
    return "";

  return std::string(blob + 4, len);
}

std::optional<std::string>
DiskMapOfSets::lookup(const std::set<std::string> &query_set) {
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

std::vector<klee::mapofsets::DiskMapOfSets::Entry>
DiskMapOfSets::subsets(const std::set<std::string> &query_set) {
  std::vector<klee::mapofsets::DiskMapOfSets::Entry> results;
  std::set<std::string> accum;
  klee_message("Start subsets\n");
  find_subsets(header_file_.header().root_id(), accum, query_set.begin(),
               query_set.end(), results);
  klee_message("End subsets\n");
  return results;
}

void DiskMapOfSets::find_subsets(
    uint32_t node_id, std::set<std::string> accum,
    std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end,
    std::vector<klee::mapofsets::DiskMapOfSets::Entry> &results) {
  const auto &node = get_node(node_id);
  if (node.is_end_of_set()) {
    std::string v = read_value(node.value_offset());
    if (v.rfind("UNSAT", 0) == 0) {
      results.push_back(Entry{accum, std::move(v)});
    }
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
      auto child_accum = accum;      // copy
      child_accum.insert(elt);
      find_subsets(child_it->child_id(), std::move(child_accum),
                  std::next(q_it), q_end, results);
    }
  }
}

std::vector<klee::mapofsets::DiskMapOfSets::Entry>
DiskMapOfSets::supersets(const std::set<std::string> &query_set) {
  std::vector<klee::mapofsets::DiskMapOfSets::Entry> results;
  std::set<std::string> accum;
  klee_message("Start supersets\n");
  find_supersets(header_file_.header().root_id(), std::move(accum),
                 query_set.begin(), query_set.end(), results);
  klee_message("End supersets\n");
  return results;
}

void DiskMapOfSets::find_supersets(
    uint32_t node_id, std::set<std::string> accum,
    std::set<std::string>::const_iterator q_begin,
    std::set<std::string>::const_iterator q_end,
    std::vector<klee::mapofsets::DiskMapOfSets::Entry> &results) {
  const auto &node = get_node(node_id);

  if (q_begin == q_end) {
    // All query elements matched; any end-of-set here or deeper is a superset
    if (node.is_end_of_set()) {
      klee::mapofsets::DiskMapOfSets::Entry e{accum,
                                             read_value(node.value_offset())};
      results.push_back(e);
    }
    // Continue into all children (adding extra elements makes bigger supersets)
    for (const auto &child : node.children()) {
      accum.insert(deserialize_key(child.key()));
      find_supersets(child.child_id(), accum, q_begin, q_end,
                     results);
      accum.erase(std::prev(accum.end())); // backtrack
    }
  } else {
    // Still need to match remaining query elements
    const std::string &elt = *q_begin;
    auto next_q = std::next(q_begin);

    // Scan all children in order (they're sorted)
    for (const auto &child : node.children()) {
      const std::string child_key = deserialize_key(child.key());
      accum.insert(child_key);

      if (child_key == elt) {
        // Exact match: recurse with next query element
        find_supersets(child.child_id(), accum, next_q, q_end,
                       results);
      } else if (child_key < elt) {
        // Extra element before required elt: recurse still needing elt
        find_supersets(child.child_id(), accum, q_begin, q_end,
                       results);
      } else {
        // child_key > elt: cannot match elt in this subtree, skip rest
        break;
      }

      accum.erase(std::prev(accum.end())); // backtrack
    }
  }
}
