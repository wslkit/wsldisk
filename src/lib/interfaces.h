#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "errors.h"

namespace wsldisk {

/// What a volume reports about itself. Sizes are bytes.
struct VolumeInfo {
    /// Filesystem name as Windows reports it: `NTFS`, `ReFS`, `exFAT`, `FAT32`.
    std::string filesystem_name;
    /// Total size of the volume.
    std::uint64_t total_bytes = 0;
    /// Bytes available to the calling user (respects quotas).
    std::uint64_t free_bytes = 0;

    /// Whether the volume supports sparse files and >4 GiB files, i.e. whether a
    /// VHDX may live on it. FAT and exFAT do not qualify.
    [[nodiscard]] bool supports_vhdx() const noexcept {
        return filesystem_name == "NTFS" || filesystem_name == "ReFS";
    }
};

/// What `GetVirtualDiskInformation` reports about a VHDX. Sizes are bytes.
struct VirtualDiskInfo {
    /// The maximum the disk can grow to -- what `shrink` and `grow` change.
    std::uint64_t virtual_size = 0;
    /// What the disk currently occupies, as the VHDX format sees it. This is not
    /// the same as the file's size on the host volume, which is what
    /// `IFileSystem::file_size_on_disk` reports and what users actually notice.
    std::uint64_t physical_size = 0;
    std::uint32_t block_size = 0;
    std::uint32_t sector_size = 0;
    /// Set for a differencing disk. Empty otherwise, which is every WSL disk
    /// unless someone has built a chain by hand.
    std::wstring parent_path;
};

/// Progress of a long-running virtual-disk operation, as a fraction.
struct DiskProgress {
    std::uint64_t current = 0;
    std::uint64_t total = 0;
};

/// Called periodically while a compaction runs. Returning false asks the
/// operation to stop at the next opportunity.
using ProgressCallback = std::function<bool(const DiskProgress&)>;

/// An open handle to a virtual disk. Closing is the destructor's job, so an
/// operation cannot leak one down an error path.
class IVirtualDiskHandle {
public:
    IVirtualDiskHandle() = default;
    IVirtualDiskHandle(const IVirtualDiskHandle&) = delete;
    IVirtualDiskHandle& operator=(const IVirtualDiskHandle&) = delete;
    IVirtualDiskHandle(IVirtualDiskHandle&&) = delete;
    IVirtualDiskHandle& operator=(IVirtualDiskHandle&&) = delete;
    virtual ~IVirtualDiskHandle() = default;

    [[nodiscard]] virtual Result<VirtualDiskInfo> information() const = 0;

    /// Compacts in place, reporting progress until it finishes. The disk must
    /// not be attached, and nothing here needs administrator rights.
    [[nodiscard]] virtual Status compact(const ProgressCallback& progress) = 0;
};

/// Opens virtual disks.
///
/// The parameter shape is not a caller's choice. The implementation hard-codes
/// `OPEN_VIRTUAL_DISK_VERSION_2` with `VIRTUAL_DISK_ACCESS_NONE`, which is the
/// only mask V2 accepts and which compacts unelevated (PLAN.md D10).
class IVirtualDisk {
public:
    IVirtualDisk() = default;
    IVirtualDisk(const IVirtualDisk&) = delete;
    IVirtualDisk& operator=(const IVirtualDisk&) = delete;
    IVirtualDisk(IVirtualDisk&&) = delete;
    IVirtualDisk& operator=(IVirtualDisk&&) = delete;
    virtual ~IVirtualDisk() = default;

    /// Opens an existing VHDX for metadata and compaction.
    [[nodiscard]] virtual Result<std::unique_ptr<IVirtualDiskHandle>> open(
        const std::filesystem::path& path) const = 0;

    /// Creates a fixed-maximum dynamic VHDX. Only the tests need this -- it is
    /// how contract tests get a real disk without shelling out to `diskpart`.
    [[nodiscard]] virtual Status create(const std::filesystem::path& path,
                                        std::uint64_t maximum_size) const = 0;
};

/// A disk attached read-only, detached again by the destructor.
///
/// Attaching is what "full" compaction needs: with the disk attached, the
/// compaction consults the filesystem bitmap instead of relying on what the
/// guest already discarded. It is worth it only for a disk that was never
/// trimmed -- after `fstrim` the unattached path reclaims everything (D10) --
/// and it is the one part of the job that needs an administrator token.
class IAttachedDisk {
public:
    IAttachedDisk() = default;
    IAttachedDisk(const IAttachedDisk&) = delete;
    IAttachedDisk& operator=(const IAttachedDisk&) = delete;
    IAttachedDisk(IAttachedDisk&&) = delete;
    IAttachedDisk& operator=(IAttachedDisk&&) = delete;
    /// Detaches. This runs even when the compaction failed or was cancelled:
    /// leaving a disk attached is worse than leaving it uncompacted.
    virtual ~IAttachedDisk() = default;

    /// Compacts with the disk attached, reporting progress until it finishes.
    /// Returning false from the callback asks it to stop at the next
    /// opportunity, and a stopped compaction still detaches.
    [[nodiscard]] virtual Status compact_full(const ProgressCallback& progress) = 0;
};

/// Attaches a virtual disk read-only.
///
/// Deliberately separate from `IVirtualDisk` rather than another mode on its
/// `open()`. That interface hard-codes the one parameter shape that compacts
/// unelevated, and its single valid spelling is the property worth keeping: a
/// wrong mask fails at open rather than at the compaction, after the user has
/// been told their disk is about to shrink. Attaching needs a different shape
/// *and* an elevated token, so it is a different capability with its own
/// interface, used only by the elevated worker (D11).
class IDiskAttach {
public:
    IDiskAttach() = default;
    IDiskAttach(const IDiskAttach&) = delete;
    IDiskAttach& operator=(const IDiskAttach&) = delete;
    IDiskAttach(IDiskAttach&&) = delete;
    IDiskAttach& operator=(IDiskAttach&&) = delete;
    virtual ~IDiskAttach() = default;

    /// Opens and attaches read-only, with no drive letter and no local host
    /// mount -- the disk becomes compactable, not browsable.
    [[nodiscard]] virtual Result<std::unique_ptr<IAttachedDisk>> attach_read_only(
        const std::filesystem::path& path) const = 0;
};

/// Where an elevated worker's output goes while it runs.
///
/// The worker's own console is hidden, so anything it does not report here is
/// never seen. `progress` returning false asks it to stop -- that is the only
/// thing that travels the other way, and it rides a separate event rather than
/// the pipe, because a pending read on a synchronous handle blocks the write
/// the other direction needs (measured, docs/RESEARCH.md).
struct ElevatedSink {
    std::function<void(std::string_view)> message;
    ProgressCallback progress;
};

/// Whether this process holds an administrator token, and how to borrow one.
///
/// The split exists because most of what wsldisk does needs no elevation at all
/// (D10). Only the attach path does, so only that runs elevated, and the user
/// still sees one output stream and one exit code.
class IElevation {
public:
    IElevation() = default;
    IElevation(const IElevation&) = delete;
    IElevation& operator=(const IElevation&) = delete;
    IElevation(IElevation&&) = delete;
    IElevation& operator=(IElevation&&) = delete;
    virtual ~IElevation() = default;

    /// Is this process already running with an administrator token?
    [[nodiscard]] virtual bool is_elevated() const = 0;

    /// Relaunches this executable elevated to run one privileged verb against
    /// one path, streams what it reports into `sink`, and returns its exit code.
    ///
    /// The verb and the path are all the elevated half is told; it re-validates
    /// the path against the registry itself and reads nothing back from the
    /// pipe (D11). Declining the prompt is not a failure of this call in any
    /// interesting sense -- it comes back as `ErrorCode::NeedsElevation`.
    [[nodiscard]] virtual Result<int> run_elevated(std::string_view verb,
                                                   const std::filesystem::path& target,
                                                   const ElevatedSink& sink) const = 0;
};

/// What running one `wsl.exe` command produced.
struct WslCommandResult {
    /// The process exit code. This is the only success signal that can be
    /// trusted; the streams carry noise even on a clean run.
    int exit_code = 0;
    /// Standard output, decoded to UTF-8.
    std::string standard_output;
    /// Standard error, decoded to UTF-8. On a successful `--exec` this still
    /// holds one `Failed to translate '<path>'` line per Windows PATH entry.
    std::string standard_error;

    [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0; }
};

/// Runs `wsl.exe` and reports what it printed.
///
/// This is a process wrapper and nothing else. `wslapi.dll` is unusable from an
/// unpackaged process (spike #3), and everything the registry can answer --
/// which distributions exist, where their disks are, which is default -- goes
/// through `IRegistry` instead. Only facts that require a running WSL come from
/// here.
///
/// Nothing in here parses prose. `--quiet` output is names only, and every other
/// question is answered by an exit code, so a localized Windows does not change
/// what the tool understands.
class IWslHost {
public:
    IWslHost() = default;
    IWslHost(const IWslHost&) = delete;
    IWslHost& operator=(const IWslHost&) = delete;
    IWslHost(IWslHost&&) = delete;
    IWslHost& operator=(IWslHost&&) = delete;
    virtual ~IWslHost() = default;

    /// Names of the distributions that are currently running.
    [[nodiscard]] virtual Result<std::vector<std::string>> running() const = 0;

    /// Stops one distribution.
    [[nodiscard]] virtual Status terminate(std::string_view name) const = 0;

    /// Stops every distribution and the utility VM, which is what releases the
    /// lock on a VHDX so it can be compacted.
    [[nodiscard]] virtual Status shutdown() const = 0;

    /// Runs a command in the guest as root and captures both streams.
    ///
    /// `--exec` does not search PATH, so `argv[0]` must be an absolute guest
    /// path; a relative one is refused here rather than surfacing as WSL's own
    /// `execvpe` error. The guest's output is UTF-8, unlike `wsl.exe`'s own.
    [[nodiscard]] virtual Result<WslCommandResult> run_as_root(std::string_view name,
                                                               std::span<const std::string> argv,
                                                               std::chrono::milliseconds timeout) const = 0;

    /// Attaches a VHDX to the utility VM without mounting a filesystem. Needed
    /// by `shrink` in M2; the process wrapper is the same, so it lives here now.
    [[nodiscard]] virtual Status mount_bare(const std::filesystem::path& vhdx) const = 0;

    /// Detaches a VHDX attached by `mount_bare`.
    [[nodiscard]] virtual Status unmount(const std::filesystem::path& vhdx) const = 0;
};

/// Read access to the WSL registry, plus the one write `relink` needs.
///
/// Key paths are relative to a root the implementation holds (`HKEY_CURRENT_USER`
/// in production, a scratch key in the contract tests), so nothing here has to
/// name a hive.
///
/// Everything is UTF-16 because the registry is: distribution names and paths
/// keep their original encoding until the model layer converts them for display.
/// A value that does not exist is `std::nullopt`, which is not the same as a read
/// that failed -- the WSL layout has genuinely optional values (`VhdFileName` is
/// absent on the legacy packaged layout) and the caller has to tell them apart.
class IRegistry {
public:
    IRegistry() = default;
    IRegistry(const IRegistry&) = delete;
    IRegistry& operator=(const IRegistry&) = delete;
    IRegistry(IRegistry&&) = delete;
    IRegistry& operator=(IRegistry&&) = delete;
    virtual ~IRegistry() = default;

    /// Names of the immediate subkeys of `key`, in enumeration order.
    [[nodiscard]] virtual Result<std::vector<std::wstring>> subkeys(std::wstring_view key) const = 0;

    /// A `REG_SZ` (or `REG_EXPAND_SZ`) value, unexpanded.
    [[nodiscard]] virtual Result<std::optional<std::wstring>> read_string(std::wstring_view key,
                                                                          std::wstring_view value) const = 0;

    /// A `REG_DWORD` value.
    [[nodiscard]] virtual Result<std::optional<std::uint32_t>> read_dword(std::wstring_view key,
                                                                          std::wstring_view value) const = 0;

    /// Writes a `REG_SZ`. The only mutation M1 performs, for `orphans --relink`.
    [[nodiscard]] virtual Status write_string(std::wstring_view key, std::wstring_view value,
                                              std::wstring_view data) = 0;
};

/// One entry from a directory scan.
struct DirectoryEntry {
    /// Full path, not the bare name: every caller wants to open or report it.
    std::filesystem::path path;
    /// Logical size. Zero for a directory, which Windows does not track.
    std::uint64_t size = 0;
    bool is_directory = false;
};

/// A run of bytes a sparse file actually occupies on the volume.
struct AllocatedRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

/// Wall-clock waiting, behind an interface so a retry loop can be tested without
/// one.
///
/// `compact` waits for the utility VM to release a disk, which is seconds of
/// real time and the slowest thing in the unit suite if it is left real. A fake
/// advances on demand instead.
class IClock {
public:
    IClock() = default;
    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;
    virtual ~IClock() = default;

    /// Monotonic, so a clock change mid-wait cannot make a timeout go backwards.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const = 0;

    virtual void sleep_for(std::chrono::milliseconds duration) const = 0;
};

/// Host filesystem queries. The only implementation that touches Win32 lives in
/// `platform/`; unit tests substitute an in-memory fake.
class IFileSystem {
public:
    IFileSystem() = default;
    IFileSystem(const IFileSystem&) = delete;
    IFileSystem& operator=(const IFileSystem&) = delete;
    IFileSystem(IFileSystem&&) = delete;
    IFileSystem& operator=(IFileSystem&&) = delete;
    virtual ~IFileSystem() = default;

    /// Whether the path exists (as a file or a directory).
    [[nodiscard]] virtual bool exists(const std::filesystem::path& path) const = 0;

    /// Logical file length, as reported by the directory entry.
    [[nodiscard]] virtual Result<std::uint64_t> file_size(const std::filesystem::path& path) const = 0;

    /// Bytes the file actually occupies on the volume. For a sparse or compressed
    /// file this is smaller than `file_size` -- it is the number that matters when
    /// reporting how much disk a VHDX is really using.
    [[nodiscard]] virtual Result<std::uint64_t> file_size_on_disk(
        const std::filesystem::path& path) const = 0;

    /// Whether the file carries `FILE_ATTRIBUTE_SPARSE_FILE`.
    [[nodiscard]] virtual Result<bool> is_sparse(const std::filesystem::path& path) const = 0;

    /// Filesystem type and free space of the volume holding `path`.
    [[nodiscard]] virtual Result<VolumeInfo> volume_info(const std::filesystem::path& path) const = 0;

    /// Entries of `directory` matching `pattern`, without recursing.
    ///
    /// `pattern` is a Win32 wildcard (`*.vhdx`), matched by the filesystem
    /// rather than by us -- which means it also matches the 8.3 short name, so
    /// `*.vhd` finds `disk.vhdx` too. Callers that care re-check the extension.
    /// A directory that does not exist is an error; one that is empty is not.
    [[nodiscard]] virtual Result<std::vector<DirectoryEntry>> list_directory(
        const std::filesystem::path& directory, std::wstring_view pattern) const = 0;

    /// The runs of bytes a file actually occupies.
    ///
    /// Summing the lengths gives what a sparse file costs on the volume, which
    /// is what `list` reports. `is_sparse` only says whether the attribute is
    /// set; spike #4 measured that the attribute cannot tell you how much of the
    /// file is real, so quantifying it has to come from here. A non-sparse file
    /// reports one range covering the whole length, and an empty file none.
    [[nodiscard]] virtual Result<std::vector<AllocatedRange>> allocated_ranges(
        const std::filesystem::path& path) const = 0;

    /// Whether another process holds the file open for writing.
    ///
    /// The guard on `orphans --delete`. A disk can be absent from the WSL
    /// registry and still be in use -- Docker Desktop keeps a `docker_data.vhdx`
    /// that no distribution claims and that holds every volume the user has --
    /// so "no registry entry points at it" is not the same as "nothing needs
    /// it", and deleting on that basis alone would be destructive.
    [[nodiscard]] virtual Result<bool> is_locked(const std::filesystem::path& path) const = 0;

    /// Deletes a file. Only ever called after the user has confirmed.
    [[nodiscard]] virtual Status remove(const std::filesystem::path& path) = 0;

    /// Copies a file, keeping the holes.
    ///
    /// Not `CopyFileEx`. A `.vhdx` grows as it fills, so its logical length is
    /// roughly its current size rather than the virtual capacity recorded inside
    /// it -- but WSL 2.5+ creates the file sparse on some machines, and a copy
    /// that walked the logical length would fill those holes in and land a
    /// noticeably larger disk. So the destination is created sparse and only the
    /// source's allocated ranges are written; the holes are left as holes. On a
    /// file that is not sparse this copies it whole, which costs nothing.
    ///
    /// `progress` is called with bytes copied so far and the total to copy,
    /// counting only real bytes. Returning false asks the copy to stop, which
    /// leaves the partial destination behind for the caller to remove -- there
    /// is nothing useful to be done with half a disk, and deleting it here would
    /// take that decision away from the operation that knows why it stopped.
    ///
    /// The destination must not already exist. Overwriting is not something this
    /// interface offers, because every caller so far would be doing it by
    /// mistake.
    [[nodiscard]] virtual Status copy_file_sparse(
        const std::filesystem::path& from, const std::filesystem::path& to,
        const std::function<bool(std::uint64_t copied, std::uint64_t total)>& progress) = 0;

    /// Renames a file, which is instant when both paths are on one volume.
    ///
    /// The fast path for a move that is not really a move. Across volumes
    /// Windows would fall back to a copy-and-delete that does not preserve
    /// sparseness, so this deliberately does not ask for that: it fails, and the
    /// caller copies instead.
    [[nodiscard]] virtual Status rename(const std::filesystem::path& from,
                                        const std::filesystem::path& to) = 0;

    /// Whether two paths would live on the same volume.
    ///
    /// Asked before a move, to choose between the rename and the copy. Both
    /// paths are resolved to their volume mount point, so a directory mounted
    /// into another volume's namespace is answered correctly rather than by
    /// comparing drive letters.
    [[nodiscard]] virtual Result<bool> same_volume(const std::filesystem::path& first,
                                                   const std::filesystem::path& second) const = 0;

    /// Expands `%VAR%` references, for the default scan directories and config.
    [[nodiscard]] virtual Result<std::filesystem::path> expand_environment(
        const std::filesystem::path& path) const = 0;

    /// Reads a whole text file as bytes.
    ///
    /// For `config.toml` and `.wslconfig`, both of which are small and both of
    /// which are parsed rather than streamed. The bytes come back untouched --
    /// no encoding is assumed here, because a config file the user hand-edited
    /// can be anything, and the parser is where that gets decided.
    [[nodiscard]] virtual Result<std::string> read_text_file(const std::filesystem::path& path) const = 0;

    /// Writes a whole text file, replacing whatever was there.
    ///
    /// Only ever called with the output of a serialiser, never with a
    /// hand-assembled string: `config set` round-trips through the parser so a
    /// malformed edit cannot be written back.
    [[nodiscard]] virtual Status write_text_file(const std::filesystem::path& path,
                                                 std::string_view contents) = 0;

    /// Creates a directory and its parents. Succeeds if it already exists.
    [[nodiscard]] virtual Status create_directories(const std::filesystem::path& path) = 0;
};

}  // namespace wsldisk
