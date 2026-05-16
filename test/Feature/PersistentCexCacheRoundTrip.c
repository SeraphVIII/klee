// Persistent CEX cache round trip:
//   prime run  -> write log
//   offline    -> build cache from log
//   warm run   -> exercise cache, expect disk-cache hits in run.stats
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out1 %t.klee-out2 %t.log %t.cache
//
// Prime: populate the log.
// RUN: %klee --output-dir=%t.klee-out1 --write-disk-cex-cache=%t.log %t.bc
// RUN: test -s %t.log
//
// Build a cache from the log.
// RUN: %klee-pcache-opt --log %t.log -o %t.cache
// RUN: test -s %t.cache
//
// Warm: run again against the cache. --disk-cex-cache-min-hit-rate=0 disables
// the sliding-window monitor so suspension cannot mask hits on a short workload.
// RUN: %klee --output-dir=%t.klee-out2 --disk-cex-cache=%t.cache --disk-cex-cache-min-hit-rate=0 %t.bc
//
// At least one disk-cache hit must have been recorded. klee-stats reads the
// run.stats sqlite database via Python's stdlib sqlite3 module, avoiding any
// external-binary dependency. Stats rows are running totals across the run;
// the final row is the value at exit.
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
