#ifndef GUS_GIT_CAPTURE_H
#define GUS_GIT_CAPTURE_H

#include <stdbool.h>

/* A hint is used only when the observed Git state has changed. The state,
 * event, and pointer-only checkpoint are committed in one SQLite transaction. */
typedef enum {
    GUS_CAPTURE_AUTO,
    GUS_CAPTURE_COMMIT,
    GUS_CAPTURE_CHECKOUT
} gus_capture_hint;

bool git_capture_reconcile(const char *database, const char *git_head,
                           const char *branch, gus_capture_hint hint);

#endif
