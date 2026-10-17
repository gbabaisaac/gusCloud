#!/bin/sh
set -eu

gus=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL=/dev/null

mkdir "$tmp/repo with ' quote"
cd "$tmp/repo with ' quote"
git init -q
mkdir -p .git/hooks nested/deeper
printf '#!/bin/sh\nprintf "old-commit:%%s\\n" "$#" > .git/post-commit-observed\ncat .git/post-commit-observed\nexit 0\n' > .git/hooks/post-commit
printf '#!/bin/sh\nprintf "old-checkout:%%s|%%s|%%s\\n" "$1" "$2" "$3" > .git/post-checkout-observed\ncat .git/post-checkout-observed\nif [ "$1" = probe ]; then IFS= read -r input; printf "stdin:%%s\\n" "$input"; exit 7; fi\nexit 0\n' > .git/hooks/post-checkout
chmod +x .git/hooks/post-commit .git/hooks/post-checkout
"$gus" init >/dev/null
test -f .git/hooks/post-commit.gus-original
test -f .git/hooks/post-checkout.gus-original
test "$(.git/hooks/post-commit)" = 'old-commit:0'
test "$(printf ignored | .git/hooks/post-checkout a b c)" = 'old-checkout:a|b|c'
if observed=$(printf 'data\n' | .git/hooks/post-checkout probe b c); then
    printf 'existing hook exit status was lost\n' >&2
    exit 1
else
    test "$?" = 7
    test "$observed" = "$(printf 'old-checkout:probe|b|c\nstdin:data')"
fi
"$gus" init >/dev/null
test ! -e .git/hooks/post-commit.gus-original.gus-original

cd nested/deeper
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm first
test "$(cat ../../.git/post-commit-observed)" = 'old-commit:0'
db=../../.gus/gus.db
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM events WHERE type="GIT_COMMIT"')" = 1
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 1
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM events WHERE type="CHECKPOINT"')" = 1
test "$(find ../../.gus/objects -type f | wc -l | tr -d ' ')" = 0
"$gus" status >/dev/null
"$gus" log >/dev/null
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 1

head=$(git rev-parse HEAD)
git checkout -qb topic
test "$(cat ../../.git/post-checkout-observed)" = "old-checkout:$head|$head|1"
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM events WHERE type="GIT_CHECKOUT"')" = 1
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 2
git checkout -q --detach HEAD
test "$(cat ../../.git/post-checkout-observed)" = "old-checkout:$head|$head|1"
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints WHERE branch IS NULL')" = 1
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 3

# An IDE or user may remove a hook. The next Gus command catches the missed
# change and later commands leave it alone.
rm ../../.git/hooks/post-commit
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm missed
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 3
"$gus" status >/dev/null
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM events WHERE type="GIT_COMMIT"')" = 2
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 4
"$gus" status >/dev/null
"$gus" log >/dev/null
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 4

rm ../../.git/hooks/post-checkout
git checkout -q master
"$gus" log >/dev/null
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM events WHERE type="GIT_CHECKOUT"')" = 3
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 5
"$gus" -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm via-gus
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM events WHERE type="GIT_COMMIT"')" = 3
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 6
"$gus" status >/dev/null
test "$(sqlite3 "$db" 'SELECT COUNT(*) FROM checkpoints')" = 6

# Reinitialization restores deleted wrappers and retains the original hooks.
"$gus" init >/dev/null
test -f ../../.git/hooks/post-commit.gus-original
cd ../..
test "$(.git/hooks/post-commit)" = 'old-commit:0'

# A failed checkpoint insert must roll back the Git event and capture state.
sqlite3 .gus/gus.db "CREATE TRIGGER reject_capture BEFORE INSERT ON checkpoints BEGIN SELECT RAISE(ABORT, 'injected failure'); END"
rm .git/hooks/post-commit
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm retry
if "$gus" status >/dev/null 2>&1; then
    printf 'capture unexpectedly succeeded with a failing checkpoint insert\n' >&2
    exit 1
fi
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM events WHERE type="GIT_COMMIT"')" = 3
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM checkpoints')" = 6
sqlite3 .gus/gus.db 'DROP TRIGGER reject_capture'
"$gus" status >/dev/null
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM events WHERE type="GIT_COMMIT"')" = 4
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM checkpoints')" = 7
"$gus" status >/dev/null
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM checkpoints')" = 7

mkdir "$tmp/custom-hooks-repo" "$tmp/custom hook's path"
cd "$tmp/custom-hooks-repo"
git init -q
git config core.hooksPath "$tmp/custom hook's path"
printf '#!/bin/sh\nprintf custom-hook-ran > hook-observed\n' > "$tmp/custom hook's path/post-commit"
chmod +x "$tmp/custom hook's path/post-commit"
"$gus" init >/dev/null
test -f "$tmp/custom hook's path/post-commit.gus-original"
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm custom
test "$(cat hook-observed)" = custom-hook-ran
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM checkpoints')" = 1

mkdir "$tmp/preexisting-history"
cd "$tmp/preexisting-history"
git init -q
git -c user.name=Gus -c user.email=gus@example.test commit --allow-empty -qm before-gus
"$gus" init >/dev/null
git checkout -qb same-head-branch
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM events WHERE type="GIT_CHECKOUT"')" = 1
test "$(sqlite3 .gus/gus.db 'SELECT COUNT(*) FROM checkpoints')" = 1

printf 'git capture integration tests passed\n'
