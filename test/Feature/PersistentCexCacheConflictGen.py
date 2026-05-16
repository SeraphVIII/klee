#!/usr/bin/env python3
"""
Doctor a klee-pcache log file to introduce a SAT/UNSAT collision at the same
canonical key. Walks log records; for the first record with a non-empty value
(a SAT entry) appends a clone of the same record with the value field zeroed
(an UNSAT entry under the empty-payload sentinel).

Log record layout (CexCachingSolver::appendToCacheLog):
  u32 num_keys
  num_keys * { u32 klen, klen bytes }
  u32 vlen, vlen bytes
  u32 num_concrete_arrays
  num_concrete_arrays * { u8 idx, u32 clen, clen bytes }

Usage: PersistentCexCacheConflictGen.py LOG_FILE
"""
import struct
import sys


def find_first_sat(data: bytes):
    pos = 0
    while pos < len(data):
        rec_start = pos
        if pos + 4 > len(data):
            return None
        (num_keys,) = struct.unpack_from("<I", data, pos)
        pos += 4
        for _ in range(num_keys):
            if pos + 4 > len(data):
                return None
            (klen,) = struct.unpack_from("<I", data, pos)
            pos += 4
            if pos + klen > len(data):
                return None
            pos += klen
        if pos + 4 > len(data):
            return None
        vlen_off = pos
        (vlen,) = struct.unpack_from("<I", data, pos)
        pos += 4
        if pos + vlen > len(data):
            return None
        pos += vlen
        if pos + 4 > len(data):
            return None
        (nconc,) = struct.unpack_from("<I", data, pos)
        pos += 4
        for _ in range(nconc):
            if pos + 5 > len(data):
                return None
            pos += 1  # idx
            (clen,) = struct.unpack_from("<I", data, pos)
            pos += 4
            if pos + clen > len(data):
                return None
            pos += clen
        rec_end = pos
        if vlen > 0:
            return rec_start, vlen_off, vlen, rec_end
    return None


def main(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()
    info = find_first_sat(data)
    if info is None:
        print("no SAT record found in log; cannot construct conflict", file=sys.stderr)
        return 1
    rec_start, vlen_off, vlen, rec_end = info

    # Clone the record up to (and including) the vlen field, then everything
    # after the value bytes. Zero out vlen to mark the clone UNSAT.
    head = bytearray(data[rec_start:vlen_off + 4])
    struct.pack_into("<I", head, vlen_off - rec_start, 0)
    tail = data[vlen_off + 4 + vlen:rec_end]
    clone = bytes(head) + tail

    with open(path, "ab") as f:
        f.write(clone)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
