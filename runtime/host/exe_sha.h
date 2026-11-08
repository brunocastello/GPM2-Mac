/* Own SHA-256 (CommonCrypto only exists from 10.5) and GPM2.EXE 1.02B validation; shared by the Carbon and Mac OS 9 backends. */
#ifndef GPM2_EXE_SHA_H
#define GPM2_EXE_SHA_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EXE_SHA256 "266f214714b6a2d3396f969f0e142dc267b63d80beb255b9998f66ffcd253546"
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t s[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s[0]; b = s[1]; c = s[2]; d = s[3]; e = s[4]; f = s[5]; g = s[6]; h = s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}
typedef struct { uint32_t s[8]; uint8_t buf[64]; size_t fill; uint64_t total; } sha_ctx;
static void sha_init(sha_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->s, iv, sizeof iv); c->fill = 0; c->total = 0;
}
/* accepts any chunk size: short host reads do not change the result */
static void sha_update(sha_ctx *c, const uint8_t *p, size_t n) {
    c->total += n;
    while (n) {
        size_t k = 64 - c->fill;
        if (k > n) k = n;
        memcpy(c->buf + c->fill, p, k); c->fill += k; p += k; n -= k;
        if (c->fill == 64) { sha_block(c->s, c->buf); c->fill = 0; }
    }
}
static void sha_final(sha_ctx *c, char hex[65]) {
    uint64_t bits = c->total * 8u;
    uint8_t pad[72];
    size_t padn = (c->fill < 56 ? 56 : 120) - c->fill;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padn + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(c, pad, padn + 8);
    c->total -= padn + 8;                                       /* total goes back to being only the file bytes */
    for (int i = 0; i < 8; i++) snprintf(hex + 8 * i, 9, "%08lx", (unsigned long)c->s[i]);
}
/* hash of a file via stdio; reads until fread returns 0 (does not stop on a short read); -1 if it cannot be opened */
static long sha_file(const char *path, char hex[65]) {
    static uint8_t buf[4096];
    sha_ctx c;
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) { hex[0] = 0; return -1; }
    sha_init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha_update(&c, buf, n);
    fclose(f);
    sha_final(&c, hex);
    return (long)c.total;
}
static int is_exe_102b(const char *path) {
    char hex[65];
    long n = sha_file(path, hex);
    fprintf(stderr, "[host] exe: %s, %ld bytes read, sha256 %s (expected size 1651712)\n", path, n, hex);
    return n >= 0 && strcmp(hex, EXE_SHA256) == 0;
}
#endif
