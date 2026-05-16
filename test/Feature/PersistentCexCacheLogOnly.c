// --write-disk-cex-cache produces a non-empty log that the offline tool can
// ingest. No read-side cache is mounted; this exercises the write path in
// isolation.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out %t.log
//
// Log path produces a non-empty file.
// RUN: %klee --output-dir=%t.klee-out --write-disk-cex-cache=%t.log %t.bc 2> %t.err
// RUN: test -s %t.log
// RUN: not grep "Cannot open CEX cache log" %t.err
//
// The log is parseable by klee-pcache-opt.
// RUN: %klee-pcache-opt --log %t.log --stats > %t.stats
// RUN: grep -E -q "Log file" %t.stats
//
// Without the flag, no log file appears at the target path.
// RUN: rm -rf %t.klee-out2 %t.log2
// RUN: %klee --output-dir=%t.klee-out2 %t.bc
// RUN: test ! -e %t.log2

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
