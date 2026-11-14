# Release runbook

How a release is made with `.github/workflows/release.yml`, and what to do
when a check fails. The workflow never publishes on its own and never rolls
anything back by itself.

## Before tagging

1. `release/platforms.json` names this version and exactly the platforms to
   publish. A release that leaves out a platform must have
   `"prerelease": true` and a `"reason"`.
2. `docs/releases/vX.Y.Z.md` exists, and the CLI and IDE versions are X.Y.Z.
3. The commit to tag is already on `main` (merged). The workflow fetches
   `main` and refuses a tag on any other commit.

## Step 1: draft

Push the tag `vX.Y.Z`. The workflow checks the tag, the versions, the release
notes, the platform policy and that the commit is on `main`, builds exactly
the policy's packages, writes `SHA256SUMS` and creates a **draft** release with
those files. It stops if any release or draft already exists for the tag.

Look at the draft on the Releases page: the files, the notes, and that it is
marked as a prerelease when the policy says so.

## Step 2: publish

Actions → Release → Run workflow, choose the **tag** `vX.Y.Z` (not a branch)
and check `publish_draft`. The run finds the draft once and keeps its release
ID, downloads its files by asset ID and checks them against `SHA256SUMS`,
checks the same release again and publishes that ID with the policy's
`prerelease` and `make_latest`. Afterwards it checks the published release and
the latest release.

## When something fails

- **Before publication** (preflight, build, draft checks, file checks, the
  check just before publishing): nothing is public. Fix the cause. A draft
  that must be rebuilt has to be deleted by hand first; the workflow never
  replaces or adds to an existing release or draft.
- **After a failed publication check** ("Check the published release"): the
  release **is already public**. Nothing is rolled back automatically, and
  what to do is a decision for the maintainers:
  1. Read the error: which property differs (draft, prerelease flag, assets,
     or which release is the latest).
  2. To take the release out of view while deciding, turn it back into a
     draft: `gh release edit vX.Y.Z --draft=true` (or "Edit release" →
     "Save as draft"). Anyone who downloaded it in the meantime keeps their
     copy.
  3. If a prerelease became the latest release, mark the previous stable
     release as latest again (`gh release edit <previous tag> --latest`)
     and make the new one a prerelease (`--prerelease`).
  4. Fix the cause, then run step 2 again on the corrected draft.
