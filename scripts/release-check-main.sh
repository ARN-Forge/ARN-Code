#!/usr/bin/env bash
# Checks that a release commit is already on the main branch, as it is on the
# remote now: a release tag on a commit that was never merged (for example on
# a pull request branch before its merge) is refused.
#
#   release-check-main.sh <commit> [<remote>] [<branch>]
#
# <commit> is the tagged commit (a tag name or SHA; a tag object is resolved to
# its commit). The branch (default main) is fetched from the remote (default
# origin) right now, never taken from a local or remote-tracking ref that may
# be out of date, and the commit must be an ancestor of it. Run in a full
# clone (actions/checkout with fetch-depth: 0); in a shallow clone the
# history cannot show ancestry and the check fails. Every problem, a failed
# fetch included, fails the check.

set -euo pipefail

die() { echo "::error::$*" >&2; exit 1; }
commit_ref="${1:?usage: release-check-main.sh <commit> [<remote>] [<branch>]}"
remote="${2:-origin}"
branch="${3:-main}"
[[ "$branch" =~ ^[A-Za-z0-9._/-]+$ ]] || die "Branch name '$branch' is not valid."

commit="$(git rev-parse --verify --quiet "${commit_ref}^{commit}")" || die "$commit_ref is not a commit in this repository."
if [ "$(git rev-parse --is-shallow-repository)" = true ]; then
    die "This clone is shallow, so it cannot show whether $commit is on $branch; check out with fetch-depth: 0."
fi
git fetch --no-tags --quiet "$remote" "refs/heads/$branch" ||
    die "Could not fetch $branch from $remote, so it cannot be shown that $commit is on $branch."
main="$(git rev-parse --verify --quiet "FETCH_HEAD^{commit}")" || die "Fetching $branch from $remote returned no commit."

if git merge-base --is-ancestor "$commit" "$main"; then
    echo "$commit is on $branch of $remote (at $main)."
    exit 0
else
    status=$?
fi
if [ "$status" -eq 1 ]; then
    die "$commit is not on $branch of $remote (at $main): release tags must be on commits already merged into $branch."
fi
die "Could not compare $commit with $branch (git merge-base exited with $status)."
