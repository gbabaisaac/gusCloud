#ifndef GUS_OBJECT_STORE_H
#define GUS_OBJECT_STORE_H

#include <stdbool.h>
#include <stddef.h>

#define GUS_OBJECT_HASH_LENGTH 64

/* gus_dir is the path to an initialized .gus directory. On success, hash is
 * the lowercase SHA-256 digest followed by a NUL byte. NULL data is valid
 * only when length is zero. */
bool object_store_put(const char *gus_dir, const void *data, size_t length,
                      char hash[GUS_OBJECT_HASH_LENGTH + 1]);

/* Read to EOF without closing fd. Supports regular files, pipes, and stdin. */
bool object_store_put_fd(const char *gus_dir, int fd,
                         char hash[GUS_OBJECT_HASH_LENGTH + 1]);

#endif
