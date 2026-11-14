#!/usr/bin/env bash
# Tests scripts/release-check-main.sh with real Git repositories in a
# temporary directory: a "remote" bare repository and clones of it.
#   release_main_ancestry_test.sh <release-check-main.sh>

set -u
export LC_ALL=C
script="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
command -v git > /dev/null || { echo "skip: git is not installed"; exit 77; }
root="$(mktemp -d "${TMPDIR:-/tmp}/arn-release-main-test.XXXXXX")"
trap 'rm -rf "$root"' EXIT
export GIT_CONFIG_NOSYSTEM=1 HOME="$root/home" GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid \
       GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid
mkdir -p "$HOME"
git config --global init.defaultBranch main
git config --global protocol.file.allow always
cases=0; failures=0
fail() { echo "FAIL: $1"; failures=$((failures + 1)); }

# The remote: main with two commits; a feature branch with one more commit.
git init -q --bare "$root/remote.git"
git clone -q "$root/remote.git" "$root/work" 2> /dev/null
cd "$root/work"
echo one > file; git add file; git commit -q -m one
echo two > file; git commit -q -am two
git push -q origin main
main_commit="$(git rev-parse HEAD)"
git checkout -q -b feature
echo three > file; git commit -q -am three
git push -q origin feature
feature_commit="$(git rev-parse HEAD)"
git tag v1.0.0-on-feature "$feature_commit"
git tag -a v1.0.0-annotated -m annotated "$main_commit"
git push -q origin v1.0.0-on-feature v1.0.0-annotated

# The release checkout: a full clone, taken now.
git clone -q "$root/remote.git" "$root/checkout" 2> /dev/null
cd "$root/checkout"
git fetch -q origin 'refs/heads/*:refs/remotes/origin/*' 'refs/tags/*:refs/tags/*'

run() { # run <name> <pass|fail> <fragment> <args...>
    local name="$1" outcome="$2" fragment="$3" out status
    shift 3
    cases=$((cases + 1))
    out="$(bash "$script" "$@" 2>&1)"; status=$?
    if [ "$outcome" = pass ] && [ $status -ne 0 ]; then fail "$name: expected success, exit $status: $out"; return; fi
    if [ "$outcome" = fail ] && [ $status -eq 0 ]; then fail "$name: expected failure, but it passed: $out"; return; fi
    if [ -n "$fragment" ] && ! printf '%s' "$out" | grep -qF -- "$fragment"; then fail "$name: output lacks '$fragment': $out"; return; fi
    echo "ok: $name"
}

run "a commit on main" pass "is on main of origin" "$main_commit" origin main
run "an annotated tag on main (resolved to its commit)" pass "is on main of origin" v1.0.0-annotated origin main
run "a tag on a feature commit before its merge" fail "is not on main of origin" v1.0.0-on-feature origin main
run "the feature commit itself" fail "release tags must be on commits already merged into main" "$feature_commit" origin main

# The pull request is merged on the remote after this checkout was made: the
# local origin/main is out of date, and only a fresh fetch shows the merge.
cd "$root/work"
git checkout -q main
git merge -q --no-ff -m merge feature
git push -q origin main
cd "$root/checkout"
[ "$(git rev-parse origin/main)" = "$main_commit" ] || fail "test setup: origin/main should still be the old main"
run "after the merge on the remote, with a stale local origin/main" pass "is on main of origin" v1.0.0-on-feature origin main

# A stale local ref is never used: if the remote's main moves away (here it
# is rewound to before the merge), the fresh state decides.
cd "$root/work"
git push -q --force origin "$main_commit:refs/heads/main"
cd "$root/checkout"
git fetch -q origin 'refs/heads/main:refs/remotes/origin/main' 2> /dev/null || true
git update-ref refs/remotes/origin/main "$(git -C "$root/work" rev-parse HEAD)"   # local ref claims the merge
run "a local origin/main that includes the commit does not count" fail "is not on main of origin" v1.0.0-on-feature origin main

run "a remote without the branch" fail "Could not fetch release-main from origin" "$main_commit" origin release-main
run "an unreachable remote" fail "Could not fetch main from $root/no-such-remote.git" "$main_commit" "$root/no-such-remote.git" main
run "a commit that does not exist" fail "is not a commit in this repository" 0123456789abcdef0123456789abcdef01234567 origin main
run "a branch name with unexpected characters" fail "is not valid" "$main_commit" origin 'main;x'

git clone -q --depth 1 "file://$root/remote.git" "$root/shallow" 2> /dev/null
cd "$root/shallow"
run "a shallow clone" fail "This clone is shallow" HEAD origin main

echo "$cases cases, $failures failure(s)"
[ "$failures" -eq 0 ]
