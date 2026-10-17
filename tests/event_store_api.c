#define _POSIX_C_SOURCE 200809L

#include "event_store.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/gus-event-store-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);

    int result = 1;
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    if (!event_store_initialize(path, "initial-head")) goto done;
    for (int type = GUS_EVENT_GIT_COMMIT; type <= GUS_EVENT_CHECKPOINT; type++) {
        int64_t id = 0;
        if (!event_store_append(path, (gus_event_type)type, "next-head", "gus-ref",
                                "payload-digest", "payload", &id) || id != type + 1)
            goto done;
    }
    if (event_store_append(path, GUS_EVENT_PROJECT_INIT, NULL, NULL, NULL, NULL, NULL))
        goto done;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
    const char *sql =
        "SELECT COUNT(*), SUM(parent=id-1), SUM(git_head='next-head'), "
        "SUM(gus_head='gus-ref'), SUM(payload_hash='payload-digest'), "
        "SUM(payload='payload') FROM events WHERE id > 1";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) goto done;
    int code = sqlite3_step(statement);
    if (code == SQLITE_ROW && sqlite3_column_int(statement, 0) == 9 &&
        sqlite3_column_int(statement, 1) == 9 && sqlite3_column_int(statement, 2) == 9 &&
        sqlite3_column_int(statement, 3) == 9 && sqlite3_column_int(statement, 4) == 9 &&
        sqlite3_column_int(statement, 5) == 9) result = 0;

done:
    sqlite3_finalize(statement);
    if (db) sqlite3_close(db);
    unlink(path);
    if (result != 0) fputs("event store API test failed\n", stderr);
    return result;
}
