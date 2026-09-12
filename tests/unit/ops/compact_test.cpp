#include "ops/compact.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "fake_clock.h"
#include "fake_disk_attach.h"
#include "fake_elevation.h"
#include "fake_filesystem.h"
#include "fake_registry.h"
#include "fake_virtual_disk.h"
#include "fake_wsl_host.h"
#include "lxss_hives.h"
#include "model/config.h"
#include "model/distro.h"
#include "ops/runner.h"
#include "recording_sink.h"

using wsldisk::ErrorCode;
using wsldisk::WslCommandResult;
using wsldisk::model::Distro;
using wsldisk::model::enumerate;
using wsldisk::model::max_unlock_timeout_seconds;
using wsldisk::ops::CompactOperation;
using wsldisk::ops::CompactOptions;
using wsldisk::ops::run;
using wsldisk::ops::RunOptions;
using wsldisk::testing::FakeClock;
using wsldisk::testing::FakeFileSystem;
using wsldisk::testing::FakeRegistry;
using wsldisk::testing::FakeDiskAttach;
using wsldisk::testing::FakeElevation;
using wsldisk::testing::FakeVirtualDisk;
using wsldisk::testing::FakeWslHost;
using wsldisk::testing::RecordingSink;
namespace hives = wsldisk::testing::hives;

namespace {

constexpr std::uint64_t gigabyte = 1024ULL * 1024 * 1024;

const std::filesystem::path ubuntu_disk =
    LR"(C:\Users\example\AppData\Local\wsl\{4d1297e9-bac4-4da1-9867-a2ab591e9581}\ext4.vhdx)";

/// A machine with Ubuntu's disk present at 14 GiB, compacting to 9 GiB.
struct Machine {
    FakeRegistry registry = hives::everything();
    FakeFileSystem filesystem;
    FakeVirtualDisk disks;
    FakeWslHost host;
    FakeClock clock;
    RecordingSink sink;

    Machine() { set_size(ubuntu_disk, 14 * gigabyte); }

    /// Gives `path` a file of `size`, and a virtual disk behind it that
    /// compacts to `after` -- or stays the same size, which is what a disk with
    /// nothing to reclaim does.
    void set_size(const std::filesystem::path& path, std::uint64_t size,
                  std::optional<std::uint64_t> after = std::nullopt) {
        FakeFileSystem::File file;
        file.size = size;
        file.size_on_disk = size;
        filesystem.add_file(path, file);

        FakeVirtualDisk::Disk disk;
        disk.info.virtual_size = 1024 * gigabyte;
        disk.info.physical_size = size;
        disk.physical_size_after_compact = after.value_or(size);
        disks.add_disk(path, disk);
    }

    /// Makes a compaction shrink the backing file too, the way a real one does.
    ///
    /// Wired through the disk fake's hook rather than set by the test after the
    /// fact, so the saving `compact` reports is a consequence of the compaction
    /// having happened rather than something asserted into existence.
    void compacts_to(const std::filesystem::path& path, std::uint64_t before, std::uint64_t after) {
        set_size(path, before, after);
        disks.on_compact([this](const std::filesystem::path& compacted, std::uint64_t size) {
            set_size(compacted, size);
        });
    }

    [[nodiscard]] Distro distro(std::string_view name) {
        const auto distros = enumerate(registry);
        REQUIRE(distros.has_value());
        const Distro* found = distros->find(name);
        REQUIRE(found != nullptr);
        return *found;
    }
};

}  // namespace

TEST_CASE("compact refuses a WSL1 distribution", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Legacy-WSL1")};

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::Preflight);
    CHECK(plan.error().remedy.find("--set-version") != std::string::npos);
}

TEST_CASE("compact refuses a disk that is not there", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Moved-Away")};

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::Preflight);
    CHECK(plan.error().message.find("does not exist") != std::string::npos);
    // The command that repairs it, rather than a dead end.
    CHECK(plan.error().remedy.find("orphans --relink") != std::string::npos);
}

TEST_CASE("compact plans trim, stop and compact in that order", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto plan = operation.plan();

    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 3);
    CHECK(plan->steps[0].description.find("fstrim") != std::string::npos);
    CHECK(plan->steps[1].description.find("stop Ubuntu") != std::string::npos);
    CHECK(plan->steps[2].description.find("compact") != std::string::npos);
    // Nothing here can be put back, so nothing pretends it can.
    CHECK(wsldisk::ops::irreversible_steps_are_last(*plan));
    CHECK_FALSE(plan->estimate.bytes_freed.has_value());
}

TEST_CASE("compact leaves out the trim step when asked to", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.trim = false}};

    const auto plan = operation.plan();

    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 2);
    CHECK(plan->steps[0].description.find("fstrim") == std::string::npos);
}

TEST_CASE("compact warns that it cannot be undone", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto plan = operation.plan();

    REQUIRE(plan.has_value());
    REQUIRE_FALSE(plan->warnings.empty());
    CHECK(plan->warnings[0].message.find("cannot be undone") != std::string::npos);
    // And that a refusal has a way forward, before the user meets it.
    CHECK(plan->warnings[1].remedy.find("--shutdown") != std::string::npos);
}

TEST_CASE("compact trims, stops and compacts", "[ops][compact]") {
    Machine machine;
    machine.host.on_command("/sbin/fstrim",
                            WslCommandResult{.standard_output = "/: 1078939029504 bytes trimmed\n"});
    machine.compacts_to(ubuntu_disk, 14 * gigabyte, 9 * gigabyte);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(machine.host.commands().size() == 1);
    CHECK(machine.host.terminated() == std::vector<std::string>{"Ubuntu"});
    CHECK(machine.disks.compacted().size() == 1);
    CHECK(operation.size_before() == 14 * gigabyte);
    CHECK(operation.size_after() == 9 * gigabyte);
    CHECK(operation.reclaimed() == 5 * gigabyte);
    CHECK(operation.trimmed_bytes() == 1078939029504ULL);
    REQUIRE(outcome->report.has_value());
    CHECK(outcome->report->actual.bytes_freed == 5 * gigabyte);
}

TEST_CASE("compact never shuts WSL down on its own", "[ops][compact]") {
    // Decision D9. Stopping every distribution to save disk space is not a
    // trade this tool makes for the user.
    Machine machine;
    machine.host.set_running({"Ubuntu", "docker-desktop"});
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::DistroBusy);
    CHECK(machine.host.shutdowns() == 0);
    CHECK(machine.disks.compacted().empty());
    // Naming who is holding it is the point of the refusal.
    CHECK(outcome.error().message.find("docker-desktop") != std::string::npos);
    CHECK(outcome.error().remedy.find("--shutdown") != std::string::npos);
}

TEST_CASE("compact still waits when the only name running is the one it stopped", "[ops][compact]") {
    // `wsl --list --running` can still name a distribution for a moment after
    // it has been terminated. Counting that as somebody else holding the disk
    // refused every ordinary `compact <distro>` a second after starting it --
    // naming, as the holder, the distribution wsldisk had just stopped itself.
    Machine machine;
    machine.host.set_running({"Ubuntu"});
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    // It waited the whole timeout rather than refusing at once.
    CHECK(machine.clock.total_slept() >= std::chrono::seconds{66});
}

TEST_CASE("compact does not wait out a timeout it cannot win", "[ops][compact]") {
    // The VM lets go only once nothing is running. While docker-desktop is up
    // it never will, so the ninety seconds would buy an answer that was already
    // known -- and the user would sit in front of them before being told.
    Machine machine;
    machine.host.set_running({"docker-desktop"});
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::DistroBusy);
    CHECK(machine.clock.slept().empty());
    CHECK(machine.sink.statuses.empty());
}

TEST_CASE("compact refuses a held disk even when nothing else is running", "[ops][compact]") {
    // Nothing is running, so there is nobody to name. That used to read as
    // "something else is holding it" with advice to stop distributions that
    // were already stopped; it is the utility VM winding down, and the remedy
    // says so.
    Machine machine;
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("still held by the WSL utility VM") != std::string::npos);
    CHECK(outcome.error().remedy.find("winding down") != std::string::npos);
    // The suggested longer wait is double the current one.
    CHECK(outcome.error().remedy.find("unlock_timeout_seconds 180") != std::string::npos);
}

TEST_CASE("the longer wait compact suggests is one the config would accept", "[ops][compact]") {
    // It suggests double the current wait, and the config caps the value at an
    // hour. Doubling something already near the cap would produce advice that
    // `wsldisk config set` then refuses, which is worse than no advice.
    Machine machine;
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{
        machine.disks,
        machine.filesystem,
        machine.host,
        machine.clock,
        machine.distro("Ubuntu"),
        CompactOptions{.unlock_timeout = std::chrono::seconds{max_unlock_timeout_seconds}}};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().remedy.find(std::format("unlock_timeout_seconds {}", max_unlock_timeout_seconds)) !=
          std::string::npos);
}

TEST_CASE("compact --shutdown stops everything and compacts", "[ops][compact]") {
    Machine machine;
    machine.host.set_running({"Ubuntu", "docker-desktop"});

    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.shutdown = true}};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(machine.host.shutdowns() == 1);
    // Terminating one is pointless once everything is going down.
    CHECK(machine.host.terminated().empty());
    CHECK(machine.disks.compacted().size() == 1);
}

TEST_CASE("compact waits long enough for the utility VM to idle out", "[ops][compact]") {
    // The wait used to be five seconds, on a measurement that said the handle is
    // never released on a timer. It is: with nothing running, the VM shuts down
    // on its idle timeout and lets go -- 66.6s and 66.7s on two consecutive runs
    // against WSL 2.7.8.0, whose vmIdleTimeout defaults to 60s. Five seconds
    // gave up about a minute early and sent the user to --shutdown for nothing.
    Machine machine;
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    // Long enough to outlast the idle timeout, and still bounded: a running
    // distribution keeps the VM up forever and no wait would help.
    CHECK(machine.clock.total_slept() >= std::chrono::seconds{66});
    CHECK(machine.clock.total_slept() <= std::chrono::seconds{91});
}

TEST_CASE("compact explains the wait once and then just counts", "[ops][compact]") {
    // It polls twice a second and used to print the whole sentence every time.
    // Over a five-second wait that was ten identical lines; over ninety it would
    // be a screenful of the same words. Now the explanation is said once and the
    // countdown goes on a line that redraws.
    Machine machine;
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    REQUIRE_FALSE(run(operation, machine.sink, RunOptions{}).has_value());

    const auto explained = std::ranges::count_if(machine.sink.messages, [](const std::string& line) {
        return line.find("utility VM still has the disk open") != std::string::npos;
    });
    CHECK(explained == 1);
    // One tick per second of the ninety, none of them a repeat.
    CHECK(machine.sink.statuses.size() == 90);
    CHECK(machine.sink.statuses.front() == "waiting for the disk ... 0s of 90s");
    CHECK(machine.sink.statuses.back() == "waiting for the disk ... 89s of 90s");
    CHECK(std::ranges::adjacent_find(machine.sink.statuses) == machine.sink.statuses.end());
}

TEST_CASE("compact reports a lock check it could not make", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    REQUIRE(operation.plan().has_value());
    // The disk went away between the preflight and the stop. "Cannot tell" is
    // not "free", and compacting on that answer would be acting blind.
    machine.filesystem.fail_queries(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot open", "run as the owning user"});

    const auto outcome = operation.execute(machine.sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::NeedsElevation);
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact is unmeasured rather than refused when the size cannot be read", "[ops][compact]") {
    // A size that cannot be taken is a gap in the report, not a reason to leave
    // the user's disk full.
    Machine machine;
    machine.filesystem.fail_size_on_disk(
        wsldisk::Error{ErrorCode::Preflight, "no such file", "check the path"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    REQUIRE(operation.plan().has_value());
    CHECK_FALSE(operation.size_before().has_value());
}

TEST_CASE("compact reports a terminate that failed", "[ops][compact]") {
    Machine machine;
    machine.host.fail_terminate(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact reports a shutdown that failed", "[ops][compact]") {
    Machine machine;
    machine.host.fail_shutdown(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.shutdown = true}};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact reports a failed fstrim without touching the disk", "[ops][compact]") {
    Machine machine;
    machine.host.on_command(
        "/sbin/fstrim",
        WslCommandResult{.exit_code = 1, .standard_error = "fstrim: /: FITRIM ioctl failed\n"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(machine.disks.compacted().empty());
    CHECK(machine.host.terminated().empty());
}

TEST_CASE("compact reports a disk it cannot open", "[ops][compact]") {
    Machine machine;
    machine.disks.fail_open(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot open the disk", "run as the owning user"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("compact reports a compaction that failed", "[ops][compact]") {
    Machine machine;
    machine.disks.fail_compact(
        wsldisk::Error{ErrorCode::Generic, "the compaction failed", "try again after a shutdown"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("the compaction failed") != std::string::npos);
}

TEST_CASE("compact reports the compaction's progress", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    REQUIRE(run(operation, machine.sink, RunOptions{}).has_value());

    // A compaction is the one step long enough that silence looks like a hang.
    CHECK_FALSE(machine.sink.progress_reports.empty());
}

TEST_CASE("compact changes nothing on a dry run", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{.dry_run = true});

    REQUIRE(outcome.has_value());
    CHECK(machine.disks.compacted().empty());
    CHECK(machine.host.commands().empty());
    CHECK(machine.host.terminated().empty());
    CHECK(machine.host.shutdowns() == 0);
}

TEST_CASE("compact starts the distribution again when asked", "[ops][compact]") {
    Machine machine;
    machine.host.set_running({"Ubuntu"});

    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.restart = true}};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    // fstrim, then the restart.
    REQUIRE(machine.host.commands().size() == 2);
    CHECK(machine.host.commands()[1].argv == std::vector<std::string>{"/bin/sh", "-c", ":"});
}

TEST_CASE("compact does not restart a distribution that was not running", "[ops][compact]") {
    Machine machine;

    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.restart = true}};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    // Only the fstrim. Starting it would be a change nobody asked for.
    CHECK(machine.host.commands().size() == 1);
}

TEST_CASE("compact survives a restart that fails", "[ops][compact]") {
    // The compaction succeeded. A distribution that did not come back up starts
    // on the user's next command, and failing the whole run would misreport it.
    Machine machine;
    machine.host.set_running({"Ubuntu"});
    machine.host.fail_command_from(
        2, wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.restart = true}};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(machine.sink.said("could not start Ubuntu again"));
}

TEST_CASE("compact of a loose file trims nothing and stops nothing", "[ops][compact]") {
    Machine machine;
    machine.set_size(LR"(D:\disks\docker_data.vhdx)", 68 * gigabyte);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               std::filesystem::path{LR"(D:\disks\docker_data.vhdx)"}};

    const auto plan = operation.plan();
    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 1);

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(machine.host.commands().empty());
    CHECK(machine.host.terminated().empty());
    CHECK(machine.disks.compacted().size() == 1);
}

TEST_CASE("compact of a loose file refuses one something else has open", "[ops][compact]") {
    // Docker Desktop's data volume, whenever Docker is running.
    Machine machine;
    machine.set_size(LR"(D:\disks\docker_data.vhdx)", 68 * gigabyte);
    machine.filesystem.lock_file(LR"(D:\disks\docker_data.vhdx)");

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               std::filesystem::path{LR"(D:\disks\docker_data.vhdx)"}};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::DistroBusy);
    CHECK(outcome.error().remedy.find("Docker Desktop") != std::string::npos);
    CHECK(machine.disks.compacted().empty());
}

TEST_CASE("compact of a loose file reports a lock check it could not make", "[ops][compact]") {
    Machine machine;
    machine.set_size(LR"(D:\disks\docker_data.vhdx)", 68 * gigabyte);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               std::filesystem::path{LR"(D:\disks\docker_data.vhdx)"}};

    // Refused after the preflight read it and before the disk is opened.
    REQUIRE(operation.plan().has_value());
    machine.filesystem.fail_queries(
        wsldisk::Error{ErrorCode::NeedsElevation, "cannot open", "run as the owning user"});

    const auto outcome = operation.execute(machine.sink);

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("compact of a loose file that is not there is refused", "[ops][compact]") {
    Machine machine;

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               std::filesystem::path{LR"(D:\disks\gone.vhdx)"}};

    const auto plan = operation.plan();

    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().remedy == "check the path");
}

TEST_CASE("compact reports a file that could not be measured", "[ops][compact]") {
    // Compacted, but with nothing to say about the saving. Better than a zero.
    Machine machine;
    machine.filesystem.fail_size_on_disk(
        wsldisk::Error{ErrorCode::Preflight, "no such file", "re-run the scan"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK_FALSE(operation.size_before().has_value());
    CHECK_FALSE(operation.reclaimed().has_value());
}

TEST_CASE("compact reports nothing reclaimed when the file did not shrink", "[ops][compact]") {
    // A real and common answer on a disk that was already compact. Zero, not a
    // negative saving.
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE(outcome.has_value());
    CHECK(operation.reclaimed() == 0);
}

TEST_CASE("compact notices a disk that grew", "[ops][compact]") {
    // Not something that should ever happen. Cheap to rule out, and serious
    // enough to be worth saying if it does.
    Machine machine;
    machine.compacts_to(ubuntu_disk, 14 * gigabyte, 15 * gigabyte);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::Partial);
    CHECK(outcome.error().message.find("grew during compaction") != std::string::npos);
}

TEST_CASE("compact has nothing to undo", "[ops][compact]") {
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    operation.rollback(machine.sink);

    CHECK(machine.sink.messages.empty());
    CHECK(operation.path() == ubuntu_disk);
}

TEST_CASE("compact survives not being able to ask what is running", "[ops][compact]") {
    // wsl.exe not answering that question is not a reason to refuse: the lock
    // check is the one that decides, and it works either way.
    Machine machine;
    machine.host.fail_running(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    CHECK(run(operation, machine.sink, RunOptions{}).has_value());
}

TEST_CASE("compact names every distribution holding the disk", "[ops][compact]") {
    // One name is a hint; three is the answer. The refusal has to list them all
    // or the user closes one window and hits the same wall.
    Machine machine;
    machine.host.set_running({"Ubuntu", "docker-desktop", "rancher-desktop"});
    machine.filesystem.lock_file(ubuntu_disk);

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("still open in docker-desktop, rancher-desktop") != std::string::npos);
    // The target is not in the list because it was terminated first, not
    // because it was filtered out: what WSL says is running is what the user is
    // told.
    CHECK(outcome.error().message.find("Ubuntu,") == std::string::npos);
}

TEST_CASE("compact cannot name who is holding the disk when wsl.exe will not say", "[ops][compact]") {
    Machine machine;
    machine.filesystem.lock_file(ubuntu_disk);
    machine.host.fail_running(
        wsldisk::Error{ErrorCode::Generic, "wsl.exe did not answer", "check WSL is installed"});

    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    const auto outcome = run(operation, machine.sink, RunOptions{});

    // Still refused, and still with a way forward -- just without the names.
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ErrorCode::DistroBusy);
    CHECK(outcome.error().remedy.find("--shutdown") != std::string::npos);
}

TEST_CASE("compact reports no saving when only one end could be measured", "[ops][compact]") {
    // The file was there for the preflight and gone for the measurement. Half a
    // measurement is not a saving.
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    REQUIRE(operation.plan().has_value());
    REQUIRE(operation.size_before().has_value());
    machine.filesystem.fail_size_on_disk(
        wsldisk::Error{ErrorCode::Preflight, "no such file", "check the path"});

    const auto outcome = operation.execute(machine.sink);

    REQUIRE(outcome.has_value());
    CHECK_FALSE(operation.size_after().has_value());
    CHECK_FALSE(operation.reclaimed().has_value());
    // Verify has nothing to compare, and says so by passing rather than
    // inventing a failure.
    CHECK(operation.verify().has_value());
}

TEST_CASE("compact --shutdown still checks the disk was released", "[ops][compact]") {
    // The check used to be skipped on exactly the path where the user has paid
    // the highest price for it: every distribution and every container stopped.
    // `wsl --shutdown` can exit 0 while the utility VM's handle lingers, and it
    // can do nothing at all about a backup agent holding the file -- so the
    // compaction failed at `open` with a raw virtual-disk error instead of a
    // named refusal.
    Machine machine;
    machine.filesystem.lock_file(ubuntu_disk);
    machine.host.set_running({"Ubuntu"});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.shutdown = true}};

    REQUIRE(operation.plan().has_value());
    const auto report = operation.execute(machine.sink);

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == ErrorCode::DistroBusy);
    // And the remedy does not send them round the loop they just came out of.
    CHECK(report.error().remedy.find("re-run with --shutdown") == std::string::npos);
    CHECK(report.error().remedy.find("something else") != std::string::npos);
}

TEST_CASE("compact measures its baseline after the guest is quiet", "[ops][compact]") {
    // `plan()` runs before the distribution is stopped and before an fstrim that
    // is allowed ten minutes, so a guest write in between inflated the file and
    // `verify()` reported "grew during compaction" for a compaction that worked.
    // A build running inside the distribution was enough to do it.
    Machine machine;
    machine.compacts_to(ubuntu_disk, 14 * gigabyte, 9 * gigabyte);
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    REQUIRE(operation.plan().has_value());
    // The guest writes between planning and compacting, as a build would.
    machine.set_size(ubuntu_disk, 20 * gigabyte, 9 * gigabyte);

    REQUIRE(operation.execute(machine.sink).has_value());

    // Measured at 20 GiB, not the 14 GiB seen at plan time, so the saving is
    // real and `verify()` does not cry wolf.
    REQUIRE(operation.size_before().has_value());
    CHECK(*operation.size_before() == 20 * gigabyte);
    CHECK(operation.verify().has_value());
}

TEST_CASE("compact restarts what it stopped even when the compaction fails", "[ops][compact]") {
    // `--restart` used to be kept only on the success path, so a run that
    // stopped the distribution and then failed left it stopped -- on exactly the
    // run where the user did not get what they came for.
    Machine machine;
    machine.host.set_running({"Ubuntu"});
    machine.disks.fail_compact(
        wsldisk::Error{ErrorCode::Generic, "the compaction failed", "try again after a reboot"});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.restart = true}};

    REQUIRE(operation.plan().has_value());
    const auto report = operation.execute(machine.sink);

    REQUIRE_FALSE(report.has_value());
    const bool restarted = std::ranges::any_of(machine.host.commands(), [](const auto& invocation) {
        return invocation.distribution == "Ubuntu" && !invocation.argv.empty() &&
               invocation.argv.front() == "/bin/sh";
    });
    CHECK(restarted);
}

TEST_CASE("compact does not claim it restarted a distribution that would not boot", "[ops][compact]") {
    // `interfaces.h` calls the exit code the only success signal that can be
    // trusted, and the restart used to ignore it -- so a distribution that failed
    // to come back up was reported as restarted, in the text and in the --json.
    // That is the one signal that the compacted disk has a problem.
    Machine machine;
    machine.compacts_to(ubuntu_disk, 14 * gigabyte, 9 * gigabyte);
    machine.host.set_running({"Ubuntu"});
    machine.host.on_command("/bin/sh", wsldisk::WslCommandResult{.exit_code = 1});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.restart = true}};

    REQUIRE(operation.plan().has_value());
    const auto report = operation.execute(machine.sink);

    REQUIRE(report.has_value());
    const bool claimed = std::ranges::any_of(report->completed, [](const std::string& step) {
        return step.find("start Ubuntu again") != std::string::npos;
    });
    CHECK_FALSE(claimed);
}

TEST_CASE("a failed compaction restarts nothing when --restart was not asked for", "[ops][compact]") {
    // The other side of restart_if_asked: it must not start a distribution the
    // user never asked it to start, even one it stopped.
    Machine machine;
    machine.host.set_running({"Ubuntu"});
    machine.disks.fail_compact(
        wsldisk::Error{ErrorCode::Generic, "the compaction failed", "try again after a reboot"});
    CompactOperation operation{machine.disks, machine.filesystem, machine.host, machine.clock,
                               machine.distro("Ubuntu")};

    REQUIRE(operation.plan().has_value());
    REQUIRE_FALSE(operation.execute(machine.sink).has_value());

    const bool restarted = std::ranges::any_of(machine.host.commands(), [](const auto& invocation) {
        return !invocation.argv.empty() && invocation.argv.front() == "/bin/sh";
    });
    CHECK_FALSE(restarted);
}

TEST_CASE("a failed compaction starts nothing that was not running", "[ops][compact]") {
    // `--restart` puts back what was there. A distribution that was already
    // stopped stays stopped, on the failure path as on the success path.
    Machine machine;
    machine.host.set_running({});
    machine.disks.fail_compact(
        wsldisk::Error{ErrorCode::Generic, "the compaction failed", "try again after a reboot"});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.restart = true}};

    REQUIRE(operation.plan().has_value());
    REQUIRE_FALSE(operation.execute(machine.sink).has_value());

    const bool restarted = std::ranges::any_of(machine.host.commands(), [](const auto& invocation) {
        return !invocation.argv.empty() && invocation.argv.front() == "/bin/sh";
    });
    CHECK_FALSE(restarted);
}

TEST_CASE("elevate refuses when nothing wired it up", "[ops][compact][elevate]") {
    // Compacting the ordinary way instead would report success for a path the
    // user did not ask for, and the difference between the two is the only
    // reason to pass the flag.
    Machine machine;
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};

    REQUIRE(operation.plan().has_value());
    const auto report = operation.execute(machine.sink);

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == ErrorCode::Usage);
    CHECK_FALSE(report.error().remedy.empty());
}

TEST_CASE("elevate attaches in place when the token is already held", "[ops][compact][elevate]") {
    // An administrator shell needs no second prompt, and relaunching from one
    // would ask the user to approve something they had already approved.
    Machine machine;
    FakeElevation elevation;
    FakeDiskAttach attach;
    elevation.set_elevated(true);
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};
    operation.set_elevation(&elevation, &attach);

    REQUIRE(operation.plan().has_value());
    REQUIRE(operation.execute(machine.sink).has_value());

    CHECK(elevation.requests().empty());
    REQUIRE(attach.attached().size() == 1);
    CHECK(attach.attached().front() == ubuntu_disk);
}

TEST_CASE("an attached disk is detached even when the compaction fails", "[ops][compact][elevate]") {
    // A disk left attached stays attached until the machine reboots, and the
    // distribution will not start while it is. Worse than not compacting.
    Machine machine;
    FakeElevation elevation;
    FakeDiskAttach attach;
    elevation.set_elevated(true);
    attach.fail_compact(
        wsldisk::Error{ErrorCode::Generic, "the compaction failed", "try again after a reboot"});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};
    operation.set_elevation(&elevation, &attach);

    REQUIRE(operation.plan().has_value());
    REQUIRE_FALSE(operation.execute(machine.sink).has_value());

    CHECK(attach.attached() == attach.detached());
}

TEST_CASE("elevate relaunches when the token is not held", "[ops][compact][elevate]") {
    Machine machine;
    FakeElevation elevation;
    FakeDiskAttach attach;
    elevation.set_elevated(false);
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};
    operation.set_elevation(&elevation, &attach);

    REQUIRE(operation.plan().has_value());
    REQUIRE(operation.execute(machine.sink).has_value());

    // Relaunched rather than attached here, and told one verb and one path (D11).
    REQUIRE(elevation.requests().size() == 1);
    CHECK(elevation.requests().front().verb == "compact-full");
    CHECK(elevation.requests().front().target == ubuntu_disk);
    CHECK(attach.attached().empty());
}

TEST_CASE("declining the prompt is not a crash", "[ops][compact][elevate]") {
    // The user was asked and said no. Nothing ran, nothing needs undoing, and
    // the exit code says which of those it was.
    Machine machine;
    FakeElevation elevation;
    FakeDiskAttach attach;
    elevation.fail_relaunch(wsldisk::Error{ErrorCode::NeedsElevation, "the administrator prompt was declined",
                                           "nothing was changed; re-run and approve it"});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};
    operation.set_elevation(&elevation, &attach);

    REQUIRE(operation.plan().has_value());
    const auto report = operation.execute(machine.sink);

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == ErrorCode::NeedsElevation);
}

TEST_CASE("a worker that exits non-zero fails the compaction", "[ops][compact][elevate]") {
    // The worker has already explained itself over the pipe, so this carries the
    // exit code rather than inventing a second story about what went wrong.
    Machine machine;
    FakeElevation elevation;
    FakeDiskAttach attach;
    elevation.set_exit_code(wsldisk::exit_code_for(ErrorCode::DistroBusy));
    elevation.set_messages({"the disk is held open by another process"});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};
    operation.set_elevation(&elevation, &attach);

    REQUIRE(operation.plan().has_value());
    const auto report = operation.execute(machine.sink);

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == ErrorCode::Generic);
    CHECK(report.error().message.find("11") != std::string::npos);
}

TEST_CASE("a cancelled worker reports partial rather than failure", "[ops][compact][elevate]") {
    // Exit 5 means it unwound deliberately: the disk is detached and nothing was
    // half-written. That is a different answer from "the compaction broke".
    Machine machine;
    FakeElevation elevation;
    FakeDiskAttach attach;
    elevation.set_exit_code(wsldisk::exit_code_for(ErrorCode::Partial));
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};
    operation.set_elevation(&elevation, &attach);

    REQUIRE(operation.plan().has_value());
    const auto report = operation.execute(machine.sink);

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == ErrorCode::Partial);
}

TEST_CASE("the elevated worker's output reaches the user", "[ops][compact][elevate]") {
    // Its console is hidden. A line it sends that the parent drops is a line
    // nobody ever sees.
    Machine machine;
    FakeElevation elevation;
    FakeDiskAttach attach;
    elevation.set_messages({"attached read-only", "consulting the filesystem bitmap"});
    CompactOperation operation{machine.disks, machine.filesystem,       machine.host,
                               machine.clock, machine.distro("Ubuntu"), CompactOptions{.elevate = true}};
    operation.set_elevation(&elevation, &attach);

    REQUIRE(operation.plan().has_value());
    REQUIRE(operation.execute(machine.sink).has_value());

    CHECK(machine.sink.said("attached read-only"));
    CHECK(machine.sink.said("consulting the filesystem bitmap"));
}
