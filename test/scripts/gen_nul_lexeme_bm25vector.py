#!/usr/bin/env python3
#
# gen_nul_lexeme_bm25vector.py — emit a COPY BINARY frame containing a
# row with a hand-encoded v2 bm25vector whose lexeme carries an
# embedded NUL byte. Used by test/sql/vector_v1_rejected.sql to verify
# that such values are rejected at the boundary rather than producing a
# silently truncated text rendering.
#
# A NUL can never appear in a lexeme built from tokenized text or the
# text input parser (both operate on NUL-terminated cstrings), so this
# value is only reachable via a crafted binary input.
#
# Output (to stdout): COPY BINARY framing + 1 row [int4 id, v2
# bm25vector]. Hard-coded payload: id=42, index='compat_idx',
# entries=[(freq=1, lex='bad\0lex')].

import struct
import sys


def maxalign(n: int, a: int = 8) -> int:
    return (n + a - 1) & ~(a - 1)


def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


idx_name = b"compat_idx"
inl = len(idx_name)
# The lexeme deliberately contains an embedded NUL in the middle so the
# entry passes the buffer-bounds checks but must be rejected by the
# explicit NUL check.
lexeme = b"bad\x00lex"
freq = 1

# v2 layout (see src/types/vector.h):
#   int32 vl_len_
#   char  magic[4] = "BM25"
#   uint8 version = 2, uint8 reserved[3] = 0
#   int32 index_name_len
#   int32 entry_count
#   char  index_name[] + '\0' + padding to MAXALIGN(index_name_len + 1)
#   per entry (variable-length, no padding):
#     varint frequency
#     varint lexeme_len
#     char   lexeme[lexeme_len]
hdr_size = 20
idx_block = maxalign(inl + 1)
entry = varint(freq) + varint(len(lexeme)) + lexeme
total = hdr_size + idx_block + len(entry)

# tpvector_send writes the first 4 bytes via pq_sendint32 (big-endian)
# = total size, then pq_sendbytes the rest of the value as raw bytes
# (host byte order on the wire). Mirror that here.
v2 = struct.pack(">I", total)
v2 += b"BM25"
v2 += struct.pack("=B", 2) + b"\x00" * 3
v2 += struct.pack("=ii", inl, 1)
v2 += idx_name + b"\x00" + b"\x00" * (idx_block - inl - 1)
v2 += entry

# COPY BINARY framing (network byte order for header / length prefixes).
sig = b"PGCOPY\n\xff\r\n\x00"
flags = struct.pack(">I", 0)
hdrext = struct.pack(">I", 0)
ncols = struct.pack(">H", 2)
id_col = struct.pack(">I", 4) + struct.pack(">i", 42)
vec_col = struct.pack(">I", len(v2)) + v2
trailer = struct.pack(">h", -1)

sys.stdout.buffer.write(sig + flags + hdrext + ncols + id_col + vec_col + trailer)
