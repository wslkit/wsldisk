#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "../interfaces.h"

namespace wsldisk::platform {

/// `IElevation` on top of `runas` and a one-way named pipe.
///
/// The shape is the one the spike for issue #6 settled, and every part of it was
/// measured rather than assumed (docs/RESEARCH.md):
///
/// - The result pipe is **one-way**, worker to parent. A duplex pipe deadlocks:
///   I/O on a synchronous file object is serialized, so a pending `ReadFile`
///   blocks the concurrent `WriteFile` the other direction needs, and both
///   halves hang. Cancellation therefore rides a separate event.
/// - Its name carries 128 bits from `BCryptGenRandom` and it is created with
///   `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a squatter holding the name makes this
///   fail loudly rather than letting the worker talk to someone else.
/// - Its DACL names the launching user and nobody else, not even SYSTEM. The
///   elevated child is the same user one integrity level up, so it opens
///   without the ACL being weakened.
/// - Declining the prompt is `ERROR_CANCELLED`, reported as
///   `ErrorCode::NeedsElevation` rather than as a crash or a hang.
///
/// What the worker is told is a verb and a path, and nothing travels back up the
/// pipe into it (D11): it re-validates the path itself.
class Win32Elevation final : public IElevation {
public:
    [[nodiscard]] bool is_elevated() const override;

    [[nodiscard]] Result<int> run_elevated(std::string_view verb, const std::filesystem::path& target,
                                           const ElevatedSink& sink) const override;
};

/// The record protocol the two halves speak, exposed for the worker side and
/// for tests. One record per pipe message, `<tag>|<fields>`:
///
///     I|<sid>|<elevated>|<integrity>   the worker's own identity, sent first
///     L|<text>                         a line for the user
///     P|<current>|<total>              progress
///     R|<exit code>|<text>             the final result, sent once
///
/// Deliberately not JSON: the worker's console is hidden, so this is the only
/// channel, and a format that cannot fail to serialize is worth more here than
/// one that is pleasant to extend.
namespace records {

[[nodiscard]] std::string progress(std::uint64_t current, std::uint64_t total);
[[nodiscard]] std::string line(std::string_view text);
[[nodiscard]] std::string result(int exit_code, std::string_view text);

}  // namespace records

}  // namespace wsldisk::platform
