// Mount a cache built from a structurally distinct program and confirm the
// sliding-window monitor suspends after a window of misses.
//
// RUN: %clang %s -DCACHE_SOURCE -emit-llvm -g %O0opt -c -o %t_a.bc
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t_b.bc
// RUN: rm -rf %t.klee-out_a %t.klee-out_b %t.log %t.cache
//
// Prime run uses program A.
// RUN: %klee --output-dir=%t.klee-out_a --write-disk-cex-cache=%t.log %t_a.bc
// RUN: %klee-pcache-opt --log %t.log -o %t.cache
//
// Warm run uses program B against the unrelated cache. Hit rate is 0%, so
// after a full window of W=4 misses the monitor must suspend.
// RUN: %klee --output-dir=%t.klee-out_b --disk-cex-cache=%t.cache --disk-cex-cache-min-hit-rate=50 --disk-cex-cache-hit-rate-window=4 %t_b.bc 2> %t.err
// RUN: grep "DiskCexCache: hit rate" %t.err
// RUN: grep "suspending" %t.err

#include "klee/klee.h"

int main(void) {
  unsigned char buf[4];
  klee_make_symbolic(buf, sizeof(buf), "buf");

#ifdef CACHE_SOURCE
  // Program A: distinct constants → distinct canonical keys.
  if (buf[0] == 0xAA) return 1;
  if (buf[1] == 0xBB) return 2;
  if (buf[2] == 0xCC) return 3;
  if (buf[3] == 0xDD) return 4;
#else
  // Program B: same shape, different constants → no canonical-key overlap.
  if (buf[0] == 0x11) return 1;
  if (buf[1] == 0x22) return 2;
  if (buf[2] == 0x33) return 3;
  if (buf[3] == 0x44) return 4;
#endif
  return 0;
}
