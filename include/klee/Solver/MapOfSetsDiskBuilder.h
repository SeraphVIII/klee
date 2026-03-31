#ifndef MAPOFSETS_DISK_BUILDER_H
#define MAPOFSETS_DISK_BUILDER_H

#include "mapofsets.pb.h"
#include "klee/ADT/MapOfSets.h"
#include <vector>
#include <string>

namespace klee {

struct BuildNode {
  uint32_t id;
  bool isEndOfSet;
  std::string value;
  std::vector<std::pair<std::string, uint32_t>> children;
};

/// Informational metadata written into the cache file header.
/// Stored for diagnostic purposes; a mismatch on load produces a warning
/// but does not invalidate the cache (SAT assignments are re-verified by
/// Assignment::satisfies(), and UNSAT results are logically solver-agnostic).
struct CacheMetadata {
  std::string solverBackend; ///< e.g. "z3", "stp"
  std::string kleeVersion;   ///< e.g. "KLEE 3.3-pre"
};

class MapOfSetsDiskBuilder {
public:
  using K = std::string;
  using V = std::string;
  using UBTree = MapOfSets<K, V>;

  static void build(const UBTree &tree,
                    const std::string &filename,
                    const CacheMetadata &metadata = {},
                    uint32_t chunkSize = 1024);

private:
  static void dfsAssign(const UBTree::Node *src, 
                        std::vector<BuildNode> &outNodes);
};

} // namespace klee

#endif
