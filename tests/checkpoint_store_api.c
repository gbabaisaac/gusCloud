#define _POSIX_C_SOURCE 200809L

#include "checkpoint_store.h"
#include "event_store.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW)
        result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

int main(void) {
    char path[] = "/tmp/gus-checkpoint-store-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);

    int result = 1;
    sqlite3 *db = NULL;
    int64_t first = 0, second = 0, decision = 0;
    if (!event_store_initialize(path, NULL) ||
        !checkpoint_store_append(path, NULL, "main", &first) || first != 1 ||
        !event_store_append(path, GUS_EVENT_DECISION_CREATED, NULL, NULL, NULL, NULL,
                            &decision) || decision != 3 ||
        !checkpoint_store_append(path, "abc123", NULL, &second) || second != 2 ||
        !event_store_initialize(path, NULL)) goto done;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) goto done;
    if (scalar(db, "SELECT COUNT(*) FROM checkpoints") != 2 ||
        scalar(db, "SELECT COUNT(*) FROM events") != 4 ||
        scalar(db, "SELECT COUNT(*) FROM checkpoints WHERE id=1 AND parent IS NULL "
                   "AND git_head IS NULL AND branch='main' AND decision_head IS NULL "
                   "AND event_head=2 AND created_at=(SELECT timestamp FROM events WHERE id=2)") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM checkpoints WHERE id=2 AND parent=1 "
                   "AND git_head='abc123' AND branch IS NULL AND decision_head=3 "
                   "AND event_head=4 AND created_at=(SELECT timestamp FROM events WHERE id=4)") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM events WHERE type='CHECKPOINT' "
                   "AND payload IS NULL AND payload_hash IS NULL AND gus_head IS NULL") != 2)
        goto done;
    if (sqlite3_exec(db, "UPDATE checkpoints SET branch='other' WHERE id=1", NULL, NULL, NULL) == SQLITE_OK ||
        sqlite3_exec(db, "DELETE FROM checkpoints WHERE id=1", NULL, NULL, NULL) == SQLITE_OK)
        goto done;
    if (sqlite3_exec(db,
        "CREATE TRIGGER reject_checkpoint BEFORE INSERT ON checkpoints "
        "BEGIN SELECT RAISE(ABORT, 'injected failure'); END;", NULL, NULL, NULL) != SQLITE_OK)
        goto done;
    sqlite3_close(db);
    db = NULL;
    if (checkpoint_store_append(path, "abc123", "main", NULL)) goto done;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
    if (scalar(db, "SELECT COUNT(*) FROM events") != 4 ||
        scalar(db, "SELECT COUNT(*) FROM checkpoints") != 2) goto done;
    result = 0;

done:
    if (db) sqlite3_close(db);
    unlink(path);
    if (result != 0) fputs("checkpoint store API test failed\n", stderr);
    return result;
}
