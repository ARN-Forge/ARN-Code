#!/usr/bin/env bash
# Tests scripts/release-publish.sh against a fake GitHub: fake gh and curl
# commands earlier on PATH answer from fixture files. No real release is read,
# created or changed. Needs jq and sha256sum or shasum.
#   release_publish_test.sh <release-publish.sh>

set -u
export LC_ALL=C
script="$1"
command -v jq > /dev/null || { echo "skip: jq is not installed"; exit 77; }
root="$(mktemp -d "${TMPDIR:-/tmp}/arn-release-publish-test.XXXXXX")"
trap 'rm -rf "$root"' EXIT
fx="$root/fx"; bin="$root/bin"
mkdir -p "$fx" "$bin"
cases=0; failures=0
fail() { echo "FAIL: $1"; failures=$((failures + 1)); }
sha256_of() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi; }

# Fake gh:
#   gh api --paginate --slurp repos/R/releases?per_page=100
#       prints [page, ...] from $fx/page-*.json (in name order); FAKE_LIST_FAIL=1 fails
#   gh api repos/R/releases/<id>    -> $fx/release-<id>.json (404 if absent)
#   gh api repos/R/releases/latest  -> $fx/latest.json; with --jq, applies it
#   gh api --method PATCH repos/R/releases/<id> --input -
#       records the body in $fx/patch.json and the path in $fx/patch.path;
#       then runs $fx/after-patch.sh if it exists
cat > "$bin/gh" <<EOF
#!/usr/bin/env bash
fx="$fx"
echo "gh \$*" >> "\$fx/gh.log"
[ "\$1" = api ] || { echo "unexpected gh \$*" >&2; exit 1; }
shift
method=GET; jq_expr=""; path=""; input=""; paginate=0; slurp=0
while [ \$# -gt 0 ]; do
    case "\$1" in
        --paginate) paginate=1 ;;
        --slurp) slurp=1 ;;
        --method|-X) method="\$2"; shift ;;
        --jq|-q) jq_expr="\$2"; shift ;;
        --input) input="\$2"; shift ;;
        -*) echo "unexpected gh flag \$1" >&2; exit 1 ;;
        *) path="\$1" ;;
    esac
    shift
done
case "\$method \$path" in
    "GET repos/ARN-Forge/ARN-Code/releases?per_page=100")
        [ "\${FAKE_LIST_FAIL:-0}" = 1 ] && { echo "gh: HTTP 502" >&2; exit 1; }
        [ \$paginate = 1 ] && [ \$slurp = 1 ] || { echo "the list must be read with --paginate --slurp" >&2; exit 1; }
        printf '['; first=1
        for page in "\$fx"/page-*.json; do
            [ -f "\$page" ] || continue
            [ \$first = 1 ] || printf ','; first=0; cat "\$page"
        done
        printf ']\n' ;;
    "GET repos/ARN-Forge/ARN-Code/releases/latest")
        [ -f "\$fx/latest.json" ] || { echo "gh: Not Found (HTTP 404)" >&2; exit 1; }
        if [ -n "\$jq_expr" ]; then jq -r "\$jq_expr" "\$fx/latest.json"; else cat "\$fx/latest.json"; fi ;;
    "GET repos/ARN-Forge/ARN-Code/releases/"*)
        id="\${path##*/}"
        [ -f "\$fx/release-\$id.json" ] || { echo "gh: Not Found (HTTP 404)" >&2; exit 1; }
        cat "\$fx/release-\$id.json" ;;
    "PATCH repos/ARN-Forge/ARN-Code/releases/"*)
        [ "\$input" = - ] || { echo "PATCH body must come from --input -" >&2; exit 1; }
        cat > "\$fx/patch.json"; echo "\$path" > "\$fx/patch.path"
        [ -x "\$fx/after-patch.sh" ] && "\$fx/after-patch.sh"
        echo '{}' ;;
    *) echo "unexpected gh api \$method \$path" >&2; exit 1 ;;
esac
EOF
# Fake curl: serves $fx/assets/<asset id>. It requires the Authorization header
# from a file (never a token on the command line), Accept:
# application/octet-stream, https only, and the asset endpoint.
cat > "$bin/curl" <<EOF
#!/usr/bin/env bash
fx="$fx"
echo "curl \$*" >> "\$fx/curl.log"
case " \$* " in *test-token*) echo "token on the command line" >> "\$fx/curl-violations" ;; esac
output=""; url=""; auth_ok=0; accept_ok=0; proto_ok=0
while [ \$# -gt 0 ]; do
    case "\$1" in
        --output|-o) output="\$2"; shift ;;
        --header|-H)
            case "\$2" in
                @*) grep -qx 'Authorization: Bearer test-token' "\${2#@}" && auth_ok=1 ;;
                'Accept: application/octet-stream') accept_ok=1 ;;
            esac
            shift ;;
        --proto) [ "\$2" = '=https' ] && proto_ok=1; shift ;;
        --proto-redir) shift ;;
        https://*) url="\$1" ;;
    esac
    shift
done
[ \$auth_ok = 1 ] && [ \$accept_ok = 1 ] && [ \$proto_ok = 1 ] || { echo "curl: bad request headers" >&2; exit 2; }
case "\$url" in
    https://api.github.com/repos/ARN-Forge/ARN-Code/releases/assets/*) id="\${url##*/}" ;;
    *) echo "curl: unexpected URL \$url" >&2; exit 2 ;;
esac
[ -f "\$fx/assets/\$id" ] || { echo "curl: (22) The requested URL returned error: 404" >&2; exit 22; }
cp "\$fx/assets/\$id" "\$output"
EOF
chmod 755 "$bin/gh" "$bin/curl"
export PATH="$bin:$PATH" GH_TOKEN=test-token GH_REPO=ARN-Forge/ARN-Code TAG=v0.9.0
export EXPECTED="arn-linux-x64.tar.gz;arn-macos-arm64.tar.gz;arn-macos-x64.tar.gz"
export PRERELEASE=true MAKE_LATEST=false GITHUB_API_URL=https://api.github.com

# The release files: three archives and their SHA256SUMS.
mkdir -p "$root/files"
printf 'linux archive\n' > "$root/files/arn-linux-x64.tar.gz"
printf 'macos arm64 archive\n' > "$root/files/arn-macos-arm64.tar.gz"
printf 'macos intel archive \033[31m with an escape byte\n' > "$root/files/arn-macos-x64.tar.gz"
(cd "$root/files" && for f in arn-linux-x64.tar.gz arn-macos-arm64.tar.gz arn-macos-x64.tar.gz; do echo "$(sha256_of "$f")  $f"; done) > "$root/files/SHA256SUMS"

reset() { rm -rf "$fx"; mkdir -p "$fx/assets"; : > "$fx/gh.log"; : > "$fx/curl.log"; }
# asset <id> <file> [name]: one asset object, its file served under its ID.
asset() {
    local id="$1" file="$2" name="${3:-${2##*/}}"
    cp "$file" "$fx/assets/$id"
    jq -n --argjson id "$id" --arg name "$name" --argjson size "$(wc -c < "$file" | tr -d ' ')" \
        --arg digest "sha256:$(sha256_of "$file")" '{id: $id, name: $name, size: $size, digest: $digest, state: "uploaded"}'
}
standard_assets() { # ids 11-14
    asset 11 "$root/files/arn-linux-x64.tar.gz"
    asset 12 "$root/files/arn-macos-arm64.tar.gz"
    asset 13 "$root/files/arn-macos-x64.tar.gz"
    asset 14 "$root/files/SHA256SUMS"
}
# release <id> <tag> <draft> <prerelease> <assets JSON objects...>
release() {
    local id="$1" tag="$2" draft="$3" pre="$4"; shift 4
    printf '%s\n' "$@" | jq -s --argjson id "$id" --arg tag "$tag" --argjson draft "$draft" --argjson pre "$pre" \
        '{id: $id, tag_name: $tag, draft: $draft, prerelease: $pre, assets: .}'
}
v080() { release 8 v0.8.0 false false; }
page() { jq -s . > "$fx/page-$1.json"; }
latest() { echo "{\"tag_name\": \"$1\"}" > "$fx/latest.json"; }

run() { # run <name> <pass|fail> <fragment> <args...>
    local name="$1" outcome="$2" fragment="$3" out status
    shift 3
    cases=$((cases + 1))
    out="$(cd "$root" && bash "$script" "$@" 2>&1)"; status=$?
    if [ "$outcome" = pass ] && [ $status -ne 0 ]; then fail "$name: expected success, exit $status: $out"; return 1; fi
    if [ "$outcome" = fail ] && [ $status -eq 0 ]; then fail "$name: expected failure, but it passed: $out"; return 1; fi
    if [ -n "$fragment" ] && ! printf '%s' "$out" | grep -qF -- "$fragment"; then fail "$name: output lacks '$fragment': $out"; return 1; fi
    echo "ok: $name"
}
no_patch() { [ ! -e "$fx/patch.json" ] || fail "$1: a PATCH was sent: $(cat "$fx/patch.json")"; }
snap="$root/snapshot.json"
# good_draft: v0.8.0 published, the v0.9.0 draft (id 900) with the standard assets.
good_draft() { reset; { v080; release 900 v0.9.0 true true "$(standard_assets)"; } | page 1; latest v0.8.0; }
# The good draft as GET /releases/900 returns it after publication.
published_state() { { release 900 v0.9.0 false true "$(cd "$fx" && jq -c '.[] | select(.id == 900) | .assets[]' page-1.json)"; } > "$fx/release-900.json"; }

echo "== refuse-existing"
reset; v080 | page 1
run "no release for the tag: continue" pass "No release exists for v0.9.0" refuse-existing
good_draft
run "a draft exists for the tag: stop" fail "A release for v0.9.0 already exists: 900 (draft: true)" refuse-existing
reset; { v080; release 901 v0.9.0 false true; } | page 1
run "a published release exists for the tag: stop" fail "already exists: 901 (draft: false)" refuse-existing
reset; v080 | page 1
FAKE_LIST_FAIL=1 run "the list cannot be read: stop" fail "" refuse-existing

echo "== find-draft"
good_draft
run "one draft with the expected assets" pass "Draft release 900 of v0.9.0" find-draft "$snap" &&
    { [ "$(jq -c '{id, tag_name, names: [.assets[].name]}' "$snap")" = '{"id":900,"tag_name":"v0.9.0","names":["SHA256SUMS","arn-linux-x64.tar.gz","arn-macos-arm64.tar.gz","arn-macos-x64.tar.gz"]}' ] ||
      fail "snapshot: $(cat "$snap")"; }
reset; { v080; } | page 1; { release 900 v0.9.0 true true "$(standard_assets)"; } | page 2
run "the draft on the second page of the list" pass "Draft release 900" find-draft "$snap"
reset; v080 | page 1
run "no draft for the tag" fail "Expected exactly one release with tag v0.9.0, found 0" find-draft "$snap"
reset; { release 900 v0.9.0 true true "$(standard_assets)"; release 902 v0.9.0 true true "$(standard_assets)"; } | page 1
run "two drafts with the same tag" fail "found 2" find-draft "$snap"
reset; { release 900 v0.9.0 false true "$(standard_assets)"; } | page 1
run "the release is already published" fail "draft is false, expected true" find-draft "$snap"
reset; { release 900 v0.9.0 true false "$(standard_assets)"; } | page 1
run "the draft is not marked as a prerelease" fail "prerelease is false, expected true" find-draft "$snap"
reset; { release '"900"' v0.9.0 true true "$(standard_assets)"; } | page 1
run "a release ID that is not a number" fail "has no valid numeric ID" find-draft "$snap"
reset; { release 900 v0.9.0 true true "$(asset 11 "$root/files/arn-linux-x64.tar.gz")" "$(asset 12 "$root/files/arn-macos-arm64.tar.gz")" "$(asset 14 "$root/files/SHA256SUMS")"; } | page 1
run "a missing asset (macOS x64)" fail "differ from the release policy" find-draft "$snap"
reset; printf 'win' > "$root/win.zip"; { release 900 v0.9.0 true true "$(standard_assets)" "$(asset 15 "$root/win.zip" arn-windows-x64.zip)"; } | page 1
run "an extra Windows asset" fail "differ from the release policy" find-draft "$snap"
reset; { release 900 v0.9.0 true true "$(standard_assets)" "$(asset 16 "$root/files/SHA256SUMS")"; } | page 1
run "an asset name listed twice" fail "lists an asset name twice" find-draft "$snap"
reset; { release 900 v0.9.0 true true "$(asset 11 "$root/files/arn-linux-x64.tar.gz" ../arn-linux-x64.tar.gz)" "$(asset 12 "$root/files/arn-macos-arm64.tar.gz")" "$(asset 13 "$root/files/arn-macos-x64.tar.gz")" "$(asset 14 "$root/files/SHA256SUMS")"; } | page 1
run "an asset name with a path" fail "differ from the release policy" find-draft "$snap"
reset; { release 900 v0.9.0 true true "$(asset 11 "$root/files/arn-linux-x64.tar.gz" | jq -c '.state = "starter"')" "$(asset 12 "$root/files/arn-macos-arm64.tar.gz")" "$(asset 13 "$root/files/arn-macos-x64.tar.gz")" "$(asset 14 "$root/files/SHA256SUMS")"; } | page 1
run "an asset that is not fully uploaded" fail "not fully uploaded" find-draft "$snap"
reset; v080 | page 1
FAKE_LIST_FAIL=1 run "the list cannot be read" fail "" find-draft "$snap"

echo "== download"
good_draft; bash "$script" find-draft "$snap" > /dev/null
run "the draft's files match SHA256SUMS" pass "Every file of the draft matches SHA256SUMS" download "$snap" "$root/dl-ok" &&
    { cmp -s "$root/dl-ok/arn-macos-x64.tar.gz" "$root/files/arn-macos-x64.tar.gz" || fail "binary file not saved byte for byte"; }
[ ! -s "$fx/curl-violations" ] || fail "the token appeared on a curl command line"
[ "$(grep -c 'releases/assets/' "$fx/curl.log")" = 4 ] || fail "expected 4 downloads by asset ID: $(cat "$fx/curl.log")"
run "the download directory already exists" fail "already exists" download "$snap" "$root/dl-ok"
good_draft; bash "$script" find-draft "$snap" > /dev/null
printf 'linux archivX\n' > "$fx/assets/11"   # same size, different bytes; digest check disabled below
jq '(.assets[] | select(.name == "arn-linux-x64.tar.gz") | .digest) = null' "$snap" > "$snap.tmp" && mv "$snap.tmp" "$snap"
run "a damaged asset (same size) fails the SHA256SUMS check" fail "FAILED" download "$snap" "$root/dl-damaged"
good_draft; bash "$script" find-draft "$snap" > /dev/null
printf 'linux archive, longer\n' > "$fx/assets/11"
run "an asset of the wrong size" fail "arn-linux-x64.tar.gz: downloaded" download "$snap" "$root/dl-size"
good_draft; bash "$script" find-draft "$snap" > /dev/null
printf 'linux archivY\n' > "$fx/assets/11"
run "an asset that differs from GitHub's digest" fail "differs from GitHub's digest" download "$snap" "$root/dl-digest"
# A substituted SHA256SUMS whose hash for an archive matches nothing.
good_draft
sed "s/^[0-9a-f]\{64\}  arn-linux-x64.tar.gz\$/$(printf '0%.0s' $(seq 64))  arn-linux-x64.tar.gz/" "$root/files/SHA256SUMS" > "$root/sums-bad"
{ release 900 v0.9.0 true true "$(asset 11 "$root/files/arn-linux-x64.tar.gz")" "$(asset 12 "$root/files/arn-macos-arm64.tar.gz")" "$(asset 13 "$root/files/arn-macos-x64.tar.gz")" "$(asset 14 "$root/sums-bad" SHA256SUMS)"; } | page 1
bash "$script" find-draft "$snap" > /dev/null
run "a substituted SHA256SUMS" fail "FAILED" download "$snap" "$root/dl-sums"
manifest_case() { # manifest_case <name> <fragment> <SHA256SUMS text>
    reset; printf '%s' "$3" > "$root/sums-case"
    { release 900 v0.9.0 true true "$(asset 11 "$root/files/arn-linux-x64.tar.gz")" "$(asset 12 "$root/files/arn-macos-arm64.tar.gz")" "$(asset 13 "$root/files/arn-macos-x64.tar.gz")" "$(asset 14 "$root/sums-case" SHA256SUMS)"; } | page 1
    bash "$script" find-draft "$snap" > /dev/null
    run "$1" fail "$2" download "$snap" "$root/dl-$cases"
}
good_sums="$(cat "$root/files/SHA256SUMS")"
manifest_case "SHA256SUMS lists a Windows file too" "does not list exactly the expected assets" "$good_sums
$(printf '1%.0s' $(seq 64))  arn-windows-x64.zip
"
manifest_case "SHA256SUMS lacks an archive" "does not list exactly the expected assets" "$(grep -v 'arn-macos-x64' "$root/files/SHA256SUMS")
"
manifest_case "SHA256SUMS with a malformed line" "not '<sha256>  <file>'" "$(sed 's/  / /' "$root/files/SHA256SUMS")
"
good_draft; bash "$script" find-draft "$snap" > /dev/null; rm "$fx/assets/12"
run "an asset that cannot be downloaded" fail "Could not download asset 12" download "$snap" "$root/dl-404"

echo "== publish"
good_draft; bash "$script" find-draft "$snap" > /dev/null
run "publish the checked draft by its ID" pass "Publishing release 900" publish "$snap" &&
    { [ "$(cat "$fx/patch.path")" = "repos/ARN-Forge/ARN-Code/releases/900" ] || fail "PATCH path: $(cat "$fx/patch.path")"; } &&
    { [ "$(jq -c . "$fx/patch.json")" = '{"draft":false,"prerelease":true,"make_latest":"false"}' ] || fail "PATCH body: $(cat "$fx/patch.json")"; } &&
    { [ "$(jq -c '[(.draft|type), (.prerelease|type), (.make_latest|type)]' "$fx/patch.json")" = '["boolean","boolean","string"]' ] || fail "PATCH types"; }
good_draft; bash "$script" find-draft "$snap" > /dev/null
PRERELEASE=false MAKE_LATEST=true run "a complete release" fail "prerelease is true, expected false" publish "$snap"
no_patch "complete release with a prerelease draft"
reset; { v080; release 900 v0.9.0 true false "$(standard_assets)"; } | page 1
PRERELEASE=false MAKE_LATEST=true bash "$script" find-draft "$snap" > /dev/null
PRERELEASE=false MAKE_LATEST=true run "a complete release: published as latest" pass "Publishing release 900" publish "$snap" &&
    { [ "$(jq -c . "$fx/patch.json")" = '{"draft":false,"prerelease":false,"make_latest":"true"}' ] || fail "PATCH body: $(cat "$fx/patch.json")"; }
# The release changed between the check and the publication: nothing is sent.
good_draft; bash "$script" find-draft "$snap" > /dev/null
{ v080; release 900 v0.9.0 true true "$(standard_assets)"; release 903 v0.9.0 true true "$(standard_assets)"; } | page 1
run "a second draft appeared for the tag" fail "found 2" publish "$snap"; no_patch "second draft"
good_draft; bash "$script" find-draft "$snap" > /dev/null
{ v080; release 904 v0.9.0 true true "$(standard_assets)"; } | page 1
run "the draft was replaced by another release ID" fail "is now 904, not the checked release 900" publish "$snap"; no_patch "replaced draft"
good_draft; bash "$script" find-draft "$snap" > /dev/null
jq '.id = 999' "$snap" > "$snap.tmp" && mv "$snap.tmp" "$snap"
run "a wrong release ID in the snapshot" fail "not the checked release 999" publish "$snap"; no_patch "wrong ID"
good_draft; bash "$script" find-draft "$snap" > /dev/null
{ v080; release 900 v0.9.0 false true "$(standard_assets)"; } | page 1
run "the draft was already published by someone else" fail "draft is false, expected true" publish "$snap"; no_patch "already published"
good_draft; bash "$script" find-draft "$snap" > /dev/null
{ v080; release 900 v0.9.0 true true "$(asset 21 "$root/files/arn-linux-x64.tar.gz")" "$(asset 12 "$root/files/arn-macos-arm64.tar.gz")" "$(asset 13 "$root/files/arn-macos-x64.tar.gz")" "$(asset 14 "$root/files/SHA256SUMS")"; } | page 1
run "an asset was replaced after the check" fail "changed after they were checked" publish "$snap"; no_patch "replaced asset"
good_draft; bash "$script" find-draft "$snap" > /dev/null
run "a missing snapshot" fail "is missing; run find-draft first" publish "$root/no-such-snapshot.json"; no_patch "missing snapshot"
jq '.id = "900"' "$snap" > "$root/string-id.json"
run "a snapshot whose ID is a string" fail "has no valid release ID" publish "$root/string-id.json"; no_patch "string ID"

echo "== check-published"
good_draft; bash "$script" find-draft "$snap" > /dev/null; published_state
run "published as a prerelease, v0.8.0 still latest" pass "Release 900 (v0.9.0) is published" check-published "$snap"
good_draft; bash "$script" find-draft "$snap" > /dev/null; published_state; latest v0.9.0
run "the prerelease became the latest release" fail "became the latest release" check-published "$snap" &&
    { printf '%s' "$(cd "$root" && bash "$script" check-published "$snap" 2>&1)" | grep -qF "docs/release-runbook.md" || fail "no runbook pointer"; }
good_draft; bash "$script" find-draft "$snap" > /dev/null; published_state
jq '.draft = true' "$fx/release-900.json" > "$fx/r.tmp" && mv "$fx/r.tmp" "$fx/release-900.json"
run "still a draft after publication" fail "is not published as the policy says" check-published "$snap"
good_draft; bash "$script" find-draft "$snap" > /dev/null; published_state
jq '.prerelease = false' "$fx/release-900.json" > "$fx/r.tmp" && mv "$fx/r.tmp" "$fx/release-900.json"
run "published without the prerelease flag" fail "is not published as the policy says" check-published "$snap"
good_draft; bash "$script" find-draft "$snap" > /dev/null; published_state
jq '.assets[0].id = 77' "$fx/release-900.json" > "$fx/r.tmp" && mv "$fx/r.tmp" "$fx/release-900.json"
run "assets changed during publication" fail "changed during publication" check-published "$snap"
good_draft; bash "$script" find-draft "$snap" > /dev/null
run "the release ID cannot be read after publication" fail "cannot be read after publication" check-published "$snap"
reset; { v080; release 900 v0.9.0 true false "$(standard_assets)"; } | page 1
PRERELEASE=false MAKE_LATEST=true bash "$script" find-draft "$snap" > /dev/null
{ release 900 v0.9.0 false false "$(jq -c '.[] | select(.id == 900) | .assets[]' "$fx/page-1.json")"; } > "$fx/release-900.json"
PRERELEASE=false MAKE_LATEST=true run "a complete release that did not become latest" fail "should be the latest release" check-published "$snap"
latest v0.9.0
PRERELEASE=false MAKE_LATEST=true run "a complete release that is the latest" pass "is published" check-published "$snap"

echo "== environment"
reset
TAG=main run "a tag that is not vX.Y.Z" fail "is not vX.Y.Z" refuse-existing
PRERELEASE=yes run "a prerelease flag that is not true/false" fail "PRERELEASE must be true or false" refuse-existing
EXPECTED="arn-linux-x64.tar.gz;../x" run "an expected name with a path" fail "is not a plain file name" refuse-existing

echo "$cases cases, $failures failure(s)"
[ "$failures" -eq 0 ]
