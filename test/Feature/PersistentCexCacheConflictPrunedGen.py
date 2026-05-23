#!/usr/bin/env python3
"""
Rewrite a klee-pcache log into an isolated fixture that exercises conflict
reporting under UNSAT dominance pruning.

A real run logs incidental entries (including empty-key queries that may already
collide), which would mask the case under test. So this script extracts one real
SAT key set K (the first SAT record with at least one key) and *overwrites* the
log with exactly three records:

  1. SAT   @ K              (the original SAT witness, kept verbatim)
  2. UNSAT @ K[:-1]         (a strict subset of K -- empty when |K| == 1)
  3. UNSAT @ K              (an UNSAT entry at the same key -> the collision)

The optimiser sorts UNSAT entries by ascending size, so record 2 is inserted
first and then dominates record 3. Record 3 is therefore the *only* conflict in
the fixture, and it is dominated -- so a detector that bails out on dominated
entries reports nothing (exit 0), while one that runs independently of dominance
reports it (exit 2). The test asserts the latter.

Log record layout (CexCachingSolver::appendToCacheLog):
  u32 num_keys
  num_keys * { u32 klen, klen bytes }
  u32 vlen, vlen bytes
  u32 num_concrete_arrays
  num_concrete_arrays * { u8 idx, u32 clen, clen bytes }

Usage: PersistentCexCacheConflictPrunedGen.py LOG_FILE
"""
import struct
import sys


def parse_record(data: bytes, pos: int):
    """Parse one log record at pos. Returns dict or None on a partial record."""
    if pos + 4 > len(data):
        return None
    (num_keys,) = struct.unpack_from("<I", data, pos)
    pos += 4
    keys = []
    for _ in range(num_keys):
        if pos + 4 > len(data):
            return None
        (klen,) = struct.unpack_from("<I", data, pos)
        pos += 4
        if pos + klen > len(data):
            return None
        keys.append(data[pos:pos + klen])
        pos += klen
    if pos + 4 > len(data):
        return None
    (vlen,) = struct.unpack_from("<I", data, pos)
    pos += 4
    if pos + vlen > len(data):
        return None
    value = data[pos:pos + vlen]
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
    return {"keys": keys, "value": value, "end": pos}


def encode_record(keys, value: bytes) -> bytes:
    out = bytearray()
    out += struct.pack("<I", len(keys))
    for k in keys:
        out += struct.pack("<I", len(k))
        out += k
    out += struct.pack("<I", len(value))
    out += value
    out += struct.pack("<I", 0)  # num_concrete_arrays = 0
    return bytes(out)


def main(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()

    pos = 0
    sat = None
    while pos < len(data):
        rec = parse_record(data, pos)
        if rec is None:
            break
        # Need a SAT record (non-empty value) with >= 1 key: an empty-key
        # conflict has no strict subset and so cannot be dominated.
        if len(rec["value"]) > 0 and len(rec["keys"]) >= 1:
            sat = rec
            break
        pos = rec["end"]

    if sat is None:
        print("no SAT record with >= 1 key found in log; cannot build a "
              "dominated conflict", file=sys.stderr)
        return 1

    keys = sat["keys"]
    fixture = (
        encode_record(keys, sat["value"]) +  # SAT  @ K
        encode_record(keys[:-1], b"") +       # UNSAT @ strict subset (dominator)
        encode_record(keys, b"")              # UNSAT @ K (the dominated conflict)
    )
    with open(path, "wb") as f:
        f.write(fixture)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
