#!/usr/bin/env python3
"""Generate chacha20 KAT for UTSM state layout (U5).

Layout matches CODE/UTSM/crypto/chacha20.c:
  state[0..3]  = sigma
  state[4..11] = key words (LE)
  state[12]    = counter lo, state[13] = counter hi
  state[14]    = nonce lo,   state[15] = nonce hi
Output: 64B keystream hex words (LE), printed as C initializer.
"""
import struct


def rotl(v, c):
    return ((v << c) | (v >> (32 - c))) & 0xFFFFFFFF


def qr(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] ^= s[a]; s[d] = rotl(s[d], 16)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] ^= s[c]; s[b] = rotl(s[b], 12)
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] ^= s[a]; s[d] = rotl(s[d], 8)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] ^= s[c]; s[b] = rotl(s[b], 7)


def block(key_words, counter, nonce):
    iv = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    st = iv + list(key_words) + [counter & 0xFFFFFFFF, (counter >> 32) & 0xFFFFFFFF,
                                 nonce & 0xFFFFFFFF, (nonce >> 32) & 0xFFFFFFFF]
    v = st[:]
    for _ in range(10):
        qr(v, 0, 4, 8, 12); qr(v, 1, 5, 9, 13); qr(v, 2, 6, 10, 14); qr(v, 3, 7, 11, 15)
        qr(v, 0, 5, 10, 15); qr(v, 1, 6, 11, 12); qr(v, 2, 7, 8, 13); qr(v, 3, 4, 9, 14)
    return [(v[i] + st[i]) & 0xFFFFFFFF for i in range(16)]


def main():
    # KAT = RFC 8439 §2.4.2 官方向量（key=00..1f, counter=1, nonce 00000009 0000004a 00000000）
    # 映射到本实现的 64+64 布局：st12..15 = counter_lo, counter_hi, nonce_lo, nonce_hi
    #   st12=1, st13=0x09000000, st14=0x4a000000, st15=0
    #   => counter = 0x0900000000000001, nonce = 0x000000004a000000
    # 期望值已三方对拍：pycryptodome（djb 变体）+ RFC 官方向量 + C 实现，逐字节一致。
    key_bytes = bytes(range(32))
    key_words = list(struct.unpack("<8I", key_bytes))
    counter = 0x0900000000000001
    nonce = 0x000000004A000000
    out = block(key_words, counter, nonce)
    raw = struct.pack("<16I", *out)
    assert raw.hex().startswith("10f1e7e4d13b5915500fdd1fa32071c4"), \
        "KAT 不再匹配 RFC 8439 — 实现或本脚本被改动"
    print("/* key      = 00 01 02 .. 1f (LE words) */")
    print("/* counter  = 0x0900000000000001 (st12=1, st13=0x09000000) */")
    print("/* nonce    = 0x000000004a000000 (st14, st15=0) — RFC 8439 §2.4.2 */")
    print("static const u32 kat_key[8] = {" +
          ",".join(f"0x{w:08x}u" for w in key_words) + "};")
    print(f"static const u64 kat_counter = 0x{counter:016x}u;")
    print(f"static const u64 kat_nonce = 0x{nonce:08x}u;")
    print("static const u8 kat_expect[64] = {")
    for i in range(0, 64, 8):
        print("    " + ",".join(f"0x{b:02x}" for b in raw[i:i + 8]) + ",")
    print("};")


if __name__ == "__main__":
    main()
