#!/bin/sh
#
# Turns the commits since the last release into notes somebody would actually read.
#
#   usage: scripts/release-notes.sh [<previous-tag>]
#
# With no argument the previous tag is found the same way scripts/next-version.sh
# finds it, so the notes always cover exactly the commits the version was calculated
# from.
#
# ── Why this is not just `git log --pretty='- %s'` ──────────────────────────
#
# That is what it used to be, and the output read like this:
#
#   - feat: serve HTTPS by default, add the application icon, ...
#   - fix(ci): package only the SQL driver the application uses
#   - ci: build and package for Windows, macOS and Linux
#   - updating few rules for PR merging and pushing to production branch
#
# Four problems, all visible at once. A security fix and a pipeline tweak carry equal
# weight. `fix(ci):` is machine syntax appearing in text meant for a person. Work on
# the build system — which nobody downloading the application has any use for — sits
# in the same list as the features. And whatever was typed at one in the morning
# shows up verbatim.
#
# So: grouped by what the reader came for, prefixes stripped, and the internal
# commits moved out of the way rather than deleted. Nothing is hidden — the full list
# is still there, one click away — but the first thing on the page is what changed
# for whoever installs it.

set -eu

previous=${1:-}

if [ -z "$previous" ]; then
    previous=$(git tag -l 'v*' --merged HEAD --sort=-v:refname | head -n 1 || true)
fi

if [ -n "$previous" ]; then
    range="${previous}..HEAD"
else
    range="HEAD"          # First release: everything.
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

: > "$work/breaking"
: > "$work/feat"
: > "$work/fix"
: > "$work/other"

# ── Read the commits ─────────────────────────────────────────────────────────
# A sentinel per commit so a body can be scanned for a BREAKING CHANGE footer while
# still being attributed to the right subject. --no-merges because a merge commit's
# subject describes the merge and not the change.
subject=""

while IFS= read -r line; do
    case "$line" in
        '<<<commit>>>'*)
            subject=${line#'<<<commit>>>'}

            # Strip `type(scope)!:` and capitalise, so the bullet reads as prose
            # rather than as the syntax a tool needed. A subject that was never
            # conventional in the first place passes through untouched.
            text=$(printf '%s' "$subject" \
                   | sed -E 's/^[a-z]+(\([^)]*\))?!?: *//' \
                   | sed -E 's/^(.)/\U\1/')

            case "$subject" in
                *'!:'*)         printf '%s\n' "- $text" >> "$work/breaking" ;;
                feat:*|feat\(*) printf '%s\n' "- $text" >> "$work/feat" ;;
                fix:*|fix\(*)   printf '%s\n' "- $text" >> "$work/fix" ;;
                perf:*|perf\(*) printf '%s\n' "- $text" >> "$work/fix" ;;
                *)              printf '%s\n' "- $subject" >> "$work/other" ;;
            esac
            ;;
        'BREAKING CHANGE:'*|'BREAKING-CHANGE:'*)
            # The footer's own wording, which says what broke and what to do about
            # it — far more useful than the subject line that carried it.
            printf '%s\n' "${line#*: }" \
                | sed -E 's/^(.)/\U\1/' \
                | sed -E 's/^/- /' >> "$work/breaking"
            ;;
    esac
done <<EOF
$(git log --no-merges --pretty=format:'<<<commit>>>%s%n%b' $range)
EOF

section() {
    [ -s "$2" ] || return 0
    printf '\n### %s\n\n' "$1"
    cat "$2"
}

# Breaking first, because it is the only kind of change that can require the reader
# to do something before upgrading.
section 'Breaking changes' "$work/breaking"
section 'New'              "$work/feat"
section 'Fixed'            "$work/fix"

# Collapsed rather than dropped. Build and documentation work is real work and
# belongs in the record; it just should not be the first thing on the page.
if [ -s "$work/other" ]; then
    count=$(wc -l < "$work/other" | tr -d ' ')
    printf '\n<details>\n<summary>Internal changes (%s)</summary>\n\n' "$count"
    cat "$work/other"
    printf '\n</details>\n'
fi

# Said plainly rather than left as an empty page.
if ! [ -s "$work/breaking" ] && ! [ -s "$work/feat" ] \
   && ! [ -s "$work/fix" ] && ! [ -s "$work/other" ]; then
    printf '\nNo changes recorded since %s.\n' "${previous:-the start of the project}"
fi
