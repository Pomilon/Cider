/* rt_native.c — the native runtime: print, panic, alloc, realloc, free.
 *
 * MEMORY.md section 2 owns this contract:
 *   - alloc(size, align) answers nilPtr when it cannot satisfy a request;
 *     dereferencing that answer traps by the same rule as any null pointer.
 *   - Every block carries a size field and canary bytes at both ends; free
 *     verifies them and a mismatch aborts the process.
 *   - free(nilPtr) is a defined no-op.
 *
 * Layout of one block, all offsets from the user pointer:
 *
 *     [-24]  size field  (uint64_t, the user-visible size)
 *     [-16]  base pointer (the raw pointer the allocator owns and releases)
 *     [ -8]  front canary
 *     [  0]  user bytes ............................
 *     [size] rear canary
 *
 * The user pointer itself carries the requested alignment: the first aligned
 * address at or after raw+24, with the raw base stored so release never
 * recomputes it. Overhead is 40 bytes plus alignment padding, every byte of
 * it accounted for here rather than assumed.
 *
 * This file is plain C with no Cider dependency: the compiler is not built
 * yet when the runtime must already link, and the runtime must never import
 * the thing it serves.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A value the program can never legally hold: recognizable in dumps, and not
 * a valid size, address, or length, so a canary that leaks into program data
 * fails loudly downstream instead of passing quietly. */
#define CIDER_CANARY ((uint64_t)0x9E3779B97F4A7C15UL)

typedef struct {
    const char *data;
    int64_t len;
} cider_string_t;

static void write_all(int fd, const char *data, int64_t len) {
    while (len > 0) {
        ssize_t wrote = write(fd, data, (size_t)len);
        if (wrote <= 0) {
            /* The descriptor is gone; there is nowhere to report it. A print
             * that cannot print is not a fault in the program. */
            return;
        }
        data += wrote;
        len -= wrote;
    }
}

void cider_print(cider_string_t s) {
    if (s.data == 0 || s.len <= 0) {
        return;
    }
    write_all(STDOUT_FILENO, s.data, s.len);
}

void cider_panic(cider_string_t s) {
    if (s.data != 0 && s.len > 0) {
        write_all(STDERR_FILENO, s.data, s.len);
        write_all(STDERR_FILENO, "\n", 1);
    }
    abort();
}

static uint64_t checked_add(uint64_t a, uint64_t b, int *overflow) {
    uint64_t out = a + b;
    if (out < a) {
        *overflow = 1;
    }
    return out;
}

void *cider_alloc(int64_t size, int64_t align) {
    if (size < 0 || align <= 0) {
        return 0;
    }
    /* Sixteen is the floor: every user pointer satisfies at least the
     * platform's max alignment, whatever smaller value was asked for. */
    uint64_t eff = (uint64_t)align < 16 ? 16 : (uint64_t)align;
    /* 24 bytes of header, 8 of rear canary, plus room to align the user. */
    uint64_t need = (uint64_t)size;
    int overflow = 0;
    need = checked_add(need, 40, &overflow);
    if (!overflow) {
        need = checked_add(need, eff, &overflow);
    }
    if (overflow) {
        return 0;
    }
    void *raw = 0;
    if (eff <= 16) {
        raw = malloc(need);
    } else {
        /* aligned_alloc needs a size that is a multiple of the alignment. */
        uint64_t total = (need + eff - 1) & ~(eff - 1);
        if (total < need) {
            return 0;
        }
        raw = aligned_alloc((size_t)eff, (size_t)total);
    }
    if (raw == 0) {
        return 0;
    }
    uintptr_t user = (((uintptr_t)raw + 24 + eff - 1) & ~(eff - 1));
    uint64_t *at = (uint64_t *)(user - 24);
    at[0] = (uint64_t)size;
    at[1] = (uint64_t)raw;
    at[2] = CIDER_CANARY;
    char *bytes = (char *)user;
    memcpy(bytes + (uint64_t)size, &((uint64_t){CIDER_CANARY}), sizeof(uint64_t));
    return (void *)user;
}

static int verify(const char *user) {
    const uint64_t *at = (const uint64_t *)(user - 24);
    uint64_t size = at[0];
    uint64_t canary = CIDER_CANARY;
    if (at[2] != canary) {
        return 0;
    }
    if (memcmp(user + size, &canary, sizeof(uint64_t)) != 0) {
        return 0;
    }
    return 1;
}

static uint64_t block_size(const char *user) {
    return ((const uint64_t *)(user - 24))[0];
}

static void *block_base(const char *user) {
    return (void *)((const uint64_t *)(user - 24))[1];
}

void cider_free(void *p) {
    if (p == 0) {
        return;
    }
    char *user = (char *)p;
    if (!verify(user)) {
        abort();
    }
    uint64_t size = block_size(user);
    void *base = block_base(user);
    /* Scrub the canaries so a use-after-free reads dead markers, then release
     * the base the allocator actually owns. */
    uint64_t dead = (uint64_t)0xDEADDEADDEADDEADUL;
    memcpy(user - 8, &dead, sizeof(uint64_t));
    memcpy(user + size, &dead, sizeof(uint64_t));
    free(base);
}

void *cider_realloc(void *p, int64_t size, int64_t align) {
    if (p == 0) {
        return cider_alloc(size, align);
    }
    if (size < 0) {
        return 0;
    }
    char *user = (char *)p;
    if (!verify(user)) {
        abort();
    }
    uint64_t old_size = block_size(user);
    void *grown = cider_alloc(size, align);
    if (grown == 0) {
        return 0;
    }
    uint64_t keep = old_size < (uint64_t)size ? old_size : (uint64_t)size;
    memcpy(grown, user, (size_t)keep);
    cider_free(p);
    return grown;
}
