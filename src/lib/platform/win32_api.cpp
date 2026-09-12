#include "win32_api.h"

#include <utility>

namespace wsldisk::platform {
namespace {

/// The table currently installed by a `ScopedWin32Api`, or null for the real one.
///
/// A function-local static rather than a namespace-scope variable: it has no
/// static initialisation order to get wrong, and mutable global state is
/// something to keep deliberately narrow.
const Win32Api*& active_table() {
    static const Win32Api* table = nullptr;
    return table;
}

}  // namespace

const Win32Api& real_win32_api() {
    static const Win32Api table{
        .get_compressed_file_size =
            [](LPCWSTR file_name, LPDWORD file_size_high) {
                return ::GetCompressedFileSizeW(file_name, file_size_high);
            },
        .get_file_attributes = [](LPCWSTR file_name) { return ::GetFileAttributesW(file_name); },
        .get_file_attributes_ex =
            [](LPCWSTR file_name, GET_FILEEX_INFO_LEVELS info_level_id, LPVOID file_information) {
                return ::GetFileAttributesExW(file_name, info_level_id, file_information);
            },
        .get_disk_free_space_ex =
            [](LPCWSTR directory_name, PULARGE_INTEGER free_to_caller, PULARGE_INTEGER total_bytes,
               PULARGE_INTEGER total_free) {
                return ::GetDiskFreeSpaceExW(directory_name, free_to_caller, total_bytes, total_free);
            },
        .get_volume_information =
            [](LPCWSTR root_path_name, LPWSTR volume_name_buffer, DWORD volume_name_size,
               LPDWORD volume_serial_number, LPDWORD maximum_component_length, LPDWORD file_system_flags,
               LPWSTR file_system_name_buffer, DWORD file_system_name_size) {
                return ::GetVolumeInformationW(root_path_name, volume_name_buffer, volume_name_size,
                                               volume_serial_number, maximum_component_length,
                                               file_system_flags, file_system_name_buffer,
                                               file_system_name_size);
            },
        .get_volume_path_name =
            [](LPCWSTR file_name, LPWSTR volume_path_name, DWORD buffer_length) {
                return ::GetVolumePathNameW(file_name, volume_path_name, buffer_length);
            },
        .get_last_error = []() { return ::GetLastError(); },
        .find_first_file_ex =
            [](LPCWSTR file_name, FINDEX_INFO_LEVELS info_level_id, LPVOID find_file_data,
               FINDEX_SEARCH_OPS search_op, LPVOID search_filter, DWORD additional_flags) {
                return ::FindFirstFileExW(file_name, info_level_id, find_file_data, search_op, search_filter,
                                          additional_flags);
            },
        .find_next_file =
            [](HANDLE find_file, LPWIN32_FIND_DATAW find_file_data) {
                return ::FindNextFileW(find_file, find_file_data);
            },
        .find_close = [](HANDLE find_file) { return ::FindClose(find_file); },
        .create_file =
            [](LPCWSTR file_name, DWORD desired_access, DWORD share_mode,
               LPSECURITY_ATTRIBUTES security_attributes, DWORD creation_disposition,
               DWORD flags_and_attributes, HANDLE template_file) {
                return ::CreateFileW(file_name, desired_access, share_mode, security_attributes,
                                     creation_disposition, flags_and_attributes, template_file);
            },
        .device_io_control =
            [](HANDLE device, DWORD control_code, LPVOID in_buffer, DWORD in_size, LPVOID out_buffer,
               DWORD out_size, LPDWORD bytes_returned, LPOVERLAPPED overlapped) {
                return ::DeviceIoControl(device, control_code, in_buffer, in_size, out_buffer, out_size,
                                         bytes_returned, overlapped);
            },
        .delete_file = [](LPCWSTR file_name) { return ::DeleteFileW(file_name); },
        .expand_environment_strings =
            [](LPCWSTR source, LPWSTR destination, DWORD size) {
                return ::ExpandEnvironmentStringsW(source, destination, size);
            },
        .sleep = [](DWORD milliseconds) { ::Sleep(milliseconds); },
        .reg_open_key_ex =
            [](HKEY key, LPCWSTR sub_key, DWORD options, REGSAM desired, PHKEY result) {
                return ::RegOpenKeyExW(key, sub_key, options, desired, result);
            },
        .reg_close_key = [](HKEY key) { return ::RegCloseKey(key); },
        .reg_enum_key_ex =
            [](HKEY key, DWORD index, LPWSTR name, LPDWORD name_length, LPDWORD reserved, LPWSTR class_name,
               LPDWORD class_length, PFILETIME last_write_time) {
                return ::RegEnumKeyExW(key, index, name, name_length, reserved, class_name, class_length,
                                       last_write_time);
            },
        .reg_query_value_ex =
            [](HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type, LPBYTE data,
               LPDWORD data_length) {
                return ::RegQueryValueExW(key, value_name, reserved, type, data, data_length);
            },
        .reg_set_value_ex =
            [](HKEY key, LPCWSTR value_name, DWORD reserved, DWORD type, const BYTE* data,
               DWORD data_length) {
                return ::RegSetValueExW(key, value_name, reserved, type, data, data_length);
            },
        .open_virtual_disk =
            [](PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
               OPEN_VIRTUAL_DISK_FLAG flags, POPEN_VIRTUAL_DISK_PARAMETERS parameters, PHANDLE handle) {
                return ::OpenVirtualDisk(storage_type, path, access_mask, flags, parameters, handle);
            },
        .get_virtual_disk_information =
            [](HANDLE handle, GET_VIRTUAL_DISK_INFO_VERSION version, PULONG size, PGET_VIRTUAL_DISK_INFO info,
               PULONG used_size) {
                // GetVirtualDiskInformation reads the version out of the struct.
                // Taking it as a parameter keeps the table signature explicit
                // about what is being asked for.
                info->Version = version;
                // size and used_size are in the order the API declares them.
                // NOLINTNEXTLINE(readability-suspicious-call-argument)
                return ::GetVirtualDiskInformation(handle, size, info, used_size);
            },
        .compact_virtual_disk =
            [](HANDLE handle, COMPACT_VIRTUAL_DISK_FLAG flags, PCOMPACT_VIRTUAL_DISK_PARAMETERS parameters,
               LPOVERLAPPED overlapped) {
                return ::CompactVirtualDisk(handle, flags, parameters, overlapped);
            },
        .get_virtual_disk_operation_progress =
            [](HANDLE handle, LPOVERLAPPED overlapped, PVIRTUAL_DISK_PROGRESS progress) {
                return ::GetVirtualDiskOperationProgress(handle, overlapped, progress);
            },
        .attach_virtual_disk =
            [](HANDLE handle, PSECURITY_DESCRIPTOR security_descriptor, ATTACH_VIRTUAL_DISK_FLAG flags,
               ULONG provider_flags, PATTACH_VIRTUAL_DISK_PARAMETERS parameters, LPOVERLAPPED overlapped) {
                return ::AttachVirtualDisk(handle, security_descriptor, flags, provider_flags, parameters,
                                           overlapped);
            },
        .detach_virtual_disk =
            [](HANDLE handle, DETACH_VIRTUAL_DISK_FLAG flags, ULONG provider_flags) {
                return ::DetachVirtualDisk(handle, flags, provider_flags);
            },
        .create_virtual_disk =
            [](PVIRTUAL_STORAGE_TYPE storage_type, PCWSTR path, VIRTUAL_DISK_ACCESS_MASK access_mask,
               PSECURITY_DESCRIPTOR security_descriptor, CREATE_VIRTUAL_DISK_FLAG flags, ULONG provider_flags,
               PCREATE_VIRTUAL_DISK_PARAMETERS parameters, LPOVERLAPPED overlapped, PHANDLE handle) {
                return ::CreateVirtualDisk(storage_type, path, access_mask, security_descriptor, flags,
                                           provider_flags, parameters, overlapped, handle);
            },
        .create_process =
            [](LPCWSTR application_name, LPWSTR command_line, LPSECURITY_ATTRIBUTES process_attributes,
               LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit_handles, DWORD creation_flags,
               LPVOID environment, LPCWSTR current_directory, LPSTARTUPINFOW startup_info,
               LPPROCESS_INFORMATION process_information) {
                return ::CreateProcessW(application_name, command_line, process_attributes, thread_attributes,
                                        inherit_handles, creation_flags, environment, current_directory,
                                        startup_info, process_information);
            },
        .create_pipe = [](PHANDLE read_pipe, PHANDLE write_pipe, LPSECURITY_ATTRIBUTES attributes,
                          DWORD size) { return ::CreatePipe(read_pipe, write_pipe, attributes, size); },
        .set_handle_information = [](HANDLE object, DWORD mask,
                                     DWORD flags) { return ::SetHandleInformation(object, mask, flags); },
        .read_file =
            [](HANDLE file, LPVOID buffer, DWORD to_read, LPDWORD read, LPOVERLAPPED overlapped) {
                return ::ReadFile(file, buffer, to_read, read, overlapped);
            },
        .write_file =
            [](HANDLE file, LPCVOID buffer, DWORD to_write, LPDWORD written, LPOVERLAPPED overlapped) {
                return ::WriteFile(file, buffer, to_write, written, overlapped);
            },
        .set_file_pointer_ex =
            [](HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER new_pointer, DWORD method) {
                return ::SetFilePointerEx(file, distance, new_pointer, method);
            },
        .set_end_of_file = [](HANDLE file) { return ::SetEndOfFile(file); },
        .move_file_ex = [](LPCWSTR existing, LPCWSTR replacement,
                           DWORD flags) { return ::MoveFileExW(existing, replacement, flags); },
        .create_directory =
            [](LPCWSTR path, LPSECURITY_ATTRIBUTES attributes) {
                return ::CreateDirectoryW(path, attributes);
            },
        .peek_named_pipe =
            [](HANDLE pipe, LPVOID buffer, DWORD buffer_size, LPDWORD read, LPDWORD total_available,
               LPDWORD left_this_message) {
                return ::PeekNamedPipe(pipe, buffer, buffer_size, read, total_available, left_this_message);
            },
        .get_exit_code_process = [](HANDLE process,
                                    LPDWORD exit_code) { return ::GetExitCodeProcess(process, exit_code); },
        .terminate_process = [](HANDLE process,
                                UINT exit_code) { return ::TerminateProcess(process, exit_code); },
        .close_handle = [](HANDLE handle) { return ::CloseHandle(handle); },
        .create_event =
            [](LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state, LPCWSTR name) {
                return ::CreateEventW(attributes, manual_reset, initial_state, name);
            },
        .wait_for_single_object =
            [](HANDLE handle, DWORD milliseconds) { return ::WaitForSingleObject(handle, milliseconds); },
        .cancel_io_ex = [](HANDLE handle,
                           LPOVERLAPPED overlapped) { return ::CancelIoEx(handle, overlapped); },
        .set_event = [](HANDLE handle) { return ::SetEvent(handle); },
        .get_process_id = [](HANDLE process) { return ::GetProcessId(process); },
        .allocate_and_initialize_sid =
            [](PSID_IDENTIFIER_AUTHORITY authority, BYTE sub_authority_count, DWORD sub_authority0,
               DWORD sub_authority1, DWORD sub_authority2, DWORD sub_authority3, DWORD sub_authority4,
               DWORD sub_authority5, DWORD sub_authority6, DWORD sub_authority7, PSID* sid) {
                return ::AllocateAndInitializeSid(authority, sub_authority_count, sub_authority0,
                                                  sub_authority1, sub_authority2, sub_authority3,
                                                  sub_authority4, sub_authority5, sub_authority6,
                                                  sub_authority7, sid);
            },
        .free_sid = [](PSID sid) { return ::FreeSid(sid); },
        .check_token_membership =
            [](HANDLE token, PSID group, PBOOL is_member) {
                return ::CheckTokenMembership(token, group, is_member);
            },
        .shell_execute_ex = [](LPSHELLEXECUTEINFOW info) { return ::ShellExecuteExW(info); },
        .create_named_pipe =
            [](LPCWSTR name, DWORD open_mode, DWORD pipe_mode, DWORD max_instances, DWORD out_buffer_size,
               DWORD in_buffer_size, DWORD default_timeout, LPSECURITY_ATTRIBUTES security_attributes) {
                return ::CreateNamedPipeW(name, open_mode, pipe_mode, max_instances, out_buffer_size,
                                          in_buffer_size, default_timeout, security_attributes);
            },
        .connect_named_pipe = [](HANDLE pipe,
                                 LPOVERLAPPED overlapped) { return ::ConnectNamedPipe(pipe, overlapped); },
        .get_named_pipe_client_process_id =
            [](HANDLE pipe, PULONG process_id) { return ::GetNamedPipeClientProcessId(pipe, process_id); },
        .convert_string_sd_to_sd =
            [](LPCWSTR string_security_descriptor, DWORD revision,
               PSECURITY_DESCRIPTOR* security_descriptor, PULONG size) {
                return ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    string_security_descriptor, revision, security_descriptor, size);
            },
        .local_free = [](HLOCAL memory) { return ::LocalFree(memory); },
        .bcrypt_gen_random =
            [](PVOID algorithm, PUCHAR buffer, ULONG size, ULONG flags) {
                return ::BCryptGenRandom(algorithm, buffer, size, flags);
            },
    };
    return table;
}

const Win32Api& win32() {
    const Win32Api* const active = active_table();
    return active != nullptr ? *active : real_win32_api();
}

ScopedWin32Api::ScopedWin32Api(Win32Api replacement)
    : previous_(active_table()), replacement_(std::move(replacement)) {
    active_table() = &replacement_;
}

ScopedWin32Api::~ScopedWin32Api() {
    active_table() = previous_;
}

}  // namespace wsldisk::platform
