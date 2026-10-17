#ifndef GUS_EVENT_STORE_H
#define GUS_EVENT_STORE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GUS_EVENT_PROJECT_INIT,
    GUS_EVENT_GIT_COMMIT,
    GUS_EVENT_GIT_CHECKOUT,
    GUS_EVENT_DECISION_CREATED,
    GUS_EVENT_DECISION_SUPERSEDED,
    GUS_EVENT_AGENT_STARTED,
    GUS_EVENT_PROMPT,
    GUS_EVENT_AGENT_RESPONSE,
    GUS_EVENT_FILE_EDIT,
    GUS_EVENT_CHECKPOINT
} gus_event_type;

bool event_store_initialize(const char *path, const char *git_head);
bool event_store_append(const char *path, gus_event_type type, const char *git_head,
                        const char *gus_head, const char *payload_hash,
                        const char *payload, int64_t *id);
bool event_store_count(const char *path, int *count);
bool event_store_log(const char *path);

#endif
