#include "elevation.h"

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#include <array>
#include <charconv>
#include <cstdint>
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

/// How long to wait for the elevated child to connect before giving up.
///
/// The UAC prompt is answered by a human, but `ShellExecuteEx` does not return
/// until it has been, so this covers only the child's own start-up. Thirty
/// seconds is generous for that and still bounded: a worker that never connects
/// is better reported than waited on forever.
constexpr DWORD connect_timeout_ms = 30'000;

/// How long to wait for the worker to exit once it has sent its result.
constexpr DWORD exit_timeout_ms = 30'000;

/// One pipe message. Bigger than any record the worker sends; a record that
/// does not fit is truncated rather than misparsed, because message-mode reads
/// do not split records across reads.
constexpr DWORD record_buffer_bytes = 4096;

/// 128 bits, hex-encoded, so the name cannot be guessed by an squatter racing
/// for it. `FILE_FLAG_FIRST_PIPE_INSTANCE` is what makes losing that race loud.
Result<std::wstring> random_pipe_name() {
    std::array<unsigned char, 16> bytes{};
    const NTSTATUS status = win32().bcrypt_gen_random(nullptr, bytes.data(),
                                                      static_cast<ULONG>(bytes.size()),
                                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return fail(ErrorCode::Generic, "generate a name for the result pipe",
                    "this is a system RNG failure; retrying is unlikely to help");
    }
    std::wstring name = L"\\\\.\\pipe\\wsldisk-";
    for (const unsigned char byte : bytes) {
        name += std::format(L"{:02x}", byte);
    }
    return name;
}

/// The security descriptor both the pipe and the cancel event get: this user,
/// nothing inherited, and no other ACE at all.
class UserOnlyDescriptor {
public:
    [[nodiscard]] Status build() {
        // "D:P(A;;GA;;;OW)" would be the owner, which is not the same thing
        // under an elevated token. CO -- creator owner -- is, and it resolves to
        // the account that created the object either way.
        const wchar_t* sddl = L"D:P(A;;GA;;;CO)";
        if (!win32().convert_string_sd_to_sd(sddl, SDDL_REVISION_1, &descriptor_, nullptr)) {
            return std::unexpected(error_from_win32(win32().get_last_error(),
                                                    "build the security descriptor for the result pipe"));
        }
        return {};
    }

    UserOnlyDescriptor() = default;
    UserOnlyDescriptor(const UserOnlyDescriptor&) = delete;
    UserOnlyDescriptor& operator=(const UserOnlyDescriptor&) = delete;
    UserOnlyDescriptor(UserOnlyDescriptor&&) = delete;
    UserOnlyDescriptor& operator=(UserOnlyDescriptor&&) = delete;

    ~UserOnlyDescriptor() {
        if (descriptor_ != nullptr) std::ignore = win32().local_free(descriptor_);
    }

    [[nodiscard]] SECURITY_ATTRIBUTES attributes() noexcept {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor_;
        attributes.bInheritHandle = FALSE;
        return attributes;
    }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
};

/// Splits `<tag>|<rest>` without allocating a vector of fields.
std::string_view field(std::string_view record, std::size_t index) {
    std::size_t start = 0;
    for (std::size_t seen = 0; seen < index; ++seen) {
        const std::size_t bar = record.find('|', start);
        if (bar == std::string_view::npos) return {};
        start = bar + 1;
    }
    const std::size_t bar = record.find('|', start);
    return record.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
}

std::uint64_t to_number(std::string_view text) {
    std::uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

}  // namespace

namespace records {

std::string progress(std::uint64_t current, std::uint64_t total) {
    return std::format("P|{}|{}", current, total);
}

std::string line(std::string_view text) { return std::format("L|{}", text); }

std::string result(int exit_code, std::string_view text) {
    return std::format("R|{}|{}", exit_code, text);
}

}  // namespace records

bool Win32Elevation::is_elevated() const {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (!win32().allocate_and_initialize_sid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                             DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        return false;
    }
    BOOL is_member = FALSE;
    // A null token means the calling thread's effective token, which is what
    // matters: a filtered administrator token answers false here, correctly.
    const BOOL checked = win32().check_token_membership(nullptr, admins, &is_member);
    std::ignore = win32().free_sid(admins);
    return checked != FALSE && is_member != FALSE;
}

Result<int> Win32Elevation::run_elevated(std::string_view verb, const std::filesystem::path& target,
                                         const ElevatedSink& sink) const {
    const Result<std::wstring> pipe_name = random_pipe_name();
    if (!pipe_name) return std::unexpected(pipe_name.error());

    // The cancel event's name is derived from the same random bits, so one
    // secret covers both objects.
    const std::wstring event_name = L"Local\\wsldisk-cancel-" + pipe_name->substr(pipe_name->rfind(L'-') + 1);

    UserOnlyDescriptor descriptor;
    if (const Status built = descriptor.build(); !built) return std::unexpected(built.error());
    SECURITY_ATTRIBUTES attributes = descriptor.attributes();

    // Inbound only. See the class comment: a second direction on this handle is
    // what deadlocked the spike.
    const ScopedHandle pipe{win32().create_named_pipe(
        pipe_name->c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        record_buffer_bytes, record_buffer_bytes, 0, &attributes)};
    if (pipe.get() == INVALID_HANDLE_VALUE || pipe.get() == nullptr) {
        const DWORD failure = win32().get_last_error();
        // ERROR_ACCESS_DENIED here is the squatting case: something already
        // holds the name. Refusing is the whole point of asking for the first
        // instance, so it is not reported as an ordinary failure to create.
        if (failure == ERROR_ACCESS_DENIED || failure == ERROR_PIPE_BUSY) {
            return fail(ErrorCode::Preflight, "another process is already holding the result pipe's name",
                        "nothing was elevated; re-run, and if it persists something is watching for it");
        }
        return std::unexpected(error_from_win32(failure, "create the result pipe"));
    }

    const ScopedHandle cancel{win32().create_event(&attributes, TRUE, FALSE, event_name.c_str())};
    if (cancel.get() == nullptr) {
        return std::unexpected(error_from_win32(win32().get_last_error(), "create the cancel event"));
    }

    std::wstring image(MAX_PATH, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (length == 0 || length >= image.size()) {
        return std::unexpected(error_from_win32(win32().get_last_error(), "find our own executable"));
    }
    image.resize(length);

    // A verb and a path, and nothing else (D11). The worker re-validates the
    // path against the registry rather than trusting it.
    const std::wstring parameters =
        std::format(L"--elevated-worker {} --pipe {} --cancel-event {} --target \"{}\"",
                    std::wstring(verb.begin(), verb.end()), *pipe_name, event_name, target.wstring());

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = image.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;

    if (!win32().shell_execute_ex(&info)) {
        const DWORD failure = win32().get_last_error();
        if (failure == ERROR_CANCELLED) {
            // Not an error in any interesting sense: the user was asked and said
            // no. Nothing has run, nothing needs undoing.
            return fail(ErrorCode::NeedsElevation, "the administrator prompt was declined",
                        "nothing was changed; re-run and approve it to use the attached path");
        }
        return std::unexpected(error_from_win32(failure, "relaunch with an administrator token"));
    }
    const ScopedHandle child{info.hProcess};
    const DWORD child_pid = win32().get_process_id(child.get());

    if (!win32().connect_named_pipe(pipe.get(), nullptr) &&
        win32().get_last_error() != ERROR_PIPE_CONNECTED) {
        return std::unexpected(error_from_win32(win32().get_last_error(), "wait for the elevated worker"));
    }

    // The connection is only trusted once it is our own child on the other end.
    // The name is random and the DACL is this user alone, so this is the last
    // rung rather than the only one -- but it is cheap and it is exact.
    ULONG client_pid = 0;
    if (!win32().get_named_pipe_client_process_id(pipe.get(), &client_pid)) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), "identify what connected to the result pipe"));
    }
    if (client_pid != child_pid) {
        return fail(ErrorCode::Preflight, "something other than the elevated worker connected",
                    "nothing was changed; this should not happen, so treat it as a security event");
    }

    int reported_exit = exit_code_for(ErrorCode::Generic);
    std::vector<char> buffer(record_buffer_bytes);
    while (true) {  // LCOV_EXCL_BR_LINE
        DWORD read = 0;
        if (!win32().read_file(pipe.get(), buffer.data(), record_buffer_bytes, &read, nullptr)) {
            // The worker closed without a result record. Its exit code is the
            // only thing left to report, and it is more trustworthy than a guess.
            break;
        }
        const std::string_view record{buffer.data(), read};
        if (record.starts_with("P|")) {
            const DiskProgress reported{to_number(field(record, 1)), to_number(field(record, 2))};
            if (sink.progress && !sink.progress(reported)) {
                std::ignore = win32().set_event(cancel.get());
            }
        } else if (record.starts_with("L|")) {
            if (sink.message) sink.message(field(record, 1));
        } else if (record.starts_with("R|")) {
            reported_exit = static_cast<int>(to_number(field(record, 1)));
            if (sink.message) sink.message(field(record, 2));
            break;
        }
        // An unknown tag is ignored rather than fatal: a newer worker talking to
        // an older parent should degrade, not abort a compaction half-way.
    }

    std::ignore = win32().wait_for_single_object(child.get(), exit_timeout_ms);
    DWORD child_exit = 0;
    if (!win32().get_exit_code_process(child.get(), &child_exit)) {
        return reported_exit;
    }
    // The worker's own exit code wins over the record it sent: a process that
    // died after reporting success did not succeed.
    return static_cast<int>(child_exit);
}

}  // namespace wsldisk::platform
