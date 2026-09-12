#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "errors.h"
#include "interfaces.h"

namespace wsldisk::testing {

/// An in-memory `IDiskAttach`.
///
/// The property most worth asserting here is not the compaction but the detach:
/// a disk that was attached must come back detached however the run ended --
/// success, failure or cancellation. `detached()` records that, so a test can
/// prove it rather than trust it.
class FakeDiskAttach final : public IDiskAttach {
public:
    FakeDiskAttach() = default;

    /// `IDiskAttach` deletes move to stop slicing through a base reference.
    /// This one is `final`, so there is nothing to slice.
    FakeDiskAttach(FakeDiskAttach&& other) noexcept
        : IDiskAttach(),
          attach_failure_(std::move(other.attach_failure_)),
          compact_failure_(std::move(other.compact_failure_)),
          progress_steps_(other.progress_steps_),
          attached_(std::move(other.attached_)),
          detached_(std::move(other.detached_)) {}

    void fail_attach(Error error) { attach_failure_ = std::move(error); }

    void fail_compact(Error error) { compact_failure_ = std::move(error); }

    /// How many progress callbacks a full compaction reports before finishing.
    void set_progress_steps(int steps) noexcept { progress_steps_ = steps; }

    /// Paths that were attached, in order.
    [[nodiscard]] const std::vector<std::filesystem::path>& attached() const noexcept {
        return attached_;
    }

    /// Paths that were detached, in order. A path present in `attached()` and
    /// missing here is a leaked attachment.
    [[nodiscard]] const std::vector<std::filesystem::path>& detached() const noexcept {
        return detached_;
    }

    [[nodiscard]] Result<std::unique_ptr<IAttachedDisk>> attach_read_only(
        const std::filesystem::path& path) const override {
        if (attach_failure_) return std::unexpected(*attach_failure_);
        attached_.push_back(path);
        return std::make_unique<Attached>(*this, path);
    }

private:
    /// Detaches in its destructor, which is the whole point of the type.
    class Attached final : public IAttachedDisk {
    public:
        Attached(const FakeDiskAttach& owner, std::filesystem::path path)
            : owner_(owner), path_(std::move(path)) {}

        ~Attached() override { owner_.detached_.push_back(path_); }

        [[nodiscard]] Status compact_full(const ProgressCallback& progress) override {
            if (owner_.compact_failure_) return std::unexpected(*owner_.compact_failure_);
            for (int step = 1; step <= owner_.progress_steps_; ++step) {
                const DiskProgress reported{static_cast<std::uint64_t>(step),
                                            static_cast<std::uint64_t>(owner_.progress_steps_)};
                if (progress && !progress(reported)) {
                    return fail(ErrorCode::Partial, "the compaction was cancelled",
                                "nothing was changed; the disk is detached again");
                }
            }
            return {};
        }

    private:
        const FakeDiskAttach& owner_;
        std::filesystem::path path_;
    };

    std::optional<Error> attach_failure_;
    std::optional<Error> compact_failure_;
    int progress_steps_ = 2;
    mutable std::vector<std::filesystem::path> attached_;
    mutable std::vector<std::filesystem::path> detached_;
};

}  // namespace wsldisk::testing
