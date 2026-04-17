#define UNICODE
#define _UNICODE

#include "w32fm_fs.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <vector>

namespace {
constexpr wchar_t kInvalidNameChars[] = L"<>:\"/\\|?*";
}

std::wstring fm_last_error_message(unsigned long error) {
    LPWSTR msg = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (len == 0 || !msg) {
        return L"Windows error " + std::to_wstring(error);
    }
    std::wstring out(msg, len);
    LocalFree(msg);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ' || out.back() == L'\t')) {
        out.pop_back();
    }
    return out;
}

bool fm_is_regular_file(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool fm_is_directory(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring fm_join_path(const std::wstring& base, const std::wstring& name) {
    if (base.empty()) return name;
    if (base.back() == L'\\' || base.back() == L'/') return base + name;
    return base + L"\\" + name;
}

bool fm_get_canonical_path(const std::wstring& input, std::wstring& out, std::wstring& err) {
    DWORD need = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (need == 0) {
        err = fm_last_error_message(GetLastError());
        return false;
    }
    std::vector<wchar_t> buf(need + 1);
    DWORD got = GetFullPathNameW(input.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (got == 0 || got >= buf.size()) {
        err = fm_last_error_message(GetLastError());
        return false;
    }
    out.assign(buf.data());
    return true;
}

bool fm_is_root_path(const std::wstring& path) {
    if (path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        size_t p = 2;
        size_t s1 = path.find(L'\\', p);
        if (s1 == std::wstring::npos) return false;
        size_t s2 = path.find(L'\\', s1 + 1);
        if (s2 == std::wstring::npos) return true;
        return s2 == path.size() - 1;
    }
    return false;
}

bool fm_parent_path(const std::wstring& path, std::wstring& out_parent) {
    if (fm_is_root_path(path)) return false;
    std::wstring tmp = path;
    while (!tmp.empty() && (tmp.back() == L'\\' || tmp.back() == L'/')) {
        if (tmp.size() == 3 && tmp[1] == L':') break;
        tmp.pop_back();
    }
    size_t pos = tmp.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return false;
    out_parent = tmp.substr(0, pos + 1);
    if (!out_parent.empty() && out_parent.size() > 3 && out_parent.back() == L'\\') {
        if (!fm_is_root_path(out_parent)) out_parent.pop_back();
    }
    return !out_parent.empty();
}

bool fm_cwd_writable(const std::wstring& cwd) {
    std::wstring probe = fm_join_path(cwd, L".w32fm_write_probe.tmp");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

bool fm_copy_file(const std::wstring& src, const std::wstring& dst, std::wstring& err) {
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    err = fm_last_error_message(GetLastError());
    return false;
}

bool fm_move_file(const std::wstring& src, const std::wstring& dst, std::wstring& err) {
    DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING;
    if (MoveFileExW(src.c_str(), dst.c_str(), flags)) return true;
    err = fm_last_error_message(GetLastError());
    return false;
}

bool fm_delete_file(const std::wstring& path, std::wstring& err) {
    if (DeleteFileW(path.c_str())) return true;
    err = fm_last_error_message(GetLastError());
    return false;
}

bool fm_create_empty_file(const std::wstring& path, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = fm_last_error_message(GetLastError());
        return false;
    }
    CloseHandle(h);
    return true;
}

bool fm_read_file_limited(const std::wstring& path, size_t max_bytes, std::string& out, std::wstring& err) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = fm_last_error_message(GetLastError());
        return false;
    }

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        err = fm_last_error_message(GetLastError());
        CloseHandle(h);
        return false;
    }
    if (sz.QuadPart < 0 || static_cast<unsigned long long>(sz.QuadPart) > max_bytes) {
        err = L"File too large to view.";
        CloseHandle(h);
        return false;
    }

    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    if (!out.empty() && !ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
        err = fm_last_error_message(GetLastError());
        CloseHandle(h);
        out.clear();
        return false;
    }
    if (read < out.size()) out.resize(read);
    CloseHandle(h);
    return true;
}

bool fm_launch_editor(const std::wstring& path, std::wstring& err) {
    wchar_t editor[512]{};
    DWORD got = GetEnvironmentVariableW(L"VISUAL", editor, static_cast<DWORD>(std::size(editor)));
    if (got == 0 || got >= std::size(editor)) {
        editor[0] = L'\0';
        got = GetEnvironmentVariableW(L"EDITOR", editor, static_cast<DWORD>(std::size(editor)));
        if (got == 0 || got >= std::size(editor)) editor[0] = L'\0';
    }

    std::wstring cmd = (editor[0] != L'\0') ? std::wstring(editor) : L"edit";
    std::wstring command_line = L"cmd.exe /c " + cmd + L" \"" + path + L"\"";
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi);
    if (!ok) {
        err = fm_last_error_message(GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0) {
        err = L"Editor exited with code " + std::to_wstring(exit_code) + L".";
        return false;
    }
    return true;
}

bool fm_is_valid_filename(const std::wstring& name, std::wstring& reason) {
    if (name.empty()) {
        reason = L"Filename cannot be empty.";
        return false;
    }
    if (name == L"." || name == L"..") {
        reason = L"Invalid filename.";
        return false;
    }
    if (name.find_first_of(kInvalidNameChars) != std::wstring::npos) {
        reason = L"Filename has invalid characters.";
        return false;
    }
    if (name.back() == L' ' || name.back() == L'.') {
        reason = L"Filename cannot end with space or dot.";
        return false;
    }
    return true;
}
