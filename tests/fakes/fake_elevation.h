#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "errors.h"
#include "interfaces.h"

namespace wsldisk::testing {

/// An `IElevation` that never launches anything.
///
/// The three states worth testing are "already elevated, no relaunch needed",
/// "not elevated, the relaunch works" and "not elevated, the user declined" --
/// that last one being the ordinary outcome rather than an error, since a user
/// is allowed to say no. Every relaunch is recorded so a test can assert that a
/// `--dry-run` asked for nothing and that the verb and path handed over were the
/// ones the operation meant.
class FakeElevation final : public IElevation {
public:
    FakeElevation() = default;

    /// `IElevation` deletes move to stop slicing through a base reference.
    /// This one is `final`, so there is nothing to slice.
    FakeElevation(FakeElevation&& other) noexcept
        : IElevation(),
          elevated_(other.elevated_),
          failure_(std::move(other.failure_)),
          exit_code_(other.exit_code_),
          messages_(std::move(other.messages_)),
          progress_steps_(other.progress_steps_),
          requests_(std::move(other.requests_)) {}

    /// What one relaunch was asked to do.
    struct Request {
        std::string verb;
        std::filesystem::path target;
    };

    /// Pretends the current process already holds an administrator token, so
    /// nothing should relaunch.
    void set_elevated(bool elevated) noexcept { elevated_ = elevated; }

    /// Makes the relaunch fail. `ErrorCode::NeedsElevation` is what declining
    /// the prompt looks like; anything else is a real failure.
    void fail_relaunch(Error error) { failure_ = std::move(error); }

    /// The exit code the elevated worker reports. The parent propagates it, so
    /// 5 here is how a cancelled worker is simulated.
    void set_exit_code(int code) noexcept { exit_code_ = code; }

    /// Lines the worker "streams" back before it finishes.
    void set_messages(std::vector<std::string> messages) { messages_ = std::move(messages); }

    /// How many progress callbacks the worker reports. A test that cancels
    /// returns false from the sink and asserts the run stopped.
    void set_progress_steps(int steps) noexcept { progress_steps_ = steps; }

    [[nodiscard]] const std::vector<Request>& requests() const noexcept { return requests_; }

    [[nodiscard]] bool is_elevated() const override { return elevated_; }

    [[nodiscard]] Result<int> run_elevated(std::string_view verb,
                                           const std::filesystem::path& target,
                                           const ElevatedSink& sink) const override {
        requests_.push_back(Request{std::string{verb}, target});
        if (failure_) return std::unexpected(*failure_);

        for (const auto& message : messages_) {
            if (sink.message) sink.message(message);
        }
        for (int step = 1; step <= progress_steps_; ++step) {
            if (!sink.progress) break;
            const DiskProgress progress{static_cast<std::uint64_t>(step),
                                        static_cast<std::uint64_t>(progress_steps_)};
            // False means the caller asked to stop; a real worker unwinds and
            // exits 5, so that is what this reports too.
            if (!sink.progress(progress)) {
                return exit_code_for(ErrorCode::Partial);
            }
        }
        return exit_code_;
    }

private:
    bool elevated_ = false;
    std::optional<Error> failure_;
    int exit_code_ = 0;
    std::vector<std::string> messages_;
    int progress_steps_ = 2;
    mutable std::vector<Request> requests_;
};

}  // namespace wsldisk::testing
