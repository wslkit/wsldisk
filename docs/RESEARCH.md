# Research & prior art

_Snapshot as of 2026-08-29. Spike results from M0 go at the bottom._

## Existing tools (all PowerShell/scripts unless noted)

| Tool | Does | Gaps |
|---|---|---|
| [okibcn/wslcompact](https://github.com/okibcn/wslcompact) | Most popular compactor; no admin needed (export/import under the hood) | Slow on large disks; rewrites the whole disk; no move/snapshot/shrink |
| [MechC-ODE/wsl2-shrink](https://github.com/MechC-ODE/wsl2-shrink) | Automates fstrim + Optimize-VHD/diskpart | Wrapper only |
| [brooks-code/WSL-VHDX-Compact](https://github.com/brooks-code/WSL-VHDX-Compact) | fstrim → shutdown → Optimize-VHD, diskpart fallback | Wrapper only |
| [Haenes/wsl2-compact](https://github.com/Haenes/wsl2-compact) | Compaction scripts | Wrapper only |
| [pxlrbt/move-wsl](https://github.com/pxlrbt/move-wsl) | Move distro via export/import | Slow; loses default user unless fixed manually |
| [Jammrock/Move-WSL2NewDrive](https://github.com/Jammrock/Move-WSL2NewDrive) | Menu-driven move | Same |
| [marvint24/wsl-backup-tool](https://github.com/marvint24/wsl-backup-tool) | GUI backup (Go/Wails), alpha | Full exports only |
| [augustoconconi/wsl-backup](https://github.com/augustoconconi/wsl-backup) | Snapshot script | Full exports only |
| `wsl --manage --set-sparse` | Auto-reclaim via TRIM | Behind `--allow-unsafe` since 2.5.6 due to corruption; unreliable ([WSL#12103](https://github.com/microsoft/WSL/issues/12103)) |
| `wsl --manage --resize` (2.5+) | Grow virtual size | Does not run `resize2fs`; no shrink |
| `Optimize-VHD` | Compact | Requires Hyper-V PowerShell module (not on Home) |
| `diskpart compact vdisk` | Compact | Manual script, no progress, admin |

## Reference reading

- Hanselman — [Shrink your WSL2 Virtual Disks and Docker Images](https://www.hanselman.com/blog/shrink-your-wsl2-virtual-disks-and-docker-images-and-reclaim-disk-space)
- Stephen Rees-Carter — [How to Shrink a WSL2 Virtual Disk](https://stephenreescarter.net/how-to-shrink-a-wsl2-virtual-disk/)
- Daniel Cosenza — [Sparse VHD Support in WSL](https://danielcosenza.com/posts/wsl-sparse-vhd/)
- VRAM Lab — [Sparse VHD vs diskpart, measured](https://vramlab.com/posts/wsl2-sparse-vhd-cannot-compact/)
- ashn — [How to fix WSL2 disk space bloat](https://www.ashn.dev/blog/2025-08-14-how-to-fix-wsl2-disk-space-bloat.html)
- Microsoft Q&A — [sparse vhd does not shrink / cannot compact](https://learn.microsoft.com/en-us/answers/questions/1526083/in-wsl2-with-sparse-vhd-the-storage-usage-does-not)
- Microsoft Docs — [Manage disk space (WSL)](https://learn.microsoft.com/en-us/windows/wsl/disk-space), [Virtual Disk API](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/), [wslapi.h](https://learn.microsoft.com/en-us/windows/win32/api/wslapi/)
- Source — [microsoft/WSL](https://github.com/microsoft/WSL) (C++; see `src/windows/service` for registry layout and disk handling), [microsoft/wil](https://github.com/microsoft/wil)

## M0 spike results

Measured on Windows 11 26200.9168, WSL 2.7.8.0 (Store), kernel 6.18.33.1-1, with
Ubuntu 24.04, Docker Desktop and Rancher Desktop installed. Every experiment used
a throwaway distribution imported from the pinned Alpine 3.22.4 rootfs and
unregistered afterwards; no real distribution was read, written or compacted.
Usernames are redacted as `<user>`.

### Compaction (issue #1) — answered

**Unattached compaction works, needs no administrator rights, and reclaims
everything `fstrim` freed.**

Method: import Alpine, `dd` 1 GiB of `/dev/urandom` to `/big.bin`, delete it,
`fstrim /`, `wsl --shutdown`, then `OpenVirtualDisk` + `CompactVirtualDisk`
from an **unelevated** process, trying several parameter shapes against the same file.

> **Corrected 2026-08-30.** The access-mask rows below originally reported that
> `VIRTUAL_DISK_ACCESS_METAOPS` opens but fails to compact. That was wrong: the
> spike's P/Invoke declared `ACCESS_METAOPS = 0x00020000`, which is
> `VIRTUAL_DISK_ACCESS_ATTACH_RW`, not `METAOPS` (`0x00200000`). Attaching
> read-write is what needs elevation, so the spike measured an attach denial and
> attributed it to `METAOPS`. Re-measured with the correct constant while
> building the #21 contract test; the table now reflects the corrected run.
> The reclaim figures below were always measured through V2 + `ACCESS_NONE` and
> are unaffected.

| `OpenVirtualDisk` shape | Access mask | Result |
|---|---|---|
| `OPEN_VIRTUAL_DISK_VERSION_1` (`RWDepth = 1`) | `METAOPS` (`0x00200000`) | opens and compacts, rc = 0 |
| `OPEN_VIRTUAL_DISK_VERSION_1` | `ATTACH_RW` (`0x00020000`) | opens, then compact fails with **5 (ERROR_ACCESS_DENIED)** |
| `OPEN_VIRTUAL_DISK_VERSION_1` | `NONE` | opens, then compact fails with **5** |
| **`OPEN_VIRTUAL_DISK_VERSION_2`** | **`VIRTUAL_DISK_ACCESS_NONE`** | **opens and compacts, rc = 0** |
| `OPEN_VIRTUAL_DISK_VERSION_2` | `METAOPS` | fails to open with 87 (`ERROR_INVALID_PARAMETER`) |
| `OPEN_VIRTUAL_DISK_VERSION_2` | `ATTACH_RW` | fails to open with 87 |

The rule the corrected run shows is that the V2 parameters accept
`VIRTUAL_DISK_ACCESS_NONE` and nothing else — any non-zero mask is rejected at
open with 87 — while V1 derives its rights from the mask and so needs `METAOPS`
to compact. Both `V1 + METAOPS` and `V2 + NONE` compact unelevated.

Sizes for the successful run:

| | bytes |
|---|---|
| after import | 79,691,776 |
| after writing 1 GiB | 1,145,044,992 |
| after `rm` + `fstrim` (file does not shrink by itself) | 1,145,044,992 |
| after `CompactVirtualDisk` | 71,303,168 |
| **reclaimed** | **1,073,741,824 (exactly 1 GiB)** |

Elapsed: **0.2 s**. The distribution booted normally afterwards.

**Consequences:**

1. **PLAN.md §4.2 step 4 is workable, but `OPEN_VIRTUAL_DISK_VERSION_2` with
   `VIRTUAL_DISK_ACCESS_NONE` is the shape to use.** The plan's original
   `VIRTUAL_DISK_ACCESS_METAOPS` does compact unelevated, so it was not the bug
   this section first claimed. The V2 shape is preferred because it has exactly
   one valid spelling: the mask must be `VIRTUAL_DISK_ACCESS_NONE`, and every
   other value fails loudly at open with 87. The V1 shape has two spellings that
   both open and only one that compacts — `NONE` opens and then fails at the
   compaction with `ERROR_ACCESS_DENIED` — so a mistake there surfaces late,
   after the preflight has already told the user their disk is about to shrink.
   `METAOPS` must still not be combined with V2 parameters.
2. **The unelevated path is the default, not a fallback.** `fstrim` followed by
   unattached compaction reclaimed 100% of the freed space without administrator
   rights. The attached read-only "full" mode is not needed for this case and can
   stay an opt-in for disks that were never trimmed. This is the result the whole
   Windows Home premise rested on.
3. **`fstrim -av` is not portable.** busybox (Alpine, and therefore the test
   fixture) rejects `-a`: `fstrim: unrecognized option: a`. Only
   `fstrim [-v] <mountpoint>` works everywhere, so the guest command must be
   `fstrim /` with `-v` attempted and its failure tolerated.
4. `fstrim /` reported `1078939029504 bytes trimmed` — the whole free extent of
   the 1 TB default `vhdSize`, not the 1 GiB actually freed. The number is not a
   useful measure of what compaction will reclaim; report before/after file sizes
   instead.

### VHDX lock behaviour (issues #1, #5) — answered, and it contradicts the plan

`wsl --terminate <distro>` **does not release the distribution's VHDX** while the
WSL utility VM is still running for another distribution.

- while the scratch distro runs: file locked (expected)
- after `wsl --terminate`: the distro disappears from `wsl --list --running`, but
  the VHDX **stays locked for at least 300 s** — polled every 5 s for five
  minutes, never released — and `wsl --manage --set-sparse` fails with
  `Wsl/Service/ERROR_SHARING_VIOLATION`
- after `wsl --shutdown`: the handle **is** released, within one poll interval

The disk is not released on a timer: it is held for as long as the utility VM
lives, and the VM lives as long as any distribution is running.

**Who holds it:** Restart Manager reports `pid 4 System` — the file is attached to
the host storage stack for the utility VM, not open in a user-mode process.
Stopped distributions report no holder at all.

**Consequences:**

- PLAN.md §4.2 step 3 ("`wsl --terminate <distro>` — only this distro, do not
  `--shutdown` others") is not sufficient on any machine where another
  distribution is running, which is the normal case with Docker Desktop
  installed. Recorded as decision **D9**: `compact` terminates the target, and if
  the disk is still locked it exits 11 naming the distributions that are keeping
  the VM alive, and tells the user to re-run with `--shutdown`. It never stops
  another distribution without being asked.
- `wsldisk lock` cannot name a user process for a VM-attached disk. It has to
  distinguish two cases and give different remedies: `System`/pid 4 means "the WSL
  VM still has it attached, run `wsl --shutdown`", while a real pid means "quit
  that application". Reporting "held by System" alone would be useless.

### Shrink mechanism (issue #2) — answered, no helper distro needed

`wsl --mount --vhd --bare` attaches a terminated distribution's VHDX into the
running VM, **unelevated**, and any other running distribution can then fsck and
resize it. A dedicated helper distribution is not required as a *mechanism*; it
is only a convenience for when no other distribution is available.

Method: import a target and a helper from the Alpine fixture, mark the target,
`wsl --shutdown`, start only the helper, mount the target's disk bare, and work
on it from the helper.

| Step | Result |
|---|---|
| `wsl --mount <vhdx> --vhd --bare` | exit 0, **no administrator rights** |
| Device it becomes | a new `/dev/sdX` (`/dev/sdf` in this run) |
| `blkid` | `UUID="f954...6b76" TYPE="ext4"` |
| `blockdev --getsize64` | 1099511627776 (1 TiB) |
| `e2fsck -fn` | clean: `545/67108864 files (0.2% non-contiguous), 4497461/268435456 blocks` |
| `resize2fs -P` | `Estimated minimum size of the filesystem: 2655555` blocks |
| `wsl --unmount` | exit 0 |
| Target afterwards | boots, marker file intact |

**Finding the device: diff `/proc/partitions` across the mount.** Size is useless
for identification — every WSL disk is 1 TiB by default, so this machine already
had three identical 1 TiB devices before mounting a fourth. `blkid` confirms the
filesystem afterwards, but only the diff says *which* device is ours.

**`resize2fs -P` is the preflight `shrink` should use, and the floor is higher
than expected.** On a nearly empty distribution whose disk is the default 1 TiB,
the reported minimum was 2,655,555 blocks — about **10.9 GiB** at 4 KiB blocks —
because the inode table was sized for a 1 TiB filesystem (67 million inodes).
PLAN.md §4.3 proposed "guest used bytes + 10% margin" as the fit check; that
would happily accept a target far below what `resize2fs` can actually produce.
Ask the filesystem instead, and report the floor in the refusal message.

**Guest tooling is not a given.** The stock Alpine minirootfs has `/sbin/apk`,
`/sbin/blkid`, `/sbin/blockdev` and `/sbin/fstrim` — all busybox applets — but
**no `e2fsck` and no `resize2fs`**. `apk add e2fsprogs-extra` installed them from
inside WSL in a few seconds, so the network is available, but that makes `shrink`
depend on the guest having a package manager and connectivity. The helper
distribution wsldisk ships or imports must include e2fsprogs rather than assume it.

**`wsl --exec` needs absolute paths.** Every probe first failed with
`execvpe(blkid) failed: No such file or directory`, including `apk`, even though
the child environment reports
`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:...`. The
lookup does not use that PATH. `IWslHost` must therefore invoke `/sbin/fstrim`,
`/sbin/e2fsck` and friends by absolute path — which is why the compaction spike,
which happened to use `/sbin/fstrim`, worked first time.

**Driving `wsl.exe` from PowerShell has two traps** worth knowing before the
integration helpers are written: an embedded quote does not survive
PowerShell → `wsl.exe` → `sh -c`, so guest commands should avoid `sh -c`
altogether; and with `$ErrorActionPreference = 'Stop'`, the `Failed to translate`
chatter `wsl.exe` writes to stderr is treated as a fatal native-command error.

### Registry layout (issue #4) — answered

`HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss` holds one `{GUID}` subkey
per distribution plus these values at the root:

| Value | Type | Observed |
|---|---|---|
| `DefaultDistribution` | REG_SZ | `{GUID}` of the default |
| `DefaultVersion` | DWORD | `2` |
| `NatIpAddress` | REG_SZ | present even with `networkingMode=mirrored` |
| `OOBEComplete` | DWORD | `1` |

Per distribution:

| Value | Type | Notes |
|---|---|---|
| `DistributionName` | REG_SZ | the name `wsl -l` shows |
| `BasePath` | REG_SZ | **prefix form varies per distro on the same machine** |
| `VhdFileName` | REG_SZ | `ext4.vhdx` on every distro seen, but must not be assumed |
| `Version` | DWORD | `1` or `2` |
| `Flags` | DWORD | `15` (`0xF`) on every distro seen |
| `DefaultUid` | DWORD | `1000` for Ubuntu, `0` for the appliance distros |
| `State` | DWORD | `1` |
| `Modern` | DWORD | `1` — not in our docs; marks the non-MSIX install layout |
| `Flavor` | REG_SZ | `ubuntu`, `alpine`, … — detected from `/etc/os-release` on import |
| `OsVersion` | REG_SZ | `24.04`, `3.22.4` — likewise |
| `RunOOBE` | DWORD | `0` after first run |
| `ShortcutPath` | REG_SZ | Start Menu `.lnk`; absent for `docker-desktop` |
| `TerminalProfilePath` | REG_SZ | Windows Terminal fragment; absent for `docker-desktop` |
| `DockerDesktopBuildNumber` | REG_SZ | Docker Desktop only |

**Findings that change the plan:**

1. **`BasePath` prefix forms differ between distributions on one machine.**
   Ubuntu and both Rancher distros store a bare path
   (`C:\Users\<user>\AppData\Local\wsl\{GUID}`); `docker-desktop` stores
   `\\?\C:\Users\<user>\AppData\Local\Docker\wsl\main`. `move` and `relink` must
   preserve whatever form was already there rather than normalising — PLAN.md
   §4.4 step 4 anticipated this, and it is now confirmed rather than assumed.
2. **Modern WSL does not use the packaged path PLAN.md §1 describes.** Ubuntu
   24.04 installed by `wsl --install` lives in `%LOCALAPPDATA%\wsl\{GUID}\ext4.vhdx`,
   not `%LOCALAPPDATA%\Packages\<pkg>\LocalState\ext4.vhdx`. `Modern=1` marks the
   new layout. Both belong in the `orphans` scan list, and §1 should describe the
   packaged path as legacy.
3. **`docker-desktop-data` no longer exists** on current Docker Desktop — there is
   a single `docker-desktop` distribution whose VHDX is 96 MiB, with image data
   held elsewhere. PLAN.md §3 and §4.2 name `docker-desktop-data` as the thing
   users want to compact; that is version-dependent and must be discovered from
   the registry, never hardcoded.
4. **`Flags` was `15` everywhere, so the sparse bit could not be identified.**
   `WSL_DISTRIBUTION_FLAGS` documents only bits 0–2 (interop, NT path, drive
   mounting); bit 3 is undocumented but always set. No distribution here has
   sparse mode on, so `list` should read sparseness from
   `FILE_ATTRIBUTE_SPARSE_FILE` / `FSCTL_QUERY_ALLOCATED_RANGES` rather than from
   `Flags`.
5. **`Flavor`/`OsVersion` are populated by `wsl --import`** from the rootfs, so
   `info` can show the guest OS without booting the distribution.

### `wslapi.dll` and uid 0 (issue #3) — answered, and it removes an API from the plan

**`wslapi.dll` is unusable from an ordinary unpackaged process.** Every entry
point tried returns the same refusal regardless of its argument:

| Distribution | `WslIsDistributionRegistered` | `WslGetDistributionConfiguration` |
|---|---|---|
| `Ubuntu` (registered for months) | `False` | `0x80070005` E_ACCESSDENIED |
| `docker-desktop` | `False` | `0x80070005` |
| `rancher-desktop`, `rancher-desktop-data` | `False` | `0x80070005` |
| a freshly imported distro | `False` | `0x80070005` |
| the same, after it has been started | `False` | `0x80070005` |
| **a name that does not exist at all** | `False` | `0x80070005` |

Measured from a 64-bit unelevated process with `wslapi.dll` present in
`System32`, while `wsl.exe --list` listed all four distributions.

The last row is the informative one: a name that does not exist returns exactly
the same status as one that does, so the call is being rejected before it ever
looks at the argument. The most likely reason is that these APIs require the
caller to have a package identity — they exist for the MSIX distribution
launchers that ship on the Store — but whatever the mechanism, the behaviour is
uniform refusal, and running elevated was not tested because an API that needed
administrator rights to enumerate distributions would be unusable for `list`
anyway.

**Consequences for the plan:**

1. **PLAN.md §5.3 lists `wslapi.dll` as a primary API.** It cannot be, and the
   conflict is with a goal rather than a detail: goal 6 is a single
   `wsldisk.exe` distributed through winget and scoop, which is exactly the
   unpackaged shape these APIs refuse. `IWslHost` is therefore `wsl.exe` plus the
   registry, with the COM `ILxssUserSession` surface from the open-sourced
   microsoft/WSL as the later upgrade path that §5.3 already anticipated.
2. **The uid-0 question is moot as originally posed.** `WslLaunch` takes no uid —
   it runs as the distribution's `DefaultUid` — so the plan's "`WslLaunch` with
   uid 0" was never going to work as written, and the function is unreachable
   regardless. `wsl.exe -d <distro> -u root --exec /absolute/path` is the
   mechanism, and the compaction spike already proved it: `fstrim` ran as root
   against a distribution whose default user was root, and `-u root` overrides
   `DefaultUid` in every case.
3. **Enumeration comes from the registry, which the registry spike already
   mapped.** Nothing is lost: `WslGetDistributionConfiguration` would have
   returned version, `DefaultUid` and flags, all of which are registry values we
   read directly.
4. **The risk table's mitigation needs rewording.** "Prefer registry +
   `wslapi.dll` + WSL COM" leaves only registry and COM; `wsl.exe` output parsing
   is not a last resort but the primary mechanism for terminate, mount and
   manage, so tolerating its UTF-16, CRLF and localisation matters more than the
   table implied.

### Hosted-runner WSL2 (issue #7) — answered

The integration job runs on hosted `windows-2025`: `wsl --install --no-distribution`,
`wsl --update`, importing the pinned Alpine rootfs and booting it all succeed, in
about 1.5 minutes end to end. Nested virtualisation is available on the hosted
image and no self-hosted runner is needed.

### Fragmented free space (issue #65) — answered, and it contradicts D10

`e4defrag` does nothing. That is the easy half of the answer. The hard half is
what the measurement turned up on the way: **`compact` can reclaim nothing at
all**, and how much it reclaims depends entirely on how the free space is
shaped.

Measured with `spikes/e4defrag/measure.ps1` on a scratch Alpine distribution,
sparse mode off, 512 MiB written and every other file deleted. Size on disk, in
bytes:

| | 4 MiB files | 64 KiB files |
|---|---|---|
| fragmented baseline | 615,514,112 | 930,086,912 |
| `fstrim` + compact | 349,175,808 | 930,086,912 |
| `e4defrag` + `fstrim` + compact | 349,175,808 | 930,086,912 |
| `wsl --export` + `--import` | 337,641,472 | 339,738,624 |

Two results, and the second is the one that matters.

**With coarse free space** (4 MiB holes) compaction reclaims 43% and a rebuild
beats it by 3.3% — a gap small enough to be fresh-filesystem overhead rather
than fragmentation, and not worth a command.

**With fine free space** (64 KiB holes) compaction reclaims **zero bytes**, and
a rebuild reclaims 63% of the file. Not "less"; nothing.

That was checked rather than assumed, because a clean zero is the shape of a
command that failed silently. It did not:

```text
df:      /dev/sdd  1006.9G  138.7M  955.5G   0% /      <- the guest did free it
fstrim:  /: 1080803397632 bytes trimmed   exit=0       <- discard succeeded
compact: {"compacted":true,"reclaimed":0,
          "size_before":502267904,"size_after":502267904}   exit=0
```

The guest freed the space, `fstrim` discarded it, `CompactVirtualDisk` ran and
returned success, and the file did not move by a byte.

The mechanism is granularity. `CompactVirtualDisk` works in VHDX blocks — 2 MiB
by default, which `info` reports as `block_size`. Free space scattered in 64 KiB
pieces never leaves a whole block free, so there is nothing for compaction to
give back. `e4defrag` does not help because it defragments *files*, and what
needs consolidating here is the free space between them.

#### What this changes

**D10 is narrower than it reads.** It records that unattached
`CompactVirtualDisk` "reclaimed 100% of the freed space", measured in spike #1
by writing and deleting one large file. That is true, and it is the best case.
It is not the general case, and nothing said so until now.

**The rebuild in #67 is not optional.** It was filed as "worth having if
`e4defrag` does not close the gap". `e4defrag` does not close the gap, and the
gap is not a few percent — it is everything, in the case a real distribution is
most likely to be in after months of package installs and container layers.

**The user-facing claim needs qualifying.** README and `docs/COMPACT.md` should
say what compaction can and cannot reclaim, rather than leaving a user whose
disk did not shrink to conclude the tool is broken. `compact` reporting
`reclaimed: 0` while exiting 0 is correct and unhelpful; it should say why.

#### Caveats

One machine, one guest, one fragmentation pattern, 512 MiB. The pattern —
delete every other file — is deliberately adversarial and a real filesystem sits
somewhere between the two columns. What the numbers establish is that the range
runs from "reclaims everything" to "reclaims nothing", not where a given
distribution falls in it.

### The utility VM's idle timeout (D9 correction) — measured

D9 records that the VHDX handle "is never released on a timer — it survived
five minutes of polling". That is wrong, and it was making `compact <distro>`
fail on machines where it should have worked.

With **no** distribution running, the utility VM shuts down on its idle timeout
and releases every disk it held. Measured on WSL 2.7.8.0 by terminating the only
running distribution and polling for an exclusive open:

| run | released after |
|---|---|
| 1 | 66.6s |
| 2 | 66.7s |

`vmIdleTimeout` defaults to 60 seconds and is settable in `.wslconfig`, which
matches the numbers: sixty seconds of idle, then a few to wind down. `vmmemWSL`
disappears at the same moment.

The half of D9 that stands is the important one: while *any* distribution is
running the VM stays up and holds every attached disk, so stopping only the
target achieves nothing. The likely explanation for the original measurement is
that something else was running — Docker Desktop registers two distributions
and keeps them up, which is easy to miss.

#### What this changed

`unlock_timeout` was five seconds, chosen *because* the handle was thought never
to be released. It gave up about a minute early, so `compact Ubuntu` refused on a
machine where simply waiting would have worked, and told the user to re-run with
`--shutdown` — stopping every distribution and every container — for no reason.

Now 90 seconds, which outlasts the idle timeout with margin — and spent only
when it can be won. If another distribution is running the VM never idles out, so
the wait is skipped and the refusal is immediate rather than a minute and a half
later.

One trap on the way: `wsl --list --running` can still name a distribution for a
moment after it has been terminated. Asked straight after the terminate, which is
where that decision is made, the stale entry refused every ordinary run a second
after starting it — naming as the holder the distribution wsldisk had just
stopped itself. The check drops the target for that reason; the message printed
after a full wait does not, because by then the list has settled and a target
that appears in it really has been started again.

Found by dogfooding: the reported failure was `compact Ubuntu` sitting through
ten "waiting for the disk to be released" lines and then refusing, on a machine
where `wsl --list --running` said nothing was running at all.

### Elevation relaunch and result streaming (issue #6) — answered, and it changes the IPC shape

**The split works, and the control channel cannot share the pipe.** An
unelevated parent can relaunch itself with `runas`, stream progress back from the
elevated half over a named pipe the launching user alone can open, cancel it from
the unelevated console, and exit with the elevated half's own exit code. What the
plan got wrong is the channel: §5.3 says "named pipe for IPC", and a *duplex*
pipe carrying progress one way and cancellation the other deadlocks both
processes.

Measured on Windows 10 Pro 22H2 (build 19045) — a different host from the M0
spikes above, which ran on Windows 11 26200 — with an unsigned x64 binary built
by MSVC from `spikes/elevation/elevate.cpp`, driven by `spikes/elevation/run.ps1`.
The account is a **split-token administrator**: `BUILTIN\Administrators` is
present in the filtered token as "Group used for deny only". UAC policy was
`EnableLUA=1`, `ConsentPromptBehaviorAdmin=5` (consent prompt for non-Windows
binaries), `PromptOnSecureDesktop=1`. SIDs are redacted as
`S-1-5-21-<redacted>-1001`. Nothing was compacted: the elevated worker sleeps and
reports progress.

#### The two halves, measured

| | parent | elevated worker |
|---|---|---|
| `CheckTokenMembership` (Administrators) | no | yes |
| Integrity level | `0x2000` medium | `0x3000` high |
| Token user SID | `S-1-5-21-<redacted>-1001` | `S-1-5-21-<redacted>-1001` — same |
| `TokenIsElevated` seen through the pipe | — | yes |

The child is the same user one integrity level up, which is why a pipe whose
DACL is `D:P(A;;GA;;;<user sid>)` — that user and nobody else, not even SYSTEM,
with inheritance blocked — is openable by the elevated child with no weakening
at all. Mandatory integrity control does not get in the way either: the
restriction is no-write-**up**, and here the high-IL client is writing to a
medium-IL object.

> **Not measured, and it matters.** This holds because a split-token admin's
> filtered and elevated tokens carry the *same* user SID. Over-the-shoulder
> elevation — a standard user typing a different account's administrator
> credentials — gives the worker a different SID, and this DACL would then deny
> it. There is no second account on the test machine, so that path is untested.
> The implementation must either grant the elevated identity explicitly or fail
> with a clear message instead of an unexplained access denial.

#### Declining the prompt (issue question 2)

`ShellExecuteEx` returns `FALSE` with `GetLastError() == ERROR_CANCELLED` (1223).
No crash, no hang, no orphaned child. Mapping that one error to
`ErrorCode::NeedsElevation` gives the exit code 4 the issue asked for, and it is
the only error worth special-casing at that call site.

#### Cancellation (issue question 4)

A real `CTRL_C_EVENT` — delivered by a second process that does
`AttachConsole(parent_pid)` + `GenerateConsoleCtrlEvent`, not simulated — reaches
the parent's handler, which returns `TRUE` so the default handler does not kill
the process before the worker's exit code can be collected. The worker stops
within one 200 ms poll, writes its result record and exits 5; the parent
propagates 5.

**The Ctrl+C does not reach the elevated child.** The worker installs its own
console control handler and reports over the pipe if it ever fires. It never
did — the child is launched through the AppInfo service and does not join the
parent's console process group. So cancellation *must* be explicit; there is no
inherited signal to rely on. That is a safety property, not a limitation: an
elevated worker holding an attached disk should unwind deliberately, never die
where the console happened to be.

#### The deadlock that changes the design

The first shape tried was the obvious one: a duplex message pipe, the worker
writing progress from its main thread while a second thread sat in `ReadFile`
waiting for a cancel record. Both processes hung after the *first* progress
record, indefinitely, and only unwedged when the parent was killed — which
released the worker's pending read and let its write complete.

The cause is not the pipe but the handle. I/O on a synchronous file object is
serialized: a pending `ReadFile` blocks any concurrent `WriteFile` on the same
handle, whichever thread issues it. The parent had the same bug in mirror image
— its Ctrl+C handler tried to write the cancel record while the main thread was
parked in `ReadFile` on that handle.

Three ways out; the third is what the spike settled on:

| Option | Cost |
|---|---|
| `FILE_FLAG_OVERLAPPED` on both ends | Correct, but overlapped I/O in both halves for one bit of state |
| A second pipe instance for control | Another name, another ACL, another connect to verify |
| **A named event for cancellation** | One manual-reset event, same user-only DACL, `Local\` namespace; the worker polls it each tick |

Cancellation is one bit and never needs a reason, so the event wins. The pipe
becomes one-way (`PIPE_ACCESS_INBOUND`, worker → parent), which also removes any
question of the elevated half *reading* instructions from a channel — see below.
`Local\` is correct because elevation keeps the child in the same session.

#### Name squatting and tampered arguments (issue question 3)

The pipe namespace is machine-wide: any process on the box can create
`\\.\pipe\<name>` first, and a medium-IL process can ordinarily do so. Two
defences, both measured:

| Defence | Result |
|---|---|
| Server creates with `FILE_FLAG_FIRST_PIPE_INSTANCE` | A squatter holding the name makes our own `CreateNamedPipe` fail with `ERROR_PIPE_BUSY` (231), so the parent aborts instead of proceeding |
| Worker verifies the server before trusting it | Refused: server PID did not match the launcher PID it was given. It also compares the server's image path and token user SID to its own |

The name is 128 bits from `BCryptGenRandom`, so winning the race means guessing
the name, not merely being early.

The deeper answer to "arguments an unprivileged process could tamper with" is to
make the elevated half not worth tampering with. Its command line is visible to
any same-user process, and UAC is not a security boundary against the same user
anyway — so the rule for the implementation is:

- The elevated worker implements exactly **one verb** (attach read-only, compact,
  detach), never a re-parsed copy of the full CLI.
- It takes the target path as an argument and **re-validates it itself**:
  canonicalize, confirm it is the `VhdFileName` of a registered distribution in
  the caller's own `Lxss` registry hive, refuse anything else.
- It reads no instructions from the pipe. The pipe is output only.

#### What the shape looks like

```text
parent (medium IL)                    worker (high IL, via runas)
------------------                    ---------------------------
CheckTokenMembership -> not admin
128-bit random pipe name
CreateNamedPipe INBOUND + FIRST_PIPE_INSTANCE, DACL = user only
CreateEvent Local\...-cancel, same DACL
ShellExecuteEx "runas" -------------> verify server pid/image/sid, else exit
ConnectNamedPipe                 <--- I|sid|elevated|integrity
ImpersonateNamedPipeClient       <--- P|pct|text
  (fails with 1368 until a
   message has been read)
Ctrl+C -> SetEvent ---------------->  polled each tick, unwinds
exit with the worker's code      <--- R|code|text
```

`ImpersonateNamedPipeClient` is worth calling out: it fails with
`ERROR_CANNOT_IMPERSONATE` (1368) until data has been read from the pipe, so the
client check belongs after the first record, not at connect time.

### Incidental

`wsl.exe` prints `Failed to translate '<path>'` to stderr for every Windows PATH
entry it cannot map when the calling process has a POSIX-style PATH. It is noise,
not an error, but `IWslHost` must not treat stderr output as failure and should
consider passing `WSLENV`/a clean environment.

### Still open

- Over-the-shoulder elevation (#6): whether the worker can be reached at all
  when a standard user elevates with *another* account's credentials, and what
  the pipe DACL has to say in that case. Needs a second account to measure.
