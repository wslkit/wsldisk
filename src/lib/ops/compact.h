#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../interfaces.h"
#include "../model/distro.h"
#include "operation.h"

namespace wsldisk::ops {

/// How `compact` should go about it.
struct CompactOptions {
    /// Run `fstrim` first. Without it the compaction has nothing to reclaim on
    /// a disk whose guest has not discarded its freed blocks -- which is the
    /// usual state, since ext4 in WSL does not discard as it goes.
    bool trim = true;

    /// Permit `wsl --shutdown` when something else is holding the disk.
    ///
    /// Off by default, and that is decision D9 rather than caution for its own
    /// sake: the utility VM keeps every attached disk open for as long as *any*
    /// distribution runs, so the only way to release one disk is to stop them
    /// all -- including whatever the user has running in another window, and
    /// including Docker Desktop's containers. Stopping that silently to save
    /// some disk space is not a trade this tool makes for the user.
    bool shutdown = false;

    /// Start the distribution again afterwards if it was running before.
    bool restart = false;

    /// How long to wait for the disk to be released after terminating.
    ///
    /// Ninety seconds, because the handle *is* released on a timer -- the
    /// measurement behind D9 was wrong. With no distribution running, the
    /// utility VM shuts down on its idle timeout and lets go: measured at 66.6s
    /// and 66.7s on two consecutive runs against WSL 2.7.8.0, whose
    /// `vmIdleTimeout` defaults to 60 seconds.
    ///
    /// The old five seconds gave up about a minute early, so `compact <distro>`
    /// refused on a machine where waiting would simply have worked, and told the
    /// user to re-run with `--shutdown` for no reason.
    ///
    /// Only spent when it can be won: if another distribution is running the VM
    /// never idles out, so the wait is skipped and the refusal comes at once.
    std::chrono::milliseconds unlock_timeout{std::chrono::seconds{90}};

    /// How long to give the guest's `fstrim`.
    std::chrono::milliseconds trim_timeout{std::chrono::minutes{10}};

    /// Compact with the disk attached read-only, which consults the filesystem
    /// bitmap instead of relying on what the guest already discarded.
    ///
    /// Off by default and worth asking for rarely: after `fstrim` the ordinary
    /// path reclaims everything (D10), so this earns its keep only on a disk
    /// that was never trimmed. It is also the one part of the job that needs an
    /// administrator token, so asking for it may put a UAC prompt on screen.
    bool elevate = false;
};

/// Reclaims the space a VHDX is holding but no longer using.
///
/// `fstrim` in the guest, stop whatever has the disk open, then
/// `CompactVirtualDisk` on the unattached file. That last step needs no
/// administrator rights and reclaimed 100% of the freed space in the spike
/// (D10), which is the result this whole project rests on.
///
/// Nothing here can be undone: a compaction rewrites the file. The lifecycle
/// still earns its keep, because everything that *can* refuse does so in
/// `plan()`, before any of it has run.
class CompactOperation final : public IOperation {
public:
    /// Compacts a distribution's disk: trim, stop, compact.
    CompactOperation(const IVirtualDisk& disks, const IFileSystem& filesystem, const IWslHost& host,
                     const IClock& clock, model::Distro distro, CompactOptions options = {});

    /// Compacts a loose VHDX that no distribution claims -- Docker Desktop's
    /// data volume, or something `orphans` turned up.
    ///
    /// There is no guest to trim and nothing to terminate, so this is the
    /// compaction alone. It still refuses a file something else has open.
    CompactOperation(const IVirtualDisk& disks, const IFileSystem& filesystem, const IWslHost& host,
                     const IClock& clock, std::filesystem::path path, CompactOptions options = {});

    [[nodiscard]] Result<Plan> plan() override;
    [[nodiscard]] Result<Report> execute(ProgressSink& progress) override;
    [[nodiscard]] Status verify() override;
    void rollback(ProgressSink& progress) noexcept override;

    /// The disk this acts on.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// What the file occupied on the host volume before and after.
    ///
    /// The host-volume figure, not the VHDX's own `physical_size`: what the user
    /// notices is what the drive says is free. Absent when it could not be
    /// measured, which is not a reason to refuse to compact.
    [[nodiscard]] const std::optional<std::uint64_t>& size_before() const noexcept { return before_; }

    [[nodiscard]] const std::optional<std::uint64_t>& size_after() const noexcept { return after_; }

    /// How much the file shrank. Absent unless both ends were measured; zero
    /// when it did not shrink, which is a real and common answer.
    [[nodiscard]] std::optional<std::uint64_t> reclaimed() const noexcept;

    /// What `fstrim` reported, when it ran and said anything. Misleading on its
    /// own -- see `trimmed_bytes_are_misleading` in trim.h.
    [[nodiscard]] const std::optional<std::uint64_t>& trimmed_bytes() const noexcept {
        return trimmed_bytes_;
    }

    /// Tells the operation which distributions were running before the run began.
    ///
    /// `--all --shutdown --restart` needs this. Each target plans when its turn
    /// comes, and the first target's shutdown stops every later one -- so by the
    /// time they plan they look as though they were never running, and
    /// `--restart` silently skips them. The tool stopped them and then declined
    /// to put them back.
    ///
    /// Left unset, the operation asks the host itself, which is right for a
    /// single target.
    void set_running_before(const std::vector<std::string>& names);

    /// Supplies what `--elevate` needs: a way to ask for an administrator token
    /// and a way to attach once one is held.
    ///
    /// A setter rather than a constructor parameter because the ordinary path
    /// needs neither, and every existing caller compacts without elevating.
    /// Left unset, `--elevate` refuses instead of silently doing the other thing.
    void set_elevation(const IElevation* elevation, const IDiskAttach* attach) noexcept;

private:
    /// The compaction itself, by whichever route the options asked for:
    /// unattached here, attached here if we already hold a token, or attached in
    /// an elevated child that streams its progress back.
    [[nodiscard]] Status compact_disk(ProgressSink& progress);
    /// The steps that only exist when there is a distribution: the trim and the
    /// stop. Split out so the distribution is a reference rather than an
    /// optional dereferenced in a dozen places.
    void plan_guest_steps(const model::Distro& distro, Plan& plan);

    /// Runs those steps, advancing `index` past the ones it reported.
    [[nodiscard]] Status run_guest_steps(const model::Distro& distro, ProgressSink& progress, Report& report,
                                         std::size_t& index);

    /// Refuses a loose file that something else has open.
    [[nodiscard]] Status require_free_file() const;

    /// Starts the distribution again. Best effort: a failure here is reported
    /// and does not fail the run, because the compaction already succeeded.
    void restart_guest(const model::Distro& distro, ProgressSink& progress, Report& report,
                       std::size_t index);

    /// Starts the distribution again after a failure, when `--restart` asked for
    /// it and it was running before. Best effort; never fatal.
    void restart_if_asked(ProgressSink& progress);

    /// Stops whatever is holding the disk, or explains who is.
    [[nodiscard]] Status release_disk(const model::Distro& distro, ProgressSink& progress);

    /// The distributions keeping the utility VM alive.
    [[nodiscard]] std::vector<std::string> others_running() const;

    /// The same, minus the target.
    ///
    /// `wsl --list --running` can still name a distribution for a moment after
    /// it has been terminated. Asked straight after the terminate -- which is
    /// where the decision to wait at all is made -- that stale entry would
    /// refuse a compaction that was seconds away from working.
    [[nodiscard]] std::vector<std::string> others_running_excluding(const model::Distro& distro) const;

    const IVirtualDisk* disks_;
    const IFileSystem* filesystem_;
    const IWslHost* host_;
    const IClock* clock_;
    /// Null unless `set_elevation` supplied them; only `--elevate` uses them.
    const IElevation* elevation_ = nullptr;
    const IDiskAttach* attach_ = nullptr;
    /// Absent when compacting a loose file.
    std::optional<model::Distro> distro_;
    std::filesystem::path path_;
    CompactOptions options_;
    std::optional<std::uint64_t> before_;
    std::optional<std::uint64_t> after_;
    std::optional<std::uint64_t> trimmed_bytes_;
    bool was_running_ = false;
    std::optional<std::vector<std::string>> running_before_;
};

}  // namespace wsldisk::ops
