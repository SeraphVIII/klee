// klee-pcache-opt --stats must report a non-zero record count for a real log
// and explicitly note that no output was written.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out %t.log
// RUN: %klee --output-dir=%t.klee-out --write-disk-cex-cache=%t.log %t.bc
// RUN: %klee-pcache-opt --log %t.log --stats > %t.stats
//
// Log file line must report >= 1 records.
// RUN: grep -E "Log file.+\([1-9][0-9]* records\)" %t.stats
// Input-entries breakdown must be present.
// RUN: grep -E "Input entries.+UNSAT: [0-9]+, SAT: [0-9]+" %t.stats
// Stats mode never writes an output cache.
// RUN: grep "no output written" %t.stats

#include "klee/klee.h"

int main(void) {
  int a;
  klee_make_symbolic(&a, sizeof(a), "a");
  if (a > 0)
    return 1;
  return 2;
}
