// --persistent-cex-cache=PATH is the combined deployment mode: KLEE reads from
// PATH if it exists, and appends new entries to PATH.log in the same run. The
// log is consumed by the offline tool to grow the cache between runs.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out1 %t.klee-out2 %t.cache %t.cache.log
//
// Prime: no cache yet. KLEE auto-writes to %t.cache.log.
// RUN: %klee --output-dir=%t.klee-out1 --persistent-cex-cache=%t.cache %t.bc
// RUN: test -s %t.cache.log
//
// Build the cache from the auto-generated log path.
// RUN: %klee-pcache-opt --log %t.cache.log -o %t.cache
// RUN: rm %t.cache.log
//
// Warm: same flag, cache now exists. Reads from cache; any new entries continue
// to append to %t.cache.log.
// RUN: %klee --output-dir=%t.klee-out2 --persistent-cex-cache=%t.cache --disk-cex-cache-min-hit-rate=0 %t.bc
//
// Disk hits must have been recorded on the warm run.
// RUN: %klee-stats --to-csv %t.klee-out2 > %t.csv
// RUN: python3 -c "import csv,sys; rows=list(csv.DictReader(open('%t.csv'))); h=int(rows[-1]['QueryCexDiskCacheHits']); sys.exit(0 if h>0 else 1)"

#include "klee/klee.h"

int main(void) {
  int a;
  klee_make_symbolic(&a, sizeof(a), "a");
  if (a > 0) {
    if (a < 100)
      return 1;
    return 2;
  }
  return 3;
}
