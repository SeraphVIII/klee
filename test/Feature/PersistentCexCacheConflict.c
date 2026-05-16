// A SAT/UNSAT collision at the same canonical key must be detected by the
// offline tool: an INCONSISTENCY line on stderr and a conflicts counter on
// stdout. UNSAT is taken as authoritative; the tool still exits cleanly so the
// CI pipeline can choose to gate on the stderr text rather than the exit code.
//
// The conflict is constructed by post-processing a real log: the helper script
// clones the first SAT record with its value field zeroed, so the cloned entry
// has the same canonical key but represents UNSAT.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out %t.log
// RUN: %klee --output-dir=%t.klee-out --write-disk-cex-cache=%t.log %t.bc
// RUN: test -s %t.log
//
// Doctor the log to introduce a collision.
// RUN: %S/PersistentCexCacheConflictGen.py %t.log
//
// The optimiser must announce the collision on both streams.
// RUN: %klee-pcache-opt --log %t.log --stats > %t.out 2> %t.err
// RUN: grep "^INCONSISTENCY " %t.err
// RUN: grep "SAT/UNSAT inconsistenc" %t.err
// RUN: grep -E "SAT/UNSAT conflicts.+: [1-9][0-9]*" %t.out

#include "klee/klee.h"

int main(void) {
  int a;
  klee_make_symbolic(&a, sizeof(a), "a");
  if (a > 0)
    return 1;
  return 2;
}
