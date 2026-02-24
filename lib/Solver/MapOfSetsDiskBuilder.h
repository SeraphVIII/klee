// MapOfSetsDiskBuilder.h

#ifndef MAPOFSETS_DISK_BUILDER_H
#define MAPOFSETS_DISK_BUILDER_H

#include "mapofsets.pb.h"
#include "MapOfSets.h"      // original in-memory UBTree
#include <vector>
#include <unordered_map>
#include <fstream>

namespace klee { namespace mapofsets {

class MapOfSetsDiskBuilder {
public:
  using K = std::string;
  using V = std::string;
  using UBTree = MapOfSets<K,V>;

  // Build a disk file from an in-memory UBTree
  static void build(const UBTree &tree,
                    const std::string &filename,
                    uint32_t chunkSize = 1024);
};

}} // namespace klee::mapofsets

#endif
