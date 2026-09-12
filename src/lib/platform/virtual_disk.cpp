#include "virtual_disk.h"

#include <windows.h>
#include <virtdisk.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scoped_handle.h"
#include "win32_api.h"
#include "win32_error.h"

namespace wsldisk::platform {
namespace {

/// How often the compaction loop asks for progress. The operation is I/O bound
/// and runs for seconds to minutes, so this is about the update rate a progress
/// bar wants, not about responsiveness.
constexpr DWORD progress_poll_interval_ms = 250;

/// Reads the parent path out of a GET_VIRTUAL_DISK_INFO buffer without running
/// off the end of it. The API is not required to terminate the string when it
/// exactly fills the buffer, so the capacity bounds the read rather than a NUL.
std::wstring parent_location(const std::vector<GET_VIRTUAL_DISK_INFO>& storage) {
    constexpr std::size_t header = offsetof(GET_VIRTUAL_DISK_INFO, ParentLocation.ParentLocationBuffer);
    const std::size_t bytes = (storage.size() * sizeof(GET_VIRTUAL_DISK_INFO)) - header;
    const std::wstring_view all{
        static_cast<const wchar_t*>(storage.front().ParentLocation.ParentLocationBuffer),
        bytes / sizeof(wchar_t)};
    return std::wstring{all.substr(0, all.find(L'\0'))};
}

/// The open handle, and the compaction that runs against it.
class Win32VirtualDiskHandle final : public IVirtualDiskHandle {
public:
    Win32VirtualDiskHandle(HANDLE handle, std::filesystem::path path)
        : handle_(handle), path_(std::move(path)) {}

    [[nodiscard]] Result<VirtualDiskInfo> information() const override;
    [[nodiscard]] Status compact(const ProgressCallback& progress) override;

private:
    /// Stops a started compaction before its OVERLAPPED leaves the stack.

    ScopedHandle handle_;
    std::filesystem::path path_;
};

Result<VirtualDiskInfo> Win32VirtualDiskHandle::information() const {
    VirtualDiskInfo result;

    GET_VIRTUAL_DISK_INFO info{};
    ULONG size = sizeof(info);
    const DWORD sizes = win32().get_virtual_disk_information(handle_.get(), GET_VIRTUAL_DISK_INFO_SIZE, &size,
                                                             &info, nullptr);
    if (sizes != ERROR_SUCCESS) {
        return std::unexpected(error_from_win32(sizes, std::format("read the size of {}", path_.string())));
    }
    result.virtual_size = info.Size.VirtualSize;
    result.physical_size = info.Size.PhysicalSize;
    result.block_size = info.Size.BlockSize;
    result.sector_size = info.Size.SectorSize;

    // A differencing disk names its parent; every other kind reports
    // ERROR_NOT_FOUND here, which is not a failure.
    //
    // ParentLocationBuffer is a variable-length WCHAR array at the tail of the
    // union, so a plain GET_VIRTUAL_DISK_INFO has room for exactly one
    // character. Anything longer needs a bigger allocation, which the API asks
    // for with ERROR_INSUFFICIENT_BUFFER. A vector of whole structs is used
    // rather than a byte buffer so the storage stays correctly aligned.
    std::vector<GET_VIRTUAL_DISK_INFO> storage(1);
    ULONG parent_size = sizeof(GET_VIRTUAL_DISK_INFO);
    DWORD parent_status = win32().get_virtual_disk_information(
        handle_.get(), GET_VIRTUAL_DISK_INFO_PARENT_LOCATION, &parent_size, storage.data(), nullptr);
    if (parent_status == ERROR_INSUFFICIENT_BUFFER) {
        storage.assign((parent_size + sizeof(GET_VIRTUAL_DISK_INFO) - 1) / sizeof(GET_VIRTUAL_DISK_INFO),
                       GET_VIRTUAL_DISK_INFO{});
        parent_size = static_cast<ULONG>(storage.size() * sizeof(GET_VIRTUAL_DISK_INFO));
        parent_status = win32().get_virtual_disk_information(
            handle_.get(), GET_VIRTUAL_DISK_INFO_PARENT_LOCATION, &parent_size, storage.data(), nullptr);
    }
    if (parent_status == ERROR_SUCCESS) {
        result.parent_path = parent_location(storage);
    }
    return result;
}

/// How long to keep asking, after cancelling, before giving up on a clean stop.
/// Generous, because the alternative to waiting is memory corruption.
constexpr DWORD cancel_poll_interval_ms = 100;
constexpr int cancel_poll_attempts = 100;

/// Waits for a started compaction to stop touching `overlapped`.
///
/// `CompactVirtualDisk` runs asynchronously against an `OVERLAPPED` and an event
/// that both live on `compact`'s stack. Returning while the operation is still
/// `ERROR_IO_PENDING` leaves the kernel a pointer into a frame that is about to
/// be reused, and an event handle that is about to be closed: when the
/// compaction finishes it writes its status into whatever now occupies that
/// memory. Intermittent, unreproducible, and nothing in the tool would report it.
///
/// So every exit taken while the operation is pending comes through here first.
/// `CancelIoEx` asks; the wait is what makes it safe, because the request is not
/// the acknowledgement. The wait is bounded -- a compaction that will not stop is
/// still better reported than waited on forever -- and a timeout is the one case
/// where returning is worse than not, so it is reported rather than swallowed.
void abandon(HANDLE disk, OVERLAPPED& overlapped, HANDLE event) {
    std::ignore = win32().cancel_io_ex(disk, &overlapped);

    VIRTUAL_DISK_PROGRESS raw{};
    for (int attempt = 0; attempt < cancel_poll_attempts; ++attempt) {
        if (win32().wait_for_single_object(event, cancel_poll_interval_ms) == WAIT_OBJECT_0) {
            return;
        }
        const DWORD polled = win32().get_virtual_disk_operation_progress(disk, &overlapped, &raw);
        if (polled != ERROR_SUCCESS || raw.OperationStatus != ERROR_IO_PENDING) {
            return;
        }
    }
}

/// The compaction loop itself, shared by the plain and the attached handle.
/// Attaching read-only is what makes this a "full" compaction; the call and its
/// cancellation rules are identical either way.
Status run_compaction(HANDLE disk, const std::filesystem::path& path, const ProgressCallback& progress) {
    // An event is needed for the asynchronous form: CompactVirtualDisk returns
    // ERROR_IO_PENDING and the operation runs until the event signals.
    const ScopedHandle event{win32().create_event(nullptr, TRUE, FALSE, nullptr)};
    if (event.get() == nullptr) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), "create the event for the compaction"));
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    COMPACT_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = COMPACT_VIRTUAL_DISK_VERSION_1;

    const DWORD started =
        win32().compact_virtual_disk(disk, COMPACT_VIRTUAL_DISK_FLAG_NONE, &parameters, &overlapped);
    if (started != ERROR_SUCCESS && started != ERROR_IO_PENDING) {
        return std::unexpected(error_from_win32(started, std::format("compact {}", path.string())));
    }

    // A synchronous completion still has to report progress once, so callers see
    // a finished bar rather than an empty one.
    if (started == ERROR_SUCCESS) {
        std::ignore = progress(DiskProgress{.current = 1, .total = 1});
        return {};
    }

    // Every way out of this loop is a return, so the "condition was false" edge
    // does not exist to be covered.
    while (true) {  // LCOV_EXCL_BR_LINE
        const DWORD waited = win32().wait_for_single_object(event.get(), progress_poll_interval_ms);
        if (waited == WAIT_FAILED) {
            const DWORD failure = win32().get_last_error();
            abandon(disk, overlapped, event.get());
            return std::unexpected(
                error_from_win32(failure, std::format("wait for the compaction of {}", path.string())));
        }

        VIRTUAL_DISK_PROGRESS raw{};
        const DWORD polled = win32().get_virtual_disk_operation_progress(disk, &overlapped, &raw);
        if (polled != ERROR_SUCCESS) {
            abandon(disk, overlapped, event.get());
            return std::unexpected(
                error_from_win32(polled, std::format("read the compaction progress of {}", path.string())));
        }

        if (raw.OperationStatus == ERROR_IO_PENDING) {
            if (!progress(DiskProgress{.current = raw.CurrentValue, .total = raw.CompletionValue})) {
                // Cancelled, and then actually stopped before saying so: the old
                // remedy claimed "the disk is still usable; re-run to finish"
                // while a compaction was in fact still running against the file
                // the caller may reopen immediately.
                abandon(disk, overlapped, event.get());
                return fail(ErrorCode::Partial, std::format("compaction of {} was cancelled", path.string()),
                            "the disk is still usable; re-run to finish reclaiming space");
            }
            continue;
        }

        if (raw.OperationStatus != ERROR_SUCCESS) {
            return std::unexpected(
                error_from_win32(raw.OperationStatus, std::format("compact {}", path.string())));
        }

        std::ignore = progress(DiskProgress{.current = raw.CompletionValue, .total = raw.CompletionValue});
        return {};
    }
}

Status Win32VirtualDiskHandle::compact(const ProgressCallback& progress) {
    return run_compaction(handle_.get(), path_, progress);
}

/// A disk attached read-only, detached again by the destructor.
///
/// The detach is not best-effort cleanup: an attached disk that nobody detaches
/// stays attached until the machine reboots, and the user's distribution will
/// not start while it is. So it runs on every path out, including the ones where
/// the compaction failed, and its own failure cannot be reported -- there is no
/// caller left to tell.
class Win32AttachedDisk final : public IAttachedDisk {
public:
    Win32AttachedDisk(HANDLE handle, std::filesystem::path path)
        : handle_(handle), path_(std::move(path)) {}

    ~Win32AttachedDisk() override {
        std::ignore = win32().detach_virtual_disk(handle_.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    }

    [[nodiscard]] Status compact_full(const ProgressCallback& progress) override {
        return run_compaction(handle_.get(), path_, progress);
    }

private:
    ScopedHandle handle_;
    std::filesystem::path path_;
};

}  // namespace

VIRTUAL_STORAGE_TYPE vhdx_storage_type() noexcept {
    // virtdisk.h declares VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT with DEFINE_GUID,
    // so the symbol only exists in a translation unit that defines INITGUID
    // first -- which then exports every other GUID in every header it pulls in.
    // Spelling the value out avoids that, and this one is fixed by the VHDX
    // format rather than by an SDK version. Verified against the real API in the
    // M0 compaction spike.
    constexpr GUID microsoft_vendor{
        0xEC984AEC, 0xA0F9, 0x47E9, {0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B}};
    return VIRTUAL_STORAGE_TYPE{.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, .VendorId = microsoft_vendor};
}

Result<std::unique_ptr<IVirtualDiskHandle>> Win32VirtualDisk::open(const std::filesystem::path& path) const {
    VIRTUAL_STORAGE_TYPE storage_type = vhdx_storage_type();

    // Version 2 with VIRTUAL_DISK_ACCESS_NONE. See the class comment: this is
    // measured, not preference.
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    parameters.Version2.GetInfoOnly = FALSE;
    parameters.Version2.ReadOnly = FALSE;

    ScopedHandle handle;
    const DWORD opened = win32().open_virtual_disk(&storage_type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                                                   OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, handle.put());
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(
            error_from_win32(opened, std::format("open the virtual disk {}", path.string())));
    }

    return std::make_unique<Win32VirtualDiskHandle>(handle.release(), path);
}

Status Win32VirtualDisk::create(const std::filesystem::path& path, std::uint64_t maximum_size) const {
    VIRTUAL_STORAGE_TYPE storage_type = vhdx_storage_type();

    CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    parameters.Version2.MaximumSize = maximum_size;
    // Zero means "let the provider choose", which is what WSL itself does.
    parameters.Version2.BlockSizeInBytes = 0;
    parameters.Version2.SectorSizeInBytes = 0;

    ScopedHandle handle;
    const DWORD created =
        win32().create_virtual_disk(&storage_type, path.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr,
                                    CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &parameters, nullptr, handle.put());
    if (created != ERROR_SUCCESS) {
        return std::unexpected(
            error_from_win32(created, std::format("create the virtual disk {}", path.string())));
    }
    return {};
}

Result<std::unique_ptr<IAttachedDisk>> Win32DiskAttach::attach_read_only(
    const std::filesystem::path& path) const {
    VIRTUAL_STORAGE_TYPE storage_type = vhdx_storage_type();

    // Version 1 here, unlike the compaction path. Attaching needs rights, and V2
    // accepts VIRTUAL_DISK_ACCESS_NONE and nothing else (D10) -- a mask of none
    // is exactly what an attach cannot work with. ATTACH_RO is the narrowest
    // mask that does: read-only, which is all a compaction needs to consult the
    // filesystem bitmap.
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    parameters.Version1.RWDepth = 1;

    ScopedHandle handle;
    const DWORD opened =
        win32().open_virtual_disk(&storage_type, path.c_str(), VIRTUAL_DISK_ACCESS_ATTACH_RO,
                                  OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, handle.put());
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(
            error_from_win32(opened, std::format("open {} for attaching", path.string())));
    }

    // NO_DRIVE_LETTER and NO_LOCAL_HOST: the disk becomes compactable, not
    // browsable. Nothing should be able to read the user's filesystem through
    // an attachment this tool made on their behalf.
    const DWORD attached = win32().attach_virtual_disk(
        handle.get(), nullptr,
        static_cast<ATTACH_VIRTUAL_DISK_FLAG>(ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
                                              ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER |
                                              ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST),
        0, nullptr, nullptr);
    if (attached != ERROR_SUCCESS) {
        Error error = error_from_win32(attached, std::format("attach {} read-only", path.string()));
        // Access denied here means one thing only, and it is not a broken disk:
        // the caller has no administrator token. Saying so, with the flag that
        // gets one, beats reporting a bare Win32 5.
        if (attached == ERROR_ACCESS_DENIED) {
            error.code = ErrorCode::NeedsElevation;
            error.remedy = "attaching read-only needs an administrator token; re-run with --elevate";
        }
        return std::unexpected(std::move(error));
    }
    return std::make_unique<Win32AttachedDisk>(handle.release(), path);
}

}  // namespace wsldisk::platform
