#pragma once

#include <windows.h>
#include <virtdisk.h>

#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#include <functional>

namespace wsldisk::platform {

/// Indirection table for every Win32 function the platform layer calls.
///
/// Two reasons it exists: `platform/` stays the only place that includes
/// `<windows.h>`, and tests can swap the table to make any call fail with any
/// error code. That is what makes the 100% branch-coverage gate in
/// docs/TESTING.md reachable -- error paths that need a locked file, a full
/// volume or a denied ACL are exercised by injection instead of by fixture
/// gymnastics.
///
/// Signatures mirror the Win32 ones exactly so the real table is a plain
/// forwarding wrapper with no translation logic to get wrong.
struct Win32Api {
    std::function<DWORD(LPCWSTR file_name, LPDWORD file_size_high)> get_compressed_file_size;
    std::function<DWORD(LPCWSTR file_name)> get_file_attributes;
    std::function<BOOL(LPCWSTR file_name, GET_FILEEX_INFO_LEVELS info_level_id, LPVOID file_information)>
        get_file_attributes_ex;
    std::function<BOOL(LPCWSTR directory_name, PULARGE_INTEGER free_bytes_available_to_caller,
                       PULARGE_INTEGER total_number_of_bytes, PULARGE_INTEGER total_number_of_free_bytes)>
        get_disk_free_space_ex;
    std::function<BOOL(LPCWSTR root_path_name, LPWSTR volume_name_buffer, DWORD volume_name_size,
                       LPDWORD volume_serial_number, LPDWORD maximum_component_length,
                       LPDWORD file_system_flags, LPWSTR file_system_name_buffer,
                       DWORD file_system_name_size)>
        get_volume_information;
    std::function<BOOL(LPCWSTR file_name, LPWSTR volume_path_name, DWORD buffer_length)> get_volume_path_name;
    std::function<DWORD()> get_last_error;

    // Directory enumeration, file deletion and the ioctl behind sparse-range
    // queries.
    std::function<HANDLE(LPCWSTR file_name, FINDEX_INFO_LEVELS info_level_id, LPVOID find_file_data,
                         FINDEX_SEARCH_OPS search_op, LPVOID search_filter, DWORD additional_flags)>
        find_first_file_ex;
    std::function<BOOL(HANDLE find_file, LPWIN32_FIND_DATAW find_file_data)> find_next_file;
    std::function<BOOL(HANDLE find_file)> find_close;
    std::function<HANDLE(LPCWSTR file_name, DWORD desired_access, DWORD share_mode,
                         LPSECURITY_ATTRIBUTES security_attributes, DWORD creation_disposition,
                         DWORD flags_and_attributes, HANDLE template_file)>
        create_file;
    std::function<BOOL(HANDLE device, DWORD control_code, LPVOID in_buffer, DWORD in_size, LPVOID out_buffer,
                       DWORD out_size, LPDWORD bytes_returned, LPOVERLAPPED overlapped)>
        device_io_control;
    std::function<BOOL(LPCWSTR file_name)> delete_file;
    std::function<DWORD(LPCWSTR source, LPWSTR destination, DWORD size)> expand_environment_strings;
    std::function<void(DWORD milliseconds)> sleep;

    // Registry. These return an LSTATUS directly rather than setting the last
    // error, so their wrappers must not consult get_last_error.
    std::function<LSTATUS(HKEY key, LPCWSTR sub_key, DWORD options, REGSAM desired, PHKEY result)>
        reg_open_key_ex;
    std::function<LSTATUS(HKEY key)> reg_close_key;
    std::function<LSTATUS(HKEY key, DWORD index, LPWSTR name, LPDWORD name_length, LPDWORD reserved,
                          LPWSTR class_name, LPDWORD class_length, PFILETIME last_write_time)>
        reg_enum_key_ex;
    std::function<LSTATUS(HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                          LPDWORD data_length)>
        reg_query_value_ex;
    std::function<LSTATUS(HKEY key, LPCWSTR value_name, DWORD reserved, DWORD type, const BYTE* data,
                          DWORD data_length)>
        reg_set_value_ex;

    // Virtual Disk Service. Like the registry calls, these return an error code
    // rather than setting the last error.
    std::function<DWORD(PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
                        OPEN_VIRTUAL_DISK_FLAG flags, POPEN_VIRTUAL_DISK_PARAMETERS parameters,
                        PHANDLE handle)>
        open_virtual_disk;
    std::function<DWORD(HANDLE handle, GET_VIRTUAL_DISK_INFO_VERSION version, PULONG size,
                        PGET_VIRTUAL_DISK_INFO info, PULONG used_size)>
        get_virtual_disk_information;
    std::function<DWORD(HANDLE handle, COMPACT_VIRTUAL_DISK_FLAG flags,
                        PCOMPACT_VIRTUAL_DISK_PARAMETERS parameters, LPOVERLAPPED overlapped)>
        compact_virtual_disk;
    std::function<DWORD(HANDLE handle, LPOVERLAPPED overlapped, PVIRTUAL_DISK_PROGRESS progress)>
        get_virtual_disk_operation_progress;
    // Attach and detach. Only the elevated worker calls these: attaching needs
    // an administrator token, which the ordinary compaction path does not (D10).
    std::function<DWORD(HANDLE handle, PSECURITY_DESCRIPTOR security_descriptor,
                        ATTACH_VIRTUAL_DISK_FLAG flags, ULONG provider_flags,
                        PATTACH_VIRTUAL_DISK_PARAMETERS parameters, LPOVERLAPPED overlapped)>
        attach_virtual_disk;
    std::function<DWORD(HANDLE handle, DETACH_VIRTUAL_DISK_FLAG flags, ULONG provider_flags)>
        detach_virtual_disk;
    std::function<DWORD(PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
                        PSECURITY_DESCRIPTOR security_descriptor, CREATE_VIRTUAL_DISK_FLAG flags,
                        ULONG provider_flags, PCREATE_VIRTUAL_DISK_PARAMETERS parameters,
                        LPOVERLAPPED overlapped, PHANDLE handle)>
        create_virtual_disk;

    // Process creation and pipe reads, for the wsl.exe wrapper.
    std::function<BOOL(LPCWSTR application_name, LPWSTR command_line,
                       LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
                       BOOL inherit_handles, DWORD creation_flags, LPVOID environment,
                       LPCWSTR current_directory, LPSTARTUPINFOW startup_info,
                       LPPROCESS_INFORMATION process_information)>
        create_process;
    std::function<BOOL(PHANDLE read_pipe, PHANDLE write_pipe, LPSECURITY_ATTRIBUTES attributes, DWORD size)>
        create_pipe;
    std::function<BOOL(HANDLE object, DWORD mask, DWORD flags)> set_handle_information;
    std::function<BOOL(HANDLE file, LPVOID buffer, DWORD to_read, LPDWORD read, LPOVERLAPPED overlapped)>
        read_file;
    std::function<BOOL(HANDLE file, LPCVOID buffer, DWORD to_write, LPDWORD written, LPOVERLAPPED overlapped)>
        write_file;
    std::function<BOOL(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER new_pointer, DWORD method)>
        set_file_pointer_ex;
    std::function<BOOL(HANDLE file)> set_end_of_file;
    /// `MoveFileExW`, for the same-volume move that needs no copy at all.
    std::function<BOOL(LPCWSTR existing, LPCWSTR replacement, DWORD flags)> move_file_ex;
    std::function<BOOL(LPCWSTR path, LPSECURITY_ATTRIBUTES attributes)> create_directory;
    std::function<BOOL(HANDLE pipe, LPVOID buffer, DWORD buffer_size, LPDWORD read, LPDWORD total_available,
                       LPDWORD left_this_message)>
        peek_named_pipe;
    std::function<BOOL(HANDLE process, LPDWORD exit_code)> get_exit_code_process;
    std::function<BOOL(HANDLE process, UINT exit_code)> terminate_process;

    std::function<BOOL(HANDLE handle)> close_handle;
    std::function<HANDLE(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state,
                         LPCWSTR name)>
        create_event;
    std::function<DWORD(HANDLE handle, DWORD milliseconds)> wait_for_single_object;
    std::function<BOOL(HANDLE handle, LPOVERLAPPED overlapped)> cancel_io_ex;
    std::function<BOOL(HANDLE handle)> set_event;
    std::function<DWORD(HANDLE process)> get_process_id;

    // Elevation: asking whether we hold an administrator token, and borrowing
    // one for a single verb. Only the `--elevate` path uses these.
    std::function<BOOL(PSID_IDENTIFIER_AUTHORITY authority, BYTE sub_authority_count, DWORD sub_authority0,
                       DWORD sub_authority1, DWORD sub_authority2, DWORD sub_authority3,
                       DWORD sub_authority4, DWORD sub_authority5, DWORD sub_authority6,
                       DWORD sub_authority7, PSID* sid)>
        allocate_and_initialize_sid;
    std::function<PVOID(PSID sid)> free_sid;
    std::function<BOOL(HANDLE token, PSID group, PBOOL is_member)> check_token_membership;
    std::function<BOOL(LPSHELLEXECUTEINFOW info)> shell_execute_ex;

    // The result channel: a one-way pipe the worker writes and the parent reads.
    // Not duplex -- I/O on a synchronous handle is serialized, so a pending read
    // would block the write the other direction needs (docs/RESEARCH.md).
    std::function<HANDLE(LPCWSTR name, DWORD open_mode, DWORD pipe_mode, DWORD max_instances,
                         DWORD out_buffer_size, DWORD in_buffer_size, DWORD default_timeout,
                         LPSECURITY_ATTRIBUTES security_attributes)>
        create_named_pipe;
    std::function<BOOL(HANDLE pipe, LPOVERLAPPED overlapped)> connect_named_pipe;
    std::function<BOOL(HANDLE pipe, PULONG process_id)> get_named_pipe_client_process_id;
    std::function<BOOL(LPCWSTR string_security_descriptor, DWORD revision,
                       PSECURITY_DESCRIPTOR* security_descriptor, PULONG size)>
        convert_string_sd_to_sd;
    std::function<HLOCAL(HLOCAL memory)> local_free;
    std::function<NTSTATUS(PVOID algorithm, PUCHAR buffer, ULONG size, ULONG flags)> bcrypt_gen_random;
};

/// The table that forwards to the real Win32 entry points.
[[nodiscard]] const Win32Api& real_win32_api();

/// The table the platform layer is currently using -- the real one unless a
/// `ScopedWin32Api` is in scope on this thread.
[[nodiscard]] const Win32Api& win32();

/// Installs a replacement table for the lifetime of the object. Test-only, and
/// deliberately not thread-safe: the override is process-wide, so tests that use
/// it must not run concurrently with other tests that touch `platform/`.
class ScopedWin32Api {
public:
    explicit ScopedWin32Api(Win32Api replacement);
    ~ScopedWin32Api();

    ScopedWin32Api(const ScopedWin32Api&) = delete;
    ScopedWin32Api& operator=(const ScopedWin32Api&) = delete;
    ScopedWin32Api(ScopedWin32Api&&) = delete;
    ScopedWin32Api& operator=(ScopedWin32Api&&) = delete;

private:
    const Win32Api* previous_;
    Win32Api replacement_;
};

}  // namespace wsldisk::platform
