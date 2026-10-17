#include "checkpoint_store.h"

#include <sqlite3.h>
#include <stdio.h>

static bool execute(sqlite3 *db, const char *sql) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code == SQLITE_OK) return true;
    fprintf(stderr, "gus: SQLite: %s\n", message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return false;
}

static bool insert_event(sqlite3 *db, const char *git_head, sqlite3_int64 *event_id) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db,
        "INSERT INTO events(type,timestamp,parent,git_head) "
        "VALUES ('CHECKPOINT',strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
        "(SELECT MAX(id) FROM events),?)", -1, &statement, NULL);
    if (code == SQLITE_OK && git_head)
        code = sqlite3_bind_text(statement, 1, git_head, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    if (code == SQLITE_DONE) *event_id = sqlite3_last_insert_rowid(db);
    else fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    return code == SQLITE_DONE;
}

static bool insert_checkpoint(sqlite3 *db, const char *git_head, const char *branch,
                              sqlite3_int64 event_id, int64_t *checkpoint_id) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db,
        "INSERT INTO checkpoints(parent,git_head,branch,decision_head,event_head,created_at) "
        "SELECT (SELECT MAX(id) FROM checkpoints),?,?,"
        "(SELECT MAX(id) FROM events WHERE type IN ('DECISION_CREATED','DECISION_SUPERSEDED')),"
        "id,timestamp FROM events WHERE id=?", -1, &statement, NULL);
    if (code == SQLITE_OK && git_head)
        code = sqlite3_bind_text(statement, 1, git_head, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK && branch)
        code = sqlite3_bind_text(statement, 2, branch, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_int64(statement, 3, event_id);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    if (code == SQLITE_DONE) *checkpoint_id = sqlite3_last_insert_rowid(db);
    else fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    return code == SQLITE_DONE;
}

bool checkpoint_store_append(const char *path, const char *git_head, const char *branch,
                             int64_t *checkpoint_id) {
    if (!git_head && !branch) {
        fputs("gus: checkpoint requires a Git branch or HEAD\n", stderr);
        return false;
    }
    sqlite3 *db = NULL;
    int code = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL);
    if (code == SQLITE_OK) code = sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    if (code != SQLITE_OK) {
        fprintf(stderr, "gus: SQLite: %s\n", db ? sqlite3_errmsg(db) : "open failed");
        sqlite3_close(db);
        return false;
    }
    bool ok = execute(db, "BEGIN IMMEDIATE;");
    sqlite3_int64 event_id = 0;
    int64_t inserted_id = 0;
    if (ok) ok = insert_event(db, git_head, &event_id);
    if (ok) ok = insert_checkpoint(db, git_head, branch, event_id, &inserted_id);
    if (ok) ok = execute(db, "COMMIT;");
    if (!ok) execute(db, "ROLLBACK;");
    if (ok && checkpoint_id) *checkpoint_id = inserted_id;
    sqlite3_close(db);
    return ok;
}
