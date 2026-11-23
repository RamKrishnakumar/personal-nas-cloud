#!/bin/sh
#
# Checks one subject line against Conventional Commits.
#
#   usage: scripts/check-commit-subject.sh "feat(ui): add a storage folder picker"
#
# Used by .github/workflows/pr-title.yml against the pull request title, because with
# squash merging the title *becomes* the commit subject. That is what makes this worth
# enforcing at all: the title is read later by three different things.
#
#   1. scripts/next-version.sh, to decide whether the release is a minor or a patch.
#   2. scripts/release-notes.sh, to sort the change into New, Fixed or Internal.
#   3. A person reading `git log` or the release page in a year's time.
#
# A title of "updates" satisfies nobody and silently costs a version bump.
#
# ── What is deliberately not enforced ───────────────────────────────────────
#
# No rule about imperative mood, no spell check, no ban on the word "and". Checks
# that argue about wording get ignored or worked around, and the cost of a slightly
# awkward title is far lower than the cost of a check nobody trusts.
#
# The length limit is 100, not the 72 that is conventional for git. 72 is a good
# target and a poor rule: the subject of the commit this check was added in is 92
# characters and reads perfectly well. The limit exists to catch a paragraph pasted
# into the title field, not to win an argument about concision.

set -eu

subject=${1:-}

# The types, and what each one does to the version. Keeping the effect next to the
# name is the whole point — somebody choosing a type can see what it costs.
#
#   feat      minor      a capability that was not there before
#   fix       patch      behaviour that was wrong and now is not
#   perf      patch      same behaviour, less of something
#   refactor  none       same behaviour, different shape
#   docs      none       documentation only
#   test      none       tests only
#   build     none       build system, dependencies, packaging
#   ci        none       the pipeline itself
#   chore     none       housekeeping that fits nothing else
#   style     none       formatting with no change in meaning
#   revert    none       undoing an earlier commit
types='feat|fix|perf|refactor|docs|test|build|ci|chore|style|revert'

fail() {
    printf '\n❌ %s\n\n' "$1" >&2
    cat >&2 <<EOF
The title becomes the commit subject when the pull request is squash-merged, and it
is then read by the version calculation, the release notes and anyone looking at the
history. It has to say what changed and in which category.

    <type>[(<scope>)][!]: <subject>

Types, and what each does to the version:

    feat      minor      a capability that was not there before
    fix       patch      behaviour that was wrong and now is not
    perf      patch      same behaviour, less of something
    refactor  none       same behaviour, different shape
    docs      none       documentation only
    test      none       tests only
    build     none       build system, dependencies, packaging
    ci        none       the pipeline itself
    chore     none       housekeeping that fits nothing else
    style     none       formatting with no change in meaning
    revert    none       undoing an earlier commit

Add ! before the colon for a breaking change: feat(api)!: rename the list endpoint

Good:
    feat(ui): add a picker for the shared storage folder
    fix(remote): obtain a Tailscale certificate without waiting for useHttps
    ci: group the release notes by change type
    docs: correct the macOS Gatekeeper steps for macOS 15 and later

Not accepted, and why:
    Fix the path to the application        no type, so it counts as nothing
    fix macOS dmg build                    missing the colon after the type
    feat: updates                          says nothing a reader can use
    feat:                                  no subject at all
    Feat(ui): add a picker                 type must be lower case
    fix(ui): add a picker.                 no full stop on a subject line
EOF
    exit 1
}

[ -n "$subject" ] && [ "$subject" != "" ] \
    || fail 'The title is empty.'

# ── Shape ────────────────────────────────────────────────────────────────────
# Checked before the type, so "Fix the path to the application" is told it has no
# type rather than being told the type "Fix the path to the application" is unknown.
echo "$subject" | grep -Eq '^[a-zA-Z]+(\([^)]*\))?!?: .+' \
    || fail "No conventional type found in: $subject"

type=$(echo "$subject" | sed -E 's/^([a-zA-Z]+).*/\1/')
body=$(echo "$subject" | sed -E 's/^[a-zA-Z]+(\([^)]*\))?!?: *//')

# ── Type ─────────────────────────────────────────────────────────────────────
# Case is checked separately from membership, because "Feat" is a different and much
# more fixable mistake than "feature".
echo "$type" | grep -Eq '^[a-z]+$' \
    || fail "The type must be lower case: '$type' should be '$(echo "$type" | tr 'A-Z' 'a-z')'"

echo "$type" | grep -Eq "^($types)$" \
    || fail "'$type' is not one of the allowed types."

# ── Subject ──────────────────────────────────────────────────────────────────
[ -n "$body" ] || fail 'The title has a type but no subject.'

case "$body" in
    *.) fail "Drop the full stop from the end: $subject" ;;
esac

# Long enough to be a sentence rather than a shrug. "updates" and "wip" both pass a
# format check and tell a reader nothing.
if [ "${#body}" -lt 12 ]; then
    fail "The subject is too short to be useful: '$body'"
fi

if [ "${#subject}" -gt 100 ]; then
    fail "The title is ${#subject} characters; keep it under 100 (72 is a good target)."
fi

printf '✅ %s\n' "$subject"
