// A truncated cache file must trigger a soft fail: KLEE warns, ignores the
// cache, and continues without it. Exit code must be 0.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out1 %t.klee-out2 %t.log %t.cache %t.corrupt
//
// Build a valid cache.
// RUN: %klee --output-dir=%t.klee-out1 --write-disk-cex-cache=%t.log %t.bc
// RUN: %klee-pcache-opt --log %t.log -o %t.cache
//
// Truncate to 32 bytes: past the 16-byte preamble but inside the header
// region. The loader rejects the file with a warning.
// RUN: dd if=%t.cache of=%t.corrupt bs=1 count=32 2> /dev/null
//
// Run KLEE against the corrupt cache. Exit must be clean, and the loader's
// soft-fail warning must appear on stderr.
// RUN: %klee --output-dir=%t.klee-out2 --disk-cex-cache=%t.corrupt --disk-cex-cache-min-hit-rate=0 %t.bc 2> %t.err
// RUN: grep "DiskMapOfSets" %t.err

#include "klee/klee.h"

int main(void) {
  int a;
  klee_make_symbolic(&a, sizeof(a), "a");
  if (a > 0)
    return 1;
  return 2;
}
