// Flip the canonicalization_version field in the cache header to a value the
// loader does not accept. The loader must emit the canon-version-mismatch
// warning rather than silently producing stale hits.
//
// RUN: %clang %s -emit-llvm -g %O0opt -c -o %t.bc
// RUN: rm -rf %t.klee-out1 %t.klee-out2 %t.log %t.cache
//
// Build a valid cache.
// RUN: %klee --output-dir=%t.klee-out1 --write-disk-cex-cache=%t.log %t.bc
// RUN: %klee-pcache-opt --log %t.log -o %t.cache
//
// Header layout: [u64 magic][u64 header_size][Header protobuf][...]. Inside the
// protobuf, canonicalization_version (field 5, varint) is encoded as the byte
// pair 0x28 0x01. Patch it to 0x28 0x02 to force a version mismatch. Verify
// the marker is unique within the header to avoid corrupting an unrelated field.
// RUN: python3 -c "d=bytearray(open(r'%t.cache','rb').read()); n=int.from_bytes(d[8:16],'little'); h=bytes(d[16:16+n]); assert h.count(b'\x28\x01')==1, 'canon_version marker not unique'; i=h.index(b'\x28\x01'); d[16+i+1]=0x00; open(r'%t.cache','wb').write(bytes(d))"
//
// Loading the patched cache must produce the version-mismatch warning. KLEE
// itself continues without the cache, so exit must still be clean.
// RUN: %klee --output-dir=%t.klee-out2 --disk-cex-cache=%t.cache --disk-cex-cache-min-hit-rate=0 %t.bc 2> %t.err
// RUN: grep "canonicalization version mismatch" %t.err

#include "klee/klee.h"

int main(void) {
  int a;
  klee_make_symbolic(&a, sizeof(a), "a");
  if (a > 0)
    return 1;
  return 2;
}
