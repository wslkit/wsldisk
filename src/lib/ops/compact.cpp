#include "compact.h"

#include <algorithm>
#include <format>
#include <utility>

#include "../model/config.h"
#include "trim.h"

namespace wsldisk::ops {
namespace {

/// Joins names for a message: "Ubuntu, docker-desktop".
[[nodiscard]] std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    }
    return text;
}

/// How often the wait loop asks whether the disk is free.
constexpr std::chrono::milliseconds poll_interval{500};

/// How often it redraws the countdown. Slower than the poll, because a line
/// that changes twice a second is a flicker rather than information.
constexpr std::chrono::milliseconds tick_interval{1'000};

/// A longer wait to suggest when the current one ran out.
///
/// Double it, capped at what the config will actually accept -- suggesting a
/// value `wsldisk config set` would then reject is worse than suggesting
/// nothing.
[[nodiscard]] std::uint32_t suggested_longer_wait(std::chrono::milliseconds current) {
    const auto doubled = std::chrono::duration_cast<std::chrono::seconds>(current).count() * 2;
    return static_cast<std::uint32_t>(std::min<std::int64_t>(doubled, model::max_unlock_timeout_seconds));
}

}  // namespace

CompactOperation::CompactOperation(const IVirtualDisk& disks, const IFileSystem& filesystem,
                                   const IWslHost& host, const IClock& clock, model::Distro distro,
                                   CompactOptions options)
    : disks_(&disks),
      filesystem_(&filesystem),
      host_(&host),
      clock_(&clock),
      distro_(std::move(distro)),
      path_(distro_->vhdx_path),
      options_(options) {}

CompactOperation::CompactOperation(const IVirtualDisk& disks, const IFileSystem& filesystem,
                                   const IWslHost& host, const IClock& clock, std::filesystem::path path,
                                   CompactOptions options)
    : disks_(&disks),
      filesystem_(&filesystem),
      host_(&host),
      clock_(&clock),
      path_(std::move(path)),
      options_(options) {}

std::optional<std::uint64_t> CompactOperation::reclaimed() const noexcept {
    if (!before_.has_value() || !after_.has_value()) {
        return std::nullopt;
    }
    // Saturating rather than signed: a file that grew is reported as nothing
    // reclaimed, which is what happened, and a negative saving is not a thing.
    return *after_ >= *before_ ? 0 : *before_ - *after_;
}

std::vector<std::string> CompactOperation::others_running() const {
    // Whatever WSL says is running, unfiltered. This is asked after a full wait,
    // by which point the list has settled -- so if it still names the target,
    // the target has been started again, which is the truth worth telling the
    // user rather than something to quietly drop.
    return host_->running().value_or(std::vector<std::string>{});
}

std::vector<std::string> CompactOperation::others_running_excluding(const model::Distro& distro) const {
    std::vector<std::string> running = others_running();
    const auto is_target = [&distro](const std::string& name) { return distro.find_matches(name); };
    running.erase(std::ranges::begin(std::ranges::remove_if(running, is_target)), running.end());
    return running;
}

void CompactOperation::plan_guest_steps(const model::Distro& distro, Plan& plan) {
    if (options_.trim) {
        plan.steps.push_back(
            StepPlan{.description = std::format("run fstrim in {}", distro.name), .mutates = true});
    }

    // The caller's snapshot when it has one, because by the time a later target
    // plans under `--all --shutdown`, the first target's shutdown has already
    // stopped it.
    const auto matches = [&distro](const std::vector<std::string>& names) {
        return std::ranges::any_of(names,
                                   [&distro](const std::string& name) { return distro.find_matches(name); });
    };
    if (running_before_.has_value()) {
        was_running_ = matches(*running_before_);
    } else if (const auto running = host_->running(); running.has_value()) {
        was_running_ = matches(*running);
    } else {
        was_running_ = false;
    }

    plan.steps.push_back(
        StepPlan{.description = options_.shutdown ? "shut WSL down so the disk is released"
                                                  : std::format("stop {} and wait for its disk", distro.name),
                 .mutates = true});
}

Result<Plan> CompactOperation::plan() {
    if (distro_.has_value() && !distro_->is_wsl2()) {
        return fail(ErrorCode::Preflight,
                    std::format("{} is a WSL1 distribution and has no virtual disk", distro_->name),
                    std::format("convert it with `wsl --set-version {} 2`, then try again", distro_->name));
    }

    if (!filesystem_->exists(path_)) {
        return fail(ErrorCode::Preflight, std::format("{} does not exist", path_.string()),
                    distro_.has_value() ? "run `wsldisk info` to see where the distribution points, and "
                                          "`wsldisk orphans --relink` if the disk has moved"
                                        : "check the path");
    }

    if (const auto size = filesystem_->file_size_on_disk(path_); size.has_value()) {
        before_ = *size;
    }

    Plan plan;
    if (distro_.has_value()) {
        plan_guest_steps(*distro_, plan);
    }
    plan.steps.push_back(StepPlan{.description = std::format("compact {}", path_.string()), .mutates = true});
    if (distro_.has_value() && options_.restart && was_running_) {
        plan.steps.push_back(
            StepPlan{.description = std::format("start {} again", distro_->name), .mutates = true});
    }

    // `plan.estimate.bytes_freed` is deliberately left unset: nothing can
    // predict what a compaction will reclaim before the trim has run, and a
    // guess in that field would be read as a promise.
    plan.warnings.push_back(
        Warning{.message = "compaction rewrites the disk file and cannot be undone",
                .remedy = "nothing inside the distribution changes; only unused blocks go"});
    if (!options_.shutdown) {
        plan.warnings.push_back(
            Warning{.message = "the disk can only be released by stopping every distribution",
                    .remedy = "re-run with --shutdown if this refuses because something else "
                              "is holding it"});
    }
    return plan;
}

Status CompactOperation::release_disk(const model::Distro& distro, ProgressSink& progress) {
    // Either way of stopping things is followed by the same wait, because
    // neither is an answer on its own: `wsl --shutdown` can exit 0 while the
    // utility VM's handle lingers, and it cannot do anything at all about a
    // backup agent or an antivirus scanner holding the file.
    //
    // Skipping the check after `--shutdown` was the worst place to skip it. The
    // user has just paid the highest price available -- every distribution and
    // every container stopped -- and the compaction would then fail at `open`
    // with a raw virtual-disk error: no DistroBusy, no exit 11, no naming of
    // the holder, and a remedy that does not apply.
    if (options_.shutdown) {
        // Everything, because that is the only thing that works for WSL: the
        // utility VM holds every attached disk for as long as any distribution
        // runs (D9).
        if (const Status stopped = host_->shutdown(); !stopped.has_value()) {
            return stopped;
        }
    } else if (const Status stopped = host_->terminate(distro.name); !stopped.has_value()) {
        return stopped;
    }

    // Asked before the wait, not only after it. The VM idles out and lets go
    // only once nothing is running; while something is, every second of the
    // wait is spent on an answer that cannot change, and ninety of them is a
    // long time to sit in front of a refusal that was knowable at the start.
    const std::vector<std::string> blockers = others_running_excluding(distro);
    const bool waiting_can_help = blockers.empty();

    // Poll rather than sleep once: on a machine where nothing else is running,
    // the disk comes free almost immediately, and waiting out the whole timeout
    // would be time spent for nothing.
    // The check comes before the wait, so a zero timeout still gets an answer
    // rather than an assumption.
    const auto deadline = clock_->now() + options_.unlock_timeout;
    std::chrono::milliseconds waited{0};
    while (true) {  // LCOV_EXCL_BR_LINE -- every exit is a return or a break
        const auto locked = filesystem_->is_locked(path_);
        if (!locked.has_value()) {
            return std::unexpected(locked.error());
        }
        if (!*locked) {
            return {};
        }
        if (!waiting_can_help || clock_->now() >= deadline) {
            break;
        }
        // Said once, because it is the same the whole way through: printing it
        // per poll filled the screen with one repeated sentence.
        if (waited == std::chrono::milliseconds{0}) {
            progress.message(
                "the WSL utility VM still has the disk open; it lets go about a "
                "minute after the last distribution stops");
        }
        // The countdown goes on a line that redraws, so the wait shows as one
        // number ticking rather than ninety copies of a sentence.
        if (waited % tick_interval == std::chrono::milliseconds{0}) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(waited);
            const auto limit = std::chrono::duration_cast<std::chrono::seconds>(options_.unlock_timeout);
            progress.status(
                std::format("waiting for the disk ... {}s of {}s", elapsed.count(), limit.count()));
        }
        clock_->sleep_for(poll_interval);
        waited += poll_interval;
    }

    // Naming who is holding it is the whole point of the refusal: "still locked"
    // leaves the user guessing which window to close.
    //
    // Re-asked only when the wait actually ran: something may have started
    // during it, including the target. Breaking out early means the list has
    // had no time to settle since the terminate, so use what was asked then.
    const std::vector<std::string> others = waiting_can_help ? others_running() : blockers;
    const std::string who =
        others.empty() ? "still held by the WSL utility VM" : std::format("still open in {}", joined(others));
    // Telling someone who just used `--shutdown` to re-run with `--shutdown`
    // would be sending them round a loop. Whatever has the file at that point is
    // not WSL.
    std::string remedy;
    if (options_.shutdown) {
        remedy =
            "WSL has been shut down and something else still has the file open -- a backup "
            "agent, an antivirus scanner or Hyper-V Manager are the usual ones";
    } else if (others.empty()) {
        // Nothing is running, so this is the utility VM winding down rather than
        // anything the user has to close. Waiting longer is the answer, and
        // telling them to stop distributions that are already stopped is not.
        remedy = std::format(
            "nothing is running, so the VM is still winding down -- it releases the disk about a "
            "minute after the last distribution stops. Raise the wait with `wsldisk config set "
            "wsl.unlock_timeout_seconds {}`, or re-run with --shutdown to stop it now",
            suggested_longer_wait(options_.unlock_timeout));
    } else {
        remedy =
            "the WSL utility VM keeps every disk open while any distribution runs; "
            "re-run with --shutdown to stop them all, or close them yourself first";
    }
    return fail(ErrorCode::DistroBusy, std::format("{} is {}", path_.string(), who), remedy);
}

Status CompactOperation::run_guest_steps(const model::Distro& distro, ProgressSink& progress, Report& report,
                                         std::size_t& index) {
    if (options_.trim) {
        const StepPlan step{.description = std::format("run fstrim in {}", distro.name), .mutates = true};
        progress.step_started(index, step);
        const auto trimmed = run_fstrim(*host_, distro.name, options_.trim_timeout, progress);
        if (!trimmed.has_value()) {
            return std::unexpected(trimmed.error());
        }
        trimmed_bytes_ = trimmed->trimmed_bytes;
        progress.step_finished(index, step);
        report.completed.push_back(step.description);
        ++index;
    }

    const StepPlan step{.description = options_.shutdown
                                           ? "shut WSL down so the disk is released"
                                           : std::format("stop {} and wait for its disk", distro.name),
                        .mutates = true};
    progress.step_started(index, step);
    if (const Status released = release_disk(distro, progress); !released.has_value()) {
        return std::unexpected(released.error());
    }
    progress.step_finished(index, step);
    report.completed.push_back(step.description);
    ++index;
    return {};
}

Status CompactOperation::require_free_file() const {
    // A loose file has no guest to stop, but it can still be held open --
    // Docker Desktop's data volume is, whenever Docker is running.
    const auto locked = filesystem_->is_locked(path_);
    if (!locked.has_value()) {
        return std::unexpected(locked.error());
    }
    if (*locked) {
        return fail(ErrorCode::DistroBusy, std::format("{} is open in another process", path_.string()),
                    "close whatever is using it -- `wsl --shutdown` for WSL, or quit Docker "
                    "Desktop -- and try again");
    }
    return {};
}

void CompactOperation::restart_guest(const model::Distro& distro, ProgressSink& progress, Report& report,
                                     std::size_t index) {
    const StepPlan restart{.description = std::format("start {} again", distro.name), .mutates = true};
    progress.step_started(index, restart);
    // Does nothing in the guest, which is the point: starting it is the whole
    // instruction. `/bin/sh -c :` rather than `/bin/true` for the same reason as
    // relink -- POSIX guarantees `/bin/sh` and NixOS-WSL has no `/bin/true`, so
    // the exit code would say the restart failed when the distribution had in
    // fact come back up.
    const std::vector<std::string> argv{"/bin/sh", "-c", ":"};
    const auto started = host_->run_as_root(distro.name, argv, options_.trim_timeout);
    if (!started.has_value()) {
        // Reported, not fatal: the compaction succeeded, and a distribution that
        // did not come back up starts on the user's next command.
        progress.message(
            std::format("could not start {} again: {}", distro.name, started.error().to_string()));
        return;
    }
    if (!started->succeeded()) {
        // `interfaces.h` calls the exit code the only success signal that can be
        // trusted, and this used to ignore it: a distribution that failed to boot
        // after being compacted was reported as restarted, in the text output and
        // in the `--json`. That is the one signal that the compacted disk has a
        // problem, and it was being swallowed.
        progress.message(
            std::format("could not start {} again: it exited {}", distro.name, started->exit_code));
        return;
    }
    progress.step_finished(index, restart);
    report.completed.push_back(restart.description);
}

void CompactOperation::set_running_before(const std::vector<std::string>& names) {
    running_before_ = names;
}

void CompactOperation::set_elevation(const IElevation* elevation, const IDiskAttach* attach) noexcept {
    elevation_ = elevation;
    attach_ = attach;
}

Status CompactOperation::compact_disk(ProgressSink& progress) {
    const ProgressCallback report_progress = [&progress](const DiskProgress& fraction) {
        progress.step_progress(fraction);
        return true;
    };

    if (!options_.elevate) {
        auto handle = disks_->open(path_);
        if (!handle.has_value()) return std::unexpected(handle.error());
        if (const Status compacted = (*handle)->compact(report_progress); !compacted.has_value()) {
            return std::unexpected(compacted.error());
        }
        // Closed before the file is measured: the size a still-open handle
        // reports is not necessarily the one the volume has settled on.
        handle->reset();
        return {};
    }

    if (elevation_ == nullptr || attach_ == nullptr) {
        // Refusing beats quietly compacting the other way: the user asked for
        // the attached path and would otherwise be told it had happened.
        return fail(ErrorCode::Usage, "--elevate is not wired up in this build",
                    "compact without --elevate, which reclaims everything fstrim freed");
    }

    // Already elevated -- the worker case, and the case of a user who started an
    // administrator shell. Nothing to relaunch; attach here.
    if (elevation_->is_elevated()) {
        auto attached = attach_->attach_read_only(path_);
        if (!attached.has_value()) return std::unexpected(attached.error());
        // The attachment is released by the destructor, whatever happens next.
        return (*attached)->compact_full(report_progress);
    }

    const ElevatedSink sink{
        .message = [&progress](std::string_view text) { progress.message(text); },
        .progress =
            [&progress](const DiskProgress& fraction) {
                progress.step_progress(fraction);
                return true;
            },
    };
    const Result<int> code = elevation_->run_elevated("compact-full", path_, sink);
    if (!code.has_value()) return std::unexpected(code.error());
    if (*code == exit_code_success) return {};
    // The worker has already said what went wrong through `sink.message`, so
    // this carries the exit code rather than inventing a second explanation.
    return fail(*code == exit_code_for(ErrorCode::Partial) ? ErrorCode::Partial : ErrorCode::Generic,
                std::format("the elevated worker exited {} without compacting {}", *code, path_.string()),
                "its own message above says why; nothing was left attached");
}

void CompactOperation::restart_if_asked(ProgressSink& progress) {
    // `--restart` promises the distribution comes back. It used to be kept only
    // when the compaction worked, so a run that stopped the distribution and
    // then failed left it stopped -- on exactly the run where the user did not
    // get what they came for. Recoverable, since WSL starts it on next use, but
    // not what the flag says.
    //
    // Best effort and never fatal: this runs while something has already gone
    // wrong, and a failure to restart must not replace the error that matters.
    if (!distro_.has_value() || !options_.restart || !was_running_) {
        return;
    }
    Report discard;
    const std::size_t index = 0;
    restart_guest(*distro_, progress, discard, index);
}

Result<Report> CompactOperation::execute(ProgressSink& progress) {
    Report report;
    std::size_t index = 0;

    if (distro_.has_value()) {
        if (const Status prepared = run_guest_steps(*distro_, progress, report, index);
            !prepared.has_value()) {
            return std::unexpected(prepared.error());
        }
    } else if (const Status free = require_free_file(); !free.has_value()) {
        return std::unexpected(free.error());
    }

    // Re-measured here, not at plan time. `plan()` runs before the distribution
    // is stopped and before an `fstrim` that is allowed ten minutes, so any
    // guest write in between inflates the file -- and `verify()` would then
    // report "grew during compaction" for a compaction that worked. A build
    // running inside the distribution was enough to do it.
    //
    // By this point the guest is stopped and the disk is unattached, so nothing
    // else is writing to it. The plan-time reading stays for display.
    if (const auto size = filesystem_->file_size_on_disk(path_); size.has_value()) {
        before_ = *size;
    }

    const StepPlan compaction{.description = std::format("compact {}", path_.string()), .mutates = true};
    progress.step_started(index, compaction);

    if (const Status compacted = compact_disk(progress); !compacted.has_value()) {
        restart_if_asked(progress);
        return std::unexpected(compacted.error());
    }

    if (const auto size = filesystem_->file_size_on_disk(path_); size.has_value()) {
        after_ = *size;
    }
    progress.step_finished(index, compaction);
    report.completed.push_back(compaction.description);
    ++index;

    if (distro_.has_value() && options_.restart && was_running_) {
        restart_guest(*distro_, progress, report, index);
    }

    report.actual.bytes_freed = reclaimed();
    return report;
}

Status CompactOperation::verify() {
    // The one thing worth checking: the file is not bigger than it was. A
    // compaction that grew the disk would be a serious failure, and it is
    // cheap to rule out.
    if (!before_.has_value() || !after_.has_value()) {
        return {};
    }
    if (*after_ > *before_) {
        return fail(ErrorCode::Partial, std::format("{} grew during compaction", path_.string()),
                    "the disk is intact; run `wsldisk info` to see its size now");
    }
    return {};
}

void CompactOperation::rollback(ProgressSink& /*progress*/) noexcept {
    // Nothing to undo. A compaction rewrites the file, and everything that can
    // refuse does so in plan(), before any of it has run.
}

}  // namespace wsldisk::ops
