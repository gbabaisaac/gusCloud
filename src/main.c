#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include "checkpoint_store.h"
#include "event_store.h"
#include "git_capture.h"
#include "object_store.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void error(const char *message) {
    fprintf(stderr, "gus: %s\n", message);
}

static bool path_join(char *out, size_t capacity, const char *base, const char *name) {
    int written = snprintf(out, capacity, "%s/%s", base, name);
    if (written < 0 || (size_t)written >= capacity) {
        error("path too long");
        return false;
    }
    return true;
}

static int wait_for_child(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("gus: waitpid");
            return 1;
        }
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int run_git(char *const args[], char *output, size_t capacity) {
    int pipes[2];
    bool capture = output != NULL;
    if (capture && pipe(pipes) < 0) {
        perror("gus: pipe");
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("gus: fork");
        if (capture) {
            close(pipes[0]);
            close(pipes[1]);
        }
        return 1;
    }
    if (pid == 0) {
        if (capture) {
            close(pipes[0]);
            if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(127);
            close(pipes[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp("git", args);
        perror("gus: git");
        _exit(127);
    }
    if (capture) {
        close(pipes[1]);
        size_t used = 0;
        char buffer[256];
        ssize_t count;
        while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0) {
            if (used + (size_t)count < capacity) {
                memcpy(output + used, buffer, (size_t)count);
                used += (size_t)count;
            } else {
                /* Drain the pipe so the child can finish, but reject truncated paths. */
                used = capacity;
            }
        }
        close(pipes[0]);
        if (used < capacity) {
            output[used] = '\0';
            while (used > 0 && (output[used - 1] == '\n' || output[used - 1] == '\r')) {
                output[--used] = '\0';
            }
        }
        int result = wait_for_child(pid);
        return used >= capacity || count < 0 ? 1 : result;
    }
    return wait_for_child(pid);
}

static bool git_query(const char *option, char *output, size_t capacity) {
    char *args[] = {"git", "rev-parse", (char *)option, NULL};
    return run_git(args, output, capacity) == 0;
}

static bool exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

static bool path_entry_exists(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0;
}

static bool write_once(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return true;
        perror(path);
        return false;
    }
    size_t length = strlen(content);
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, content + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            perror(path);
            close(fd);
            return false;
        }
        offset += (size_t)count;
    }
    if (close(fd) < 0) {
        perror(path);
        return false;
    }
    return true;
}

static bool read_line(const char *path, char *output, size_t capacity) {
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return false;
    }
    bool ok = fgets(output, (int)capacity, file) != NULL;
    fclose(file);
    if (!ok) return false;
    output[strcspn(output, "\r\n")] = '\0';
    return true;
}

static bool project_id(char output[37]) {
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        perror("gus: /dev/urandom");
        return false;
    }
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t count = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            perror("gus: /dev/urandom");
            close(fd);
            return false;
        }
        offset += (size_t)count;
    }
    close(fd);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    snprintf(output, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return true;
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) {
        struct stat info;
        if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) return true;
    }
    perror(path);
    return false;
}

static bool ensure_gitignore(const char *root) {
    char path[PATH_MAX];
    if (!path_join(path, sizeof(path), root, ".gitignore")) return false;
    FILE *file = fopen(path, "r");
    bool found = false;
    bool needs_newline = false;
    if (file) {
        char *line = NULL;
        size_t capacity = 0;
        ssize_t count;
        while ((count = getline(&line, &capacity, file)) >= 0) {
            size_t length = (size_t)count;
            if (length > 0 && line[length - 1] == '\n') length--;
            if (length > 0 && line[length - 1] == '\r') length--;
            if (length == 5 && memcmp(line, ".gus/", 5) == 0) found = true;
            needs_newline = count > 0 && line[count - 1] != '\n';
        }
        free(line);
        if (ferror(file)) {
            fclose(file);
            perror(path);
            return false;
        }
        fclose(file);
    } else if (errno != ENOENT) {
        perror(path);
        return false;
    }
    if (found) return true;
    file = fopen(path, "a");
    if (!file) {
        perror(path);
        return false;
    }
    bool ok = fprintf(file, "%s.gus/\n", needs_newline ? "\n" : "") >= 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) perror(path);
    return ok;
}

static bool current_git_state(char head[PATH_MAX], char branch[PATH_MAX],
                              const char **git_head, const char **branch_name) {
    char *head_args[] = {"git", "rev-parse", "--verify", "HEAD", NULL};
    char *branch_args[] = {"git", "symbolic-ref", "--quiet", "--short", "HEAD", NULL};
    *git_head = run_git(head_args, head, PATH_MAX) == 0 ? head : NULL;
    *branch_name = run_git(branch_args, branch, PATH_MAX) == 0 ? branch : NULL;
    return *git_head || *branch_name;
}

static bool reconcile(const char *root, gus_capture_hint hint) {
    char database[PATH_MAX], head[PATH_MAX], branch[PATH_MAX];
    const char *git_head, *branch_name;
    return path_join(database, sizeof(database), root, ".gus/gus.db") &&
           current_git_state(head, branch, &git_head, &branch_name) &&
           git_capture_reconcile(database, git_head, branch_name, hint);
}

static bool project_root(char root[PATH_MAX]) {
    char project[PATH_MAX];
    return git_query("--show-toplevel", root, PATH_MAX) &&
           path_join(project, sizeof(project), root, ".gus/project") && exists(project);
}

static bool executable_path(const char *argv0, char path[PATH_MAX]) {
    if (strchr(argv0, '/')) return realpath(argv0, path) != NULL;
    const char *environment = getenv("PATH");
    if (!environment) return false;
    char *copy = strdup(environment);
    if (!copy) return false;
    bool found = false;
    char *cursor = copy;
    while (cursor) {
        char *next = strchr(cursor, ':');
        if (next) *next++ = '\0';
        char candidate[PATH_MAX];
        const char *directory = *cursor ? cursor : ".";
        if (path_join(candidate, sizeof(candidate), directory, argv0) &&
            access(candidate, X_OK) == 0 && realpath(candidate, path)) {
            found = true;
            break;
        }
        cursor = next;
    }
    free(copy);
    return found;
}

static bool shell_quote(FILE *file, const char *value) {
    if (fputc('\'', file) == EOF) return false;
    for (const char *cursor = value; *cursor; cursor++) {
        if (*cursor == '\'') {
            if (fputs("'\\''", file) == EOF) return false;
        } else if (fputc(*cursor, file) == EOF) return false;
    }
    return fputc('\'', file) != EOF;
}

static bool install_hook(const char *root, const char *binary, const char *name) {
    char hook[PATH_MAX], original[PATH_MAX], temporary[PATH_MAX], directory[PATH_MAX];
    char configured[PATH_MAX];
    char *config_args[] = {"git", "config", "--path", "--get", "core.hooksPath", NULL};
    if (run_git(config_args, configured, sizeof(configured)) == 0) {
        if (configured[0] == '/') {
            if (snprintf(directory, sizeof(directory), "%s", configured) >= (int)sizeof(directory)) return false;
        } else if (!path_join(directory, sizeof(directory), root, configured)) return false;
        if (!path_join(hook, sizeof(hook), directory, name)) return false;
    } else {
        char relative[64];
        if (snprintf(relative, sizeof(relative), "hooks/%s", name) >= (int)sizeof(relative)) return false;
        char *args[] = {"git", "rev-parse", "--path-format=absolute", "--git-path", relative, NULL};
        if (run_git(args, hook, sizeof(hook)) != 0) return false;
        char *slash = strrchr(hook, '/');
        if (!slash) return false;
        size_t length = (size_t)(slash - hook);
        if (length >= sizeof(directory)) return false;
        memcpy(directory, hook, length);
        directory[length] = '\0';
    }
    if (!ensure_directory(directory)) return false;
    if (snprintf(original, sizeof(original), "%s.gus-original", hook) >= (int)sizeof(original) ||
        snprintf(temporary, sizeof(temporary), "%s.gus-XXXXXX", hook) >= (int)sizeof(temporary)) return false;
    bool has_hook = path_entry_exists(hook);
    bool has_original = path_entry_exists(original);
    bool managed = false;
    FILE *existing = has_hook ? fopen(hook, "r") : NULL;
    if (existing) {
        char marker[64] = "";
        bool read_marker = fgets(marker, sizeof(marker), existing) != NULL &&
                           fgets(marker, sizeof(marker), existing) != NULL;
        fclose(existing);
        managed = read_marker && strcmp(marker, "# gus-managed-hook-v1\n") == 0;
        if (!managed && has_original) {
            error("existing hook backup conflicts with Gus hook installation");
            return false;
        }
    } else if (has_hook && has_original) {
        error("existing hook backup conflicts with Gus hook installation");
        return false;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) { perror(temporary); return false; }
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(temporary); return false; }
    bool ok = fputs("#!/bin/sh\n# gus-managed-hook-v1\ngus_status=0\n", file) != EOF;
    if (ok && (has_original || (has_hook && !managed))) {
        ok = fputs("if [ -x ", file) != EOF && shell_quote(file, original) &&
             fputs(" ]; then ", file) != EOF && shell_quote(file, original) &&
             fputs(" \"$@\" || gus_status=$?; fi\n", file) != EOF;
    }
    if (ok) ok = fputs("if [ -z \"${GUS_CAPTURE_ACTIVE:-}\" ]; then GUS_CAPTURE_ACTIVE=1 ", file) != EOF &&
                 shell_quote(file, binary) && fputs(" __git-hook ", file) != EOF &&
                 shell_quote(file, name) &&
                 fputs(" </dev/null >/dev/null 2>&1 || :; fi\nexit \"$gus_status\"\n", file) != EOF;
    if (fclose(file) != 0) ok = false;
    if (ok && chmod(temporary, 0755) != 0) ok = false;
    if (!ok) { unlink(temporary); perror(hook); return false; }
    if (has_hook && !managed && rename(hook, original) != 0) {
        unlink(temporary);
        perror(hook);
        return false;
    }
    if (rename(temporary, hook) != 0) {
        perror(hook);
        if (has_hook && !managed) (void)rename(original, hook);
        unlink(temporary);
        return false;
    }
    return true;
}

static int command_init(const char *argv0) {
    char cwd[PATH_MAX], root[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("gus: getcwd");
        return 1;
    }
    if (!git_query("--show-toplevel", root, sizeof(root))) {
        char *args[] = {"git", "init", "-q", cwd, NULL};
        if (run_git(args, NULL, 0) != 0) return 1;
        if (!git_query("--show-toplevel", root, sizeof(root))) {
            error("could not discover the initialized Git repository");
            return 1;
        }
    }
    char git_root[PATH_MAX], git_dir[PATH_MAX];
    if (!git_query("--show-toplevel", git_root, sizeof(git_root)) ||
        !git_query("--absolute-git-dir", git_dir, sizeof(git_dir)) ||
        strcmp(git_root, root) != 0 || !exists(git_dir)) {
        error("could not initialize a Git repository here");
        return 1;
    }
    char gus_dir[PATH_MAX], path[PATH_MAX];
    if (!path_join(gus_dir, sizeof(gus_dir), root, ".gus") ||
        !ensure_directory(gus_dir)) return 1;
    if (!path_join(path, sizeof(path), gus_dir, "version") ||
        !write_once(path, "1\n")) return 1;
    if (!path_join(path, sizeof(path), gus_dir, "project")) return 1;
    if (!exists(path)) {
        char id[37], content[39];
        if (!project_id(id)) return 1;
        snprintf(content, sizeof(content), "%s\n", id);
        if (!write_once(path, content)) return 1;
    }
    if (!path_join(path, sizeof(path), gus_dir, "config") ||
        !write_once(path, "# Gus project configuration\n")) return 1;
    if (!path_join(path, sizeof(path), gus_dir, "objects") ||
        !ensure_directory(path)) return 1;
    if (!path_join(path, sizeof(path), gus_dir, "tmp") ||
        !ensure_directory(path)) return 1;
    if (!path_join(path, sizeof(path), gus_dir, "gus.db")) return 1;
    char head[PATH_MAX];
    const char *git_head = git_query("HEAD", head, sizeof(head)) ? head : NULL;
    if (!event_store_initialize(path, git_head)) return 1;
    if (!ensure_gitignore(root)) return 1;
    if (!reconcile(root, GUS_CAPTURE_AUTO)) return 1;
    char binary[PATH_MAX];
    if (!executable_path(argv0, binary)) {
        error("cannot resolve executable path for Git hooks");
        return 1;
    }
    if (!install_hook(root, binary, "post-commit") ||
        !install_hook(root, binary, "post-checkout")) return 1;
    printf("Initialized Gus repository in %s\n", root);
    return 0;
}

static int command_status(void) {
    char root[PATH_MAX], git_dir[PATH_MAX], path[PATH_MAX];
    if (!git_query("--show-toplevel", root, sizeof(root)) ||
        !git_query("--absolute-git-dir", git_dir, sizeof(git_dir)) || !exists(git_dir)) {
        error("not inside a Git repository");
        return 1;
    }
    if (!path_join(path, sizeof(path), root, ".gus/project") || !exists(path)) {
        error("not inside a Gus repository; run 'gus init'");
        return 1;
    }
    char id[128];
    if (!read_line(path, id, sizeof(id))) return 1;
    if (!path_join(path, sizeof(path), root, ".gus/gus.db")) return 1;
    if (!reconcile(root, GUS_CAPTURE_AUTO)) return 1;
    int count;
    if (!event_store_count(path, &count)) return 1;
    char branch[PATH_MAX];
    char *branch_args[] = {"git", "symbolic-ref", "--quiet", "--short", "HEAD", NULL};
    if (run_git(branch_args, branch, sizeof(branch)) != 0) strcpy(branch, "(detached)");
    char head[PATH_MAX] = "(unborn)";
    if (!git_query("HEAD", head, sizeof(head))) strcpy(head, "(unborn)");
    printf("Gus repository: yes\nProject ID: %s\nGit branch: %s\nGit HEAD: %s\nGus events: %d\n",
           id, branch, head, count);
    return 0;
}

static int command_log(int argc) {
    char root[PATH_MAX], project[PATH_MAX], database[PATH_MAX];
    if (!git_query("--show-toplevel", root, sizeof(root)) ||
        !path_join(project, sizeof(project), root, ".gus/project") ||
        !exists(project)) return -1;
    if (argc != 2) {
        error("gus log takes no arguments inside a Gus repository");
        return 1;
    }
    if (!path_join(database, sizeof(database), root, ".gus/gus.db")) return 1;
    if (!reconcile(root, GUS_CAPTURE_AUTO)) return 1;
    return event_store_log(database) ? 0 : 1;
}

static int command_checkpoint(int argc) {
    if (argc != 2) {
        error("usage: gus checkpoint");
        return 1;
    }
    char root[PATH_MAX], git_dir[PATH_MAX], project[PATH_MAX], database[PATH_MAX];
    if (!git_query("--show-toplevel", root, sizeof(root)) ||
        !git_query("--absolute-git-dir", git_dir, sizeof(git_dir)) || !exists(git_dir) ||
        !path_join(project, sizeof(project), root, ".gus/project") || !exists(project) ||
        !path_join(database, sizeof(database), root, ".gus/gus.db")) {
        error("not inside a Gus repository; run 'gus init'");
        return 1;
    }
    char head[PATH_MAX], branch[PATH_MAX];
    const char *git_head, *branch_name;
    if (!current_git_state(head, branch, &git_head, &branch_name)) {
        error("could not resolve Git HEAD or branch");
        return 1;
    }
    if (!reconcile(root, GUS_CAPTURE_AUTO)) return 1;
    int64_t id = 0;
    if (!checkpoint_store_append(database, git_head, branch_name, &id)) return 1;
    printf("Checkpoint %lld\n", (long long)id);
    return 0;
}

static int command_object(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[2], "put") != 0) {
        error("usage: gus object put <file|->");
        return 1;
    }
    char root[PATH_MAX], gus_dir[PATH_MAX], project[PATH_MAX];
    if (!git_query("--show-toplevel", root, sizeof(root)) ||
        !path_join(gus_dir, sizeof(gus_dir), root, ".gus") ||
        !path_join(project, sizeof(project), gus_dir, "project") || !exists(project)) {
        error("not inside a Gus repository; run 'gus init'");
        return 1;
    }
    if (!reconcile(root, GUS_CAPTURE_AUTO)) return 1;
    int fd = strcmp(argv[3], "-") == 0 ? STDIN_FILENO : open(argv[3], O_RDONLY);
    if (fd < 0) { perror(argv[3]); return 1; }
    char hash[GUS_OBJECT_HASH_LENGTH + 1];
    bool ok = object_store_put_fd(gus_dir, fd, hash);
    if (fd != STDIN_FILENO && close(fd) != 0) { perror(argv[3]); ok = false; }
    if (ok) puts(hash);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        puts("gus 0.0.1");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "__git-hook") == 0) {
        char root[PATH_MAX];
        if (!project_root(root)) return 0;
        gus_capture_hint hint = strcmp(argv[2], "post-commit") == 0 ? GUS_CAPTURE_COMMIT :
            strcmp(argv[2], "post-checkout") == 0 ? GUS_CAPTURE_CHECKOUT : GUS_CAPTURE_AUTO;
        return reconcile(root, hint) ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "init") == 0) return command_init(argv[0]);
    if (argc > 1 && strcmp(argv[1], "status") == 0) return command_status();
    if (argc > 1 && strcmp(argv[1], "checkpoint") == 0) return command_checkpoint(argc);
    if (argc > 1 && strcmp(argv[1], "object") == 0) return command_object(argc, argv);
    if (argc > 1 && strcmp(argv[1], "log") == 0) {
        int result = command_log(argc);
        if (result >= 0) return result;
    }
    char **args = calloc((size_t)argc + 1, sizeof(char *));
    if (!args) {
        perror("gus: calloc");
        return 1;
    }
    args[0] = "git";
    for (int i = 1; i < argc; i++) args[i] = argv[i];
    int result = run_git(args, NULL, 0);
    free(args);
    char root[PATH_MAX];
    if (result == 0 && project_root(root) && !reconcile(root, GUS_CAPTURE_AUTO)) return 1;
    return result;
}
