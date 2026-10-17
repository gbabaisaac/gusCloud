#ifndef GUS_CHECKPOINT_STORE_H
#define GUS_CHECKPOINT_STORE_H

#include <stdbool.h>
#include <stdint.h>

/* Append a pointer-only checkpoint and its CHECKPOINT event in one transaction.
 * A NULL git_head represents an unborn branch; a NULL branch represents detached HEAD.
 * event_head is the ID of the newly appended CHECKPOINT event. */
bool checkpoint_store_append(const char *path, const char *git_head, const char *branch,
                             int64_t *checkpoint_id);

#endif
