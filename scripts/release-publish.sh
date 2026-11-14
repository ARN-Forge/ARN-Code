#!/usr/bin/env bash
# The GitHub side of .github/workflows/release.yml: finds the draft release of
# a tag, checks it, downloads its files and publishes it, always by the
# numeric release ID found once, never by looking the tag up again.
#
#   release-publish.sh refuse-existing
#       Fails if any release, draft or published, has tag $TAG.
#   release-publish.sh find-draft <snapshot.json>
#       Finds exactly one release with tag $TAG in the releases list (the
#       REST API lists drafts to callers with push access, such as a
#       workflow token with contents: write), checks that it is a draft with
#       the expected prerelease flag and exactly the expected assets, and
#       writes {id, tag_name, assets: [{id, name, size, digest}]} to the
#       snapshot.
#   release-publish.sh download <snapshot.json> <dir>
#       Downloads every asset of the snapshot by asset ID with
#       Accept: application/octet-stream into <dir>, checks sizes and GitHub's
#       digests, that SHA256SUMS lists exactly the expected assets, and the
#       files against SHA256SUMS.
#   release-publish.sh publish <snapshot.json>
#       Lists the releases again; the snapshot's release must still be the
#       only release of $TAG, a draft, with the same flags and the same
#       assets (IDs, names, sizes). Only then it is published by ID with a
#       PATCH of {"draft": false, "prerelease": <bool>, "make_latest": "<string>"}.
#   release-publish.sh check-published <snapshot.json>
#       Checks the same release ID after publication, and the latest release.
#
# Environment: GH_REPO (owner/name), TAG (vX.Y.Z), EXPECTED (release assets,
# ;-separated, without SHA256SUMS), PRERELEASE and MAKE_LATEST (true/false),
# GH_TOKEN (used by gh, and by curl through a header file, never on a command
# line), GITHUB_API_URL (default https://api.github.com). Needs gh, jq, curl
# and sha256sum or shasum. Every problem stops with a non-zero exit; nothing is
# changed on GitHub except by "publish", after all its checks passed.

set -euo pipefail
export LC_ALL=C

die() { echo "::error::$*" >&2; exit 1; }
for var in GH_REPO TAG EXPECTED PRERELEASE MAKE_LATEST; do
    [ -n "${!var:-}" ] || die "release-publish.sh: $var is not set."
done
case "$PRERELEASE" in true|false) ;; *) die "PRERELEASE must be true or false, got '$PRERELEASE'." ;; esac
case "$MAKE_LATEST" in true|false) ;; *) die "MAKE_LATEST must be true or false, got '$MAKE_LATEST'." ;; esac
[[ "$TAG" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || die "TAG '$TAG' is not vX.Y.Z."
[[ "$GH_REPO" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || die "GH_REPO '$GH_REPO' is not owner/name."
api_url="${GITHUB_API_URL:-https://api.github.com}"

# The expected asset names (plus SHA256SUMS), one per line, sorted. They are
# plain file names, so they are safe as paths inside the download directory.
expected_names() { tr ';' '\n' <<< "$EXPECTED" | sed '/^$/d' | sort; }
wanted_assets="$( { expected_names; echo SHA256SUMS; } | sort)"
while IFS= read -r name; do
    [[ "$name" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "Expected asset name '$name' is not a plain file name."
done <<< "$wanted_assets"
[ "$(uniq -d <<< "$wanted_assets")" = "" ] || die "EXPECTED names an asset twice."

releases_list() {
    # --slurp puts every page into one array of pages; add joins them.
    gh api --paginate --slurp "repos/$GH_REPO/releases?per_page=100" | jq -c 'add // []'
}

# check_release <release JSON>: the release of $TAG, a draft (or not, with
# $2 = published), with the expected prerelease flag and exactly the
# expected, uploaded assets with plain, distinct names.
check_release() {
    local release="$1" want_draft="$2" id names
    id="$(jq -r 'if (.id | type) == "number" then .id else "not a number: \(.id)" end' <<< "$release")"
    [[ "$id" =~ ^[1-9][0-9]*$ ]] || die "The release of $TAG has no valid numeric ID ($id)."
    [ "$(jq -r '.tag_name' <<< "$release")" = "$TAG" ] || die "Release $id has tag $(jq -r '.tag_name' <<< "$release"), not $TAG."
    [ "$(jq '.draft' <<< "$release")" = "$want_draft" ] || die "Release $id: draft is $(jq '.draft' <<< "$release"), expected $want_draft."
    [ "$(jq '.prerelease' <<< "$release")" = "$PRERELEASE" ] || die "Release $id: prerelease is $(jq '.prerelease' <<< "$release"), expected $PRERELEASE."
    jq -e '.assets | type == "array"' <<< "$release" > /dev/null || die "Release $id has no assets list."
    jq -e 'all(.assets[]; (.id | type) == "number" and (.name | type) == "string" and (.size | type) == "number" and .state == "uploaded")' \
        <<< "$release" > /dev/null || die "Release $id has an asset without ID, name or size, or one that is not fully uploaded."
    names="$(jq -r '.assets[].name' <<< "$release" | sort)"
    [ "$(uniq -d <<< "$names")" = "" ] || die "Release $id lists an asset name twice: $(uniq -d <<< "$names" | tr '\n' ' ')"
    if [ "$names" != "$wanted_assets" ]; then
        echo "Expected assets:" >&2; echo "$wanted_assets" >&2
        echo "Assets of release $id:" >&2; echo "$names" >&2
        die "The assets of release $id differ from the release policy."
    fi
}

# The single release of $TAG in the list, or a failure.
the_release_of_tag() {
    local list="$1" count
    count="$(jq --arg tag "$TAG" '[.[] | select(.tag_name == $tag)] | length' <<< "$list")"
    [ "$count" = 1 ] || die "Expected exactly one release with tag $TAG, found $count."
    jq -c --arg tag "$TAG" '.[] | select(.tag_name == $tag)' <<< "$list"
}

# snapshot_id <snapshot.json>: the release ID recorded by find-draft.
snapshot_id() {
    local id
    [ -f "$1" ] || die "The snapshot $1 is missing; run find-draft first."
    id="$(jq -r 'if (.id | type) == "number" then .id else "" end' "$1")" || die "The snapshot $1 is not valid JSON."
    [[ "$id" =~ ^[1-9][0-9]*$ ]] || die "The snapshot $1 has no valid release ID."
    echo "$id"
}

sha256_of() {
    if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi
}

command="${1:-}"
case "$command" in
refuse-existing)
    existing="$(releases_list | jq -r --arg tag "$TAG" '.[] | select(.tag_name == $tag) | "\(.id) (draft: \(.draft))"')"
    if [ -n "$existing" ]; then
        die "A release for $TAG already exists: $existing. It is never replaced or added to; delete the draft by hand to build it again."
    fi
    echo "No release exists for $TAG yet."
    ;;

find-draft)
    snapshot="${2:?snapshot file}"
    list="$(releases_list)"
    release="$(the_release_of_tag "$list")"
    check_release "$release" true
    jq '{id, tag_name, assets: [.assets[] | {id, name, size, digest}] | sort_by(.name)}' <<< "$release" > "$snapshot"
    echo "Draft release $(jq -r .id "$snapshot") of $TAG: prerelease=$PRERELEASE, assets: $(jq -r '[.assets[].name] | join(", ")' "$snapshot")."
    ;;

download)
    snapshot="${2:?snapshot file}"; dir="${3:?download directory}"
    snapshot_id "$snapshot" > /dev/null
    [ ! -e "$dir" ] || die "$dir already exists; downloads go into a new directory."
    mkdir -p "$dir"
    auth="$(mktemp)"
    trap 'rm -f "$auth"' EXIT
    chmod 600 "$auth"
    printf 'Authorization: Bearer %s\n' "${GH_TOKEN:?GH_TOKEN is not set}" > "$auth"
    while IFS=$'\t' read -r asset_id name size digest; do
        [[ "$asset_id" =~ ^[1-9][0-9]*$ ]] || die "Asset '$name' has no valid ID."
        grep -qxF -- "$name" <<< "$wanted_assets" || die "Asset '$name' is not an expected asset."
        # The API answers with the file or a redirect to it (200 or 302);
        # curl follows the redirect and does not send the Authorization header
        # to another host. The body is written to the file byte for byte.
        curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
            --header "@$auth" --header 'Accept: application/octet-stream' \
            --output "$dir/$name" "$api_url/repos/$GH_REPO/releases/assets/$asset_id" ||
            die "Could not download asset $asset_id ($name)."
        actual_size="$(wc -c < "$dir/$name" | tr -d ' ')"
        [ "$actual_size" = "$size" ] || die "$name: downloaded $actual_size bytes, the release lists $size."
        actual="$(sha256_of "$dir/$name")"
        if [ "$digest" != "null" ] && [ -n "$digest" ]; then
            [ "$digest" = "sha256:$actual" ] || die "$name: SHA-256 $actual differs from GitHub's digest $digest."
        fi
    done < <(jq -r '.assets[] | [.id, .name, .size, (.digest // "null")] | @tsv' "$snapshot")
    cd "$dir"
    # SHA256SUMS: only "<64 hex>  <name>" lines, exactly the expected assets.
    if grep -Evq '^[0-9a-f]{64}  [A-Za-z0-9][A-Za-z0-9._-]*$' SHA256SUMS; then
        die "SHA256SUMS contains a line that is not '<sha256>  <file>'."
    fi
    [ "$(cut -c67- SHA256SUMS | sort)" = "$(expected_names)" ] || die "SHA256SUMS does not list exactly the expected assets."
    if command -v sha256sum > /dev/null; then sha256sum --check --strict SHA256SUMS; else shasum -a 256 --check --strict SHA256SUMS; fi
    echo "Every file of the draft matches SHA256SUMS."
    ;;

publish)
    snapshot="${2:?snapshot file}"
    id="$(snapshot_id "$snapshot")"
    # Unchanged since it was checked: still the only release of the tag, the
    # same ID, a draft, the same assets.
    list="$(releases_list)"
    release="$(the_release_of_tag "$list")"
    [ "$(jq -r '.id' <<< "$release")" = "$id" ] || die "The release of $TAG is now $(jq -r '.id' <<< "$release"), not the checked release $id; nothing was published."
    check_release "$release" true
    [ "$(jq -c '[.assets[] | {id, name, size}] | sort_by(.name)' <<< "$release")" = "$(jq -c '[.assets[] | {id, name, size}] | sort_by(.name)' "$snapshot")" ] ||
        die "The assets of release $id changed after they were checked; nothing was published."
    body="$(jq -n --argjson prerelease "$PRERELEASE" --arg latest "$MAKE_LATEST" \
        '{draft: false, prerelease: $prerelease, make_latest: $latest}')"
    echo "Publishing release $id ($TAG): $body"
    gh api --method PATCH "repos/$GH_REPO/releases/$id" --input - <<< "$body" > /dev/null
    ;;

check-published)
    snapshot="${2:?snapshot file}"
    id="$(snapshot_id "$snapshot")"
    recovery="See docs/release-runbook.md (After a failed publication check): nothing is rolled back automatically."
    release="$(gh api "repos/$GH_REPO/releases/$id")" || die "Release $id cannot be read after publication. $recovery"
    [ "$(jq -r '.id' <<< "$release")" = "$id" ] || die "GitHub returned another release than $id. $recovery"
    ( check_release "$release" false ) || die "Release $id is not published as the policy says. $recovery"
    [ "$(jq -c '[.assets[] | {id, name, size}] | sort_by(.name)' <<< "$release")" = "$(jq -c '[.assets[] | {id, name, size}] | sort_by(.name)' "$snapshot")" ] ||
        die "The assets of release $id changed during publication. $recovery"
    latest="$(gh api "repos/$GH_REPO/releases/latest" --jq .tag_name 2>/dev/null || echo none)"
    if [ "$MAKE_LATEST" = false ] && [ "$latest" = "$TAG" ]; then
        die "$TAG became the latest release, but its policy says it must not. $recovery"
    fi
    if [ "$MAKE_LATEST" = true ] && [ "$latest" != "$TAG" ]; then
        die "$TAG should be the latest release, but the latest is $latest. $recovery"
    fi
    echo "Release $id ($TAG) is published: prerelease=$PRERELEASE; the latest release is $latest."
    ;;

*)
    die "usage: release-publish.sh refuse-existing | find-draft <snapshot> | download <snapshot> <dir> | publish <snapshot> | check-published <snapshot>"
    ;;
esac
