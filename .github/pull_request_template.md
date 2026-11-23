<!--
  The TITLE of this pull request matters more than this description.

  With squash merging it becomes the only commit subject on the target branch, and it
  is then read by the version calculation (minor or patch), the generated release
  notes (New, Fixed or Internal), and anyone looking at the history later.

      <type>[(<scope>)][!]: <subject>

      feat     a capability that was not there before   → minor version
      fix      behaviour that was wrong and now is not  → patch version
      perf     same behaviour, less of something        → patch version
      refactor docs  test  build  ci  chore  style      → no version change

  Add ! before the colon for a breaking change.

  The "PR title" check will reject a title it cannot use and tell you why. To see the
  verdict before pushing:  sh scripts/check-commit-subject.sh "your title"
-->

## What this changes

<!-- One or two sentences. What is different after this is merged. -->

## Why

<!--
  The reasoning, not the diff. What was wrong, or what became possible. If this fixes
  something reported, say how it reproduced — the next person to touch this code will
  want to know whether their change breaks the same thing again.
-->

## How it was verified

<!--
  Delete what does not apply. "CI is green" is not verification on its own: CI has
  passed while the application served plain HTTP, shipped a bundle with no Qt in it,
  and reported an icon that was never generated.
-->

- [ ] `ctest --test-dir build --output-on-failure -j 2` passes
- [ ] Built on a second toolchain (MinGW and MSVC, or CI's three platforms)
- [ ] Ran the application and exercised the change by hand
- [ ] Tested on a real phone / second device, where this touches remote access
- [ ] New or changed behaviour has a test that fails without the change

## Anything left undone

<!--
  Known gaps, deliberate omissions, follow-ups. Say so here rather than leaving it to
  be discovered. A stated limitation is a decision; an unstated one is a bug.
-->
