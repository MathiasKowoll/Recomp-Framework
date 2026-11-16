// SPIR-V fixes applied to the game's shaders before they reach the Vulkan driver.
//
// Atomic resets. A common GPU particle simulation pattern (id Tech 6's, for one) resets its indirect draw count with a plain store from thread 0
// of the dispatch, then every visible particle adds to it with atomicAdd. Only a workgroup barrier separates the
// two, so nothing orders the reset against the atomics of other workgroups. On desktop GPUs the store lands first
// in practice. On Apple GPUs a plain store can reach memory after other workgroups' atomics and wipe their counts,
// so the draws get 0 indices and the particles are never drawn.
// The fix makes such stores atomic: a plain OpStore through an access chain into a buffer variable that the same
// shader also updates with atomics becomes OpAtomicStore (relaxed, at the scope of the shader's own atomics), so
// it is performed at the same point of coherence as the atomicAdds.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { OpTypeInt = 21, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpStore = 62, OpAccessChain = 65,
       OpInBoundsAccessChain = 66, OpAtomicStore = 228, OpAtomicExchange = 229, OpAtomicCompareExchange = 230,
       OpAtomicIIncrement = 232, OpAtomicXor = 242 };

// Returns a patched copy (malloc'd, *out_words set) or NULL when nothing needed changing. *nfixed counts stores.
uint32_t *spv_fix_atomic_resets(const uint32_t *code, size_t words, size_t *out_words, int *nfixed) {
    *nfixed = 0;
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3];
    if (bound > (1u << 22)) return NULL;
    uint32_t *base = calloc(bound, sizeof *base);      // access chain or variable -> root variable
    uint32_t *ptype = calloc(bound, sizeof *ptype);    // pointer id -> its pointer type id
    uint32_t *pointee = calloc(bound, sizeof *pointee);  // pointer type -> pointee type
    uint32_t *scope = calloc(bound, sizeof *scope);    // root variable -> scope id of its atomics (0: none)
    uint32_t *atype = calloc(bound, sizeof *atype);    // root variable -> result type of its atomics
    uint8_t *isint32 = calloc(bound, 1);
    uint32_t zero = 0;                                 // an OpConstant 0 of a 32-bit int type, for the semantics
    uint32_t *out = NULL;
    if (!base || !ptype || !pointee || !scope || !atype || !isint32) goto done;
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (!n || i + n > words) goto done;
        const uint32_t *w = code + i;
        if (op == OpTypeInt && n >= 4 && w[1] < bound && w[2] == 32) isint32[w[1]] = 1;
        else if (op == OpTypePointer && n >= 4 && w[1] < bound) pointee[w[1]] = w[3];
        else if (op == OpConstant && n == 4 && w[1] < bound && isint32[w[1]] && w[3] == 0 && !zero) zero = w[2];
        else if (op == OpVariable && n >= 4 && w[2] < bound) { base[w[2]] = w[2]; ptype[w[2]] = w[1]; }
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 && w[2] < bound && w[3] < bound) {
            base[w[2]] = base[w[3]]; ptype[w[2]] = w[1];
        } else if (op >= OpAtomicExchange && op <= OpAtomicXor && n >= 6 && w[3] < bound) {
            // result type, result, pointer, scope, semantics...
            uint32_t root = base[w[3]];
            if (root && root < bound) { scope[root] = w[4]; atype[root] = w[1]; }
        }
        i += n;
    }
    if (!zero) goto done;
    size_t cap = words + 64, k = 0;
    out = malloc(cap * sizeof *out);
    if (!out) goto done;
    memcpy(out, code, 5 * sizeof *out); k = 5;
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        const uint32_t *w = code + i;
        if (k + n + 2 > cap) { cap = cap * 2 + n; uint32_t *o = realloc(out, cap * sizeof *out); if (!o) { free(out); out = NULL; goto done; } out = o; }
        uint32_t root = (op == OpStore && n >= 3 && w[1] < bound) ? base[w[1]] : 0;
        if (root && root < bound && root != w[1] && scope[root] && ptype[w[1]] < bound && pointee[ptype[w[1]]] == atype[root]) {
            // OpStore ptr value [memory access] -> OpAtomicStore ptr scope semantics value
            out[k++] = (5u << 16) | OpAtomicStore; out[k++] = w[1]; out[k++] = scope[root]; out[k++] = zero; out[k++] = w[2];
            (*nfixed)++;
        } else { memcpy(out + k, w, n * sizeof *out); k += n; }
        i += n;
    }
    if (!*nfixed) { free(out); out = NULL; }
    else *out_words = k;
done:
    free(base); free(ptype); free(pointee); free(scope); free(atype); free(isint32);
    return out;
}
