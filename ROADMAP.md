# wsldisk — Roadmap

_Last updated: 2026-08-29. Dates are targets, not promises._

Legend: `[ ]` todo · `[~]` deliberately not done (reason and issue linked) · `[x]` done

---

## M0 — Foundations & spikes (≈ 2 weeks)

Goal: de-risk the unknowns, have a compiling skeleton and CI.

**Repo & tooling**

- [x] CMake presets (`x64-debug`, `x64-release`, `arm64-release`, plus `x64-clang`, `arm64-clang`, `x64-coverage`, `x64-asan`, `x64-lint`), vcpkg manifest with a pinned baseline, Ninja Multi-Config
- [x] `libwsldisk` static lib + `wsldisk` exe + `tests` targets; `wsldisk --version` works
- [x] `.clang-format`, `.clang-tidy`, `.editorconfig`, `.gitattributes`, pre-commit hook (`.githooks/`, `scripts/install-hooks.ps1`)
- [x] `scripts/dev-shell.ps1` — one-step local MSVC + CMake + Ninja + LLVM + vcpkg environment
- [x] `ci.yml`: lint job (clang-format, clang-tidy, markdownlint, actionlint, ruff, pytest); build-test matrix MSVC × x64 + arm64 and clang-cl × x64, Debug + Release; arm64 runs natively on `windows-11-arm`; JUnit test reports
- [x] `ci.yml`: **coverage job with 100% line/branch/function gate** (`scripts/check-coverage.py`), Codecov upload, HTML artifact
- [x] `ci.yml`: ASan job, integration job, package job (Release zips + SHA256SUMS)
- [x] `Win32Api` fault-injection table in `platform/` so every error branch is testable
- [x] Test skeleton: `tests/unit` (Catch2), `tests/contract` (real Win32 on temp files), `tests/integration` (gated by `WSLDISK_INTEGRATION`)
- [x] `tests/fuzz` targets and `nightly.yml` ([#10](https://github.com/wslkit/wsldisk/issues/10)) — size-string target; registry and `wsl.exe` output parsers follow their code in M1
- [ ] Fakes for the remaining interfaces (`FakeRegistry`, `FakeVirtualDisk`, `FakeWslHost`, `FakeFileSystem`, `FakeClock`) — land with the interfaces in M1
- [x] Make `clang-tidy` a blocking gate ([#8](https://github.com/wslkit/wsldisk/issues/8)) — needed a pinned LLVM 20; the crash was specific to clang-tidy 18
- [x] Verify WSL2 works on `windows-2025` hosted runners; decide hosted vs self-hosted ([#7](https://github.com/wslkit/wsldisk/issues/7))
- [x] `codeql.yml`, `dependabot.yml` (actions), `vcpkg-baseline.yml`, `labeler.yml`, `stale.yml`
- [x] Composite actions: `setup-toolchain`, `wsl-fixture` (SHA-pinned Alpine 3.22.4 rootfs), `wsl-cleanup`
- [~] Branch protection on `main` — [not wanted at this stage](https://github.com/wslkit/wsldisk/issues/9); CI being green gates a merge by convention. Revisit before 1.0
- [x] Issue/PR templates, CODEOWNERS, `SECURITY.md`

**Technical spikes** (throwaway code under `spikes/`, results recorded in `docs/RESEARCH.md`)

- [x] `CompactVirtualDisk` unattached vs attached-RO: measure reclaimed bytes after `fstrim` ([#1](https://github.com/wslkit/wsldisk/issues/1))
- [x] Confirm `ResizeVirtualDisk` shrink path + `resize2fs` via `wsl --mount --vhd --bare` ([#2](https://github.com/wslkit/wsldisk/issues/2))
- [x] Guest commands as uid 0; `wslapi.dll` found unusable unpackaged ([#3](https://github.com/wslkit/wsldisk/issues/3))
- [x] Registry layout across WSL inbox 1.x / Store 2.x ([#4](https://github.com/wslkit/wsldisk/issues/4))
- [x] Docker Desktop VHDX lock behaviour when Docker is "stopped" vs quit ([#5](https://github.com/wslkit/wsldisk/issues/5))
- [~] Elevation relaunch + named-pipe result streaming — [moved to M2](https://github.com/wslkit/wsldisk/issues/6) and answered there. Compaction turned out to need no elevation at all (D10), so this belonged with the attach-read-only and resize work that does

**Exit criteria: met.** CI is green across 16 required checks with the 100% coverage gate passing and no exclusions; every spike is answered in [docs/RESEARCH.md](docs/RESEARCH.md); PLAN.md §8 now separates what was measured from what is still open.

Five of the six spikes contradicted the plan, which is what they were for: `wslapi.dll` is unusable from an unpackaged process and left the design; `CompactVirtualDisk` needs V2 open parameters, not `METAOPS`; `wsl --terminate` never releases the disk (D9); the `shrink` fit check has to come from `resize2fs -P`; and `docker-desktop-data` no longer exists. The headline result is that unelevated compaction reclaims everything `fstrim` freed, in 0.2 s -- the premise the whole project rested on, now measured.

---

## M1 — `list`, `info`, `compact`, `trim`, `orphans` (v0.1.0, ≈ 3–4 weeks)

Goal: the 80% use case — see where the space is and get it back — shippable.

Eight phases in dependency order. One ticket is one pull request: its own unit,
contract and (where it touches WSL) integration tests, the 100% coverage gate,
and `scripts/lint.ps1` clean. Each ticket's body carries the measured facts from
M0 it has to encode, so they are not rediscovered.

**Phase 1 — platform foundations** (all four in parallel; no dependencies)

- [x] `IRegistry`, `Win32Registry`, `FakeRegistry` with canned hives for every layout spike #4 found ([#20](https://github.com/wslkit/wsldisk/issues/20))
- [x] `IVirtualDisk`, `Win32VirtualDisk`, `FakeVirtualDisk` — V2 open parameters + `ACCESS_NONE` only, with a contract test pinning that `METAOPS` fails ([#21](https://github.com/wslkit/wsldisk/issues/21))
- [x] `IWslHost`, `WslExeHost`, `FakeWslHost` — `wsl.exe` only, `wslapi.dll` is gone; absolute paths, UTF-16 decode, stderr noise; fuzz target for the `--list` decoder ([#22](https://github.com/wslkit/wsldisk/issues/22))
- [x] `IFileSystem` extensions (directory scan, allocated ranges, delete) and `IClock`/`FakeClock` ([#23](https://github.com/wslkit/wsldisk/issues/23))

**Phase 2 — model** (needs Phase 1)

- [x] `Distro` model and registry enumeration — prefix forms preserved, `VhdFileName` optional, WSL1 enumerated but refused elsewhere; fuzz target for the value parser ([#24](https://github.com/wslkit/wsldisk/issues/24))
- [x] Size probes — virtual, on-disk, allocated, guest `df` only when running or `--probe`; unknown is a value, not a failure ([#25](https://github.com/wslkit/wsldisk/issues/25))

**Phase 3 — operation framework** (needs `IClock`; parallel with Phase 2)

- [x] `Plan → Execute → Verify`, LIFO undo, `ProgressSink`, `OperationRunner` with automatic rollback ([#26](https://github.com/wslkit/wsldisk/issues/26))

**Phase 4 — read-only commands** (needs Phases 2 and 3)

- [x] CLI plumbing — `--json`, `-v`/`--log`, `--yes`, `--dry-run`, exit codes, table and JSON renderers, golden tests, shared WSL1 refusal ([#27](https://github.com/wslkit/wsldisk/issues/27))
- [x] `wsldisk list` ([#28](https://github.com/wslkit/wsldisk/issues/28))
- [x] `wsldisk info <distro>` ([#29](https://github.com/wslkit/wsldisk/issues/29))
- [x] `wsldisk orphans` with both scan layouts, `--delete`, `--relink` — the first mutating command, exercises rollback ([#30](https://github.com/wslkit/wsldisk/issues/30))

**Phase 5 — mutating commands** (needs Phase 4)

- [x] `wsldisk trim <distro>` — `fstrim /`, never `-av`; honest about what "bytes trimmed" means ([#31](https://github.com/wslkit/wsldisk/issues/31))
- [x] `CompactOperation` and `wsldisk compact` — D9 refuse-and-name with `--shutdown`, D10 unelevated path, `--all`/`--file`/`--dry-run`/`--no-trim`/`--restart`; the milestone's acceptance test ([#32](https://github.com/wslkit/wsldisk/issues/32))
- [~] Attached-RO "full" compaction and `--elevate` — moved to M2 with the elevation IPC ([#6](https://github.com/wslkit/wsldisk/issues/6))

**Phase 6 — configuration** (needs Phase 4; parallel with Phase 5)

- [x] `config.toml` and `wsldisk config get|set|edit|path`; read-only `.wslconfig` display; fuzz target for the parser ([#33](https://github.com/wslkit/wsldisk/issues/33))
- [x] `wsldisk completion powershell|bash|zsh`, generated from the CLI11 tree ([#34](https://github.com/wslkit/wsldisk/issues/34))

**Phase 7 — integration harness** (needs `IWslHost`; unblocks every command's integration tests, so it starts early)

- [x] `ScratchDistro` RAII fixture, junk/hash helpers, second-distro helper for D9, the `wsl.exe` traps encoded ([#35](https://github.com/wslkit/wsldisk/issues/35))

**Phase 8 — release** (docs need every command; `release.yml` can start any time)

- [x] README usage, `docs/COMPACT.md`, `docs/JSON.md`, real `docs/ARCHITECTURE.md` layout ([#36](https://github.com/wslkit/wsldisk/issues/36))
- [x] `release.yml`: tag → full matrix → SBOM → attestations → GitHub Release → post-install smoke ([#37](https://github.com/wslkit/wsldisk/issues/37))
- [ ] winget + scoop manifests — may slip to M2 if the winget review is slow ([#38](https://github.com/wslkit/wsldisk/issues/38))

Already done in M0 and carried forward: `nightly.yml` with fuzzing and the
integration suite; the size-string fuzz target.

**Status: code complete.** Every command works, is covered end to end against
real WSL2, and the release workflow is in place. What remains is publishing.

Two things surfaced along the way that the plan did not anticipate:

- **`orphans` finds Docker Desktop's `docker_data.vhdx`.** No WSL distribution
  claims it, so it is an orphan by the definition here -- and it holds every
  volume the user has. `--delete` grew an `IFileSystem::is_locked` guard and a
  warning above the prompt because of it. "No registry entry points at it" is
  not "nothing needs it".
- **The `fstrim` figure is three orders of magnitude out.** Spike #1 measured
  it; what the plan did not say is how far the honesty has to reach. The number
  is `bytes_offered` in JSON, never `Estimate::bytes_freed`, and every rendering
  carries the caveat.

**Exit criteria:** A Windows Home user with no Hyper-V module can `winget install wsldisk` and reclaim space from Ubuntu and Docker Desktop with one command.

Blocked on a published `v0.1.0`: the winget and scoop manifests need a real
version and the SHA-256 of published assets, and the exit criterion is an
install that works. `release.yml` is ready and rehearsable against a tag through
`workflow_dispatch`; pushing the first tag is a decision for the repository
owner, not something CI should do on its own.

---

## M2 — `move`, `relink`, `grow`, `shrink`, `usage`, `clean`, `verify` (v0.2.0, ≈ 5 weeks)

- [x] `MoveOperation`: preflight (fs type, free space, running), sparse-preserving copy with progress, registry repoint, start test, rollback, source cleanup; same-volume fast path ([#106](https://github.com/wslkit/wsldisk/issues/106)). `--verify` full-hash comparison is still to come
- [x] `wsldisk relink <distro> <path>` — the operation existed behind `orphans --relink`; promoted to a command of its own, and taught to honour `--json` ([#63](https://github.com/wslkit/wsldisk/issues/63))
- [x] Elevation spike: `runas` relaunch, one-way pipe + cancel event, squatting defences ([#6](https://github.com/wslkit/wsldisk/issues/6)) — `spikes/elevation`, results in [docs/RESEARCH.md](docs/RESEARCH.md). A duplex pipe deadlocks, so the plan's IPC shape changed (D11); the `--elevate` code path itself is still to build
- [ ] `GrowOperation`: `ResizeVirtualDisk` + `resize2fs`; detect partitioned disks and refuse
- [ ] Helper-distro mechanism (tiny Alpine rootfs, on-demand import/remove) or `--via <distro>`
- [ ] `ShrinkOperation`: fit check with margin, `e2fsck -f`, `resize2fs <size>`, `ResizeVirtualDisk` (safe flag only), compact, `e2fsck -n` verify
- [ ] `wsldisk mount` / `unmount` wrappers (read-only default)
- [ ] `wsldisk verify <distro>` (VHDX metadata + `e2fsck -n`, exit 6 on errors)
- [x] `wsldisk usage <distro>` with curated cache catalogue (`data/caches.toml`) ([#109](https://github.com/wslkit/wsldisk/issues/109))
- [x] `wsldisk usage <distro> --by-directory` ([#69](https://github.com/wslkit/wsldisk/issues/69))
- [ ] `wsldisk clean <distro>` per-category flags, `--dry-run`, `--compact` chaining; never outside catalogue paths
- [ ] `wsldisk default-user <distro> [name|uid]`
- [ ] `wsldisk set-sparse <distro> on|off` with caveat gate and post-off compact
- [ ] `wsldisk lock <distro>` via Restart Manager; reused in preflight error messages
- [ ] Feature detection of `wsl.exe` capabilities (`--manage`, `--mount --vhd`, version)
- [ ] Unit tests (100% coverage) incl. rollback paths with injected failures at every mutating step
- [ ] Contract tests: `ResizeVirtualDisk` grow/shrink-safe on temp VHDX; sparse-preserving copy on real NTFS; FAT/exFAT refusal
- [ ] Integration tests for move (cross-volume via VHD-backed second volume in CI), relink failure rollback, grow, shrink fit-refusal and success with `e2fsck -n`
- [ ] `release.yml` extended: winget (`wingetcreate`) and scoop bucket PRs
- [x] `docs/MOVE.md`; `docs/RESIZE.md` still to come

**Exit criteria:** Full disk lifecycle (compact/grow/shrink/move) without touching `diskpart`, PowerShell or `wsl --export`.

---

## M3 — `snapshot`, `restore`, `clone`, `rescue`, `migrate`, `doctor`, `schedule` (v0.3.0, ≈ 5 weeks)

- [ ] Snapshot repo layout + `manifest.json` schema; config in `%APPDATA%\wsldisk\config.toml`
- [ ] `copy` backend (sparse-preserving) with `--keep-last/--keep-daily/--keep-weekly` retention
- [ ] `tar` backend via `wsl --export` (format flags where supported)
- [ ] `restore` in place and `--as <newname>` (new GUID registry entry)
- [ ] `differencing` backend (experimental, behind `--experimental`)
- [ ] `wsldisk snapshots list|prune|show`
- [ ] `wsldisk clone <distro> <newname>` (VHDX copy + fresh GUID registry entry)
- [ ] `wsldisk rescue <distro>` (helper distro, target mounted rw at `/mnt/rescue`)
- [ ] `wsldisk migrate <dir>` (plan + confirm, loops `move`, `--exclude-docker`)
- [ ] Hook scripts (`pre-snapshot`, `post-snapshot`) for restic/borg/kopia hand-off
- [ ] `wsldisk doctor` checks + `--fix` remediations (sparse warning, orphans, broken BasePath, bloat, capacity, `.wslconfig` sanity)
- [ ] `wsldisk schedule install|remove|status` via Task Scheduler COM
- [ ] Unit tests (100% coverage) incl. retention policy edge cases, manifest schema validation, every `doctor` check with/without defect
- [ ] Integration tests: snapshot → mutate → restore → verify content; `restore --as`; retention; `doctor --fix` per defect; scheduled task registered and removed
- [ ] Fuzz target: `manifest.json` parser
- [ ] `docs/SNAPSHOTS.md`, `docs/DOCTOR.md`

**Exit criteria:** Nightly unattended compact + snapshot with retention on a dev machine, zero manual steps.

---

## M4 — Hardening & 1.0 (≈ 3 weeks)

- [ ] Test matrix: Windows 10 22H2, Windows 11 24H2/25H2; WSL inbox, WSL Store 2.x latest and latest-1; Home + Pro
- [ ] ARM64 build tested on real hardware
- [ ] Fuzz corpora reviewed; zero open fuzz crashes; mutation-testing survivors triaged
- [ ] Coverage exclusions audited (< 10 `LCOV_EXCL` in codebase, each justified)
- [ ] Error-message review: every failure tells the user what to do next
- [ ] Localization-safe: never parse localized `wsl.exe` text on hot paths
- [ ] Performance: compact/move progress accuracy; large-disk (500 GB+) test
- [ ] Security review: elevation IPC, path handling (`\\?\`, junctions), no writes outside intended dirs
- [ ] Authenticode signing (if certificate available) / SmartScreen reputation plan
- [ ] `CHANGELOG.md`, semantic versioning policy, support policy (last two WSL releases)
- [ ] Announce: microsoft/WSL discussions, r/bashonubuntuonwindows, Hacker News, dev.to write-up

**Exit criteria:** v1.0.0 tagged; no known data-safety issues; winget/scoop current.

---

## Later / v2 ideas (unscheduled)

- `import`/`export` wrappers with progress, `--vhd`, `--default-user`, `--location`; `import --from-docker-image <ref>`
- `export-docker <distro>` — rootfs → OCI/Docker image tarball
- `stats [--record]` — size history + growth trends/sparklines
- `convert <vhd|vmdk|qcow2>` → VHDX + register
- Tray/WinUI 3 GUI on top of `libwsldisk` (space usage at a glance, one-click compact)
- Auto-compact policy daemon (compact when idle and reclaimable > N GB)
- Deduplicated snapshot backend built-in (content-defined chunking) instead of external tools
- Support Hyper-V VM disks and Windows Sandbox explicitly
- PowerShell module wrapper (`Get-WslDisk`, `Optimize-WslDisk`) generated from `--json`
- Upstream: propose `wsl --manage --compact` to microsoft/WSL, referencing this tool's implementation
- Localization (resource strings)

---

## Versioning

- `0.x`: features land per milestone; CLI may change with notice in CHANGELOG.
- `1.0`: CLI and `--json` schema are stable; breaking changes only in `2.0`.
