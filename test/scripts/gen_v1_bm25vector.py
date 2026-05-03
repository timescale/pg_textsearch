#!/usr/bin/env python3
#
# gen_v1_bm25vector.py — emit a COPY BINARY frame containing a row
# with a hand-encoded legacy v1 bm25vector. Used by
# test/sql/vector_format_compat.sql to exercise tpvector_recv's
# v1 read-compat path (tpvector_v1_to_v2).
#
# Output (to stdout): COPY BINARY framing + 1 row [int4 id, v1
# bm25vector]. Hard-coded payload: id=42, index='compat_idx',
# entries=[(freq=1, lex='alpha'), (freq=2, lex='bravo')].
#
# Why this is a separate file: psql's \! runs single-line shell
# commands and doesn't support heredocs, so the generator script
# can't be inlined into the .sql test.

import struct
import sys


def maxalign(n: int, a: int = 8) -> int:
    return (n + a - 1) & ~(a - 1)


idx_name = b"compat_idx"
inl = len(idx_name)
entries = [(1, b"alpha"), (2, b"bravo")]

# v1 layout:
#   int32 vl_len_
#   int32 index_name_len
#   int32 entry_count
#   char  index_name[]   + '\0' + padding to MAXALIGN
#   per entry:
#     int32 frequency
#     int32 lexeme_len
#     char  lexeme[]
#     padding to MAXALIGN(8 + lexeme_len)
hdr_size = 12
idx_block = maxalign(inl + 1)
total = hdr_size + idx_block + sum(maxalign(8 + len(lex)) for _, lex in entries)

# tpvector_send writes the first 4 bytes via pq_sendint32 (big-
# endian), then pq_sendbytes the rest of the value as raw bytes
# (which on the wire is host-byte-order — the existing format is
# not cross-platform-binary-compatible, but matches what the
# recv path expects).
v1 = struct.pack(">I", total)
v1 += struct.pack("=ii", inl, len(entries))
v1 += idx_name + b"\x00" + b"\x00" * (idx_block - inl - 1)
for freq, lex in entries:
    block = struct.pack("=ii", freq, len(lex)) + lex
    v1 += block + b"\x00" * (maxalign(len(block)) - len(block))

# COPY BINARY framing (network byte order for header / length prefixes).
sig = b"PGCOPY\n\xff\r\n\x00"
flags = struct.pack(">I", 0)
hdrext = struct.pack(">I", 0)
ncols = struct.pack(">H", 2)
id_col = struct.pack(">I", 4) + struct.pack(">i", 42)
vec_col = struct.pack(">I", len(v1)) + v1
trailer = struct.pack(">h", -1)

sys.stdout.buffer.write(sig + flags + hdrext + ncols + id_col + vec_col + trailer)
