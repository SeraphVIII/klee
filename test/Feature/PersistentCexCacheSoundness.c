// The disk cache must not change which paths are reachable or how many tests
// are generated. Cross-check the headline KLEE counters with and without the
// cache mounted.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out_a %t.klee-out_b %t.klee-out_c %t.log %t.cache
//
// Baseline: in-memory CEX cache only.
// RUN: %klee --output-dir=%t.klee-out_a %t.bc 2> %t.err_a
//
// Prime + build cache.
// RUN: %klee --output-dir=%t.klee-out_b --write-disk-cex-cache=%t.log %t.bc
// RUN: %klee-pcache-opt --log %t.log -o %t.cache
//
// Warm: read against the cache. Disable the monitor so short-run flakiness
// cannot mask hits.
// RUN: %klee --output-dir=%t.klee-out_c --disk-cex-cache=%t.cache --disk-cex-cache-min-hit-rate=0 %t.bc 2> %t.err_c
//
// Headline counters must agree.
// RUN: grep "completed paths = " %t.err_a > %t.compl_a
// RUN: grep "completed paths = " %t.err_c > %t.compl_c
// RUN: diff %t.compl_a %t.compl_c
// RUN: grep "generated tests = " %t.err_a > %t.gen_a
// RUN: grep "generated tests = " %t.err_c > %t.gen_c
// RUN: diff %t.gen_a %t.gen_c

#include "klee/klee.h"

int main(void) {
  int a, b;
  klee_make_symbolic(&a, sizeof(a), "a");
  klee_make_symbolic(&b, sizeof(b), "b");
  if (a > 0 && b > 0) {
    if (a + b == 10)
      return 1;
    return 2;
  }
  if (a < 0)
    return 3;
  return 4;
}
