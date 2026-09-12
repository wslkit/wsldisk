# wsldisk — Project Plan

_Last updated: 2026-08-29_

## 1. Problem statement

WSL2 stores each distribution's root filesystem in a dynamically expanding VHDX
(`%LOCALAPPDATA%\wsl\{GUID}\ext4.vhdx` on current WSL, the legacy
`%LOCALAPPDATA%\Packages\<pkg>\LocalState\ext4.vhdx` for MSIX-packaged installs,
or a custom `--location`; measured in [docs/RESEARCH.md](docs/RESEARCH.md)).
Docker Desktop, Rancher Desktop and Podman Desktop do the same for their data volumes.
These files:

- grow whenever the guest writes and **never shrink** when the guest deletes;
- can only be compacted with Hyper-V's `Optimize-VHD` (absent on Windows Home) or a
  manual `diskpart` script, both requiring `wsl --shutdown` first;
- cannot have their *maximum* size reduced without a `resize2fs` dance nobody documents well;
- can be moved only via `wsl --export`/`--import` (slow, loses default user, breaks
  `wsl --set-default`) or by hand-editing the registry;
- have no snapshot mechanism beyond full tar exports;
- have a Microsoft-provided sparse mode (`--set-sparse`) that as of WSL 2.5.6 requires
  `--allow-unsafe` because of data-corruption reports.

The result is a landscape of ~10 partially maintained PowerShell scripts, each solving one
piece (see [docs/RESEARCH.md](docs/RESEARCH.md)). Users routinely lose 50–200 GB to bloated
VHDX files without knowing it.

## 2. Goals

1. One native CLI that covers **inspect → compact → shrink/grow → move → snapshot/restore → diagnose**.
2. Works on **every Windows edition** that runs WSL2 (Home included) by calling the Virtual
   Disk Service API directly.
3. **Safe**: cannot corrupt a running distro; dry-run everywhere; verification after every
   destructive step; clear rollback story.
4. **Fast**: compaction via the API is I/O bound only; move is a file move + registry write.
5. **Scriptable & observable**: `--json`, meaningful exit codes, `--verbose` tracing.
6. Distributable as a single `wsldisk.exe` via winget/scoop/GitHub Releases.
7. **Fully tested: 100% line/branch/function coverage enforced in CI**, plus contract, integration and fuzz tests. See [docs/TESTING.md](docs/TESTING.md).
8. **Complete GitHub Actions automation**: lint, multi-toolchain build, coverage gate, ASan, integration, CodeQL, nightly fuzz, signed releases with winget/scoop publishing. See [docs/CI.md](docs/CI.md).

### Non-goals (v1)

- GUI (a tray app / WinUI front-end is a possible v2 — the core is designed as a library to allow it).
- **Managing WSL1 distros.** WSL1 has no virtual disk — the rootfs is a plain NTFS directory — so compact/shrink/grow/snapshot/mount have no meaning there, and "move" is just a folder copy. WSL1 is also legacy (WSL2 is the default; Docker Desktop requires it). Scope: `list` shows WSL1 distros with `Version 1` and blank disk columns; every other command exits 3 with "X is a WSL1 distribution; wsldisk manages WSL2 virtual disks. Convert with `wsl --set-version X 2`." Nothing else. (Decision D8.)
- Replacing `wsl.exe` for install/uninstall/run.
- Managing generic Hyper-V VMs' disks (may work incidentally via `mount`/`compact --file`, not a target).
- Linux-side agent that must be installed inside the distro. All guest work is done by
  invoking standard tools (`fstrim`, `e2fsck`, `resize2fs`, `df`) through the WSL launch API.

## 3. Target users & user stories

**Developer on a laptop** — "My C: drive has 4 GB free. Show me where the space is and give it back."
→ `wsldisk list`, `wsldisk compact --all`.

**Docker Desktop user** — "`docker system prune` freed 60 GB but Windows still shows it used."
→ `wsldisk compact docker-desktop` (Docker Desktop's distributions are discovered from the registry; the name has changed between Docker versions, so it is never hardcoded).

**Power user with a small SSD** — "Move Ubuntu to D: without re-importing and losing my default user."
→ `wsldisk move Ubuntu D:\WSL`.

**Someone who set a 1 TB max by mistake** — "Cap the disk at 100 GB."
→ `wsldisk shrink Ubuntu --to 100G` (runs `resize2fs` for you, refuses if data doesn't fit).

**Cautious user** — "Snapshot before I upgrade the distro, restore if it breaks."
→ `wsldisk snapshot Ubuntu -m "pre-24.04"`, `wsldisk restore Ubuntu <id>`.

**Someone who ran `--set-sparse --allow-unsafe`** — "Why is my disk not shrinking / why did I get corruption?"
→ `wsldisk doctor` flags sparse distros and offers to turn it off and compact instead.

**CI / fleet admin** — "Nightly job: compact every distro on every dev box, log JSON."
→ `wsldisk compact --all --json --quiet` from Task Scheduler; `wsldisk schedule` helper.

## 4. Feature specification

### 4.1 `list` (M1)

Enumerate all WSL2 disks and print a table.

Sources:

- Registry `HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\{GUID}`: `DistributionName`, `BasePath`, `Version`, `Flags`, `DefaultUid`, `VhdFileName` (present in newer WSL).
- `Lxss\DefaultDistribution` for the default marker.
- Docker Desktop / Rancher / Podman: they register as regular distros (`docker-desktop`, `rancher-desktop`, `rancher-desktop-data`, `podman-machine-*`; older Docker Desktop also had `docker-desktop-data`), so no special casing beyond labeling. Which appliance distros exist is version-dependent and comes from the registry, never from a hardcoded list.
- Optional `--scan <dir>` to find orphaned `*.vhdx` not referenced by any registry entry.

Columns: name, default marker, WSL version, state (running/stopped via `wsl -l -v` or `WslIsDistributionRegistered`+running check), VHDX path, virtual (max) size, actual size on disk (`GetCompressedFileSize` — respects NTFS sparse/compressed), guest used/free (via `df` in the distro, only if running or `--probe`), sparse flag (registry `Flags` bit / `FSCTL_QUERY_ALLOCATED_RANGES`), reclaimable estimate = actual − guest used.

### 4.2 `compact` (M1)

Reclaim unused space in the VHDX without changing its maximum size.

Workflow:

1. Preflight: distro exists, is WSL2, VHDX not currently attached (check via `GetVirtualDiskPhysicalPath` / open with exclusive access), enough free space on host volume for the operation.
2. Unless `--no-trim`: start distro, run `fstrim /` as root. **Not `fstrim -av`** — busybox, and therefore Alpine and most appliance distros, rejects `-a`; `-v` is attempted and its failure tolerated. Its "bytes trimmed" figure reports the whole free extent of the virtual disk, not what compaction will reclaim, so report before/after file sizes instead. Record output. This converts deleted-but-allocated ext4 blocks into discards that zero/unmap the VHDX blocks.
3. `wsl --terminate <distro>`, then wait for the VHDX handle to become available. **Terminating the distribution is not enough on its own** (Decision D9): the WSL utility VM keeps every attached disk open for as long as *any* distribution runs. It does, however, idle out when none is — measured at 66.6s and 66.7s against WSL 2.7.8.0, correcting the original D9 measurement of "never released on a timer", which was almost certainly taken with something else running. So wait up to 90 seconds when nothing else is running, and skip the wait when something is — it cannot be won. Then exit 11 naming the distributions keeping the VM alive and telling the user to re-run with `--shutdown`; only that flag makes wsldisk call `wsl --shutdown`, which releases it at once.
4. Open the VHDX with `OpenVirtualDisk` using **`OPEN_VIRTUAL_DISK_VERSION_2` parameters and `VIRTUAL_DISK_ACCESS_NONE`**, then `CompactVirtualDisk` unattached. This needs no administrator rights. The V2 parameters accept `VIRTUAL_DISK_ACCESS_NONE` and nothing else — any other mask, `VIRTUAL_DISK_ACCESS_METAOPS` included, fails to open with `ERROR_INVALID_PARAMETER`. V1 parameters with `METAOPS` also compact unelevated, but V1 with the wrong mask opens and only fails at the compaction, so the V2 shape is preferred for failing early. Measured in [docs/RESEARCH.md](docs/RESEARCH.md).
   Attaching read-only with `ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY | NO_DRIVE_LETTER | NO_LOCAL_HOST` (admin only) enables the "full" mode that consults the filesystem bitmap. It is an opt-in for disks that were never trimmed, not the default: after `fstrim` the unattached path already reclaimed 100% of the freed space.
5. `CompactVirtualDisk` with `OVERLAPPED` + `GetVirtualDiskOperationProgress` polling → progress bar.
6. Detach, close. Report before/after actual size, elapsed, method used.
7. Optionally restart the distro if it was running before (`--restart`).

Flags: `--all`, `--no-trim`, `--dry-run`, `--json`, `--file <path.vhdx>` (compact an arbitrary VHDX, e.g. detached Docker volume), `--restart`, `--shutdown` (permit `wsl --shutdown` when other distributions hold the disk; see D9).

Non-elevated behaviour: `CompactVirtualDisk` on an unattached disk works without admin — measured, see D10 and docs/RESEARCH.md. Attach requires admin, and is only worth it for a disk that was never trimmed. Strategy: attempt unelevated; if attach is required for full mode and `--elevate` is set (or user is prompted in interactive mode), relaunch self with `runas` and stream results back through a named pipe.

### 4.3 `shrink` / `grow` (M2)

Change the *maximum* virtual size.

`grow <distro> --to <size>`:

1. Terminate distro. `ResizeVirtualDisk` to new size.
2. Start distro, run `resize2fs <rootdev>` (root device discovered via `findmnt -no SOURCE /`; in WSL2 it is `/dev/sdX`). If the disk is partitioned (rare; some imports), refuse with guidance.
3. Verify `df` reflects new size.

Equivalent to `wsl --manage <distro> --resize <size>` in WSL ≥ 2.5 but also fixes the filesystem, which `wsl.exe` does not.

`shrink <distro> --to <size>`:

1. Preflight: ask the filesystem, not the byte count. Mount the disk (step 2) and run `resize2fs -P` to get the true minimum, then refuse anything below it and say what the floor is. "Guest used + 10%" is not a safe check: on a nearly empty distribution with the default 1 TiB disk the measured floor was ~10.9 GiB, because the inode table was sized for 1 TiB (67 million inodes). See [docs/RESEARCH.md](docs/RESEARCH.md).
2. Terminate the distro, then `wsl --mount <vhdx> --vhd --bare` to attach it into the running VM and run `e2fsck -f -y` and `resize2fs <dev> <size>` from **any** other running distribution. Measured: this works unelevated, and no dedicated helper distro is needed as a mechanism. The mounted disk appears as a brand-new `/dev/sdX` — find it by diffing `/proc/partitions` across the mount, never by size, since every WSL disk is 1 TiB by default. Direct `AttachVirtualDisk` would need admin; `wsl.exe --mount` does not.
3. Detach with `wsl --unmount`. `ResizeVirtualDisk` with `RESIZE_VIRTUAL_DISK_FLAG_RESIZE_TO_SMALLEST_SAFE_VIRTUAL_SIZE` or explicit size (never below the fs size; the API refuses unsafe shrink unless `ALLOW_UNSAFE_VIRTUAL_SIZE`, which we never pass).
4. Compact (shrink leaves the file large otherwise).
5. Verify by mounting read-only and running `e2fsck -n`.

Guest tooling: `e2fsck` and `resize2fs` are **not** present in a stock Alpine minirootfs — it ships busybox, which provides `blkid`, `blockdev` and `fstrim` but not e2fsprogs. Whichever distribution hosts the operation must have them; `wsldisk` checks first and says which package to install rather than failing halfway through a resize.

Helper distro: needed only when no other distribution is available to host the mount, or when the user prefers not to involve one. A tiny Alpine rootfs imported on demand as `wsldisk-helper` and removed afterwards, **with e2fsprogs baked in** so it does not depend on the guest having network access; opt-in with `--helper`, or `--via <distro>` to name an existing one.

### 4.4 `move` (M2)

Relocate a distro's VHDX to a new directory/drive.

1. Preflight: target volume is NTFS (ReFS also OK; FAT/exFAT refused — no sparse, 4 GB limit), free space ≥ actual size, distro terminated, not a Store-packaged install being moved *into* a packaged folder.
2. Copy with `CopyFileEx` (progress callback, preserves sparse ranges via `COPY_FILE_ALLOW_DECRYPTED_DESTINATION`; fall back to manual block copy with `FSCTL_SET_SPARSE`+`FSCTL_QUERY_ALLOCATED_RANGES` to preserve sparseness).
3. Verify: size match + optional `--verify` full hash (BLAKE3/xxh3, streamed).
4. Update registry `BasePath` (and `VhdFileName` if present). Note: `BasePath` in the registry uses the `\\?\` prefix for newer distros — preserve whatever form was there.
5. Start distro, run `true`; if start fails, restore old registry value and keep both copies. If success, delete source (unless `--keep-source`).
6. Same-volume move: just `MoveFileEx` + registry write (instant).

Also expose `wsldisk relink <distro> <path>` — registry-only repoint for users who already moved the file manually.

### 4.5 `snapshot` / `restore` / `snapshots` (M3)

Snapshot backends, selected per repo config:

- **`copy`** (default): terminate distro, copy VHDX to `<repo>\<distro>\<timestamp>.vhdx` preserving sparseness, write `manifest.json` (distro metadata: name, GUID, default uid, flags, WSL version, size, hash). Restore = copy back + registry fix-up, or `--as <newname>` to import as a new distro (registry entry + new GUID).
- **`tar`**: `wsl --export` (respects `--format tar.gz/tar.xz/vhd` on newer WSL). Slower, portable, importable with plain `wsl.exe`.
- **`differencing`**: create a child VHDX (`CreateVirtualDisk` with `ParentPath`) and repoint the distro to the child — instant snapshot, copy-on-write. Restore = delete child, repoint to parent. Power feature; requires care because WSL must be pointed at the leaf. Listed as *experimental* in v1.
- **Dedup/incremental** via external tool (`restic`/`borg`/`kopia`) — `wsldisk` produces a stable, sparse VHDX and hands off; hook scripts. Not built in.

Retention: `wsldisk snapshot --keep-last 5 --keep-daily 7`. Scheduling: `wsldisk schedule install --daily 03:00` registers a Task Scheduler job.

### 4.6 `doctor` (M3)

Checks and, with `--fix`, remediation:

- Sparse flag set on a distro → warn (corruption reports, WSL #12103), offer `--set-sparse false` + compact.
- Orphaned VHDX files in common locations not referenced by registry → offer delete / relink.
- Registry `BasePath` pointing to a missing file → offer relink.
- VHDX actual size > guest used + 20% → recommend compact.
- Virtual size near guest used → recommend grow.
- `.wslconfig` sanity: `memory`, `swap`, `swapFile` path exists, `vhdSize` default.
- Host volume free space < 10% or < 2× largest VHDX.
- WSL version and whether `--manage --resize` etc. are available (feature detection).

### 4.7 `mount` / `unmount` (M3)

`wsldisk mount <file.vhdx> [--into <distro>] [--rw]` — thin, safer wrapper around `wsl --mount --vhd` with auto-detection of partitions/filesystem and a read-only default. Used internally by `shrink`.

### 4.8 Additional commands

Small, mostly-orchestration commands that round out the lifecycle. Each reuses the
interfaces and operation framework above; none introduces a new platform dependency.

**M1**

- `info <distro>` — single-distro detail: GUID, registry key, BasePath, default uid/user name, flags, WSL version, VHDX block/sector size, virtual/physical size, sparse, parent chain (differencing), running state, guest kernel and `df`. `--json`.
- `trim <distro>` — only the `fstrim` step, distro stays running. Light-touch, cron-friendly; the right op for sparse-mode disks.
- `orphans [--scan <dir>...]` — VHDX files not referenced by any registry entry (default scan: `%LOCALAPPDATA%\Packages\*\LocalState`, `%LOCALAPPDATA%\wsl`, `%LOCALAPPDATA%\Docker\wsl`, user-configured dirs). Shows size; `--delete` with confirmation; `--relink <distro>` to adopt.
- `completion <powershell|bash|zsh>` — emit shell completion script.
- `config [get|set|edit|path]` — `%APPDATA%\wsldisk\config.toml` (snapshot repo, retention, helper distro, scan dirs) and read-only display of disk-relevant `.wslconfig` keys (`vhdSize`, `swapFile`, `defaultVhdSize`).

**M2**

- `usage <distro> [--top N]` — where space goes inside the guest: largest directories plus a curated list of known caches (apt/dnf/pacman, pip, npm/yarn/pnpm, cargo, go, gradle/maven, docker/podman storage, journal, `~/.cache`, snap, `/tmp`). Runs `du`/`df` via `WslLaunch`; read-only.
- `clean <distro> [--caches] [--journal] [--docker] [--all] [--compact]` — act on `usage`: each category opt-in, prints what will be removed (`--dry-run`), then optionally chains `compact`. Never touches user data outside known cache paths.
- `verify <distro>` — read-only integrity check: VHDX header/metadata via `GetVirtualDiskInformation`, then `e2fsck -n` through `wsl --mount --vhd --bare`. Exit 0 clean, 6 filesystem errors found.
- `default-user <distro> [<name>|<uid>]` — show or set `DefaultUid` (resolves name via `getent passwd` in the guest). The most common post-import/`move` fix.
- `set-sparse <distro> on|off` — guided wrapper around `wsl --manage --set-sparse`: on `on`, prints the corruption caveat (WSL#12103) and requires `--i-understand`; on `off`, compacts afterwards.
- `lock <distro>` — which Windows process holds the VHDX open (Restart Manager `RmGetList` on the file); the "why can't I compact" answer. Also used by `doctor` and by preflight messages.

**M3**

- `clone <distro> <newname> [--location <dir>]` — copy VHDX + new registry entry with fresh GUID, same default uid/flags. Fast scratch copies without export/import.
- `snapshots list|prune|show` — explicit snapshot management verbs (prune runs retention without creating a new snapshot).
- `rescue <distro>` — boot the helper distro with the target VHDX mounted rw at `/mnt/rescue` and drop into a shell; for fixing `/etc/wsl.conf`, `fstab`, sudoers on an unbootable distro.
- `migrate <dir>` — move every WSL2 distro (optionally excluding Docker's) to a new drive with one plan/confirmation; loops `move`.

**Post-1.0**

- `import` / `export` wrappers: progress, `--vhd` format, `--default-user`, `--location`, and `import --from-docker-image <ref>` (pull OCI image → rootfs tar → distro).
- `export-docker <distro>` — inverse: rootfs → OCI/Docker image tarball.
- `stats [--record]` — append sizes to a local history (`%APPDATA%\wsldisk\history.jsonl`), print growth trends/sparklines; foundation for an auto-compact policy.
- `convert <file.vhd|vmdk|qcow2>` — to VHDX + register (VHD natively; others via `qemu-img` if present).

Explicitly **not** commands: distro install/uninstall/run, networking/memory tuning, Docker image management, GUI (see §2).

### 4.9 Global behaviour

- `--json` on every command → machine-readable object, one per line for `--all`.
- `--dry-run` prints the plan and preflight results, changes nothing.
- `--yes` skips confirmations; interactive prompts only when stdin is a TTY.
- Exit codes: 0 ok, 1 generic error, 2 usage, 3 preflight failed (nothing changed), 4 needs elevation, 5 partially completed (see output), 6 integrity check found errors, 10 distro not found, 11 distro running/locked.
- Command tree:

  ```text
  wsldisk
  ├── list, info, usage, orphans, lock            # inspect (read-only)
  ├── trim, compact, clean, shrink, grow, verify  # reclaim & resize
  ├── move, relink, clone, migrate,               # relocate & duplicate
  │   default-user, set-sparse                    # registry/flags
  ├── snapshot, snapshots, restore                # backup
  ├── mount, unmount, rescue                      # access a VHDX
  ├── doctor, schedule, config, completion        # maintenance & tooling
  └── (post-1.0) import, export, export-docker, stats, convert
  ```

- Logging: `--verbose`/`-v`, `--log <file>`; ETW provider optional later.
- Locale: English only in v1; all strings in one table for later i18n.

## 5. Technical approach

### 5.1 Language & toolchain

- **C++23** with MSVC (VS 2022 17.10+); clang-cl kept building in CI for diagnostics.
- CMake ≥ 3.28 presets, vcpkg manifest mode, Ninja.
- Static CRT (`/MT`) for a self-contained binary; x64 and ARM64 builds.
- Warnings as errors, `/W4 /permissive- /utf-8 /Zc:preprocessor`, clang-tidy, ASan in CI debug builds.

### 5.2 Libraries

| Purpose | Library | Notes |
|---|---|---|
| Win32 RAII/error handling | [WIL](https://github.com/microsoft/wil) | `wil::unique_handle`, `THROW_IF_WIN32_ERROR`, registry helpers. Used by WSL itself. |
| CLI | [CLI11](https://github.com/CLIUtils/CLI11) | Subcommands, validators, config file support |
| Formatting | `std::format` / [fmt](https://github.com/fmtlib/fmt) | fmt for older toolchains, colors |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) | `--json` output, manifests |
| Config | [toml++](https://github.com/marzer/tomlplusplus) | `%APPDATA%\wsldisk\config.toml` |
| Hashing | [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) (C impl) or xxHash | move/snapshot verification |
| Tests | [Catch2](https://github.com/catchorg/Catch2) v3 | unit + integration |
| Progress/TUI | [indicators](https://github.com/p-ranav/indicators) | progress bars; FTXUI later if a TUI is added |

### 5.3 Key Windows APIs

- **Virtual Disk Service** (`virtdisk.h`, `VirtDisk.lib`): `OpenVirtualDisk`, `AttachVirtualDisk`, `DetachVirtualDisk`, `CompactVirtualDisk`, `ResizeVirtualDisk`, `CreateVirtualDisk` (differencing), `GetVirtualDiskInformation` (size, physical size, parent, identifier), `GetVirtualDiskOperationProgress`, `GetVirtualDiskPhysicalPath`.
- **Registry**: `HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss` — distro enumeration and `BasePath` repointing.
- **WSL access**: `wsl.exe` for everything the registry cannot answer — launch, terminate, mount, unmount, manage — isolated behind `IWslHost` so it can be swapped for the COM `ILxssUserSession` surface from the open-sourced [microsoft/WSL](https://github.com/microsoft/WSL) later. Guest commands run as `wsl.exe -d <distro> -u root --exec /absolute/path`: `--exec` does not search PATH, and `-u root` is what gets uid 0 regardless of `DefaultUid`.
  **Not `wslapi.dll`.** Measured: every entry point returns `E_ACCESSDENIED` from an unpackaged process, including for a distribution name that does not exist, so the refusal is about the caller rather than the argument. These APIs exist for MSIX distribution launchers, which is the one shape goal 6 rules out. See [docs/RESEARCH.md](docs/RESEARCH.md).
- **File system**: `CopyFileEx`, `MoveFileEx`, `GetCompressedFileSizeW`, `FSCTL_QUERY_ALLOCATED_RANGES`, `FSCTL_SET_SPARSE`, `GetDiskFreeSpaceEx`, `GetVolumeInformation` (fs type).
- **Elevation**: `ShellExecuteEx` with `runas`, `CheckTokenMembership` to detect admin, and a **one-way** named pipe (`PIPE_ACCESS_INBOUND`, worker → parent) for progress plus a separate manual-reset event for cancellation. Not a duplex pipe: I/O on a synchronous file object is serialized, so a pending `ReadFile` blocks the concurrent `WriteFile` the other direction needs and both halves hang — measured, see [docs/RESEARCH.md](docs/RESEARCH.md). The pipe name is 128 random bits, created with `FILE_FLAG_FIRST_PIPE_INSTANCE` and a DACL naming only the launching user; the worker verifies the server's pid, image and SID before it streams, and declining the prompt is `ERROR_CANCELLED` → exit 4 (D11).
- **Task Scheduler**: `ITaskService` COM for `schedule`.

### 5.4 Architecture

Library-first: `libwsldisk` (static lib, no I/O to console) + `wsldisk.exe` (CLI) + tests. Detailed module breakdown in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). All Windows/WSL calls go through small interfaces (`IVirtualDisk`, `IWslHost`, `IRegistry`, `IFileSystem`) so the operation logic (`CompactOperation`, `MoveOperation`, …) is unit-testable with fakes.

Every operation is modeled as: `Plan` (preflight, produces a list of steps + estimated effect, used by `--dry-run`) → `Execute(progress sink)` → `Verify` → `Result`. Steps that mutate state carry an `undo` where possible (registry writes, file moves).

### 5.5 Testing strategy

Full policy in [docs/TESTING.md](docs/TESTING.md). Summary:

- **100% line, branch and function coverage is a hard CI gate** (`llvm-cov` via clang-cl; OpenCppCoverage as secondary). Achievable because all Win32 calls go through a replaceable `Win32Api` table in `platform/`, so every error branch is reachable by fault injection.
- **Unit**: operations against fakes (`FakeRegistry`, `FakeVirtualDisk`, `FakeWslHost`, `FakeFileSystem`, `FakeClock`); golden files for table/JSON output.
- **Platform contract**: `platform/` wrappers against real Win32 on temp VHDX files and a scratch registry key — no WSL required, runs on every PR.
- **Integration** (needs WSL): throwaway Alpine distro imported per run; scenarios for every command including failure/rollback paths. Hosted `windows-2025` if nested virtualisation works (M0 spike), else self-hosted.
- **Fuzz** (nightly, libFuzzer): registry values, `wsl.exe` output (UTF-16LE/BOM/localized), manifests, size strings.
- **Mutation testing** weekly, advisory.

### 5.5.1 CI/CD

Full design in [docs/CI.md](docs/CI.md): `ci.yml` (lint, MSVC+clang-cl × x64+arm64 × Debug+Release, coverage gate, ASan, integration, package), `nightly.yml` (WSL version matrix, fuzz, large-disk perf, mutation), `release.yml` (SBOM, Sigstore + attestations, Authenticode when available, GitHub Release, winget/scoop PRs, post-install smoke), `codeql.yml`, Dependabot + vcpkg baseline bumps. All actions SHA-pinned, least-privilege permissions.

### 5.6 Distribution

- GitHub Releases: `wsldisk-<ver>-x64.zip`, `-arm64.zip`, `.msix`? (no — keep plain exe + optional MSI later). Signed with Sigstore/cosign attestations; Authenticode signing if a certificate becomes available (SmartScreen).
- winget manifest (`wslkit.wsldisk`), scoop bucket entry, Chocolatey optional.
- SBOM via vcpkg export; Dependabot for vcpkg baseline.

## 6. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Corrupting a user's distro (shrink/move/snapshot) | Catastrophic | Never operate on attached disks; verify after each step; keep source until verified; `e2fsck -n` post-checks; loud `--dry-run` culture; integration tests on throwaway distros before every release |
| `wsl.exe` output parsing is localized/fragile | **High** — `wslapi.dll` turned out to be unusable unpackaged, so `wsl.exe` is the primary mechanism, not a fallback | Take everything the registry can answer from the registry, and never parse localized prose: check exit codes, match by column position, and feature-detect the WSL version. Tolerate UTF-16LE with and without a BOM, and CRLF. Migrate to the WSL COM surface when it stabilises |
| WSL internal COM interfaces change between releases | Medium | Abstract behind `IWslHost`; ship `wsl.exe` fallback |
| Store-packaged WSL vs inbox WSL differences (paths, `VhdFileName`, `\\?\` prefixes) | Medium | Test matrix across WSL 1.x inbox, 2.x store; read both `BasePath` and `VhdFileName` |
| Admin requirement for attach | UX | Unelevated compact path works post-`fstrim`; elevate only for full mode/shrink and explain why |
| Docker Desktop holds its VHDX open even when "stopped" | UX | Detect lock, tell user to quit Docker Desktop (`com.docker.backend`), offer `--wait` |
| Sparse-mode distros behave differently (file is NTFS-sparse; sizes misleading) | Medium | Use `GetCompressedFileSize` and allocated ranges; show sparse flag; doctor warning |
| Limited Windows CI for WSL integration tests | Schedule | Validate GitHub runner nested virt in M0; self-hosted fallback |
| Scope creep into GUI/agent | Schedule | Non-goals section; library-first so GUI can come later without rewrite |

## 7. Decisions log

| # | Decision | Rationale |
|---|---|---|
| D1 | Modern C++ over Rust/Go | Whole job is Win32/COM (`virtdisk.h`, registry, WSL COM); WSL itself is C++; WIL makes it ergonomic; single static binary |
| D2 | Library-first (`libwsldisk`) | Enables tests with fakes and a future GUI |
| D3 | Virtual Disk API, not `diskpart`/`Optimize-VHD` | Works on Home; progress reporting; no text scraping |
| D4 | Move via registry `BasePath`, not export/import | Instant, preserves default user/flags/GUID |
| D5 | Never enable sparse mode on behalf of the user | Data-corruption reports; compact is the safe alternative |
| D6 | `copy` snapshot backend default; differencing disks experimental | Simplicity and safety first |
| D7 | MIT license | Maximum adoption; compatible with WIL (MIT) and all chosen deps |
| D8 | WSL2 only; WSL1 is detect-and-refuse | No VHDX to manage; legacy and shrinking user base; avoids a second code path and test-matrix leg for zero functional gain |
| D9 | `compact` never stops another distribution on its own: it refuses, names the ones holding the VM open, and requires `--shutdown` | Terminating the target is not enough — the utility VM holds every attached disk for as long as any distribution runs (measured: still locked after 300 s). Shutting everything down silently would kill unrelated work, including Docker Desktop containers, so the user opts in |
| D10 | Unattached `CompactVirtualDisk` via `OPEN_VIRTUAL_DISK_VERSION_2` + `VIRTUAL_DISK_ACCESS_NONE` is the default path; attach-read-only is an opt-in | Measured: after `fstrim` the unattached path reclaimed 100% of the freed space in 0.2 s with no administrator rights. V1 + `METAOPS` compacts too, so this is a choice rather than the only option: V2 accepts exactly one mask and rejects the rest at open, whereas V1 accepts masks that open and then fail at the compaction. Corrected 2026-08-30 — the original rationale rested on a spike that measured `ATTACH_RW` while calling it `METAOPS` |
| D11 | The elevated half is one verb, not a re-parsed CLI: it takes a path, re-validates it against the caller's own `Lxss` hive, and reads nothing from the pipe — the pipe is output only | Its command line is readable by any same-user process and UAC is not a boundary against the same user, so the defence cannot be secrecy. Keeping the privileged surface to attach-read-only → compact → detach means a tampered argument can at worst name a different VHDX, which the re-validation refuses. Pipe name is 128 random bits with `FILE_FLAG_FIRST_PIPE_INSTANCE`, and the worker checks the server's pid, image and SID — measured against a squatter in docs/RESEARCH.md |

## 8. Open questions

Still open:

- Which `Flags` bit marks sparse mode — no distribution on the test machine had it set, so `list` reads sparseness from the file attributes instead. ([#4](https://github.com/wslkit/wsldisk/issues/4))
- Name collision check: `wsldisk` on winget/scoop/crates — confirm free before first release.

Answered in M0, measurements in [docs/RESEARCH.md](docs/RESEARCH.md):

- **Does unattached `CompactVirtualDisk` reclaim what `fstrim` freed?** Yes, completely, in 0.2 s and with no administrator rights — but only through the V2 open parameters (D10).
- **Is `wsl --terminate` enough to release the disk?** No. The utility VM holds it for as long as any distribution runs (D9).
- **Can guest commands run as uid 0?** Yes, with `wsl.exe -u root --exec`. `wslapi.dll` cannot be used at all from an unpackaged process, which removes `WslLaunch` from the design entirely.
- **Does `shrink` need a helper distribution?** Not as a mechanism: `wsl --mount --vhd --bare` works unelevated and any other running distribution can host the fsck and resize. A helper is a convenience for when none is available, and it must ship e2fsprogs.
- **ARM64 CI availability.** GitHub's hosted `windows-11-arm` runners are free for public repositories, so the arm64 legs build and run their tests natively.

## 9. Success metrics (12 months after 1.0)

- 1k+ GitHub stars; winget installs tracked via release download counts.
- Zero data-loss issues attributable to `wsldisk`.
- Referenced from the microsoft/WSL docs/discussions as the recommended community tool for disk management.
