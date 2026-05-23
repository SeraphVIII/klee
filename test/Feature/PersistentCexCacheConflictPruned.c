// A SAT/UNSAT collision must still be reported even when the conflicting UNSAT
// entry is dominated by a smaller UNSAT subset during pruning. Dominance only
// makes the superset entry redundant for lookups; it does not make the
// contradiction with the SAT entry go away. The optimiser must therefore detect
// the conflict independently of dominance: an INCONSISTENCY line on stderr, a
// non-zero conflicts counter on stdout, and exit code 2.
//
// The helper script extracts one real SAT key set K from the run and rewrites
// the log into an isolated fixture: SAT@K, UNSAT@(strict subset of K), and
// UNSAT@K. With UNSAT pruning on (the default), the subset entry dominates
// UNSAT@K, which is the only conflict in the fixture -- so a detector that bails
// on dominated entries would hide it (exit 0) instead of reporting it (exit 2).
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out %t.log
// RUN: %klee --output-dir=%t.klee-out --write-disk-cex-cache=%t.log %t.bc
// RUN: test -s %t.log
//
// Doctor the log: collision at K plus a dominating UNSAT subset of K.
// RUN: %S/PersistentCexCacheConflictPrunedGen.py %t.log
//
// Pruning is on by default; the collision must survive dominance. `not` flips
// the expected non-zero (2) exit to success.
// RUN: not %klee-pcache-opt --log %t.log --stats > %t.out 2> %t.err
// RUN: grep "^INCONSISTENCY " %t.err
// RUN: grep "SAT/UNSAT inconsistenc" %t.err
// RUN: grep -E "SAT/UNSAT conflicts.+: [1-9][0-9]*" %t.out

#include "klee/klee.h"

int main(void) {
  int a, b;
  klee_make_symbolic(&a, sizeof(a), "a");
  klee_make_symbolic(&b, sizeof(b), "b");
  if (a > 0) {
    if (b > 0)
      return 1;
    return 2;
  }
  return 3;
}
