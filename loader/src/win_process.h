// Shared Windows process/lock primitives for play, setup and self-update.
#pragma once

#include <windows.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace loader::win {

class Handle {
 public:
  explicit Handle(HANDLE handle = nullptr) : value_(handle) {}
  ~Handle() { Close(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
  HANDLE Get() const { return value_; }
  void Close() {
    if (*this) CloseHandle(value_);
    value_ = nullptr;
  }
  void Set(HANDLE handle) { Close(); value_ = handle; }
 private:
  HANDLE value_;
};

inline HANDLE OpenSetupLock(const std::filesystem::path& file, bool inherit = false) {
  SECURITY_ATTRIBUTES attrs = {sizeof(attrs), nullptr, TRUE};
  const HANDLE lock = CreateFileW(file.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, inherit ? &attrs : nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (lock == INVALID_HANDLE_VALUE) return lock;
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileInformationByHandle(lock, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return lock;
  CloseHandle(lock);
  SetLastError(ERROR_ACCESS_DENIED);
  return INVALID_HANDLE_VALUE;
}

// Quote argv for CommandLineToArgvW, including paths ending in a backslash and empty values.
inline std::wstring CommandLine(std::span<const std::wstring> args) {
  std::wstring command;
  for (const auto& arg : args) {
    if (!command.empty()) command += L' ';
    command += L'"';
    size_t slashes = 0;
    for (const wchar_t c : arg) {
      if (c == L'\\') { ++slashes; continue; }
      command.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      slashes = 0;
      command += c;
    }
    command.append(slashes * 2, L'\\');
    command += L'"';
  }
  return command;
}

// The child inherits only the nominated handles, not CRT file handles in another worker.
inline bool Spawn(const std::filesystem::path& exe, std::wstring command,
                  const std::filesystem::path& cwd, DWORD flags, Handle* process = nullptr,
                  std::span<HANDLE> handles = {}) {
  PROCESS_INFORMATION pi{};
  STARTUPINFOEXW si{};
  std::vector<char> buffer;
  if (!handles.empty()) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    buffer.resize(size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) return false;
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles.data(), handles.size_bytes(), nullptr, nullptr)) {
      DeleteProcThreadAttributeList(si.lpAttributeList);
      return false;
    }
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }
  si.StartupInfo.cb = handles.empty() ? sizeof(STARTUPINFOW) : sizeof(si);
  const bool ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, !handles.empty(), flags,
                                 nullptr, cwd.c_str(), &si.StartupInfo, &pi) != 0;
  if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
  if (ok) {
    CloseHandle(pi.hThread);
    if (process) process->Set(pi.hProcess);
    else CloseHandle(pi.hProcess);
  }
  return ok;
}

}  // namespace loader::win
