# CI / GitHub Workflows

All workflows live in `.github/workflows/`. Every job pins actions by SHA, uses least-privilege
`permissions:`, and caches vcpkg binaries (`x-gha` binary cache) so a full PR run stays under
~15 minutes.

## `ci.yml` — pull requests and pushes to `main`

Matrix: `{ msvc, clang-cl } × { x64, arm64 } × { Debug, Release }` on `windows-2025`
(arm64 cross-compiles; arm64 tests run only on a self-hosted arm64 runner if available, otherwise build-only).

Jobs:

1. **lint (markdown, actions, python)** — markdown lint, `actionlint` for workflows, `ruff` and the coverage gate's own tests. Runs on every pull request: it needs no C++ toolchain, so it costs seconds, and markdownlint cannot run on a machine that cannot reach the npm registry.
2. **lint (clang-format, clang-tidy)** — the same two tools `scripts/lint.ps1` runs locally, against the pinned LLVM and a `compile_commands.json`. **`main` and the weekly schedule only.** The tools are pinned and deterministic and the local script refuses to run on any other version, so a local pass means what CI would have said; what keeps this off pull requests is the six minutes of vcpkg restore and choco LLVM install before either tool starts, which parallelism cannot help. The trade is that a slip nobody ran locally lands on `main` and is fixed forward — which is exactly what happened across four merges ([#96](https://github.com/wslkit/wsldisk/issues/96)). The cause there was clang-tidy taking a quarter of an hour locally, one process over all 38 files on one core; CI and `scripts/lint.ps1` now run one process per file across the cores, under three minutes on twenty of them, so running it before pushing is a reasonable ask again rather than one people quietly drop.
3. **build-test** — configure with preset, build, `ctest -L unit`, `ctest -L contract` (real Win32, no WSL). Uploads test logs (JUnit via Catch2 reporter) for the PR summary.
4. **coverage** — clang-cl x64 Debug with `-fprofile-instr-generate -fcoverage-mapping`; runs unit + contract (+ integration when available); `llvm-profdata merge`, `llvm-cov export -format=lcov`; `scripts/check-coverage.py --lines 100 --branches 100 --functions 100`; uploads to Codecov; attaches HTML report artifact. **Required check.**
5. **asan** — MSVC Debug with `/fsanitize=address`, unit + contract tests. Runs on every pull request. It is the check most likely to find something no other build can, having caught a `stack-use-after-scope` that MSVC Debug and Release, clang-cl Debug and Release, and the coverage build all passed — which is a poor thing to leave to whether someone remembered.
6. **integration** — `WSLDISK_INTEGRATION=1`, installs WSL (`wsl --install --no-distribution`), fetches the pinned Alpine fixture, runs `ctest -L integration`. Runs on hosted `windows-2025` if the M0 spike confirms nested virtualisation works; otherwise on `[self-hosted, windows, wsl2]` and only for pushes to `main` + `pull_request_target` from trusted authors. Always cleans up test distros in a `post` step.
7. **package** — Release x64/arm64 static builds, `wsldisk --version` smoke test, zip + SHA256SUMS, uploaded as artifacts (consumed by `release.yml`).

> **What gates a pull request, and what does not.** `asan` does: it is a build
> and a test run on a toolchain the job already needs, about five minutes, and
> it is the check most likely to find something no other build can. Leaving that
> to whether someone remembered was the wrong trade.
>
> `lint-native` and `codeql` do not, and for the same reason in both cases:
> their cost is setup, not analysis. `lint-native` spends six minutes restoring
> vcpkg and installing LLVM before clang-tidy starts; CodeQL spends seven on
> whole-program dataflow that would report on `main` a few minutes later
> regardless. Both run on `main` and weekly.
>
> Putting either back is one edit: drop `if: github.event_name != 'pull_request'`
> from `lint-native`, or restore the `pull_request: branches: [main]` trigger in
> `codeql.yml` and set `cancel-in-progress` to
> `${{ github.event_name == 'pull_request' }}` so a superseded run stops instead
> of finishing.

Branch protection on `main`: lint, build-test (all matrix legs), coverage, asan, package required; linear history; signed commits encouraged.

## `nightly.yml` — schedule `0 3 * * *`

- Full integration suite on the self-hosted runner across WSL versions (`wsl --update` latest, plus pinned previous release via `wsl --update --pre-release` matrix / MSI install of last-1).
- Fuzzers (libFuzzer, 10 min each target) with corpus persisted as artifact; crashes open an issue via `peter-evans/create-issue-from-file`.
- Large-disk test: 200 GB sparse VHDX compact/move timing, results appended to `docs/perf/` on `main` via a bot commit.
- Mutation testing report (advisory).
- Dependency drift: `vcpkg x-update-baseline --dry-run` diff surfaced as a job summary.

## `release.yml` — on tag `v*.*.*`

Also runnable by hand (`workflow_dispatch`) against an existing tag, which is
how a release candidate gets rehearsed.

1. **`full matrix`** — calls `ci.yml` as a reusable workflow on the tag. Not a
   reuse of artifacts a pull request left behind: the thing being released has
   to be the thing that was tested. `github.event_name` stays `push` inside a
   called workflow, so the jobs skipped on pull requests — ASan and
   clang-format/clang-tidy — do run for a release.
2. **`build`** — Release x64 and arm64 on their native runners, configured with
   `-DWSLDISK_BUILD_TESTS=OFF -DVCPKG_MANIFEST_FEATURES=` so Catch2 is not a
   dependency of the shipped artifact and therefore not in its SBOM. Zips
   `wsldisk.exe`, `LICENSE`, `README.md`, `COMPACT.md` and `JSON.md`, and writes
   `SHA256SUMS` in the format `sha256sum -c` reads.
3. **SBOM** — CycloneDX 1.5, built from vcpkg's own `vcpkg.spdx.json` documents
   rather than by scanning the binary. The binary is statically linked, so a
   scanner finds almost nothing, while vcpkg knows exactly which port at which
   version went in. Port revisions (`3.12.0#2`) are percent-encoded in the purl,
   because `#` opens a subpath there.
4. **Attestation and signatures** — `actions/attest-build-provenance` over the
   zip and the SBOM, plus keyless Sigstore `cosign sign-blob`. Both mint an OIDC
   token, which is why the job asks for `id-token: write`.
5. **`publish`** — release notes grouped from Conventional Commit subjects since
   the previous tag (a `git describe` away), not from merge messages: a release
   page is read by someone deciding whether to upgrade. A tag containing `-` is
   published as a pre-release. The notes end with the exact commands to verify a
   download.
6. **`smoke`** — a fresh runner per architecture downloads what was *published*,
   checks the SHA-256 against `SHA256SUMS`, runs `gh attestation verify`, then
   unpacks and runs `--version`, `list --json` and `completion`. Every step
   before this one can pass while producing a release nobody can use; this is
   what would notice. `list --json` accepts exit 0 or 3 — a runner with no WSL
   should give a parseable error object, not a crash — and every stdout line is
   parsed as JSON.

Not yet done, deliberately:

- **Authenticode.** There is no certificate. Until there is, SmartScreen will
  warn on first run, and the release notes say so rather than leaving people to
  wonder. An EV or Azure Trusted Signing certificate is the only thing that
  changes this; when one exists it is a signing step before the zip.
- **winget and scoop.** [#38](https://github.com/wslkit/wsldisk/issues/38).
  The post-release smoke tests the GitHub Release; testing a `winget install`
  has to wait until there is a manifest to install.

## `codeql.yml`

CodeQL C/C++ on `main` and weekly; `security-extended` query pack.

Not on pull requests. The analysis took about seven minutes and was the single
slowest check gating a change, and it reasons about whole-program dataflow
rather than a diff -- what it would report on a branch it reports on `main` a
few minutes later.

## `dependabot.yml`

- `github-actions` weekly.
- `vcpkg` baseline bump via a scheduled workflow (`vcpkg-baseline.yml`) that opens a PR with the diff, since Dependabot lacks native vcpkg support.

## `stale.yml`, `labeler.yml`

Housekeeping: label PRs by path (`area:platform`, `area:ops`, `docs`), stale issues after 90 days with `needs-info`.

## Reusable pieces

- `.github/actions/setup-toolchain/` — composite action: install Ninja, LLVM (pinned version), OpenCppCoverage, set up vcpkg with binary cache, export `VCPKG_ROOT`.
- `.github/actions/wsl-fixture/` — composite action: fetch/cache Alpine rootfs (SHA256-pinned), import as `wsldisk-test-<run_id>`, register cleanup.
- `scripts/check-coverage.py` — parses lcov, enforces thresholds, prints per-file gaps as GitHub annotations.

## Secrets

`CODECOV_TOKEN`, `WINGET_GITHUB_TOKEN` (PAT scoped to fork of winget-pkgs), optional `AZURE_TRUSTED_SIGNING_*`. No secrets are available to `pull_request` from forks; the integration job uses `pull_request_target` with an explicit `safe-to-test` label gate.

## Implementation notes

Recorded as the workflows were written; each is a deliberate deviation from the
design above, not an oversight.

- **`clang-tidy` is advisory for now.** clang-tidy 18 crashes parsing the C++23
  standard headers shipped with MSVC 14.4x, which makes its findings unreliable
  rather than merely noisy. The step runs with `continue-on-error: true`; it
  becomes blocking once a pinned LLVM analyses the tree cleanly.
- **The lint job configures the `x64-lint` preset**, a single-config Ninja build,
  purely to produce `compile_commands.json`. A multi-config database lists every
  source once per configuration, which would triple lint time, and its
  `@...modmap` arguments are not something clang-tidy can consume. The project
  sets `CMAKE_CXX_SCAN_FOR_MODULES OFF` for the same reason (no modules are used).
- **The vcpkg binary cache uses `actions/cache` over a local directory**
  (`VCPKG_DEFAULT_BINARY_CACHE`) rather than the `x-gha` provider, which needs
  `ACTIONS_RUNTIME_TOKEN` exported into the job.
- **vcpkg is cloned in full, then checked out at the manifest baseline.** A
  shallow clone cannot resolve a port at a pinned version.
- **`wsl-fixture` has no post step.** Composite actions cannot register one, so
  every job that imports a scratch distro pairs it with
  `.github/actions/wsl-cleanup` guarded by `if: always()`. The cleanup sweeps
  every distribution named `wsldisk-test-*`, not just the current run's, so a
  leak from an earlier job cannot poison a self-hosted runner.
- **The integration job runs on hosted `windows-2025`** and is gated for fork
  pull requests behind the `safe-to-test` label. Whether nested virtualisation on
  hosted runners is reliable enough to keep it there is the open M0 spike.

### Corrections after the first CI run

The first run of these workflows failed six of fifteen checks. What changed, and
why, so the reasoning is not lost:

- **arm64 legs could not find a compiler.** `ilammy/msvc-dev-cmd` was given
  `arch: arm64`, which selects the arm64-*hosted* toolset; the runners are x64,
  so nothing was installed and `cl` never reached PATH. The cross-compile form is
  `amd64_arm64`, which `setup-toolchain` now derives from its `architecture` input.
- **Coverage could not link.** CMake drives clang-cl targets through `lld-link`
  rather than the clang driver, so `-fprofile-instr-generate` arrived as an
  unknown linker argument and `/WX` made it fatal. The objects already request
  the profile runtime through a `/DEFAULTLIB:` directive, so the fix is to put
  clang's compiler-rt directory on the library search path and pass no link flag
  at all. `-print-runtime-dir` names a per-target layout some LLVM packages do
  not ship, so `lib/windows` is tried as well.
- **Codecov failed the job.** No `CODECOV_TOKEN` is configured, and the step ran
  with `fail_ci_if_error: true`. Codecov is reporting, not gating --
  `check-coverage.py` is the gate and has already run by then -- so the upload is
  now skipped without a token and never fails the job.
- **The ASan job now uses MSVC, not clang-cl.** clang-cl's Windows ASan
  miscompiles exception handling on the toolchains we build with: a twenty-line
  program that throws and catches faults with an access violation inside the
  catch block, using clang-cl's own driver. MSVC's ASan handles it, links
  statically with `/MT`, and accepts the debug CRT, so no special vcpkg triplet is
  needed. `_DISABLE_STRING_ANNOTATION` / `_DISABLE_VECTOR_ANNOTATION` are defined
  because the vcpkg dependencies are not built with ASan and the STL's container
  annotations are an all-or-nothing choice for the whole image.
- **The integration job stays on hosted runners.** It passed on
  `windows-2025` on the first attempt, which answers the M0 spike; the
  self-hosted fallback is not needed.

`llvm-cov` also reports "3 functions have mismatched data" while merging. Those
are inline functions from excluded third-party headers; every file under `src/`
is present in the report with its full function list.

### arm64 on native runners

arm64 legs run on GitHub's hosted `windows-11-arm` runners rather than
cross-compiling on x64. Cross-compilation built and linked fine once the vcvars
architecture was fixed, but `catch_discover_tests` runs the freshly built binary
to enumerate test cases, and an arm64 executable cannot run on an x64 host — the
build failed at discovery. Building without registering the tests would have left
the arm64 legs compile-only.

Native runners are free for public repositories, so the arm64 legs now build
*and* run their unit and contract suites, and `package arm64` smoke-tests the
artifact it ships. `setup-toolchain` derives the vcvars host_target pair from
`PROCESSOR_ARCHITECTURE`, so the same action serves both runner families.

clang-cl legs stay x64-only: the arm64 runner image has no Windows LLVM, and
clang-cl is there for second-opinion diagnostics, which x64 already provides.

### clang-tidy is a blocking gate

Resolved: the crash was specific to clang-tidy 18, which ships with Visual
Studio. The pinned LLVM in `setup-toolchain` (20.1.8) analyses the tree cleanly,
so the step no longer runs with `continue-on-error`. The version must stay
pinned — floating it would reintroduce the crash on any machine whose default
clang-tidy is older.

### Reading the clang-tidy output

The lint job prints a running total that looks alarming and is not:

```text
[7/7] Processing file ...\src\lib\errors.cpp.
1199182 warnings generated.
Suppressed 1199186 warnings (1199182 in non-user code, 4 NOLINT).
```

That figure is cumulative across the seven translation units, and every one of
those diagnostics comes from a system or toolchain header — clang's own
intrinsics (`avx512*intrin.h` and friends) dominate, followed by the MSVC STL and
the Windows SDK. **None come from `src/`**: `HeaderFilterRegex` in `.clang-tidy`
restricts reporting to our own tree, and the counter reports what the compiler
generated before that filter applied.

The bulk is `bugprone-reserved-identifier` (with its `cert-dcl37-c` and
`cert-dcl51-cpp` aliases) firing on names like
`_CRT_USE_WINAPI_FAMILY_DESKTOP_APP` — leading-underscore identifiers are
reserved *for the implementation*, and those headers are the implementation.

The line that matters is the last one: anything clang-tidy actually wants changed
is printed as an error above it, and the step fails. `--quiet` does not remove the
noise, because `N warnings generated` comes from the clang frontend rather than
from clang-tidy's own reporting.

### `nightly.yml`

Implemented as of the fuzz work: a `fuzz` job per target and an `integration`
job against whatever WSL build the runner image ships that night.

The fuzz job restores the accumulated corpus from the actions cache, fuzzes on
top of it for `fuzz-seconds` (600 by default, overridable through
`workflow_dispatch`), saves the corpus again, and — on a scheduled run — opens an
issue containing the reproducer and the tail of the fuzzer log. Reproducers are
always uploaded as an artifact, including for manual runs.

`restore-keys` matters: a run that finds nothing still starts from the most
recent corpus rather than from the seeds alone, so coverage accumulates instead
of resetting every night.

The WSL-version matrix, large-disk performance timing and mutation testing that
this file describes are not implemented yet; they arrive with the commands they
would exercise.
