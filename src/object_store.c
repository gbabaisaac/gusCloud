#define _POSIX_C_SOURCE 200809L

#include "object_store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} sha256_state;

static const uint32_t round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotate(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

static void sha256_block(sha256_state *sha, const unsigned char block[64]) {
    uint32_t words[64];
    for (unsigned i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
                   ((uint32_t)block[4 * i + 2] << 8) | block[4 * i + 3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t a = words[i - 15], b = words[i - 2];
        words[i] = words[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) +
                   words[i - 7] + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    uint32_t a = sha->state[0], b = sha->state[1], c = sha->state[2], d = sha->state[3];
    uint32_t e = sha->state[4], f = sha->state[5], g = sha->state[6], h = sha->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t first = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                         choose + round_constants[i] + words[i];
        uint32_t second = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + majority;
        h = g; g = f; f = e; e = d + first;
        d = c; c = b; b = a; a = first + second;
    }
    sha->state[0] += a; sha->state[1] += b; sha->state[2] += c; sha->state[3] += d;
    sha->state[4] += e; sha->state[5] += f; sha->state[6] += g; sha->state[7] += h;
}

static void sha256_init(sha256_state *sha) {
    const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(sha->state, initial, sizeof(initial));
    sha->length = 0;
    sha->used = 0;
}

static bool sha256_update(sha256_state *sha, const void *data, size_t length) {
    if (length > UINT64_MAX / 8 - sha->length) return false;
    sha->length += length;
    const unsigned char *bytes = data;
    while (length) {
        size_t count = sizeof(sha->block) - sha->used;
        if (count > length) count = length;
        memcpy(sha->block + sha->used, bytes, count);
        sha->used += count;
        bytes += count;
        length -= count;
        if (sha->used == sizeof(sha->block)) {
            sha256_block(sha, sha->block);
            sha->used = 0;
        }
    }
    return true;
}

static void sha256_finish(sha256_state *sha, unsigned char digest[32]) {
    uint64_t bits = sha->length * 8;
    sha->block[sha->used++] = 0x80;
    if (sha->used > 56) {
        memset(sha->block + sha->used, 0, 64 - sha->used);
        sha256_block(sha, sha->block);
        sha->used = 0;
    }
    memset(sha->block + sha->used, 0, 56 - sha->used);
    for (unsigned i = 0; i < 8; i++) sha->block[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_block(sha, sha->block);
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 4; j++)
            digest[4 * i + j] = (unsigned char)(sha->state[i] >> (24 - 8 * j));
    }
}

static void digest_hex(const unsigned char digest[32], char hash[65]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        hash[2 * i] = hex[digest[i] >> 4];
        hash[2 * i + 1] = hex[digest[i] & 15];
    }
    hash[64] = '\0';
}

static bool path_join(char *out, size_t capacity, const char *dir, const char *name) {
    int count = snprintf(out, capacity, "%s/%s", dir, name);
    if (count >= 0 && (size_t)count < capacity) return true;
    fprintf(stderr, "gus: object path too long\n");
    return false;
}

static bool write_all(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length) {
        size_t chunk = length < 8192 ? length : 8192;
        ssize_t count = write(fd, bytes, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { perror("gus: object write"); return false; }
        bytes += count;
        length -= (size_t)count;
    }
    return true;
}

static bool verify_existing(const char *path, const char *expected) {
    struct stat before, after;
    if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode)) {
        fprintf(stderr, "gus: existing object is not a regular file: %s\n", path);
        return false;
    }
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) { perror(path); return false; }
    bool ok = fstat(fd, &after) == 0 && S_ISREG(after.st_mode) &&
              before.st_dev == after.st_dev && before.st_ino == after.st_ino;
    sha256_state sha;
    sha256_init(&sha);
    unsigned char buffer[8192];
    ssize_t count;
    while (ok && (count = read(fd, buffer, sizeof(buffer))) != 0) {
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || !sha256_update(&sha, buffer, (size_t)count)) ok = false;
    }
    if (close(fd) != 0) ok = false;
    if (ok) {
        unsigned char digest[32];
        char actual[65];
        sha256_finish(&sha, digest);
        digest_hex(digest, actual);
        ok = strcmp(actual, expected) == 0;
    }
    if (!ok) fprintf(stderr, "gus: existing object failed verification: %s\n", path);
    return ok;
}

static bool finish_object(const char *gus_dir, char *temporary, int fd,
                          sha256_state *sha, char hash[65]) {
    unsigned char digest[32];
    char computed[65], objects[PATH_MAX], prefix[PATH_MAX], destination[PATH_MAX];
    char bucket[3];
    sha256_finish(sha, digest);
    digest_hex(digest, computed);
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok) { perror("gus: object sync"); goto done; }
    bucket[0] = computed[0];
    bucket[1] = computed[1];
    bucket[2] = '\0';
    if (!path_join(objects, sizeof(objects), gus_dir, "objects") ||
        !path_join(prefix, sizeof(prefix), objects, bucket)) { ok = false; goto done; }
    if (mkdir(prefix, 0755) != 0 && errno != EEXIST) {
        perror(prefix); ok = false; goto done;
    }
    if (!path_join(destination, sizeof(destination), prefix, computed + 2)) {
        ok = false; goto done;
    }
    if (link(temporary, destination) != 0) {
        int link_error = errno;
        if (link_error != EEXIST || !verify_existing(destination, computed)) {
            if (link_error != EEXIST) { errno = link_error; perror(destination); }
            ok = false;
            goto done;
        }
    } else {
        int dir = open(prefix, O_RDONLY);
        if (dir < 0 || fsync(dir) != 0) ok = false;
        if (dir >= 0 && close(dir) != 0) ok = false;
        if (!ok) { perror("gus: object directory sync"); goto done; }
    }
    memcpy(hash, computed, sizeof(computed));
done:
    if (unlink(temporary) != 0) { perror(temporary); ok = false; }
    return ok;
}

static bool begin_object(const char *gus_dir, char temporary[PATH_MAX], int *fd) {
    char tmp_dir[PATH_MAX];
    if (!path_join(tmp_dir, sizeof(tmp_dir), gus_dir, "tmp") ||
        !path_join(temporary, PATH_MAX, tmp_dir, "object-XXXXXX")) return false;
    *fd = mkstemp(temporary);
    if (*fd >= 0) return true;
    perror(temporary);
    return false;
}

bool object_store_put(const char *gus_dir, const void *data, size_t length, char hash[65]) {
    if (!gus_dir || !hash || (!data && length)) return false;
    char temporary[PATH_MAX];
    int fd;
    if (!begin_object(gus_dir, temporary, &fd)) return false;
    sha256_state sha;
    sha256_init(&sha);
    bool ok = sha256_update(&sha, data, length) && write_all(fd, data, length);
    if (ok) return finish_object(gus_dir, temporary, fd, &sha, hash);
    close(fd);
    unlink(temporary);
    return false;
}

bool object_store_put_fd(const char *gus_dir, int input, char hash[65]) {
    if (!gus_dir || !hash || input < 0) return false;
    char temporary[PATH_MAX];
    int fd;
    if (!begin_object(gus_dir, temporary, &fd)) return false;
    sha256_state sha;
    sha256_init(&sha);
    bool ok = true;
    unsigned char buffer[8192];
    while (ok) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { perror("gus: object input"); ok = false; break; }
        if (count == 0) break;
        ok = sha256_update(&sha, buffer, (size_t)count) && write_all(fd, buffer, (size_t)count);
    }
    if (ok) return finish_object(gus_dir, temporary, fd, &sha, hash);
    close(fd);
    unlink(temporary);
    return false;
}
