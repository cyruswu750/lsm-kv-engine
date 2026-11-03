#!/usr/bin/env python3
"""Independent reference for the SST version 2 filter (docs/sst-format.md
section 3.7). It shares no code with the engine and is used to derive the
golden vectors in tests/harness/bloom_test.cpp.

Usage: bloom_reference.py            print the golden vectors
"""

MASK = (1 << 64) - 1


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & MASK
    return h


def mix64(z: int) -> int:
    # SplitMix64 finalizer.
    z ^= z >> 30
    z = (z * 0xBF58476D1CE4E5B9) & MASK
    z ^= z >> 27
    z = (z * 0x94D049BB133111EB) & MASK
    z ^= z >> 31
    return z


def kvhash1(key: bytes):
    a = fnv1a64(key)
    h1 = mix64(a)
    h2 = mix64(a ^ 0x9E3779B97F4A7C15) | 1
    return a, h1, h2


def probes(key: bytes, k: int, m: int):
    _, h1, h2 = kvhash1(key)
    return [((h1 + i * h2) & MASK) % m for i in range(k)]


def num_probes(bits_per_key: int) -> int:
    # round(bits_per_key * 69 / 100), clamped to [1, 30]
    return max(1, min(30, (bits_per_key * 69 + 50) // 100))


def crc32c(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0x82F63B78 if c & 1 else 0)
    return c ^ 0xFFFFFFFF


def le(v: int, n: int) -> bytes:
    return v.to_bytes(n, "little")


def build_filter(keys, bits_per_key: int) -> bytes:
    n = len(keys)
    k = num_probes(bits_per_key)
    nbytes = max(8, (n * bits_per_key + 7) // 8)
    m = 8 * nbytes
    bits = bytearray(nbytes)
    for key in keys:
        for b in probes(key, k, m):
            bits[b // 8] |= 1 << (b % 8)
    body = le(1, 4) + le(1, 4) + le(n, 4) + le(k, 4) + bytes(bits)
    return body + le(crc32c(body), 4)


def c_literal(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        lines.append('    "' + "".join("\\x%02x" % c for c in data[i:i + 16]) + '"')
    return "\n".join(lines)


# The golden SST of tests/harness/sst_test.cpp (version 1): one 84-byte data
# block and a 25-byte index, followed by the 48-byte footer.
GOLDEN_V1_DATA = bytes.fromhex(
    "02000000010000000200000000000000" "0100ff01010000000200000003000000"
    "00000000016168690100000000000000" "05000000000000000262010000000000"
    "00000400000000000000016304000000" "c469241b")
GOLDEN_V1_INDEX = bytes.fromhex(
    "010000000000000000000000" "54000000" "63" "01000000" "a8b0d900")
GOLDEN_V1 = bytes.fromhex("020000000100000002000000000000000100ff01010000000200000003000000000000000161686901000000000000000500000000000000026201000000000000000400000000000000016304000000c469241b010000000000000000000000540000006301000000a8b0d9008d2e9f0b5400000000000000190000000000000000000000000000000000000000000000010000004b56535354303031")
GOLDEN_KEYS = [b"\x00\xff", b"a", b"b", b"c"]


def footer(index_offset, index_length, filter_offset, filter_length, version):
    covered = (le(index_offset, 8) + le(index_length, 8) + le(filter_offset, 8)
               + le(filter_length, 8) + le(version, 4) + b"KVSST001")
    return le(crc32c(covered), 4) + covered


def golden_v2_sst() -> bytes:
    data, index = GOLDEN_V1_DATA, GOLDEN_V1_INDEX
    filt = build_filter(GOLDEN_KEYS, 10)
    return (data + filt + index
            + footer(len(data) + len(filt), len(index), len(data), len(filt), 2))


def main():
    # The v1 golden reassembles to the published v1 bytes' footer CRC.
    v1 = GOLDEN_V1_DATA + GOLDEN_V1_INDEX + footer(84, 25, 0, 0, 1)
    assert v1 == GOLDEN_V1, "v1 golden mismatch"
    v2 = golden_v2_sst()
    print("// golden SST version 2 (%d bytes)" % len(v2))
    print(c_literal(v2))
    assert crc32c(b"123456789") == 0xE3069283
    assert fnv1a64(b"") == 0xCBF29CE484222325
    assert fnv1a64(b"a") == 0xAF63DC4C8601EC8C  # published FNV-1a test vector
    keys = [b"", b"a", b"\x00", b"\xff\x00", b"abc", b"key00000001"]
    print("// KVHASH1 vectors: key, FNV-1a 64, h1, h2")
    for key in keys:
        a, h1, h2 = kvhash1(key)
        print('{std::string("%s", %d), 0x%016xull, 0x%016xull, 0x%016xull},'
              % ("".join("\\x%02x" % c for c in key), len(key), a, h1, h2))
    for bpk, ks in ((10, [b"", b"a", b"b"]), (10, [b"\x00\xff", b"tomb"] + [b"k%d" % i for i in range(10)])):
        f = build_filter(ks, bpk)
        print("// filter bpk=%d keys=%r len=%d k=%d" % (bpk, ks, len(f), num_probes(bpk)))
        print(c_literal(f))
    print("// num_probes:", {b: num_probes(b) for b in (1, 2, 5, 10, 16, 20, 43, 44, 64)})


if __name__ == "__main__":
    main()
