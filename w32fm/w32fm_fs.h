#pragma once

#include <cstddef>
#include <string>

std::wstring fm_last_error_message(unsigned long error);
bool fm_is_regular_file(const std::wstring& path);
bool fm_is_directory(const std::wstring& path);
bool fm_cwd_writable(const std::wstring& cwd);
bool fm_copy_file(const std::wstring& src, const std::wstring& dst, std::wstring& err);
bool fm_move_file(const std::wstring& src, const std::wstring& dst, std::wstring& err);
bool fm_delete_file(const std::wstring& path, std::wstring& err);
bool fm_create_empty_file(const std::wstring& path, std::wstring& err);
bool fm_read_file_limited(const std::wstring& path, size_t max_bytes, std::string& out, std::wstring& err);
bool fm_launch_editor(const std::wstring& path, std::wstring& err);
std::wstring fm_join_path(const std::wstring& base, const std::wstring& name);
bool fm_get_canonical_path(const std::wstring& input, std::wstring& out, std::wstring& err);
bool fm_is_root_path(const std::wstring& path);
bool fm_parent_path(const std::wstring& path, std::wstring& out_parent);
bool fm_is_valid_filename(const std::wstring& name, std::wstring& reason);
