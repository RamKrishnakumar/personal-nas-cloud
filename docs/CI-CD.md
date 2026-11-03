# Continuous integration and releases

How NestDrive gets tested on every change and packaged for three platforms.

Two workflows, with one rule between them: **`ci.yml` tests, `release.yml` publishes,
and `release.yml` only ever runs on the `production` branch.**

---

## 1. What runs on every push and pull request

`.github/workflows/ci.yml`.

| Job | Where | Blocking | What it is for |
|---|---|---|---|
| **Secrets and hygiene** | Linux | yes | No `.pem`, `.key`, `.crt`, `.pfx`, `.p12` or `config.ini` in the index. Also validates the Linux desktop entry. |
| **Linux · x86_64 · GCC** | `ubuntu-latest` | yes | Configure, build, 33 test suites, install tree. |
| **Windows · x64 · MSVC** | `windows-latest` | yes | Same. |
| **macOS · Apple Silicon · Clang** | `macos-latest` | yes | Same. |
| **Compiler warnings** | Linux | **no** | `-Wall -Wextra -Wpedantic`, counted and summarised. Advisory. |

### Why these and not a linter

There is no `clang-format` or `clang-tidy` gate, on purpose.

**A C++ compiler is the only authority on whether C++ is valid**, so "does it build on
all three platforms" *is* the syntax check — and it is the one check that catches what
this project is most exposed to. A Windows-only API reached without a `Q_OS_WIN` guard
compiles cleanly on the author's machine and fails only on hardware the author does not
own. Three compilers on three operating systems find that in eight minutes.

`clang-format` was considered and rejected for now. This codebase has a deliberate,
hand-tuned layout — aligned trailing comments, prose-width explanatory blocks — and no
`.clang-format` reproduces it. Adding one would reformat thousands of lines in a diff
that buries real changes, to settle a question nobody is arguing about.

`clang-tidy` is a better eventual fit, but it starts noisy. The warnings job is the
honest first step: it reports what `-Wall -Wextra` finds without blocking anyone, so
the count can be driven down deliberately. **Promote it to blocking once it reads
zero** — that is the point at which it starts preventing regressions rather than
reporting history.

### Details worth knowing

- **`fail-fast: false`.** A Linux failure does not cancel macOS. Each platform's
  verdict is independent, and hiding one turns a single fix-and-push cycle into two.
- **`ctest -j 2`**, not the runner's core count. A dozen suites bind real TCP sockets
  on localhost; more parallelism makes them compete for ports. A flaky gate is worse
  than a slow one, because a failure nobody trusts gets re-run instead of read.
- **`QT_QPA_PLATFORM=offscreen`.** Four suites link `Qt6::Gui`, so `QTEST_MAIN` gives
  them a `QGuiApplication`, which will not start without a platform plugin. Runners
  have no display.
- **`qtimageformats` is installed.** Without it thumbnails come out as JPEG, because
  `ThumbnailCache` prefers WebP when the plugin is there and falls back silently when
  it is not — so the suites would pass while testing the fallback on every platform.
- **The install tree is verified** on every platform, not just built. A broken install
  rule would otherwise surface first on `production`, which is the one branch where it
  must not.

---

## 2. What runs on the production branch

`.github/workflows/release.yml`. Nothing else triggers it.

| Platform | Artifact | Built with |
|---|---|---|
| Windows 10/11 x64 | `NestDrive-<version>-windows-x64.exe` (NSIS installer) and `.zip` (portable) | `windeployqt` via `cmake --install`, then CPack |
| macOS Apple Silicon | `NestDrive-<version>-macos-arm64.dmg` | `macdeployqt`, ad-hoc `codesign`, `hdiutil` |
| Linux x86_64 | `NestDrive-<version>-linux-x86_64.AppImage` | `linuxdeploy` with its Qt plugin |

Plus a `SHA256SUMS.txt`, and release notes generated from the commits since the
previous tag.

### The production-only rule, enforced three times

"We only publish from production" is the kind of rule that holds until someone picks
the wrong branch from a dropdown. So:

1. `on.push.branches` lists only `production` — no other push can start the workflow.
2. The `version` job **fails** if `github.ref` is not `refs/heads/production`. This
   matters because `workflow_dispatch` lets you run a workflow against any branch, so
   trigger filtering alone is not enough.
3. Every other job `needs: version`, so that refusal stops the whole run.

Only the final `publish` job can write to the repository, and its permission is scoped
to `contents: write` — enough to create a release, nothing else.

### The version is in one place

`project(MyNAS VERSION 0.6.0 ...)` in `CMakeLists.txt`. The workflow greps that line.

Releasing is therefore: **bump that number, merge to `production`.** There is no
version to type into a web form, so the tag cannot disagree with the binary.

---

## 3. One-time setup

Three things to do once, none of which a workflow file can do for itself.

### a. Create the production branch

```sh
git checkout main
git pull
git checkout -b production
git push -u origin production
```

### b. Let the release job create releases

**Settings → Actions → General → Workflow permissions** → *Read and write
permissions*. Without it, `gh release create` fails with a 403 after all three builds
have already run, which is a slow way to find out.

### c. Require the checks to pass

**Settings → Branches → Add branch ruleset**, targeting `main`, `develop` and
`production`. Require a pull request, and require these status checks:

```
Secrets and hygiene
Linux · x86_64 · GCC
Windows · x64 · MSVC
macOS · Apple Silicon · Clang
```

Do **not** add *Compiler warnings* — it is advisory and marked
`continue-on-error`, so it reports success regardless and would only add noise.

The check names come from each job's `name:`. Change one in the workflow and the
ruleset silently stops matching, which looks like protection and is not.

---

## 4. Cutting a release

```sh
# 1. Bump the version in CMakeLists.txt, e.g. 0.6.0 → 0.6.1
# 2. Commit it on main
git commit -am "chore: release 0.6.1"
git push

# 3. Merge main into production
git checkout production
git merge --ff-only main
git push
```

That push builds all three packages and publishes them as release `v0.6.1`.

Re-running on the same version replaces the files on the existing release rather than
failing, so a release that broke on one platform can be fixed and re-run without
inventing a version number nobody asked for.

---

## 5. Code signing — where this actually stands

**The binaries are not signed.** Users will see a warning on Windows and macOS, and
this is worth being plain about rather than discovering through a support question.

| Platform | What the user sees | How they get past it |
|---|---|---|
| Windows | SmartScreen: "Windows protected your PC" | **More info → Run anyway** |
| macOS | "cannot be opened because the developer cannot be verified" | **Right-click → Open**, once |
| Linux | nothing — AppImages are not gatekept | `chmod +x` and run |

There is one signature in the pipeline, and it is doing a different job. The macOS
build runs `codesign --sign -`, an **ad-hoc** signature that proves nothing about who
built it. It is there because on Apple Silicon the kernel refuses to execute an arm64
binary with no valid signature at all. It has to run *after* `macdeployqt`, which
rewrites every rpath in the bundle and invalidates whatever the linker left behind.

Removing the warnings entirely costs money and is not a code change:

- **macOS** — an Apple Developer Program membership (99 USD/year) for a Developer ID
  certificate, then notarisation: upload the `.dmg` to Apple, wait for a scan, staple
  the ticket. Roughly 20 lines of workflow and three repository secrets.
- **Windows** — an OV code-signing certificate (~200-400 USD/year, on a hardware token
  or a cloud HSM, since 2023 rules forbid exportable keys). An OV certificate still
  shows SmartScreen warnings until the signature builds reputation; EV certificates
  skip that and cost more.

Worth doing before asking strangers to install this. Not worth doing to install it
yourself.

---

## 6. What is not covered, honestly

**These workflows have never run.** They are written against GitHub's documented
runner images and verified as far as a local machine allows — the CMake install rules,
the deployed Windows tree, and CPack were all run and checked here, and the full suite
is green on MinGW 13.1 and MSVC 14.50. The parts that only exist on GitHub's runners
cannot be verified from here. Expect the first run on each platform to need a fix.

The likeliest places:

- **`jurplel/install-qt-action` and Qt 6.10.3.** If `aqtinstall` does not offer that
  exact patch version for a platform, the step fails early and clearly. Dropping to
  `6.10` or `6.9.*` in both workflows is the fix.
- **The macOS and Linux builds have never been compiled at all.** The `Q_OS_MACOS` and
  `Q_OS_LINUX` branches in `HardwareTier.cpp`, `ThumbnailScanner.cpp`,
  `PathResolver.cpp` and `Permissions.cpp` are written but have never seen a compiler.
  This workflow is the first thing that will ever check them, and the first run is the
  most likely to find something.
- **`linuxdeploy-plugin-qt` and the SQLite driver.** `EXTRA_QT_PLUGINS` names
  `sqldrivers` explicitly because the plugin cannot infer it, and the failure is
  invisible: the AppImage starts, `Qt6::Sql` loads with no driver, and the database
  never opens. If the Linux build runs but thumbnails and accounts do not work, this
  is the first thing to check.
- **`ubuntu-latest` sets the glibc floor.** The AppImage will not run on a
  distribution older than the runner's glibc. Pinning the release job to the oldest
  supported `ubuntu-*` label widens compatibility; it is not pinned now because
  retired labels fail the whole workflow, which is a worse first experience than a
  narrower floor.

### Not in the pipeline on purpose

- **ffmpeg is not bundled.** It is a large dependency with licensing that varies by
  build configuration, and only video features need it — everything else works without
  it. `config.ini.example` documents installing it per platform.
- **`cert.pem` and `key.pem` are not packaged.** Shipping one private key to every
  downloader would mean every install shared it, which is worse than no certificate
  because it looks like encryption and is not. A fresh install serves plain HTTP on
  the LAN until the owner sets up one of the paths in
  [REMOTE-ACCESS.md](REMOTE-ACCESS.md).
- **No integration test against a real router or a real ISP.** UPnP discovery,
  `AddPortMapping` and the Tailscale certificate path cannot be exercised on a GitHub
  runner. Those remain manual, per REMOTE-ACCESS.md §4-5.

---

## 7. A test that the pipeline would have hidden

Worth recording, because it is the exact failure mode CI is supposed to prevent and
instead would have caused.

Two suites — `tst_tailscale` and `tst_certrenewer` — asked "what happens when
Tailscale is not installed?" by pointing the wrapper at a path that does not exist.
But `core::Tailscale::resolveExecutable()` falls back deliberately: an unrunnable
configured path is a misconfiguration, and the useful response is to look in the usual
places. So those tests were really asking about the machine running them, and they
passed only because no machine here had Tailscale on it.

Then one did, and both failed with no code change behind them.

The dangerous part is the direction. **GitHub's runners have no Tailscale either**, so
the pipeline would have been green while the author's own machine was red — and the
obvious conclusion from that, "CI is right, my machine is odd", is the wrong one.

The fix is `tests/fixtures/NoTailscaleOnPath.h`: empty `PATH` for the duration of the
test, so resolution genuinely finds nothing and the test asks its intended question
everywhere. A second test now covers the fallback itself, by putting the fixture *on*
`PATH` under the name the resolver looks for.

The general rule: **a test whose result depends on what is installed outside the
repository is not a test, it is a measurement of the machine.** If CI and a developer
machine disagree, suspect that before suspecting either one.
