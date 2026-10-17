#!/bin/sh
set -eu

gus=$1
case "$gus" in
    /*) ;;
    *) gus="$(cd "$(dirname "$gus")" && pwd)/$(basename "$gus")" ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL=/dev/null

test "$("$gus" --version)" = 'gus 0.0.1'
mkdir "$tmp/new"
cd "$tmp/new"
"$gus" init >/dev/null
test -d .git
test -f .gus/version
test -f .gus/project
test -f .gus/config
test -f .gus/gus.db
test -d .gus/objects
test -d .gus/tmp
test "$(cat .gus/version)" = 1
test "$(head -c 15 .gus/gus.db)" = 'SQLite format 3'
printf abc > content.bin
object_hash=$("$gus" object put content.bin)
test "$object_hash" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
test -f ".gus/objects/ba/${object_hash#ba}"
test "$(printf abc | "$gus" object put -)" = "$object_hash"
test "$(find .gus/objects -type f | wc -l | tr -d ' ')" = 1
test "$("$gus" object put /dev/null)" = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
test "$(find .gus/tmp -type f | wc -l | tr -d ' ')" = 0
if "$gus" object put missing-file >/dev/null 2>&1; then
    printf 'object put unexpectedly accepted a missing file\n' >&2
    exit 1
fi
test "$("$gus" status | sed -n 's/^Gus events: //p')" = 1
test "$(grep -cxF '.gus/' .gitignore)" = 1
test "$(sqlite3 .gus/gus.db "SELECT COUNT(*) FROM events WHERE type='PROJECT_INIT'")" = 1
test "$(sqlite3 .gus/gus.db "SELECT group_concat(name, ', ') FROM pragma_table_info('events')")" = 'id, type, timestamp, parent, git_head, gus_head, payload_hash, payload'
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM events WHERE id=1 AND timestamp IS NOT NULL AND parent IS NULL')" = 1
if sqlite3 .gus/gus.db "UPDATE events SET type='CHECKPOINT' WHERE id=1" >/dev/null 2>&1; then
    printf 'events UPDATE unexpectedly succeeded\n' >&2
    exit 1
fi
if sqlite3 .gus/gus.db 'DELETE FROM events WHERE id=1' >/dev/null 2>&1; then
    printf 'events DELETE unexpectedly succeeded\n' >&2
    exit 1
fi
log=$("$gus" log)
printf '%s\n' "$log" | grep -Eq '^1  [0-9]{4}-[0-9]{2}-[0-9]{2}T.*  PROJECT_INIT$'
id=$(cat .gus/project)
"$gus" init >/dev/null
test "$(cat .gus/project)" = "$id"
test "$("$gus" status | sed -n 's/^Gus events: //p')" = 1
test "$(grep -cxF '.gus/' .gitignore)" = 1
test "$(sqlite3 .gus/gus.db "SELECT COUNT(*) FROM events WHERE type='PROJECT_INIT'")" = 1

mkdir -p nested/deeper
cd nested/deeper
status=$("$gus" status)
printf '%s\n' "$status" | grep -qx 'Gus repository: yes'
printf '%s\n' "$status" | grep -qx "Project ID: $id"
printf '%s\n' "$status" | grep -qx 'Gus events: 1'
printf '%s\n' "$status" | grep -q '^Git branch: '
printf '%s\n' "$status" | grep -q '^Git HEAD: '
test "$("$gus" object put ../../content.bin)" = "$object_hash"
test "$("$gus" rev-parse --show-toplevel)" = "$(git rev-parse --show-toplevel)"
test "$("$gus" branch --show-current)" = "$(git branch --show-current)"
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm initial
test "$("$gus" status | sed -n 's/^Git HEAD: //p')" = "$(git rev-parse HEAD)"
log=$("$gus" log)
printf '%s\n' "$log" | grep -q "GIT_COMMIT  git:$(git rev-parse HEAD)"
test "$("$gus" status | sed -n 's/^Gus events: //p')" = 3
test "$(sqlite3 ../../.gus/gus.db 'SELECT COUNT(*) FROM checkpoints')" = 1
if "$gus" log -1 >/dev/null 2>&1; then
    printf 'Gus log unexpectedly accepted Git-only options\n' >&2
    exit 1
fi

cd "$tmp"
mkdir existing
cd existing
git init -q
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm before-init
"$gus" init >/dev/null
test -d .git
test "$("$gus" status | sed -n 's/^Gus events: //p')" = 1
"$gus" log | grep -q "PROJECT_INIT  git:$(git rev-parse HEAD)"

cd "$tmp"
mkdir outer
cd outer
git init -q
outer_root=$(pwd -P)
mkdir inner
cd inner
"$gus" init >/dev/null
test "$(git rev-parse --show-toplevel)" = "$outer_root"
test ! -e .git
test -d ../.gus

cd "$tmp"
mkdir legacy
cd legacy
git init -q
"$gus" init >/dev/null
sqlite3 .gus/gus.db "DROP TABLE events; CREATE TABLE events(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, created_at TEXT NOT NULL); INSERT INTO events(id,kind,created_at) VALUES (7,'init','2020-01-02T03:04:05Z'),(9,'custom','2020-01-02T03:04:06Z')"
"$gus" init >/dev/null
test "$(sqlite3 .gus/gus.db "SELECT id || '|' || type || '|' || timestamp FROM events ORDER BY id")" = "$(printf '7|PROJECT_INIT|2020-01-02T03:04:05Z\n9|custom|2020-01-02T03:04:06Z')"
test "$("$gus" status | sed -n 's/^Gus events: //p')" = 2
test "$("$gus" checkpoint)" = 'Checkpoint 1'
test "$(sqlite3 .gus/gus.db 'SELECT event_head FROM checkpoints WHERE id=1')" = 10
"$gus" init >/dev/null
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM checkpoints')" = 1
if sqlite3 .gus/gus.db 'DELETE FROM events' >/dev/null 2>&1; then
    printf 'migrated events DELETE unexpectedly succeeded\n' >&2
    exit 1
fi

cd "$tmp"
mkdir checkpoint
cd checkpoint
git init -q
"$gus" init >/dev/null
initial_branch=$(git symbolic-ref --short HEAD)
mkdir -p sub/deep
cd sub/deep
test "$("$gus" checkpoint)" = 'Checkpoint 1'
test "$("$gus" checkpoint)" = 'Checkpoint 2'
test "$(sqlite3 ../../.gus/gus.db "SELECT COUNT(*) FROM checkpoints WHERE git_head IS NULL AND branch='$initial_branch'")" = 2
test "$(sqlite3 ../../.gus/gus.db 'SELECT group_concat(parent, ",") FROM checkpoints')" = 1
test "$(sqlite3 ../../.gus/gus.db 'SELECT COUNT(*) FROM checkpoints c JOIN events e ON e.id=c.event_head WHERE e.type="CHECKPOINT" AND e.git_head IS c.git_head AND e.payload IS NULL AND e.payload_hash IS NULL')" = 2
test "$(find ../../.gus/objects -type f | wc -l | tr -d ' ')" = 0
test "$(find ../../.gus/tmp -type f | wc -l | tr -d ' ')" = 0
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm first
head=$(git rev-parse HEAD)
test "$("$gus" checkpoint)" = 'Checkpoint 4'
test "$(sqlite3 ../../.gus/gus.db 'SELECT git_head FROM checkpoints WHERE id=4')" = "$head"
test "$(sqlite3 ../../.gus/gus.db 'SELECT parent FROM checkpoints WHERE id=4')" = 3
git checkout -q --detach HEAD
test "$("$gus" checkpoint)" = 'Checkpoint 6'
test "$(sqlite3 ../../.gus/gus.db 'SELECT COUNT(*) FROM checkpoints WHERE id=6 AND branch IS NULL AND git_head IS NOT NULL')" = 1
log=$("$gus" log)
printf '%s\n' "$log" | grep -q "CHECKPOINT  git:$head  checkpoint:6  parent-checkpoint:5  branch:(detached)"
printf '%s\n' "$log" | grep -q "CHECKPOINT  git:(unborn)  checkpoint:2  parent-checkpoint:1  branch:$initial_branch"
printf '%s\n' "$log" | grep -q 'PROJECT_INIT'
test "$(find ../../.gus/objects -type f | wc -l | tr -d ' ')" = 0

cd "$tmp"
mkdir unknown_schema
cd unknown_schema
git init -q
"$gus" init >/dev/null
sqlite3 .gus/gus.db "DROP TABLE events; CREATE TABLE events(id INTEGER PRIMARY KEY, unknown TEXT); INSERT INTO events VALUES (3,'keep-me')"
if "$gus" init >/dev/null 2>&1; then
    printf 'unknown schema migration unexpectedly succeeded\n' >&2
    exit 1
fi
test "$(sqlite3 .gus/gus.db 'SELECT unknown FROM events WHERE id=3')" = keep-me

cd "$tmp"
mkdir git_only
cd git_only
git init -q
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm outside
test "$("$gus" log -1 --format=%s)" = outside

printf 'integration tests passed\n'
