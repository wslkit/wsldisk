// Spike for issue #6 -- elevation relaunch and named-pipe result streaming.
//
// Throwaway code. It is not built by CMake, not linted and not covered; it
// exists to answer the three security questions in the issue and to pin down
// the IPC shape before any of it is written into src/. Build it with run.ps1.
//
// The shape under test:
//
//   parent (medium IL)                    worker (high IL, launched by runas)
//   ------------------                    ----------------------------------
//   CheckTokenMembership -> not admin
//   random pipe name, FIRST_PIPE_INSTANCE
//   DACL: only this user's SID
//   ShellExecuteEx "runas" -------------> starts, verifies the *server* before
//                                         it trusts anything it was told
//   ConnectNamedPipe                 <--- I|sid|elevated|integrity
//   verify client SID + elevation    <--- P|pct|text progress records
//   print records as one stream
//   Ctrl+C -> SetEvent(cancel) ------->   polled each tick, unwinds
//   exit with the worker's code      <--- R|code|text
//
// The pipe is one-way (worker -> parent) and cancellation rides a separate
// event, not a second direction on the same handle: a synchronous file object
// serializes I/O, so a pending ReadFile blocks a concurrent WriteFile on that
// handle and both halves hang. Measured; see docs/RESEARCH.md.
//
// Roles: (default) parent, --worker, --squat, --send-ctrl-c, --whoami.

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitGeneric = 1;
constexpr int kExitNeedsElevation = 4;  // ErrorCode::NeedsElevation
constexpr int kExitPartial = 5;         // ErrorCode::Partial -- used for cancellation

std::atomic<bool> g_cancelled{false};
HANDLE g_cancel_event = nullptr;
HANDLE g_worker_pipe = INVALID_HANDLE_VALUE;

void say(const char* role, const char* fmt, ...) {
    printf("[%s] ", role);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

std::wstring last_error_text(DWORD err) {
    LPWSTR buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = len ? std::wstring(buffer, len) : std::wstring();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    return text;
}

void report_error(const char* role, const char* what, DWORD err) {
    say(role, "%s failed: %lu (%ls)", what, err, last_error_text(err).c_str());
}

// --- token questions ---------------------------------------------------------

// "Am I already elevated" -- the issue's first mechanism. CheckTokenMembership
// with a NULL token tests the *effective* token of the calling thread.
bool is_elevated() {
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (!AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        return false;
    }
    BOOL is_member = FALSE;
    const BOOL ok = CheckTokenMembership(nullptr, admins, &is_member);
    FreeSid(admins);
    return ok && is_member;
}

std::wstring token_user_sid(HANDLE token) {
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> buffer(needed);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) return L"";
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid_text))
        return L"";
    std::wstring result = sid_text;
    LocalFree(sid_text);
    return result;
}

std::wstring current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
    std::wstring sid = token_user_sid(token);
    CloseHandle(token);
    return sid;
}

// Integrity level as a printable RID: 0x2000 medium, 0x3000 high.
DWORD integrity_level(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return 0;
    DWORD needed = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    std::vector<BYTE> buffer(needed);
    DWORD rid = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), needed, &needed)) {
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
        const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
        rid = *GetSidSubAuthority(label->Label.Sid, count - 1);
    }
    CloseHandle(token);
    return rid;
}

std::wstring own_image_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) return L"";
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// --- pipe --------------------------------------------------------------------

std::wstring random_pipe_name() {
    unsigned char bytes[16] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return L"";
    }
    std::wstring name = L"\\\\.\\pipe\\wsldisk-";
    for (unsigned char b : bytes) {
        wchar_t hex[3] = {};
        swprintf(hex, 3, L"%02x", b);
        name += hex;
    }
    return name;
}

// The DACL the issue asks about: only the launching user, nothing inherited.
// "GA" to the user's own SID and no other ACE at all -- not even SYSTEM.
PSECURITY_DESCRIPTOR user_only_descriptor(const std::wstring& user_sid) {
    const std::wstring sddl = L"D:P(A;;GA;;;" + user_sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        return nullptr;
    }
    return descriptor;
}

// One way only: the worker writes, the parent reads. Cancellation travels on a
// separate event, because a second direction on this handle deadlocks -- a
// synchronous file object serializes all I/O, so a pending ReadFile blocks the
// WriteFile the other thread is trying to make. Measured; see docs/RESEARCH.md.
HANDLE create_server_pipe(const std::wstring& name, const std::wstring& user_sid, DWORD access_mode,
                          bool first_only) {
    PSECURITY_DESCRIPTOR descriptor = user_only_descriptor(user_sid);
    if (!descriptor) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof attributes;
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    DWORD open_mode = access_mode;
    if (first_only) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

    const HANDLE pipe = CreateNamedPipeW(
        name.c_str(), open_mode,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        64 * 1024, 64 * 1024, 0, &attributes);
    const DWORD err = GetLastError();
    LocalFree(descriptor);
    SetLastError(err);
    return pipe;
}

// The control channel: one bit, manual-reset, same user-only DACL, session-local
// namespace ("Local\") because the elevated child stays in the same session.
HANDLE create_cancel_event(const std::wstring& name, const std::wstring& user_sid) {
    PSECURITY_DESCRIPTOR descriptor = user_only_descriptor(user_sid);
    if (!descriptor) return nullptr;
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof attributes;
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    const HANDLE event = CreateEventW(&attributes, TRUE, FALSE, name.c_str());
    const DWORD err = GetLastError();
    LocalFree(descriptor);
    SetLastError(err);
    return event;
}

bool write_record(HANDLE pipe, const std::string& record) {
    DWORD written = 0;
    return WriteFile(pipe, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) != 0;
}

bool read_record(HANDLE pipe, std::string& out) {
    char buffer[4096];
    DWORD read = 0;
    if (!ReadFile(pipe, buffer, sizeof buffer, &read, nullptr)) return false;
    out.assign(buffer, read);
    return true;
}

// --- worker (elevated half) --------------------------------------------------

// The third question in the issue: an unprivileged process could create a pipe
// with our name first and feed the elevated worker instructions. Two defences,
// both measured here: the server creates with FILE_FLAG_FIRST_PIPE_INSTANCE, and
// the worker refuses to talk to a server that is not the process it expects.
bool verify_server(HANDLE pipe, DWORD expected_pid, const char* role) {
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server_pid)) {
        report_error(role, "GetNamedPipeServerProcessId", GetLastError());
        return false;
    }
    say(role, "server pid reported as %lu, expected %lu", server_pid, expected_pid);
    if (expected_pid != 0 && server_pid != expected_pid) {
        say(role, "REFUSING: pipe is served by a different process than the one that launched us");
        return false;
    }
    const HANDLE server = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_pid);
    if (!server) {
        report_error(role, "OpenProcess(server)", GetLastError());
        return false;
    }
    wchar_t image[MAX_PATH * 4] = {};
    DWORD size = static_cast<DWORD>(std::size(image));
    const bool got_image = QueryFullProcessImageNameW(server, 0, image, &size) != 0;
    HANDLE server_token = nullptr;
    std::wstring server_sid;
    if (OpenProcessToken(server, TOKEN_QUERY, &server_token)) {
        server_sid = token_user_sid(server_token);
        CloseHandle(server_token);
    }
    CloseHandle(server);
    if (!got_image) return false;
    say(role, "server image  %ls", image);
    say(role, "server sid    %ls", server_sid.c_str());
    if (_wcsicmp(image, own_image_path().c_str()) != 0) {
        say(role, "REFUSING: server image is not our own binary");
        return false;
    }
    if (server_sid != current_user_sid()) {
        say(role, "REFUSING: server runs as a different user");
        return false;
    }
    return true;
}

BOOL WINAPI worker_ctrl_handler(DWORD type) {
    // Measured, not assumed: does a Ctrl+C in the *parent's* console reach the
    // elevated child at all? The worker's own console is hidden, so the answer
    // is only observable if it travels back over the pipe.
    printf("[worker] console control event %lu delivered to the elevated child\n", type);
    fflush(stdout);
    if (g_worker_pipe != INVALID_HANDLE_VALUE) {
        char record[128];
        snprintf(record, sizeof record, "L|console control event %lu reached the elevated child",
                 type);
        write_record(g_worker_pipe, record);
    }
    return TRUE;
}

int run_worker(const std::wstring& pipe_name, const std::wstring& event_name, DWORD server_pid,
               int seconds) {
    const char* role = "worker";
    SetConsoleCtrlHandler(worker_ctrl_handler, TRUE);
    say(role, "elevated=%s integrity=0x%04lx sid=%ls", is_elevated() ? "yes" : "no",
        integrity_level(GetCurrentProcess()), current_user_sid().c_str());

    const HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                                    nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        report_error(role, "CreateFile(pipe)", GetLastError());
        return kExitGeneric;
    }
    g_worker_pipe = pipe;
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    if (!verify_server(pipe, server_pid, role)) {
        CloseHandle(pipe);
        return kExitGeneric;
    }
    say(role, "server verified; streaming");

    // Opened for SYNCHRONIZE only: the worker waits on cancellation, it never
    // signals it. Absent event is not fatal -- it only means no cancel channel.
    const HANDLE cancel = event_name.empty()
                              ? nullptr
                              : OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str());
    if (!cancel && !event_name.empty()) report_error(role, "OpenEvent(cancel)", GetLastError());

    // First record: which identity the elevated half actually runs as. The
    // worker's own console is hidden, so anything not sent here is never seen.
    char identity[512];
    snprintf(identity, sizeof identity, "I|%ls|%s|0x%04lx", current_user_sid().c_str(),
             is_elevated() ? "elevated" : "not-elevated", integrity_level(GetCurrentProcess()));
    write_record(pipe, identity);

    const int ticks = seconds * 5;
    for (int i = 0; i <= ticks; ++i) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) g_cancelled = true;
        if (g_cancelled) {
            write_record(pipe, "R|5|cancelled by the unelevated parent, nothing was changed");
            FlushFileBuffers(pipe);
            if (cancel) CloseHandle(cancel);
            CloseHandle(pipe);
            return kExitPartial;
        }
        const int percent = ticks ? (i * 100 / ticks) : 100;
        char record[128];
        snprintf(record, sizeof record, "P|%d|compacting (simulated) as an elevated worker", percent);
        if (!write_record(pipe, record)) {
            report_error(role, "WriteFile(progress)", GetLastError());
            if (cancel) CloseHandle(cancel);
            CloseHandle(pipe);
            return kExitGeneric;
        }
        Sleep(200);
    }
    write_record(pipe, "R|0|done: 1234567890 bytes reclaimed (simulated)");
    FlushFileBuffers(pipe);
    if (cancel) CloseHandle(cancel);
    CloseHandle(pipe);
    return kExitOk;
}

// --- parent (unelevated half) ------------------------------------------------

BOOL WINAPI parent_ctrl_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    printf("\n[parent] Ctrl+C -- asking the elevated worker to stop\n");
    fflush(stdout);
    g_cancelled = true;
    // SetEvent, not a pipe write: the main thread is parked in ReadFile on the
    // pipe, and a synchronous handle serializes I/O, so a write from here would
    // block behind that read instead of reaching the worker.
    if (g_cancel_event) SetEvent(g_cancel_event);
    return TRUE;  // do not let the default handler kill us; we want the worker's exit code
}

// Proves the client on the other end is our own elevated child and not a
// squatter that connected first.
void verify_client(HANDLE pipe, DWORD expected_pid) {
    const char* role = "parent";
    // Captured before impersonating: while the thread carries the client's
    // token, OpenProcessToken on ourselves is checked against *that* token and
    // comes back empty, which reads as a mismatch that is not one.
    const std::wstring own_sid = current_user_sid();
    ULONG client_pid = 0;
    if (GetNamedPipeClientProcessId(pipe, &client_pid)) {
        say(role, "client pid %lu (child pid %lu) %s", client_pid, expected_pid,
            client_pid == expected_pid ? "MATCH" : "MISMATCH");
    }
    if (!ImpersonateNamedPipeClient(pipe)) {
        report_error(role, "ImpersonateNamedPipeClient", GetLastError());
        return;
    }
    HANDLE thread_token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread_token)) {
        const std::wstring sid = token_user_sid(thread_token);
        TOKEN_ELEVATION elevation = {};
        DWORD needed = 0;
        GetTokenInformation(thread_token, TokenElevation, &elevation, sizeof elevation, &needed);
        say(role, "client sid %ls %s", sid.c_str(), sid == own_sid ? "MATCH" : "MISMATCH");
        say(role, "client token elevated: %s", elevation.TokenIsElevated ? "yes" : "no");
        CloseHandle(thread_token);
    }
    RevertToSelf();
}

int run_parent(int seconds, int cancel_after_ms) {
    const char* role = "parent";
    say(role, "elevated=%s integrity=0x%04lx sid=%ls", is_elevated() ? "yes" : "no",
        integrity_level(GetCurrentProcess()), current_user_sid().c_str());

    const std::wstring user_sid = current_user_sid();
    const std::wstring pipe_name = random_pipe_name();
    const std::wstring event_name =
        L"Local\\wsldisk-cancel-" + pipe_name.substr(pipe_name.rfind(L'-') + 1);
    say(role, "pipe %ls", pipe_name.c_str());
    say(role, "cancel event %ls", event_name.c_str());
    say(role, "dacl D:P(A;;GA;;;%ls)", user_sid.c_str());

    const HANDLE pipe =
        create_server_pipe(pipe_name, user_sid, PIPE_ACCESS_INBOUND, /*first_only=*/true);
    if (pipe == INVALID_HANDLE_VALUE) {
        report_error(role, "CreateNamedPipe", GetLastError());
        return kExitGeneric;
    }
    g_cancel_event = create_cancel_event(event_name, user_sid);
    if (!g_cancel_event) report_error(role, "CreateEvent(cancel)", GetLastError());
    SetConsoleCtrlHandler(parent_ctrl_handler, TRUE);

    wchar_t parameters[512];
    swprintf(parameters, std::size(parameters),
             L"--worker --pipe %ls --cancel-event %ls --server-pid %lu --seconds %d",
             pipe_name.c_str(), event_name.c_str(), GetCurrentProcessId(), seconds);

    const std::wstring image = own_image_path();
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = image.c_str();
    info.lpParameters = parameters;
    info.nShow = SW_HIDE;

    say(role, "ShellExecuteEx runas ...");
    if (!ShellExecuteExW(&info)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            say(role, "UAC declined (ERROR_CANCELLED 1223) -- clean exit %d", kExitNeedsElevation);
            CloseHandle(pipe);
            return kExitNeedsElevation;
        }
        report_error(role, "ShellExecuteEx", err);
        CloseHandle(pipe);
        return kExitGeneric;
    }
    const DWORD child_pid = GetProcessId(info.hProcess);
    say(role, "elevated child pid %lu, integrity 0x%04lx", child_pid,
        integrity_level(info.hProcess));

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        report_error(role, "ConnectNamedPipe", GetLastError());
        CloseHandle(pipe);
        return kExitGeneric;
    }
    say(role, "connected");

    if (cancel_after_ms > 0) {
        // Same code path the real Ctrl+C handler takes, for an unattended run.
        std::thread([cancel_after_ms] {
            Sleep(static_cast<DWORD>(cancel_after_ms));
            printf("\n[parent] simulated Ctrl+C after %d ms\n", cancel_after_ms);
            fflush(stdout);
            g_cancelled = true;
            if (g_cancel_event) SetEvent(g_cancel_event);
        }).detach();
    }

    std::string record;
    int reported = -1;
    bool verified = false;
    while (read_record(pipe, record)) {
        if (!verified) {
            // ImpersonateNamedPipeClient fails with 1368 until a message has
            // been read from the pipe, so this cannot happen at connect time.
            verify_client(pipe, child_pid);
            verified = true;
        }
        if (record.rfind("I|", 0) == 0) {
            say(role, "worker identity: %s", record.substr(2).c_str());
        } else if (record.rfind("L|", 0) == 0) {
            printf("\n");
            say(role, "worker says: %s", record.substr(2).c_str());
        } else if (record.rfind("P|", 0) == 0) {
            const int percent = atoi(record.c_str() + 2);
            if (percent != reported) {
                reported = percent;
                printf("[parent] progress %3d%%\r", percent);
                fflush(stdout);
            }
        } else if (record.rfind("R|", 0) == 0) {
            const size_t bar = record.find('|', 2);
            const int code = atoi(record.c_str() + 2);
            printf("\n");
            say(role, "result: %s (code %d)", record.substr(bar + 1).c_str(), code);
            break;
        }
    }

    WaitForSingleObject(info.hProcess, 30000);
    DWORD child_exit = 0;
    GetExitCodeProcess(info.hProcess, &child_exit);
    say(role, "worker exit code %lu", child_exit);
    CloseHandle(info.hProcess);
    if (g_cancel_event) CloseHandle(g_cancel_event);
    CloseHandle(pipe);
    return static_cast<int>(child_exit);
}

// --- squatter (the attacker in the third question) ---------------------------

int run_squatter(const std::wstring& pipe_name, int seconds) {
    const char* role = "squat";
    say(role, "creating %ls as an unprivileged process", pipe_name.c_str());
    const HANDLE pipe =
        create_server_pipe(pipe_name, current_user_sid(), PIPE_ACCESS_DUPLEX, /*first_only=*/true);
    if (pipe == INVALID_HANDLE_VALUE) {
        report_error(role, "CreateNamedPipe", GetLastError());
        return kExitGeneric;
    }
    say(role, "holding the name; waiting %d s for a victim to connect", seconds);
    if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
        say(role, "a client connected -- feeding it instructions");
        write_record(pipe, "J|attach-rw|C:\\Windows\\System32\\config\\SAM");
        std::string record;
        if (read_record(pipe, record)) say(role, "victim said: %s", record.c_str());
    }
    Sleep(static_cast<DWORD>(seconds) * 1000);
    CloseHandle(pipe);
    return kExitOk;
}

// Delivers a real CTRL_C_EVENT to another console, so the Ctrl+C path is
// measured rather than simulated.
int send_ctrl_c(DWORD pid) {
    FreeConsole();
    if (!AttachConsole(pid)) {
        report_error("ctrlc", "AttachConsole", GetLastError());
        return kExitGeneric;
    }
    SetConsoleCtrlHandler(nullptr, TRUE);  // do not kill ourselves
    const BOOL ok = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    const DWORD err = GetLastError();
    FreeConsole();
    return ok ? kExitOk : static_cast<int>(err);
}

std::wstring arg_value(int argc, wchar_t** argv, const wchar_t* name, const wchar_t* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    }
    return fallback;
}

bool has_flag(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return true;
    }
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int seconds = _wtoi(arg_value(argc, argv, L"--seconds", L"3").c_str());

    if (has_flag(argc, argv, L"--whoami")) {
        say("whoami", "elevated=%s integrity=0x%04lx sid=%ls", is_elevated() ? "yes" : "no",
            integrity_level(GetCurrentProcess()), current_user_sid().c_str());
        return kExitOk;
    }
    if (has_flag(argc, argv, L"--send-ctrl-c")) {
        return send_ctrl_c(
            static_cast<DWORD>(_wtoi(arg_value(argc, argv, L"--send-ctrl-c", L"0").c_str())));
    }
    if (has_flag(argc, argv, L"--squat")) {
        return run_squatter(arg_value(argc, argv, L"--pipe", L""), seconds);
    }
    if (has_flag(argc, argv, L"--worker")) {
        return run_worker(
            arg_value(argc, argv, L"--pipe", L""), arg_value(argc, argv, L"--cancel-event", L""),
            static_cast<DWORD>(_wtoi(arg_value(argc, argv, L"--server-pid", L"0").c_str())),
            seconds);
    }
    return run_parent(seconds, _wtoi(arg_value(argc, argv, L"--cancel-after", L"0").c_str()));
}
