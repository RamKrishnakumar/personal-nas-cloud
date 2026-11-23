# Contributing to NestDrive

The short version: **branch, open a pull request, write the title carefully, let CI
check it, squash-merge.**

---

## 1. Branches

| Branch | What it is |
|---|---|
| `production` | What has been released. Merging here publishes installers. |
| `main` | Finished work waiting for the next release. |
| `develop` | Integration, where that is useful. |
| `feature/…` `fix/…` | Where work happens. |

Nothing is pushed directly to `main`, `develop` or `production` — all three require a
pull request.

## 2. The pull request title is the important part

It is not a label for the review. With squash merging it becomes **the only commit
subject** on the target branch, and three things read it afterwards:

1. `scripts/next-version.sh`, to decide whether the next release is a minor or a patch.
2. `scripts/release-notes.sh`, to sort the change into **New**, **Fixed** or **Internal**.
3. Whoever reads the history in a year.

```
<type>[(<scope>)][!]: <subject>
```

| Type | Means | Version |
|---|---|---|
| `feat` | a capability that was not there before | **minor** |
| `fix` | behaviour that was wrong and now is not | **patch** |
| `perf` | same behaviour, less of something | **patch** |
| `refactor` | same behaviour, different shape | none |
| `docs` | documentation only | none |
| `test` | tests only | none |
| `build` | build system, dependencies, packaging | none |
| `ci` | the pipeline itself | none |
| `chore` | housekeeping that fits nothing else | none |
| `style` | formatting, no change in meaning | none |
| `revert` | undoing an earlier commit | none |

`!` before the colon marks a breaking change: `feat(api)!: rename the list endpoint`.
Below version 1.0 that bumps the **minor**, not the major — `0.x` does not promise an
interface yet.

Check a title before you push:

```sh
sh scripts/check-commit-subject.sh "feat(ui): add a picker for the shared folder"
```

The **PR title** check runs this and explains any rejection. It re-runs when you edit
the title, so a correction does not need a new push.

### Getting the type right matters more than it looks

A pull request of genuine features titled `chore: updates` produces no version bump
and a release note nobody can use. Nothing fails — that is the problem. If the work
added something, say `feat`.

## 3. What a description should contain

The template asks four things, and they are the four worth asking: what changes, why,
**how it was verified**, and what is left undone.

On verification — "CI is green" is not sufficient on its own, and this project has the
receipts. CI has passed while the application served plain HTTP over its remote name,
while the macOS package contained no Qt at all, and while an icon that was never
generated was reported as present. Each was found by installing the thing and looking
at it. See `docs/CI-CD.md` §8.

A stated limitation is a decision. An unstated one is a bug.

## 4. Before you open it

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure -j 2
```

- **On Windows the Qt DLLs must be on `PATH`**, or every suite exits `0xc0000135`
  with no output:
  `export PATH="/c/Qt/6.10.3/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"`
- **`-j 2`, not more.** A dozen suites bind real TCP sockets on localhost and start
  competing for ports above that.

New behaviour needs a test that fails without the change. A test whose result depends
on what happens to be installed on the machine running it is not a test — see
`docs/CI-CD.md` §7 for the one that cost a day.

## 5. What CI will run

Pushing to a feature branch runs only the secrets check, about twenty seconds. The
full three-platform build runs on the pull request, because that is where its answer
is used. To force a full run before the work is ready, **open the pull request as a
draft** — `pull_request` fires for drafts.

Details, including what to do when a required check sits at *"Expected — waiting for
status to be reported"*, are in [docs/CI-CD.md](docs/CI-CD.md).

## 6. Releasing

Open a pull request from `main` into `production` and merge it. There is no version to
choose and no file to edit: the number is calculated from the commit messages, and the
tag is created only after all three installers have built and uploaded.
