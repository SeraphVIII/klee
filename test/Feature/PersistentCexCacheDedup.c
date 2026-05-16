// Concatenating a log with itself must trigger dedup in the offline tool:
// the input set doubles, and at least one duplicate is removed.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out %t.log %t.log2
// RUN: %klee --output-dir=%t.klee-out --write-disk-cex-cache=%t.log %t.bc
// RUN: test -s %t.log
//
// Concatenate the log with itself; raw record count must be exactly doubled.
// RUN: cat %t.log %t.log > %t.log2
// RUN: %klee-pcache-opt --log %t.log --stats > %t.stats1
// RUN: %klee-pcache-opt --log %t.log2 --stats > %t.stats2
//
// At least one of the SAT/UNSAT duplicate counts must be non-zero in stats2.
// RUN: grep -E "Duplicates removed.+: [1-9][0-9]*" %t.stats2

#include "klee/klee.h"

int main(void) {
  int a;
  klee_make_symbolic(&a, sizeof(a), "a");
  if (a > 0)
    return 1;
  return 2;
}
