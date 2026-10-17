#include "git_capture.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static bool execute(sqlite3 *db, const char *sql) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code == SQLITE_OK) return true;
    fprintf(stderr, "gus: SQLite: %s\n", message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return false;
}

static bool equal_nullable(const char *left, const char *right) {
    return (!left && !right) || (left && right && strcmp(left, right) == 0);
}

static bool append_event(sqlite3 *db, const char *type, const char *head,
                         sqlite3_int64 *id) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db,
        "INSERT INTO events(type,timestamp,parent,git_head) "
        "VALUES (?,strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
        "(SELECT MAX(id) FROM events),?)", -1, &statement, NULL);
    if (code == SQLITE_OK) code = sqlite3_bind_text(statement, 1, type, -1, SQLITE_STATIC);
    if (code == SQLITE_OK && head)
        code = sqlite3_bind_text(statement, 2, head, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    if (code == SQLITE_DONE) *id = sqlite3_last_insert_rowid(db);
    else fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    return code == SQLITE_DONE;
}

static bool append_checkpoint(sqlite3 *db, const char *head, const char *branch,
                              sqlite3_int64 event_id) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db,
        "INSERT INTO checkpoints(parent,git_head,branch,decision_head,event_head,created_at) "
        "SELECT (SELECT MAX(id) FROM checkpoints),?,?,"
        "(SELECT MAX(id) FROM events WHERE type IN ('DECISION_CREATED','DECISION_SUPERSEDED')),"
        "id,timestamp FROM events WHERE id=?", -1, &statement, NULL);
    if (code == SQLITE_OK && head)
        code = sqlite3_bind_text(statement, 1, head, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK && branch)
        code = sqlite3_bind_text(statement, 2, branch, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_bind_int64(statement, 3, event_id);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    if (code != SQLITE_DONE) fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    return code == SQLITE_DONE;
}

static bool set_state(sqlite3 *db, const char *head, const char *branch) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db,
        "INSERT INTO git_capture_state(id,git_head,branch) VALUES (1,?,?) "
        "ON CONFLICT(id) DO UPDATE SET git_head=excluded.git_head,branch=excluded.branch",
        -1, &statement, NULL);
    if (code == SQLITE_OK && head)
        code = sqlite3_bind_text(statement, 1, head, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK && branch)
        code = sqlite3_bind_text(statement, 2, branch, -1, SQLITE_TRANSIENT);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    if (code != SQLITE_DONE) fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    return code == SQLITE_DONE;
}

bool git_capture_reconcile(const char *database, const char *git_head,
                           const char *branch, gus_capture_hint hint) {
    if (!git_head && !branch) {
        fputs("gus: could not resolve Git HEAD or branch\n", stderr);
        return false;
    }
    sqlite3 *db = NULL;
    int code = sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE, NULL);
    if (code == SQLITE_OK) code = sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    if (code == SQLITE_OK) code = sqlite3_busy_timeout(db, 5000);
    if (code != SQLITE_OK) {
        fprintf(stderr, "gus: SQLite: %s\n", db ? sqlite3_errmsg(db) : "open failed");
        sqlite3_close(db);
        return false;
    }
    bool ok = execute(db, "BEGIN IMMEDIATE;") && execute(db,
        "CREATE TABLE IF NOT EXISTS git_capture_state ("
        "id INTEGER PRIMARY KEY CHECK(id=1),git_head TEXT,branch TEXT,"
        "CHECK(git_head IS NOT NULL OR branch IS NOT NULL));");
    sqlite3_stmt *statement = NULL;
    bool had_state = false;
    bool stored_state = false;
    char *old_head = NULL, *old_branch = NULL;
    if (ok) {
        code = sqlite3_prepare_v2(db,
            "SELECT git_head,branch FROM git_capture_state WHERE id=1", -1, &statement, NULL);
        if (code == SQLITE_OK) code = sqlite3_step(statement);
        if (code == SQLITE_ROW) {
            had_state = true;
            stored_state = true;
            const char *head = (const char *)sqlite3_column_text(statement, 0);
            const char *name = (const char *)sqlite3_column_text(statement, 1);
            if (head) old_head = sqlite3_mprintf("%s", head);
            if (name) old_branch = sqlite3_mprintf("%s", name);
            if ((head && !old_head) || (name && !old_branch)) ok = false;
        } else if (code != SQLITE_DONE) ok = false;
        sqlite3_finalize(statement);
    }
    if (ok && !had_state) {
        code = sqlite3_prepare_v2(db,
            "SELECT git_head,branch FROM checkpoints ORDER BY id DESC LIMIT 1",
            -1, &statement, NULL);
        if (code == SQLITE_OK) code = sqlite3_step(statement);
        if (code == SQLITE_ROW) {
            const char *head = (const char *)sqlite3_column_text(statement, 0);
            const char *name = (const char *)sqlite3_column_text(statement, 1);
            had_state = true;
            if (head) old_head = sqlite3_mprintf("%s", head);
            if (name) old_branch = sqlite3_mprintf("%s", name);
            if ((head && !old_head) || (name && !old_branch)) ok = false;
        } else if (code != SQLITE_DONE) ok = false;
        sqlite3_finalize(statement);
    }
    if (ok && !had_state) {
        code = sqlite3_prepare_v2(db,
            "SELECT git_head FROM events WHERE type IN "
            "('PROJECT_INIT','GIT_COMMIT','GIT_CHECKOUT') ORDER BY id DESC LIMIT 1",
            -1, &statement, NULL);
        if (code == SQLITE_OK) code = sqlite3_step(statement);
        if (code == SQLITE_ROW) {
            const char *head = (const char *)sqlite3_column_text(statement, 0);
            had_state = true;
            if (head) old_head = sqlite3_mprintf("%s", head);
            if (branch) old_branch = sqlite3_mprintf("%s", branch);
            if ((head && !old_head) || (branch && !old_branch)) ok = false;
        } else if (code != SQLITE_DONE) ok = false;
        sqlite3_finalize(statement);
    }
    if (ok && !had_state) {
        /* A database with no prior Git event establishes its first baseline. */
        ok = set_state(db, git_head, branch);
    } else if (ok && (!equal_nullable(old_head, git_head) ||
                      !equal_nullable(old_branch, branch))) {
        const char *type = hint == GUS_CAPTURE_COMMIT ? "GIT_COMMIT" :
            hint == GUS_CAPTURE_CHECKOUT ? "GIT_CHECKOUT" :
            equal_nullable(old_branch, branch) && git_head &&
            !equal_nullable(old_head, git_head) ? "GIT_COMMIT" : "GIT_CHECKOUT";
        sqlite3_int64 event_id = 0;
        ok = append_event(db, type, git_head, &event_id) &&
             append_event(db, "CHECKPOINT", git_head, &event_id) &&
             append_checkpoint(db, git_head, branch, event_id) &&
             set_state(db, git_head, branch);
    } else if (ok && !stored_state) {
        ok = set_state(db, git_head, branch);
    }
    sqlite3_free(old_head);
    sqlite3_free(old_branch);
    if (ok) ok = execute(db, "COMMIT;");
    if (!ok) execute(db, "ROLLBACK;");
    sqlite3_close(db);
    return ok;
}
