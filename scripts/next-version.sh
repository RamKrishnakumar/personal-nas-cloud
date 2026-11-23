#!/bin/sh
#
# Works out the next release version from the commits since the last release tag,
# following Conventional Commits. Prints just the number, e.g. 0.7.0.
#
#   usage: scripts/next-version.sh [--explain]
#
# Nobody edits a version by hand, and nothing has to be committed back: the previous
# version comes from the last `v*` tag, so the tags are the record. That is also what
# keeps every branch in step without a merge — a tag is not on a branch.
#
# ── The rules ────────────────────────────────────────────────────────────────
#
#   feat:                       → minor      0.6.0 → 0.7.0
#   fix:  perf:                 → patch      0.6.0 → 0.6.1
#   feat!:  BREAKING CHANGE:    → major, but see below
#   anything else               → no opinion
#
# `chore:`, `docs:`, `ci:`, `refactor:`, `test:`, `style:` and `build:` deliberately
# do not move the number. A release that is only a documentation change does not need
# a new version for anyone to install.
#
# ── Two decisions worth knowing about ───────────────────────────────────────
#
# **Below 1.0, a breaking change bumps the minor, not the major.** 0.x means the
# interface is not yet being promised, and letting one `feat!:` jump straight to
# 1.0.0 would announce stability that has not been decided on. This is the same
# choice semantic-release offers as its pre-1.0 behaviour.
#
# **Nothing bumpable still produces a patch.** Not every commit here is written in
# conventional form — "fix the path to the application" reads like a fix and parses
# as nothing — so a release built only from commits this script has no opinion about
# would otherwise reuse the version already published, and the publish step would
# overwrite an existing release instead of making a new one. A patch is the honest
# answer: something changed, and nothing claimed to be a feature.

set -eu

explain=0
[ "${1:-}" = "--explain" ] && explain=1

note() { [ "$explain" -eq 1 ] && printf '%s\n' "$*" >&2 || true; }

# ── Where to count from ──────────────────────────────────────────────────────
# --merged HEAD so that a tag on some other branch cannot be mistaken for the last
# release of this one. Sorted by version rather than by date, because a patch
# released after a minor would otherwise look like the newer baseline.
last_tag=$(git tag -l 'v*' --merged HEAD --sort=-v:refname | head -n 1 || true)

if [ -z "$last_tag" ]; then
    # No release has been tagged yet, so there is no baseline to bump from and no
    # commit range that means anything. Publish what the project already calls
    # itself, and let the tag that creates become the baseline for next time.
    baseline=$(sed -n 's/^set(NESTDRIVE_FALLBACK_VERSION "\([0-9]*\.[0-9]*\.[0-9]*\)").*/\1/p' \
               CMakeLists.txt | head -n 1)

    if [ -z "$baseline" ]; then
        echo "next-version: no v* tag and no version in CMakeLists.txt" >&2
        exit 1
    fi

    note "No release tag found. Treating $baseline from CMakeLists.txt as the first release."
    printf '%s\n' "$baseline"
    exit 0
fi

baseline=${last_tag#v}
note "Last release: $last_tag"

major=$(echo "$baseline" | cut -d. -f1)
minor=$(echo "$baseline" | cut -d. -f2)
patch=$(echo "$baseline" | cut -d. -f3)

# ── Read the commits ─────────────────────────────────────────────────────────
# --no-merges because a merge commit's subject describes the merge, not the change,
# and "Merge pull request #4 from ..." carries no conventional type. The commits it
# brought in are in the range on their own account.
range="${last_tag}..HEAD"

breaking=0
feature=0
patchfix=0
counted=0

# The whole message, with a sentinel between commits, so the body can be searched for
# a BREAKING CHANGE footer as well as the subject for a `!`.
while IFS= read -r line; do
    case "$line" in
        '<<<commit>>>'*)
            subject=${line#'<<<commit>>>'}
            counted=$((counted + 1))

            # type(scope)!: ... or type!: ... — the ! is the breaking marker.
            case "$subject" in
                *'!:'*) breaking=1; note "  breaking: $subject" ;;
            esac

            case "$subject" in
                feat:*|feat\(*) feature=1;  note "  feature:  $subject" ;;
                fix:*|fix\(*)   patchfix=1; note "  fix:      $subject" ;;
                perf:*|perf\(*) patchfix=1; note "  perf:     $subject" ;;
                *)              note "  ignored:  $subject" ;;
            esac
            ;;
        'BREAKING CHANGE:'*|'BREAKING-CHANGE:'*)
            breaking=1
            note "  breaking footer: $line"
            ;;
    esac
done <<EOF
$(git log --no-merges --pretty=format:'<<<commit>>>%s%n%b' "$range")
EOF

note "Commits considered: $counted"

# ── Apply them ───────────────────────────────────────────────────────────────
if [ "$breaking" -eq 1 ]; then
    if [ "$major" -eq 0 ]; then
        note "Breaking change below 1.0 → minor"
        minor=$((minor + 1)); patch=0
    else
        note "Breaking change → major"
        major=$((major + 1)); minor=0; patch=0
    fi
elif [ "$feature" -eq 1 ]; then
    note "Feature → minor"
    minor=$((minor + 1)); patch=0
elif [ "$patchfix" -eq 1 ]; then
    note "Fix → patch"
    patch=$((patch + 1))
else
    note "Nothing conventional to go on → patch"
    patch=$((patch + 1))
fi

printf '%s.%s.%s\n' "$major" "$minor" "$patch"
