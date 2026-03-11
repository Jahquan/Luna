#!/usr/bin/env python3

import hashlib
import struct
import sys
import zlib


LOCAL_HEADER_FIELDS = struct.Struct("<HHHHHIIIHH")


def iter_entries(data):
    offset = 0
    while offset < len(data):
        signature = data[offset : offset + 4]
        if signature in (b"PK\x01\x02", b"PK\x05\x06"):
            break

        if offset == 0 and data[offset : offset + 6] == b"*TIMLP":
            header_offset = offset + 10
        elif signature == b"PK\x03\x04":
            header_offset = offset + 4
        else:
            raise ValueError(f"unknown entry header at offset {offset}: {data[offset:offset + 16]!r}")

        (version, flags, method, mod_time, mod_date, crc32, compressed_size,
         uncompressed_size, name_len, extra_len) = LOCAL_HEADER_FIELDS.unpack_from(
            data, header_offset
        )

        pos = header_offset + LOCAL_HEADER_FIELDS.size
        name = data[pos : pos + name_len].decode("utf-8")
        pos += name_len + extra_len
        payload = data[pos : pos + compressed_size]
        pos += compressed_size

        if method == 8:
            payload = zlib.decompress(payload, -15)
        elif method not in (0, 13):
            raise ValueError(f"unsupported compression method {method} for {name}")

        yield name, method, len(payload), hashlib.sha256(payload).hexdigest()
        offset = pos


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: tns_manifest.py FILE.tns")

    data = open(sys.argv[1], "rb").read()
    lines = ["\t".join(map(str, entry)) for entry in iter_entries(data)]
    print(hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest())


if __name__ == "__main__":
    main()
