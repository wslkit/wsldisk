# wsldisk

> **This project has moved. It is now `wslkit disk`, part of
> [wslkit](https://github.com/wslkit/wslkit).**
>
> This repository is archived and read-only. Nothing here is maintained, and
> issues and pull requests are closed.

Everything wsldisk did, wslkit does, reimplemented in Go inside a single binary:

| wsldisk | wslkit |
|---|---|
| `wsldisk list` | `wslkit disk list` |
| `wsldisk info <distro>` | `wslkit disk info <distro>` |
| `wsldisk trim <distro>` | `wslkit disk trim <distro>` |
| `wsldisk compact [distro]` | `wslkit disk compact [distro]` |
| `wsldisk usage <distro>` | `wslkit disk usage <distro>` |
| `wsldisk orphans` | `wslkit disk orphans` |
| `wsldisk relink <distro> <path>` | `wslkit disk relink <distro> <path>` |
| `wsldisk move <distro> <dir>` | `wslkit disk move <distro> <dir>` |
| `wsldisk config ...` | `wslkit disk config ...` |
| `wsldisk completion <shell>` | `wslkit completion <shell>` |

The flags, the exit codes and the `--json` shape are the same, so a script
written against wsldisk keeps working once the command name changes.

The port is tracked row by row, including the behaviours that each cost this
project a bug report, in
[docs/wsldisk-parity.md](https://github.com/wslkit/wslkit/blob/main/docs/wsldisk-parity.md).
The reasoning, and the three places the Go version deliberately differs, are in
[ADR 0011](https://github.com/wslkit/wslkit/blob/main/docs/decisions/0011-disk-subcommand.md).

## Why it moved

The wsl tools share most of their platform layer: registry reading, VHDX
parsing, volume queries, redaction and the undo journal were all duplicated
here. One binary means one signing identity, one release, one `--json` contract
and one exit-code contract across every tool in the kit.

## What is still here

The C++ source, its history and its research notes, frozen. `docs/RESEARCH.md`
in particular holds measurements that are cited from wslkit and are not repeated
there: the virtual disk API parameter shapes that work unelevated, how long the
utility VM holds a disk after a distribution stops, and why a duplex pipe
deadlocks an elevated worker.

The `feat/elevate` branch carries the attach-read-only compaction work, which
was never wired to a flag and is not ported: wslkit does not self-elevate.

## The original README

> Compact, shrink, move, inspect and snapshot WSL2 virtual disks — from one native, dependency-free Windows CLI.

[![ci](https://github.com/wslkit/wsldisk/actions/workflows/ci.yml/badge.svg)](https://github.com/wslkit/wsldisk/actions/workflows/ci.yml)
[![codeql](https://github.com/wslkit/wsldisk/actions/workflows/codeql.yml/badge.svg)](https://github.com/wslkit/wsldisk/actions/workflows/codeql.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Status:** alpha. `list`, `info`, `compact`, `trim`, `orphans`, `config` and
`completion` work and are tested end to end against real WSL2. Everything in
[PLAN.md](PLAN.md) §4 beyond those is still ahead; see
[ROADMAP.md](ROADMAP.md).

## Why

Every WSL2 distribution (and Docker Desktop's data volume) lives in an `ext4.vhdx` file that
**only ever grows**. Reclaiming space today means a fragile ritual of `fstrim`, `wsl --shutdown`,
`Optimize-VHD` (Hyper-V module only — not on Windows Home) or a hand-written `diskpart` script.
Moving a distro to another drive means `wsl --export` / `--import` and re-fixing the default user.
Backups are full tarballs. Microsoft's sparse-VHD auto-reclaim is behind `--allow-unsafe` because
it can corrupt data.

Plenty of PowerShell scripts solve one slice of this, and some solve their slice
well — see [Prior art](#prior-art). `wsldisk` aims to be the single, tested,
native tool that covers the whole lifecycle of a WSL2 disk.

## Usage

**Scope:** WSL2 only. WSL1 distributions have no virtual disk; `wsldisk list` shows them, and every other command refuses with a hint to convert (`wsl --set-version <distro> 2`).

Every command takes `--json` for scripting and `--dry-run` where it changes
anything. Exit codes mean something; the full list is in
[docs/JSON.md](docs/JSON.md#exit-codes).

### See where the space went

```text
> wsldisk list
NAME                  VER  STATE    SIZE ON DISK  GUEST USED  RECLAIMABLE  PATH
Ubuntu *              2    running  13.8 GiB      10.0 GiB    3.7 GiB      C:\Users\example\AppData\Local\wsl\{4d1297e9-...}\ext4.vhdx
docker-desktop        2    stopped  96.0 MiB      -           -            C:\Users\example\AppData\Local\Docker\wsl\main\ext4.vhdx
rancher-desktop-data  2    stopped  3.2 GiB       -           -            C:\Users\example\AppData\Local\rancher-desktop\distro-data\ext4.vhdx
rancher-desktop       2    stopped  883.0 MiB     -           -            C:\Users\example\AppData\Local\rancher-desktop\distro\ext4.vhdx
```

`GUEST USED` and `RECLAIMABLE` need the distribution running, because the
guest-used half comes from `df` inside it. Pass `--probe` to start the stopped
ones and measure them too. A `-` means "not measured", never "zero".

### Look at one in detail

```text
> wsldisk info Ubuntu
name:          Ubuntu
guid:          {4d1297e9-bac4-4da1-9867-a2ab591e9581}
registry key:  Software\Microsoft\Windows\CurrentVersion\Lxss\{4d1297e9-...}
wsl version:   2
default:       yes
state:         running
base path:     C:\Users\example\AppData\Local\wsl\{4d1297e9-...}
vhd file name: ext4.vhdx
disk path:     C:\Users\example\AppData\Local\wsl\{4d1297e9-...}\ext4.vhdx
modern layout: yes
flavor:        ubuntu
os version:    24.04
default uid:   1000
flags:         15 (interop, append-nt-path, drive-mounting, undocumented(0x8))
virtual size:  1.0 TiB
file size:     13.8 GiB
size on disk:  13.8 GiB
allocated:     13.8 GiB
sparse:        no
block size:    1.0 MiB
sector size:   512
guest used:    10.0 GiB
guest free:    945.6 GiB
reclaimable:   3.7 GiB
```

### Get the space back

```text
> wsldisk compact Ubuntu
  run fstrim in Ubuntu ...
  stop Ubuntu and wait for its disk ...
  compact C:\Users\example\AppData\Local\wsl\{4d1297e9-...}\ext4.vhdx ...
Ubuntu: 3.7 GiB reclaimed (13.8 GiB to 10.1 GiB)
```

No administrator rights, no Hyper-V module — which is the whole point on
Windows Home. Nothing inside the distribution changes.

`compact` refuses rather than stopping WSL behind your back:

```text
> wsldisk compact Ubuntu
error: C:\Users\example\...\ext4.vhdx is still open in docker-desktop -- the WSL
utility VM keeps every disk open while any distribution runs; re-run with
--shutdown to stop them all, or close them yourself first
```

That is not caution for its own sake: releasing one disk means stopping *every*
distribution, including your containers. `--shutdown` says you are willing.
[docs/COMPACT.md](docs/COMPACT.md) explains why in full.

```text
wsldisk compact --all              # every WSL2 distribution
wsldisk compact Ubuntu --dry-run   # print the steps, change nothing
wsldisk compact --file D:\disks\docker_data.vhdx
```

### Trim without stopping anything

```text
> wsldisk trim Ubuntu
Ubuntu: trimmed. fstrim reported 1004.8 GiB.
that figure is the free extent of the disk, not space reclaimed: compaction is what shrinks the file
run `wsldisk compact Ubuntu` to shrink the file itself
```

`trim` leaves the distribution running, so it is the one reclaim step that is
safe on a schedule. It does not shrink the file on its own — that is what the
second line is telling you.

### Find disks nothing is using

```text
> wsldisk orphans
SIZE ON DISK  PATH
67.8 GiB      C:\Users\example\AppData\Local\Docker\wsl\disk\docker_data.vhdx

67.8 GiB in 1 file(s) that no distribution claims
```

**Read that carefully before deleting anything.** Docker Desktop's
`docker_data.vhdx` holds every volume you have and no WSL distribution claims
it, so it shows up here. `orphans --delete` refuses any file another process
has open, and asks before removing the rest — but the judgement is yours.

```text
wsldisk orphans --delete                       # asks first; --yes skips the prompt
```

### Finding where the space went

`compact` gives back what is free. `usage` says what is not:

```text
> wsldisk usage Ubuntu
SIZE       WHAT                 CLEARABLE  PATH
2.7 GiB    docker storage       no         /var/lib/docker
965.5 MiB  logs                 no         /var/log
840.0 MiB  systemd journal      yes        /var/log/journal
194.4 MiB  apt package lists    yes        /var/lib/apt/lists

4.0 GiB found, of 10.2 GiB the guest reports in use
```

Read-only: it measures and deletes nothing. `no` in the clearable column means
wsldisk cannot tell whether the contents matter — `/var/lib/docker` holds images
you built — not that removing them would break something.

`--by-directory` walks the whole guest and shows the largest directories
whatever they are, with a column saying how much of each the table above already
explained. See [docs/USAGE.md](docs/USAGE.md).

### Moving a disk to another drive

WSL's own answer is `wsl --export` and `wsl --import`, which loses the default
user, the flags and the GUID, and rewrites the whole filesystem to do it. `move`
moves the file and repoints the registry, keeping all of it:

```text
> wsldisk move Ubuntu D:\WSL
Ubuntu now lives at D:\WSL\ext4.vhdx (12.0 GiB)
```

The original is deleted only after the distribution has been seen to boot from
the copy, and a failure before that puts everything back. Within one volume it is
a rename, so no bytes move at all. Across volumes the copy preserves the disk's
holes, which matters on the machines where WSL creates it sparse. See
[docs/MOVE.md](docs/MOVE.md).

### Following a disk you already moved

Moved an `ext4.vhdx` in Explorer and now the distribution will not start? WSL
keeps the path in the registry, and `relink` rewrites it:

```text
> wsldisk relink Ubuntu D:\wsl\Ubuntu\ext4.vhdx
point Ubuntu at D:\wsl\Ubuntu\ext4.vhdx
start Ubuntu to check the new path works
Ubuntu now points at D:\wsl\Ubuntu\ext4.vhdx
```

It starts the distribution afterwards as a smoke test and puts the registry back
exactly as it was if it does not boot — a registry entry that parses but does
not work is worse than one that is obviously wrong, because you would find out
later and from WSL rather than from here.

`wsldisk orphans --relink Ubuntu --to <path>` is the same command reached from
the other direction, for when you are looking at the output of `orphans` and
want to adopt one of the disks it found.

### Settings and shell completion

```text
> wsldisk config
C:\Users\example\AppData\Roaming\wsldisk\config.toml

scan.dirs:                  D:\WSL
compact.trim:               true
compact.restart:            false
wsl.unlock_timeout_seconds: 90
```

```text
wsldisk config set compact.restart true
wsldisk config edit
```

A missing config file is the defaults, not an error. `wsldisk config` also shows
the disk-relevant `.wslconfig` keys, read-only — `wsldisk` never writes that
file.

```powershell
# Add to your PowerShell profile
wsldisk completion powershell | Out-String | Invoke-Expression
```

```bash
source <(wsldisk completion bash)   # or zsh
```

The completion script is generated from the command tree, so it cannot describe
a flag that does not exist. Distribution names complete by asking
`wsldisk list --json` at the time you press Tab.

## Still to come

Everything in [PLAN.md](PLAN.md) §4 that is not above: `shrink`, `grow`, `move`,
`snapshot`/`restore`, `doctor`, `usage`, `clean`, `verify`, `mount`, `clone`,
`migrate`, `rescue`, `schedule`. See [ROADMAP.md](ROADMAP.md) for the order.

## Prior art

Other people have solved parts of this, and the closest is
[wslcompact](https://github.com/okibcn/wslcompact) — a PowerShell module that
reclaims space by `wsl --export`ing a distribution and `--import`ing it back.

The approaches differ in one decision, and it is worth understanding before
picking either:

| | wslcompact (export/import) | wsldisk (in place) |
|---|---|---|
| Free space needed | a full temporary copy of the rootfs | none |
| Time | proportional to the data | seconds |
| What is replaced | the disk **and** the registry entry | nothing but free blocks |
| Recovers fragmented free space | yes — the file is rebuilt contiguous | only what `fstrim` can release |

Rebuilding is the more thorough operation. It defragments, and it does not
depend on discard working inside the guest. The costs are the temporary space,
the time, and a new registry entry — which is why the export/import route has a
reputation for meaning "and then re-fix your default user".

Compacting in place is cheap enough to schedule. It reclaims what `fstrim`
released and nothing more.

**Neither needs administrator rights, and neither needs the Hyper-V module**, so
both work on Windows Home where `Optimize-VHD` does not exist. That is a shared
virtue rather than a difference, and it is worth saying plainly: the "you need
Hyper-V to compact a VHDX" folklore is simply wrong.

Two honest notes. wsldisk has no release yet — if you need to shrink a disk this
afternoon, wslcompact is the tool that exists. And a rebuild reclaims space that
in-place compaction cannot, which is why
[whether to add one](https://github.com/wslkit/wsldisk/issues/67) is an open
question here rather than a settled no.

## Design principles

1. **Native.** Direct use of `virtdisk.h`, the registry and the WSL COM/`wslapi` surface. No `diskpart` scraping, no Hyper-V module dependency, no PowerShell runtime.
2. **Safe by default.** Refuse to touch a disk that is attached/running. Dry-run everything. Verify checksums on move. Never enable sparse mode unless explicitly asked.
3. **Single static binary.** `wsldisk.exe`, distributable via `winget` and `scoop`.
4. **Scriptable.** `--json` output on every command; exit codes that mean something.
5. **Works on Windows Home** — the Virtual Disk Service API is available everywhere Hyper-V's PowerShell module is not.
6. **Fully tested.** 100% line/branch coverage enforced in CI, plus real-Win32 contract tests, end-to-end integration tests on throwaway distros, and nightly fuzzing. This tool touches root filesystems; nothing untested ships.
7. **Fully automated.** GitHub Actions for lint, multi-toolchain builds, coverage gate, ASan, CodeQL, integration, and signed releases published to winget and scoop.

## Tech stack

C++23, CMake + Ninja + vcpkg, [CLI11](https://github.com/CLIUtils/CLI11), [nlohmann/json](https://github.com/nlohmann/json), [toml++](https://github.com/marzer/tomlplusplus), Catch2. No runtime dependencies: `wsldisk.exe` is a single static binary. Details in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Building from source

```powershell
git clone https://github.com/microsoft/vcpkg C:\src\vcpkg   # full clone, not --depth 1
C:\src\vcpkg\bootstrap-vcpkg.bat

git clone https://github.com/wslkit/wsldisk
cd wsldisk
. .\scripts\dev-shell.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Details and the full preset list are in [CONTRIBUTING.md](CONTRIBUTING.md).

## Documents

- [PLAN.md](PLAN.md) — detailed project plan: scope, user stories, technical approach, risks, decisions
- [ROADMAP.md](ROADMAP.md) — milestones and task checklists
- [docs/COMPACT.md](docs/COMPACT.md) — what compaction does, why it needs no admin, and why `--shutdown` exists
- [docs/JSON.md](docs/JSON.md) — the `--json` schema for every command, and the exit codes
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module layout, key Windows APIs, workflows
- [docs/TESTING.md](docs/TESTING.md) — testing policy, 100% coverage gate, fakes, scenarios
- [docs/CI.md](docs/CI.md) — GitHub workflows: ci, nightly, release, codeql, dependabot
- [docs/RESEARCH.md](docs/RESEARCH.md) — prior art and reference material
- [CONTRIBUTING.md](CONTRIBUTING.md) — dev environment, coding standards

## License

[MIT](LICENSE)
