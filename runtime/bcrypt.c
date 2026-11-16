// bcrypt.dll (CNG) on CommonCrypto: hashes and HMAC (MD5, SHA1, SHA256, SHA384, SHA512), AES-CBC/ECB,
// and the system RNG. Handles are host pointers to the objects below; the hash/key object buffers
// the caller passes (sized from "ObjectLength") are not used.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // CC_MD5 / CC_SHA1
#include "rt.h"
#include <CommonCrypto/CommonCrypto.h>
#include <stdlib.h>
#include <strings.h>

#define STATUS_SUCCESS 0
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define STATUS_NOT_SUPPORTED 0xC00000BBu
#define STATUS_NOT_FOUND 0xC0000225u
#define BCRYPT_ALG_HANDLE_HMAC_FLAG 0x8
#define BCRYPT_BLOCK_PADDING 0x1

enum { A_MD5 = 1, A_SHA1, A_SHA256, A_SHA384, A_SHA512, A_AES, A_RNG };
enum { M_CBC, M_ECB, M_GCM, M_OTHER };
#define MAGIC_ALG 0x41474C41u
#define MAGIC_HASH 0x48415348u
#define MAGIC_KEY 0x4B455921u

typedef struct { uint32_t magic, alg; int hmac, mode; } Alg;
typedef struct {
    uint32_t magic, alg; int hmac;
    union { CC_MD5_CTX md5; CC_SHA1_CTX sha1; CC_SHA256_CTX sha256; CC_SHA512_CTX sha512; CCHmacContext hm; } u;
} Hash;
typedef struct { uint32_t magic; int mode; size_t len; uint8_t key[32]; } Key;

static const struct { const char *name; uint32_t alg; uint32_t digest, block; } algs[] = {
    { "MD5", A_MD5, 16, 64 }, { "SHA1", A_SHA1, 20, 64 }, { "SHA256", A_SHA256, 32, 64 },
    { "SHA384", A_SHA384, 48, 128 }, { "SHA512", A_SHA512, 64, 128 }, { "AES", A_AES, 0, 16 }, { "RNG", A_RNG, 0, 0 },
};
static uint32_t digest_len(uint32_t a) { for (size_t i = 0; i < sizeof algs / sizeof *algs; i++) if (algs[i].alg == a) return algs[i].digest; return 0; }
static uint32_t block_len(uint32_t a) { for (size_t i = 0; i < sizeof algs / sizeof *algs; i++) if (algs[i].alg == a) return algs[i].block; return 0; }
static Alg *alg_of(uint64_t h) { Alg *a = PTR(h); return a && a->magic == MAGIC_ALG ? a : NULL; }
static Hash *hash_of(uint64_t h) { Hash *x = PTR(h); return x && x->magic == MAGIC_HASH ? x : NULL; }
static Key *key_of(uint64_t h) { Key *k = PTR(h); return k && k->magic == MAGIC_KEY ? k : NULL; }
static char *wstr(uint64_t p) { return p ? w16_to_utf8(PTR(p)) : strdup(""); }

// BCryptOpenAlgorithmProvider(*phAlg, algId, implementation, flags)
void I_bcrypt_dll_BCryptOpenAlgorithmProvider(CPU *c) {
    char *n = wstr(A1); uint32_t fl = (uint32_t)A3;
    for (size_t i = 0; i < sizeof algs / sizeof *algs; i++)
        if (!strcasecmp(n, algs[i].name)) {
            Alg *a = calloc(1, sizeof *a);
            *a = (Alg){ MAGIC_ALG, algs[i].alg, (fl & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0, M_CBC };
            wr64(A0, G(a));
            free(n); RET(STATUS_SUCCESS); return;
        }
    rt_log("[bcrypt] algorithm %s not supported\n", n);
    free(n);
    RET(STATUS_NOT_FOUND);
}
void I_bcrypt_dll_BCryptCloseAlgorithmProvider(CPU *c) { Alg *a = alg_of(A0); if (!a) { RET(STATUS_INVALID_HANDLE); return; } a->magic = 0; free(a); RET(0); }

static uint32_t put_prop(uint64_t out, uint32_t cb, uint64_t pres, const void *v, uint32_t n) {
    if (pres) wr32(pres, n);
    if (!out) return STATUS_SUCCESS;
    if (cb < n) return STATUS_BUFFER_TOO_SMALL;
    memcpy(PTR(out), v, n);
    return STATUS_SUCCESS;
}
// BCryptGetProperty(handle, property, out, cbOut, *cbResult, flags): works on algorithm, hash and key handles
void I_bcrypt_dll_BCryptGetProperty(CPU *c) {
    char *p = wstr(A1); uint64_t out = A2, pres = AN(4); uint32_t cb = (uint32_t)A3;
    uint32_t alg = 0, mode = M_CBC;
    if (alg_of(A0)) { alg = alg_of(A0)->alg; mode = (uint32_t)alg_of(A0)->mode; }
    else if (hash_of(A0)) alg = hash_of(A0)->alg;
    else if (key_of(A0)) { alg = A_AES; mode = (uint32_t)key_of(A0)->mode; }
    else { free(p); RET(STATUS_INVALID_HANDLE); return; }
    uint32_t r = STATUS_NOT_SUPPORTED, v;
    if (!strcmp(p, "ObjectLength")) { v = alg == A_AES ? (uint32_t)sizeof(Key) : (uint32_t)sizeof(Hash); r = put_prop(out, cb, pres, &v, 4); }
    else if (!strcmp(p, "HashDigestLength") && digest_len(alg)) { v = digest_len(alg); r = put_prop(out, cb, pres, &v, 4); }
    else if (!strcmp(p, "BlockLength") && block_len(alg)) { v = block_len(alg); r = put_prop(out, cb, pres, &v, 4); }
    else if (!strcmp(p, "KeyLengths") && alg == A_AES) { uint32_t kl[3] = { 128, 256, 64 }; r = put_prop(out, cb, pres, kl, 12); }
    else if (!strcmp(p, "AuthTagLength") && alg == A_AES) { uint32_t tl[3] = { 12, 16, 1 }; r = put_prop(out, cb, pres, tl, 12); }   // min, max, increment
    else if (!strcmp(p, "KeyLength") && key_of(A0)) { v = (uint32_t)key_of(A0)->len * 8; r = put_prop(out, cb, pres, &v, 4); }
    else if (!strcmp(p, "ChainingMode") && alg == A_AES) {
        const char *m = mode == M_ECB ? "ChainingModeECB" : mode == M_GCM ? "ChainingModeGCM" : "ChainingModeCBC";
        uint16_t *w = utf8_to_w16(m); r = put_prop(out, cb, pres, w, (uint32_t)(w16len(w) + 1) * 2); free(w);
    }
    else rt_log("[bcrypt] BCryptGetProperty(%s) not supported\n", p);
    free(p);
    RET(r);
}
// BCryptSetProperty(handle, property, in, cb, flags): only the AES chaining mode matters
void I_bcrypt_dll_BCryptSetProperty(CPU *c) {
    char *p = wstr(A1);
    uint32_t r = STATUS_SUCCESS;
    if (!strcmp(p, "ChainingMode")) {
        char *m = wstr(A2);
        int mode = !strcmp(m, "ChainingModeCBC") ? M_CBC : !strcmp(m, "ChainingModeECB") ? M_ECB : !strcmp(m, "ChainingModeGCM") ? M_GCM : M_OTHER;
        if (mode == M_OTHER) { rt_log("[bcrypt] chaining mode %s not supported\n", m); r = STATUS_NOT_SUPPORTED; }
        if (alg_of(A0)) alg_of(A0)->mode = mode; else if (key_of(A0)) key_of(A0)->mode = mode;
        free(m);
    }
    free(p);
    RET(r);
}

// ---------------- hashing ----------------
static void hash_init(Hash *h, const void *secret, size_t n) {
    if (h->hmac) {
        CCHmacAlgorithm a = h->alg == A_MD5 ? kCCHmacAlgMD5 : h->alg == A_SHA1 ? kCCHmacAlgSHA1 : h->alg == A_SHA256 ? kCCHmacAlgSHA256 :
                            h->alg == A_SHA384 ? kCCHmacAlgSHA384 : kCCHmacAlgSHA512;
        CCHmacInit(&h->u.hm, a, secret, n);
        return;
    }
    switch (h->alg) {
    case A_MD5: CC_MD5_Init(&h->u.md5); break;
    case A_SHA1: CC_SHA1_Init(&h->u.sha1); break;
    case A_SHA256: CC_SHA256_Init(&h->u.sha256); break;
    case A_SHA384: CC_SHA384_Init(&h->u.sha512); break;
    case A_SHA512: CC_SHA512_Init(&h->u.sha512); break;
    }
}
// BCryptCreateHash(alg, *phHash, obj, cbObj, secret, cbSecret, flags)
void I_bcrypt_dll_BCryptCreateHash(CPU *c) {
    Alg *a = alg_of(A0);
    if (!a || !digest_len(a->alg)) { RET(STATUS_INVALID_HANDLE); return; }
    Hash *h = calloc(1, sizeof *h);
    h->magic = MAGIC_HASH; h->alg = a->alg; h->hmac = a->hmac;
    hash_init(h, PTR(AN(4)), (size_t)(uint32_t)AN(5));
    wr64(A1, G(h));
    RET(STATUS_SUCCESS);
}
// BCryptHashData(hash, data, cb, flags)
void I_bcrypt_dll_BCryptHashData(CPU *c) {
    Hash *h = hash_of(A0); const void *d = PTR(A1); CC_LONG n = (CC_LONG)(uint32_t)A2;
    if (!h) { RET(STATUS_INVALID_HANDLE); return; }
    if (h->hmac) CCHmacUpdate(&h->u.hm, d, n);
    else switch (h->alg) {
        case A_MD5: CC_MD5_Update(&h->u.md5, d, n); break;
        case A_SHA1: CC_SHA1_Update(&h->u.sha1, d, n); break;
        case A_SHA256: CC_SHA256_Update(&h->u.sha256, d, n); break;
        case A_SHA384: CC_SHA384_Update(&h->u.sha512, d, n); break;
        case A_SHA512: CC_SHA512_Update(&h->u.sha512, d, n); break;
    }
    RET(STATUS_SUCCESS);
}
// BCryptFinishHash(hash, out, cbOut, flags)
void I_bcrypt_dll_BCryptFinishHash(CPU *c) {
    Hash *h = hash_of(A0);
    if (!h) { RET(STATUS_INVALID_HANDLE); return; }
    if ((uint32_t)A2 != digest_len(h->alg)) { RET(STATUS_INVALID_PARAMETER); return; }
    uint8_t *o = PTR(A1);
    if (h->hmac) CCHmacFinal(&h->u.hm, o);
    else switch (h->alg) {
        case A_MD5: CC_MD5_Final(o, &h->u.md5); break;
        case A_SHA1: CC_SHA1_Final(o, &h->u.sha1); break;
        case A_SHA256: CC_SHA256_Final(o, &h->u.sha256); break;
        case A_SHA384: CC_SHA384_Final(o, &h->u.sha512); break;
        case A_SHA512: CC_SHA512_Final(o, &h->u.sha512); break;
    }
    RET(STATUS_SUCCESS);
}
// BCryptDuplicateHash(hash, *phNew, obj, cbObj, flags)
void I_bcrypt_dll_BCryptDuplicateHash(CPU *c) {
    Hash *h = hash_of(A0);
    if (!h) { RET(STATUS_INVALID_HANDLE); return; }
    Hash *d = malloc(sizeof *d); *d = *h;
    wr64(A1, G(d));
    RET(STATUS_SUCCESS);
}
void I_bcrypt_dll_BCryptDestroyHash(CPU *c) { Hash *h = hash_of(A0); if (!h) { RET(STATUS_INVALID_HANDLE); return; } h->magic = 0; free(h); RET(0); }

// BCryptGenRandom(alg or NULL, buf, cb, flags)
void I_bcrypt_dll_BCryptGenRandom(CPU *c) { if (A1 && A2) arc4random_buf(PTR(A1), (size_t)(uint32_t)A2); RET(STATUS_SUCCESS); }

// ---------------- AES ----------------
// BCryptGenerateSymmetricKey(alg, *phKey, obj, cbObj, secret, cbSecret, flags)
void I_bcrypt_dll_BCryptGenerateSymmetricKey(CPU *c) {
    Alg *a = alg_of(A0); size_t n = (size_t)(uint32_t)AN(5);
    if (!a || a->alg != A_AES) { RET(STATUS_INVALID_HANDLE); return; }
    if (n != 16 && n != 24 && n != 32) { RET(STATUS_INVALID_PARAMETER); return; }
    Key *k = calloc(1, sizeof *k);
    k->magic = MAGIC_KEY; k->mode = a->mode; k->len = n;
    memcpy(k->key, PTR(AN(4)), n);
    wr64(A1, G(k));
    RET(STATUS_SUCCESS);
}
void I_bcrypt_dll_BCryptDuplicateKey(CPU *c) {
    Key *k = key_of(A0);
    if (!k) { RET(STATUS_INVALID_HANDLE); return; }
    Key *d = malloc(sizeof *d); *d = *k;
    wr64(A1, G(d));
    RET(STATUS_SUCCESS);
}
void I_bcrypt_dll_BCryptDestroyKey(CPU *c) { Key *k = key_of(A0); if (!k) { RET(STATUS_INVALID_HANDLE); return; } k->magic = 0; free(k); RET(0); }
// ---------------- AES-GCM (NIST SP 800-38D): CTR over CommonCrypto AES-ECB, GHASH in software ----------------
#define STATUS_AUTH_TAG_MISMATCH 0xC000A002u
static void gf_mul(uint8_t x[16], const uint8_t y[16]) {   // x = x * y in GF(2^128), GCM bit order
    uint8_t z[16] = { 0 }, v[16];
    memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1) for (int j = 0; j < 16; j++) z[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)(v[j] >> 1 | v[j - 1] << 7);
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}
static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i += 16) {
        size_t k = n - i < 16 ? n - i : 16;
        for (size_t j = 0; j < k; j++) y[j] ^= d[i + j];
        gf_mul(y, h);
    }
}
static int aes_ecb(const Key *k, const uint8_t *in, uint8_t *out, size_t n) {
    size_t moved;
    return CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, k->key, k->len, NULL, in, n, out, n, &moved) == kCCSuccess;
}
// BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO: cbSize +0, version +4, pbNonce +8, cbNonce +16, pbAuthData +24, cbAuthData +32,
// pbTag +40, cbTag +48, pbMacContext +56, cbMacContext +64, cbAAD +68, cbData +72, dwFlags +80
static uint32_t gcm(const Key *k, CCOperation op, uint64_t info, const uint8_t *in, size_t n, uint8_t *out) {
    const uint8_t *nonce = PTR(rd64(info + 8)), *aad = PTR(rd64(info + 24));
    size_t nn = rd32(info + 16), na = rd32(info + 32), nt = rd32(info + 48);
    uint8_t *tag = PTR(rd64(info + 40));
    if (rd32(info + 80) & 1) { rt_log("[bcrypt] chained AES-GCM calls not supported\n"); return STATUS_NOT_SUPPORTED; }
    if (!nonce || !nn || !tag || nt < 12 || nt > 16) return STATUS_INVALID_PARAMETER;
    uint8_t zero[16] = { 0 }, h[16], j0[16] = { 0 }, s[16] = { 0 }, ek[16], lens[16];
    if (!aes_ecb(k, zero, h, 16)) return STATUS_INVALID_PARAMETER;
    if (nn == 12) { memcpy(j0, nonce, 12); j0[15] = 1; }
    else {
        ghash(j0, h, nonce, nn);
        memset(lens, 0, 8); for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)((uint64_t)nn * 8 >> (56 - 8 * i));
        ghash(j0, h, lens, 16);
    }
    // the tag covers the AAD and the ciphertext: the input when decrypting, so hash it before the output may overwrite it
    ghash(s, h, aad, na);
    if (op == kCCDecrypt) ghash(s, h, in, n);
    uint8_t ctr[16]; memcpy(ctr, j0, 16);
    for (size_t i = 0; i < n; i += 16) {
        uint32_t cnt = (uint32_t)ctr[12] << 24 | (uint32_t)ctr[13] << 16 | (uint32_t)ctr[14] << 8 | ctr[15];
        cnt++; ctr[12] = (uint8_t)(cnt >> 24); ctr[13] = (uint8_t)(cnt >> 16); ctr[14] = (uint8_t)(cnt >> 8); ctr[15] = (uint8_t)cnt;
        aes_ecb(k, ctr, ek, 16);
        size_t m = n - i < 16 ? n - i : 16;
        for (size_t j = 0; j < m; j++) out[i + j] = in[i + j] ^ ek[j];
    }
    if (op == kCCEncrypt) ghash(s, h, out, n);
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)na * 8 >> (56 - 8 * i)); lens[8 + i] = (uint8_t)((uint64_t)n * 8 >> (56 - 8 * i)); }
    ghash(s, h, lens, 16);
    aes_ecb(k, j0, ek, 16);
    for (int i = 0; i < 16; i++) s[i] ^= ek[i];
    if (op == kCCEncrypt) { memcpy(tag, s, nt); return STATUS_SUCCESS; }
    uint8_t diff = 0;
    for (size_t i = 0; i < nt; i++) diff |= (uint8_t)(s[i] ^ tag[i]);
    if (diff) { memset(out, 0, n); return STATUS_AUTH_TAG_MISMATCH; }
    return STATUS_SUCCESS;
}

// BCryptEncrypt/Decrypt(key, in, cbIn, padInfo, iv, cbIV, out, cbOut, *cbResult, flags).
// CBC updates the IV buffer with the last ciphertext block, as CNG does, so calls can be chained.
static void crypt(CPU *c, CCOperation op) {
    Key *k = key_of(A0);
    if (!k) { RET(STATUS_INVALID_HANDLE); return; }
    const uint8_t *in = PTR(A1); size_t n = (size_t)(uint32_t)A2;
    uint64_t iv = AN(4), out = AN(6), pres = AN(8); uint32_t cbiv = (uint32_t)AN(5), cbout = (uint32_t)AN(7), fl = (uint32_t)AN(9);
    if (k->mode == M_GCM) {
        if (!A3) { RET(STATUS_INVALID_PARAMETER); return; }
        if (pres) wr32(pres, (uint32_t)n);
        if (!out) { RET(STATUS_SUCCESS); return; }
        if (cbout < n) { RET(STATUS_BUFFER_TOO_SMALL); return; }
        uint32_t st = gcm(k, op, A3, in, n, PTR(out));
        if (st == STATUS_AUTH_TAG_MISMATCH) rt_log("[bcrypt] AES-GCM tag mismatch (%zu bytes)\n", n);
        RET(st); return;
    }
    if (A3) { rt_log("[bcrypt] padding info for a non-GCM key not supported\n"); RET(STATUS_NOT_SUPPORTED); return; }
    int pad = (fl & BCRYPT_BLOCK_PADDING) != 0;
    if (!pad && n % 16) { RET(STATUS_INVALID_PARAMETER); return; }
    size_t need = op == kCCEncrypt && pad ? (n / 16 + 1) * 16 : n;
    if (!out) { if (pres) wr32(pres, (uint32_t)need); RET(STATUS_SUCCESS); return; }
    if (cbout < (op == kCCEncrypt ? need : n - (pad ? 16 : 0))) { if (pres) wr32(pres, (uint32_t)need); RET(STATUS_BUFFER_TOO_SMALL); return; }
    if (k->mode == M_CBC && (!iv || cbiv != 16)) { RET(STATUS_INVALID_PARAMETER); return; }
    uint8_t next_iv[16];
    if (k->mode == M_CBC && op == kCCDecrypt && n >= 16) memcpy(next_iv, in + n - 16, 16);   // saved first: output may overwrite input
    size_t moved = 0;
    CCCryptorStatus s = CCCrypt(op, kCCAlgorithmAES, (k->mode == M_ECB ? kCCOptionECBMode : 0) | (pad ? kCCOptionPKCS7Padding : 0),
                                k->key, k->len, k->mode == M_CBC ? PTR(iv) : NULL, in, n, PTR(out), cbout, &moved);
    if (s != kCCSuccess) { rt_log("[bcrypt] CCCrypt failed (%d)\n", (int)s); RET(s == kCCBufferTooSmall ? STATUS_BUFFER_TOO_SMALL : STATUS_INVALID_PARAMETER); return; }
    if (k->mode == M_CBC && moved >= 16) memcpy(PTR(iv), op == kCCEncrypt ? (uint8_t *)PTR(out) + moved - 16 : next_iv, 16);
    if (pres) wr32(pres, (uint32_t)moved);
    RET(STATUS_SUCCESS);
}
void I_bcrypt_dll_BCryptEncrypt(CPU *c) { crypt(c, kCCEncrypt); }
void I_bcrypt_dll_BCryptDecrypt(CPU *c) { crypt(c, kCCDecrypt); }
