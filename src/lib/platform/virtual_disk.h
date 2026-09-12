#pragma once

#include <windows.h>
#include <virtdisk.h>

#include "../interfaces.h"

namespace wsldisk::platform {

/// `IVirtualDisk` on top of the Virtual Disk Service.
///
/// Opens with `OPEN_VIRTUAL_DISK_VERSION_2` and `VIRTUAL_DISK_ACCESS_NONE`,
/// measured to compact without administrator rights. V2 accepts that mask and
/// no other: `VIRTUAL_DISK_ACCESS_METAOPS` with V2 parameters fails to open
/// with `ERROR_INVALID_PARAMETER`. V1 with `METAOPS` compacts too, but V1 also
/// accepts masks that open and then fail at the compaction, so the V2 shape is
/// the one that fails early (PLAN.md D10, docs/RESEARCH.md). A contract test
/// pins the behaviour this relies on.
class Win32VirtualDisk final : public IVirtualDisk {
public:
    [[nodiscard]] Result<std::unique_ptr<IVirtualDiskHandle>> open(
        const std::filesystem::path& path) const override;

    [[nodiscard]] Status create(const std::filesystem::path& path, std::uint64_t maximum_size) const override;
};

/// `IDiskAttach` on top of the Virtual Disk Service.
///
/// Separate from `Win32VirtualDisk` because it is a separate capability, not
/// another mode: attaching needs `OPEN_VIRTUAL_DISK_VERSION_1` with
/// `VIRTUAL_DISK_ACCESS_ATTACH_RO` and an administrator token, where the
/// compaction path needs V2 with `VIRTUAL_DISK_ACCESS_NONE` and no token at all.
/// Keeping them apart is what lets `IVirtualDisk::open` keep exactly one valid
/// spelling (D10), and it is the elevated worker alone that uses this one (D11).
class Win32DiskAttach final : public IDiskAttach {
public:
    [[nodiscard]] Result<std::unique_ptr<IAttachedDisk>> attach_read_only(
        const std::filesystem::path& path) const override;
};

/// The VHDX vendor GUID, exposed so contract tests can build a storage type
/// without duplicating the literal.
[[nodiscard]] VIRTUAL_STORAGE_TYPE vhdx_storage_type() noexcept;

}  // namespace wsldisk::platform
