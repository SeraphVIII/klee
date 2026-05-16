// The offline tool must be deterministic: two invocations on the same input
// must produce byte-identical output, and re-processing the tool's own output
// must not change it.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out %t.log %t.cache1 %t.cache2 %t.cache3
//
// Produce a log.
// RUN: %klee --output-dir=%t.klee-out --write-disk-cex-cache=%t.log %t.bc
//
// Two independent invocations on the same log: outputs must match byte-for-byte.
// RUN: %klee-pcache-opt --log %t.log -o %t.cache1
// RUN: %klee-pcache-opt --log %t.log -o %t.cache2
// RUN: diff %t.cache1 %t.cache2
//
// Re-processing the tool's own output (no log) must yield the same file.
// RUN: %klee-pcache-opt %t.cache1 -o %t.cache3
// RUN: diff %t.cache1 %t.cache3

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
