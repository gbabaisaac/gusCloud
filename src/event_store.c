#include "event_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static const char *const schema =
    "CREATE TABLE events ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "type TEXT NOT NULL CHECK(length(type) > 0),"
    "timestamp TEXT NOT NULL,"
    "parent INTEGER REFERENCES events(id),"
    "git_head TEXT,"
    "gus_head TEXT,"
    "payload_hash TEXT,"
    "payload TEXT);";

static const char *const guards =
    "CREATE UNIQUE INDEX IF NOT EXISTS events_one_init ON events(type) "
    "WHERE type = 'PROJECT_INIT';"
    "CREATE TRIGGER IF NOT EXISTS events_no_update BEFORE UPDATE ON events "
    "BEGIN SELECT RAISE(ABORT, 'events are append-only'); END;"
    "CREATE TRIGGER IF NOT EXISTS events_no_delete BEFORE DELETE ON events "
    "BEGIN SELECT RAISE(ABORT, 'events are append-only'); END;";

static const char *const checkpoint_schema =
    "CREATE TABLE IF NOT EXISTS checkpoints ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "parent INTEGER REFERENCES checkpoints(id),"
    "git_head TEXT,"
    "branch TEXT,"
    "decision_head INTEGER REFERENCES events(id),"
    "event_head INTEGER NOT NULL UNIQUE REFERENCES events(id),"
    "created_at TEXT NOT NULL,"
    "CHECK(git_head IS NOT NULL OR branch IS NOT NULL));"
    "CREATE TRIGGER IF NOT EXISTS checkpoints_no_update BEFORE UPDATE ON checkpoints "
    "BEGIN SELECT RAISE(ABORT, 'checkpoints are append-only'); END;"
    "CREATE TRIGGER IF NOT EXISTS checkpoints_no_delete BEFORE DELETE ON checkpoints "
    "BEGIN SELECT RAISE(ABORT, 'checkpoints are append-only'); END;";

static const char *const event_names[] = {
    "PROJECT_INIT", "GIT_COMMIT", "GIT_CHECKOUT", "DECISION_CREATED",
    "DECISION_SUPERSEDED", "AGENT_STARTED", "PROMPT", "AGENT_RESPONSE",
    "FILE_EDIT", "CHECKPOINT"
};

static bool open_database(const char *path, sqlite3 **db, int flags) {
    int code = sqlite3_open_v2(path, db, flags, NULL);
    if (code == SQLITE_OK) code = sqlite3_exec(*db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    if (code == SQLITE_OK) return true;
    fprintf(stderr, "gus: SQLite: %s\n", *db ? sqlite3_errmsg(*db) : "open failed");
    sqlite3_close(*db);
    *db = NULL;
    return false;
}

static bool execute(sqlite3 *db, const char *sql) {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code == SQLITE_OK) return true;
    fprintf(stderr, "gus: SQLite: %s\n", message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return false;
}

/* Return 0 for a new database, 1 for the original three-column schema, 2 for
 * the current schema, and -1 for a layout we cannot migrate without loss. */
static int schema_version(sqlite3 *db) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(events)", -1, &statement, NULL) != SQLITE_OK)
        return -1;
    unsigned seen = 0;
    int columns = 0;
    const char *names[] = {"id", "kind", "created_at", "type", "timestamp", "parent",
                           "git_head", "gus_head", "payload_hash", "payload"};
    int code;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(statement, 1);
        columns++;
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (name && strcmp(name, names[i]) == 0) seen |= 1u << i;
        }
    }
    sqlite3_finalize(statement);
    if (code != SQLITE_DONE) return -1;
    if (columns == 0) return 0;
    if (columns == 3 && seen == 0x7u) return 1;
    if (columns == 8 && seen == 0x3f9u) return 2;
    return -1;
}

/* A legacy events layout cannot contain valid checkpoint references. An empty
 * checkpoints table may remain after a legacy table is restored into a newer
 * database; remove it before renaming events so its foreign key is rebuilt. */
static bool prepare_legacy_checkpoints(sqlite3 *db) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='checkpoints'",
        -1, &statement, NULL);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    bool exists = code == SQLITE_ROW && sqlite3_column_int(statement, 0) != 0;
    sqlite3_finalize(statement);
    if (code != SQLITE_ROW) return false;
    if (!exists) return true;
    code = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM checkpoints", -1, &statement, NULL);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    bool empty = code == SQLITE_ROW && sqlite3_column_int(statement, 0) == 0;
    sqlite3_finalize(statement);
    if (!empty) {
        fputs("gus: cannot migrate legacy events with existing checkpoints\n", stderr);
        return false;
    }
    return execute(db, "DROP TABLE checkpoints;");
}

bool event_store_initialize(const char *path, const char *git_head) {
    sqlite3 *db = NULL;
    if (!open_database(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) return false;
    bool ok = execute(db, "BEGIN IMMEDIATE;");
    if (ok) {
        int version = schema_version(db);
        if (version == 0) ok = execute(db, schema);
        else if (version == 1) {
            ok = prepare_legacy_checkpoints(db) &&
                 execute(db, "ALTER TABLE events RENAME TO events_legacy;") &&
                 execute(db, schema) &&
                 execute(db,
                     "INSERT INTO events(id,type,timestamp,parent) "
                     "SELECT e.id, CASE e.kind WHEN 'init' THEN 'PROJECT_INIT' ELSE e.kind END,"
                     "e.created_at, (SELECT MAX(p.id) FROM events_legacy p WHERE p.id < e.id) "
                     "FROM events_legacy e ORDER BY e.id;") &&
                 execute(db, "DROP TABLE events_legacy;");
        } else if (version != 2) {
            fprintf(stderr, "gus: unsupported events schema; database left unchanged\n");
            ok = false;
        }
    }
    if (ok) ok = execute(db, guards) && execute(db, checkpoint_schema);
    if (ok) {
        sqlite3_stmt *statement = NULL;
        int code = sqlite3_prepare_v2(db,
            "INSERT INTO events(type,timestamp,parent,git_head) "
            "SELECT 'PROJECT_INIT', strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
            "(SELECT MAX(id) FROM events), ? "
            "WHERE NOT EXISTS (SELECT 1 FROM events WHERE type='PROJECT_INIT')",
            -1, &statement, NULL);
        if (code == SQLITE_OK && git_head)
            code = sqlite3_bind_text(statement, 1, git_head, -1, SQLITE_TRANSIENT);
        if (code == SQLITE_OK) code = sqlite3_step(statement);
        if (code != SQLITE_DONE) {
            fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
            ok = false;
        }
        sqlite3_finalize(statement);
    }
    if (ok) ok = execute(db, "COMMIT;");
    else execute(db, "ROLLBACK;");
    sqlite3_close(db);
    return ok;
}

bool event_store_append(const char *path, gus_event_type type, const char *git_head,
                        const char *gus_head, const char *payload_hash,
                        const char *payload, int64_t *id) {
    if (type <= GUS_EVENT_PROJECT_INIT || type > GUS_EVENT_CHECKPOINT) {
        fprintf(stderr, "gus: invalid append event type\n");
        return false;
    }
    sqlite3 *db = NULL;
    if (!open_database(path, &db, SQLITE_OPEN_READWRITE)) return false;
    bool ok = execute(db, "BEGIN IMMEDIATE;");
    sqlite3_stmt *statement = NULL;
    int code = SQLITE_ERROR;
    if (ok) {
        code = sqlite3_prepare_v2(db,
            "INSERT INTO events(type,timestamp,parent,git_head,gus_head,payload_hash,payload) "
            "VALUES (?,strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
            "(SELECT MAX(id) FROM events),?,?,?,?)", -1, &statement, NULL);
        if (code == SQLITE_OK) code = sqlite3_bind_text(statement, 1, event_names[type], -1, SQLITE_STATIC);
        if (code == SQLITE_OK && git_head)
            code = sqlite3_bind_text(statement, 2, git_head, -1, SQLITE_TRANSIENT);
        if (code == SQLITE_OK && gus_head)
            code = sqlite3_bind_text(statement, 3, gus_head, -1, SQLITE_TRANSIENT);
        if (code == SQLITE_OK && payload_hash)
            code = sqlite3_bind_text(statement, 4, payload_hash, -1, SQLITE_TRANSIENT);
        if (code == SQLITE_OK && payload)
            code = sqlite3_bind_text(statement, 5, payload, -1, SQLITE_TRANSIENT);
        if (code == SQLITE_OK) code = sqlite3_step(statement);
        if (code != SQLITE_DONE) {
            fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
            ok = false;
        }
    }
    sqlite3_finalize(statement);
    int64_t inserted_id = ok ? sqlite3_last_insert_rowid(db) : 0;
    if (ok) ok = execute(db, "COMMIT;");
    else execute(db, "ROLLBACK;");
    if (ok && id) *id = inserted_id;
    sqlite3_close(db);
    return ok;
}

bool event_store_count(const char *path, int *count) {
    sqlite3 *db = NULL;
    if (!open_database(path, &db, SQLITE_OPEN_READONLY)) return false;
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM events", -1, &statement, NULL);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    if (code == SQLITE_ROW) *count = sqlite3_column_int(statement, 0);
    else fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return code == SQLITE_ROW;
}

bool event_store_log(const char *path) {
    sqlite3 *db = NULL;
    if (!open_database(path, &db, SQLITE_OPEN_READONLY)) return false;
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db,
        "SELECT e.id,e.timestamp,e.type,e.git_head,c.id,c.parent,c.branch "
        "FROM events e LEFT JOIN checkpoints c ON c.event_head=e.id ORDER BY e.id DESC",
        -1, &statement, NULL);
    if (code == SQLITE_OK) {
        while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
            const unsigned char *timestamp = sqlite3_column_text(statement, 1);
            const unsigned char *type = sqlite3_column_text(statement, 2);
            const unsigned char *git_head = sqlite3_column_text(statement, 3);
            printf("%lld  %s  %s", sqlite3_column_int64(statement, 0), timestamp, type);
            if (git_head) printf("  git:%s", git_head);
            else if (sqlite3_column_type(statement, 4) != SQLITE_NULL) printf("  git:(unborn)");
            if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
                printf("  checkpoint:%lld", sqlite3_column_int64(statement, 4));
                if (sqlite3_column_type(statement, 5) != SQLITE_NULL)
                    printf("  parent-checkpoint:%lld", sqlite3_column_int64(statement, 5));
                const unsigned char *branch = sqlite3_column_text(statement, 6);
                printf("  branch:%s", branch ? (const char *)branch : "(detached)");
            }
            putchar('\n');
        }
    }
    if (code != SQLITE_DONE) fprintf(stderr, "gus: SQLite: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return code == SQLITE_DONE;
}
