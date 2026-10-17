#define _POSIX_C_SOURCE 200809L

#include "event_store.h"
#include "object_store.h"

#include <dirent.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_count(const char *path) {
    DIR *directory = opendir(path);
    if (!directory) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
    }
    closedir(directory);
    return count;
}

int main(void) {
    char root[] = "/tmp/gus-object-store-XXXXXX";
    int reserved = mkstemp(root);
    if (reserved < 0) return 1;
    close(reserved);
    if (unlink(root) != 0 || mkdir(root, 0700) != 0) return 1;
    char gus[256], objects[256], tmp[256], abc_path[256] = "", empty_path[256] = "";
    char binary_path[256] = "", million_path[256] = "", prefix[256], db_path[256] = "";
    snprintf(gus, sizeof(gus), "%s/.gus", root);
    snprintf(objects, sizeof(objects), "%s/objects", gus);
    snprintf(tmp, sizeof(tmp), "%s/tmp", gus);
    if (mkdir(gus, 0755) || mkdir(objects, 0755) || mkdir(tmp, 0755)) return 1;

    int result = 1;
    char abc[65], repeated[65], empty[65], binary[65] = "", streamed[65];
    char million_hash[65] = "";
    static const unsigned char bytes[] = {0, 1, 0xff, 0, 'x'};
    if (!object_store_put(gus, "abc", 3, abc) ||
        strcmp(abc, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
        goto done;
    snprintf(prefix, sizeof(prefix), "%s/ba", objects);
    snprintf(abc_path, sizeof(abc_path), "%s/%s", prefix, abc + 2);
    if (file_count(prefix) != 1) goto done;
    for (int i = 0; i < 10000; i++) {
        if (!object_store_put(gus, "abc", 3, repeated) || strcmp(repeated, abc)) goto done;
    }
    if (file_count(prefix) != 1 || file_count(tmp) != 0) goto done;

    if (!object_store_put(gus, NULL, 0, empty) ||
        strcmp(empty, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))
        goto done;
    snprintf(prefix, sizeof(prefix), "%s/e3", objects);
    snprintf(empty_path, sizeof(empty_path), "%s/%s", prefix, empty + 2);
    if (file_count(prefix) != 1) goto done;
    if (!object_store_put(gus, bytes, sizeof(bytes), binary)) goto done;
    snprintf(prefix, sizeof(prefix), "%s/%.2s", objects, binary);
    snprintf(binary_path, sizeof(binary_path), "%s/%s", prefix, binary + 2);
    FILE *file = fopen(binary_path, "rb");
    unsigned char actual[sizeof(bytes)];
    if (!file || fread(actual, 1, sizeof(actual), file) != sizeof(actual) ||
        memcmp(actual, bytes, sizeof(bytes)) || fgetc(file) != EOF) {
        if (file) fclose(file);
        goto done;
    }
    fclose(file);
    int pipefd[2];
    if (pipe(pipefd) != 0) goto done;
    if (write(pipefd[1], bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        close(pipefd[0]); close(pipefd[1]); goto done;
    }
    close(pipefd[1]);
    int streamed_ok = object_store_put_fd(gus, pipefd[0], streamed);
    close(pipefd[0]);
    if (!streamed_ok || strcmp(streamed, binary) || file_count(prefix) != 1) goto done;

    /* The million-byte standard vector crosses SHA-256 blocks and input chunks. */
    unsigned char *million = malloc(1000000);
    if (!million) goto done;
    memset(million, 'a', 1000000);
    int million_ok = object_store_put(gus, million, 1000000, million_hash);
    free(million);
    if (!million_ok ||
        strcmp(million_hash, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"))
        goto done;
    snprintf(prefix, sizeof(prefix), "%s/%.2s", objects, million_hash);
    snprintf(million_path, sizeof(million_path), "%s/%s", prefix, million_hash + 2);

    /* A corrupt blob at the expected path must never be replaced. */
    file = fopen(abc_path, "wb");
    if (!file || fwrite("bad", 1, 3, file) != 3) {
        if (file) fclose(file);
        goto done;
    }
    fclose(file);
    if (object_store_put(gus, "abc", 3, repeated)) goto done;
    file = fopen(abc_path, "rb");
    if (!file || fread(actual, 1, 3, file) != 3 || memcmp(actual, "bad", 3)) {
        if (file) fclose(file);
        goto done;
    }
    fclose(file);

    snprintf(db_path, sizeof(db_path), "%s/gus.db", gus);
    if (!event_store_initialize(db_path, NULL) ||
        !event_store_append(db_path, GUS_EVENT_FILE_EDIT, NULL, NULL, binary, NULL, NULL))
        goto done;
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db, "SELECT payload_hash FROM events WHERE type='FILE_EDIT'",
                           -1, &statement, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        goto done;
    }
    if (sqlite3_step(statement) == SQLITE_ROW &&
        strcmp((const char *)sqlite3_column_text(statement, 0), binary) == 0) result = 0;
    sqlite3_finalize(statement);
    sqlite3_close(db);

done:
    if (*db_path) unlink(db_path);
    if (*abc_path) unlink(abc_path);
    if (*empty_path) unlink(empty_path);
    if (*binary_path) unlink(binary_path);
    if (*million_path) unlink(million_path);
    snprintf(prefix, sizeof(prefix), "%s/ba", objects); rmdir(prefix);
    snprintf(prefix, sizeof(prefix), "%s/e3", objects); rmdir(prefix);
    if (*binary) {
        snprintf(prefix, sizeof(prefix), "%s/%.2s", objects, binary);
        rmdir(prefix);
    }
    if (*million_hash) {
        snprintf(prefix, sizeof(prefix), "%s/%.2s", objects, million_hash);
        rmdir(prefix);
    }
    rmdir(objects); rmdir(tmp); rmdir(gus); rmdir(root);
    if (result) fputs("object store API test failed\n", stderr);
    return result;
}
