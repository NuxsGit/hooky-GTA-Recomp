// gtav launcher - first-run install (see install.h).
//
// Layout next to the launcher (root):
//   gtav_fast.exe, rexruntimerd.dll, ...   shipped: our runtime, renderer and hooks
//   rexgluerd.exe                          shipped: ReXGlue codegen (SDK tool)
//   tools\llvm\                            shipped: clang + lld (LLVM, Apache-2.0)
//   tools\game_dll\                        shipped: freestanding header, interface lists, manifest,
//                                          expected hashes (names, addresses and hashes only)
//   gtav_game.dll                          made here: the player's recompiled game code
//   data\game\                             made here: Disc 2 files + the TU's default.xexp
//   data\update\update.rpf                 made here: from the TU
//   data\user\...\545408A7\00000002\       made here: Disc 1 install partitions
//   data\spirv\                            filled by the game: shaders translated on first use
// Linux: gtav_fast, librexruntime.so, rexglue, tools/llvm/bin/{clang++,ld.lld}, gtav_game.so; the
// same data/ layout.
// macOS: the package (root) is GTAV.app/Contents/Resources/gtav and stays read-only (the app is
// signed); everything made here goes to the game folder (home): gtav_game.dylib, data/, plus a copy
// of the runtime (gtav_fast, dylibs, shader tools) so gtav_fast finds gtav_game.dylib next to it.
#include "install.h"
#include "setup_lock.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include "win_process.h"
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#endif
// The iPhone app's setup screen uses the copy stages only (RunDataInstall): iOS cannot start
// processes, so nothing here that spawns one (codegen, clang, the game) runs there.
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
// Android (the APK's setup screen, libgtav_setup.so): RunDataInstall only, like the iPhone; bionic
// also lacks posix_spawn_file_actions_addchdir_np before API 34.
#define LOADER_NO_SPAWN 1
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include "sources.h"
#include "../../gtav/src/native/adreno_shader_fix.h"

namespace fs = std::filesystem;

namespace loader {
namespace {

#ifdef _WIN32
constexpr wchar_t kInstalledMarker[] = L"data\\installed.txt";
constexpr wchar_t kGameExe[] = L"gtav_fast.exe";
constexpr wchar_t kGameLib[] = L"gtav_game.dll";
constexpr wchar_t kCodegenExe[] = L"rexgluerd.exe";
constexpr wchar_t kClang[] = L"clang++.exe";
constexpr wchar_t kObjExt[] = L".obj";
#elif defined(__APPLE__)
constexpr wchar_t kInstalledMarker[] = L"data/installed.txt";
constexpr wchar_t kGameExe[] = L"gtav_fast";
constexpr wchar_t kGameLib[] = L"gtav_game.dylib";
constexpr wchar_t kCodegenExe[] = L"rexglue";
constexpr wchar_t kClang[] = L"clang++";
constexpr wchar_t kObjExt[] = L".o";
#else
constexpr wchar_t kInstalledMarker[] = L"data/installed.txt";
constexpr wchar_t kGameExe[] = L"gtav_fast";
constexpr wchar_t kGameLib[] = L"gtav_game.so";
constexpr wchar_t kCodegenExe[] = L"rexglue";
constexpr wchar_t kClang[] = L"clang++";
constexpr wchar_t kObjExt[] = L".o";
#endif
constexpr char kInstallVersion[] = "gtav-recomp install 1";
constexpr char kTitleId[] = "545408A7";
constexpr char kInstallPackageDir[] = "content/0000000000000000/545408A7/00000002/";

#ifdef __APPLE__
bool SyncRuntime(const fs::path& root, const fs::path& home, std::wstring* error);
#endif

//-----------------------------------------------------------------------------
// Small helpers
//-----------------------------------------------------------------------------

std::string NarrowGeneric(const fs::path& p) {
  auto utf8 = p.generic_u8string();
  return std::string(utf8.begin(), utf8.end());
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::wstring Num(uint64_t v) { return std::to_wstring(v); }

std::wstring Gb(uint64_t bytes) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%.1f GB", double(bytes) / (1u << 30));
  return buf;
}

#ifdef _WIN32
class Sha256 {
 public:
  Sha256() {
    ok_ = BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
          BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0;
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  void Update(const uint8_t* data, size_t size) {
    while (size && ok_) {
      ULONG n = ULONG(std::min<size_t>(size, 1u << 30));
      ok_ = BCryptHashData(hash_, const_cast<PUCHAR>(data), n, 0) >= 0;
      data += n;
      size -= n;
    }
  }
  std::string Hex() {
    uint8_t digest[32];
    if (!ok_ || BCryptFinishHash(hash_, digest, 32, 0) < 0) return {};
    static const char* k = "0123456789abcdef";
    std::string s;
    for (uint8_t b : digest) {
      s += k[b >> 4];
      s += k[b & 15];
    }
    return s;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  bool ok_ = false;
};
#else
// FIPS 180-4 SHA-256 (no crypto library on the player's machine is assumed).
class Sha256 {
 public:
  void Update(const uint8_t* data, size_t size) {
    total_ += size;
    if (fill_) {
      size_t n = std::min(size, sizeof(buf_) - fill_);
      memcpy(buf_ + fill_, data, n);
      fill_ += n;
      data += n;
      size -= n;
      if (fill_ < sizeof(buf_)) return;
      Block(buf_);
      fill_ = 0;
    }
    for (; size >= 64; data += 64, size -= 64) Block(data);
    memcpy(buf_, data, size);
    fill_ = size;
  }
  std::string Hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad[72] = {0x80};
    size_t n = (fill_ < 56 ? 56 : 120) - fill_;
    for (int i = 0; i < 8; ++i) pad[n + i] = uint8_t(bits >> (56 - 8 * i));
    Update(pad, n + 8);
    static const char* k = "0123456789abcdef";
    std::string s;
    for (uint32_t w : h_) {
      for (int i = 28; i >= 0; i -= 4) s += k[(w >> i) & 15];
    }
    return s;
  }

 private:
  static uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Block(const uint8_t* p) {
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 |
             p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = h + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }
  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf_[64];
  size_t fill_ = 0;
  uint64_t total_ = 0;
};
#endif

bool HashFile(const fs::path& p, std::string* hex, uint64_t* size, InstallUi* ui = nullptr) {
  std::wstring err;
  auto blob = OpenFileBlob(PathWide(p), &err);
  if (!blob) return false;
  Sha256 h;
  uint64_t off = 0, total = blob->size();
  std::vector<uint8_t> buf(static_cast<size_t>(std::min<uint64_t>(total, 4u << 20)));
  while (off < total) {
    if (ui && ui->Cancelled()) return false;
    size_t n = size_t(std::min<uint64_t>(buf.size(), total - off));
    if (!blob->ReadAt(off, buf.data(), n)) return false;
    h.Update(buf.data(), n);
    off += n;
  }
  *hex = h.Hex();
  if (hex->empty()) return false;
  if (size) *size = total;
  return true;
}

// Writes a file through a .part name and renames it into place when complete.
#ifdef _WIN32
class OutFile {
 public:
  bool Open(const fs::path& p, std::wstring* error) {
    final_ = p;
    part_ = p;
    part_ += L".part";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    h_ = CreateFileW(part_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
      *error = L"Could not create " + PathWide(part_);
      return false;
    }
    return true;
  }
  bool Write(const uint8_t* data, size_t size) {
    while (size) {
      DWORD n = DWORD(std::min<size_t>(size, 64u << 20)), done = 0;
      if (!WriteFile(h_, data, n, &done, nullptr) || done == 0) return false;
      data += done;
      size -= done;
    }
    return true;
  }
  bool Commit() {
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
    if (MoveFileExW(part_.c_str(), final_.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(part_.c_str());
    return false;
  }
  ~OutFile() {
    if (h_ != INVALID_HANDLE_VALUE) {
      CloseHandle(h_);
      DeleteFileW(part_.c_str());
    }
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
  fs::path final_, part_;
};
#else
class OutFile {
 public:
  bool Open(const fs::path& p, std::wstring* error) {
    final_ = p;
    part_ = p;
    part_ += L".part";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fd_ = open(part_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      *error = L"Could not create " + PathWide(part_) + L": " + Widen(strerror(errno));
      return false;
    }
    return true;
  }
  bool Write(const uint8_t* data, size_t size) {
    while (size) {
      ssize_t done = write(fd_, data, std::min<size_t>(size, 64u << 20));
      if (done < 0 && errno == EINTR) continue;
      if (done <= 0) return false;
      data += done;
      size -= size_t(done);
    }
    return true;
  }
  bool Commit() {
    bool ok = close(fd_) == 0;
    fd_ = -1;
    if (ok && rename(part_.c_str(), final_.c_str()) == 0) return true;
    unlink(part_.c_str());
    return false;
  }
  ~OutFile() {
    if (fd_ >= 0) {
      close(fd_);
      unlink(part_.c_str());
    }
  }

 private:
  int fd_ = -1;
  fs::path final_, part_;
};
#endif

bool WriteText(const fs::path& path, const std::string& text) {
  std::wstring error;
  OutFile file;
  return file.Open(path, &error) &&
         file.Write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) && file.Commit();
}

bool ReplaceFile(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  std::error_code ec;
  fs::rename(from, to, ec);
  return !ec;
#endif
}

// NUL-separated fields, never file times, for compiler/codegen keys.
void KeyField(Sha256& hash, std::string_view value) {
  if (!value.empty()) hash.Update(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  const uint8_t end = 0;
  hash.Update(&end, 1);
}

bool KeyFile(Sha256& key, const fs::path& file, InstallUi& ui) {
  std::string digest;
  if (!HashFile(file, &digest, nullptr, &ui)) return false;
  KeyField(key, digest);
  return true;
}

bool KeyTree(Sha256& key, const fs::path& folder, InstallUi& ui) {
  std::error_code ec;
  if (!fs::exists(folder, ec)) return !ec;  // a dev package can omit unused include trees
  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) files.push_back(it->path());
    if (ec || ui.Cancelled()) return false;
  }
  if (ec) return false;
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    KeyField(key, NarrowGeneric(file.lexically_relative(folder)));
    if (!KeyFile(key, file, ui)) return false;
  }
  return true;
}

//-----------------------------------------------------------------------------
// Expected files (tools\game_dll\expected.txt, written by tools/game_dll/make_expected.py)
//   <kind> <size> <sha256> <path>
// kinds: disc2 (Disc 2 file), part (install partition file, path below 00000002/),
//        tu (title update file), gen (codegen output file)
//-----------------------------------------------------------------------------

struct Expected {
  uint64_t size = 0;
  std::string sha256;
};

struct ExpectedSet {
  std::map<std::string, Expected> disc2, part, tu, gen;
  bool Load(const fs::path& p, std::wstring* error) {
    std::ifstream f(p);
    if (!f) {
      *error = L"Missing " + PathWide(p) + L" (the launcher folder is incomplete).";
      return false;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      std::string kind, sha, path;
      uint64_t size = 0;
      ss >> kind >> size >> sha;
      std::getline(ss >> std::ws, path);
      auto* m = kind == "disc2" ? &disc2 : kind == "part" ? &part : kind == "tu" ? &tu
                : kind == "gen" ? &gen : nullptr;
      if (!m || path.empty()) continue;
      (*m)[path] = Expected{size, sha};
    }
    if (disc2.empty() || part.empty() || tu.empty() || gen.empty()) {
      *error = PathWide(p) + L" is damaged.";
      return false;
    }
    return true;
  }
  uint64_t TotalBytes() const {
    uint64_t t = 0;
    for (auto* m : {&disc2, &part, &tu})
      for (auto& [k, v] : *m) t += v.size;
    return t;
  }
};

fs::path PartitionPath(const fs::path& data) {
  return data / "user" / "0000000000000000" / kTitleId / "00000002";
}

template <typename Visit>
bool VisitLocalFiles(const ExpectedSet& expected, const fs::path& data, Visit visit) {
  for (const auto& [path, exp] : expected.disc2)
    if (!visit(data / "game" / PathFromUtf8(path), exp)) return false;
  for (const auto& [path, exp] : expected.part)
    if (!visit(PartitionPath(data) / PathFromUtf8(path), exp)) return false;
  for (const auto& [path, exp] : expected.tu)
    if (!visit(data / (path == "default.xexp" ? "game" : "update") / PathFromUtf8(path), exp)) return false;
  return true;
}

constexpr char kDevBuildId[] = "unversioned";

bool ReadPackageBuildId(const fs::path& root, std::string* id) {
  const auto path = root / "tools/game_dll/build-id.txt";
  std::error_code ec;
  if (!fs::exists(path, ec) && !ec) {
    *id = kDevBuildId;  // local, unpackaged development tree
    return true;
  }
  std::ifstream f(path);
  std::string line;
  if (!std::getline(f, line) || line.size() != 64 ||
      line.find_first_not_of("0123456789abcdef") != line.npos) return false;
  *id = std::move(line);
  return true;
}

std::string InstalledBuildId(const fs::path& home) {
  std::ifstream f(home / kInstalledMarker);
  std::string version, id;
  // A legacy one-line marker has no proven ABI; it can still reuse its game data.
  if (!std::getline(f, version) || version != kInstallVersion || !std::getline(f, id)) return {};
  return id;
}

//-----------------------------------------------------------------------------
// Child processes
//-----------------------------------------------------------------------------

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// One argument per element; Windows joins them into a command line.
using Args = std::vector<std::wstring>;

#ifdef _WIN32
std::wstring CommandLine(const Args& args) {
  std::wstring cmd;
  for (const std::wstring& a : args) {
    if (!cmd.empty()) cmd += L" ";
    cmd += a.empty() || a.find_first_of(L" \t\"") != std::wstring::npos ? Quote(a) : a;
  }
  return cmd;
}

struct Child {
  PROCESS_INFORMATION pi = {};
  bool Start(const Args& args, const fs::path& cwd, const fs::path& log) {
    std::wstring cmdline = CommandLine(args);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    // The child inherits the log handle only. Children start from several worker threads, and the
    // CRT opens files (std::ofstream) with inheritable handles: with plain inheritance a child
    // started on one thread kept another thread's .hlsl open for writing, and that thread's dxc
    // then failed with "being used by another process" (365 of 3,617 iPhone shaders).
    const bool have_out = out != INVALID_HANDLE_VALUE;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool have_attrs = have_out && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                      UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out,
                                                sizeof(out), nullptr, nullptr);
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    if (have_attrs) {
      si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
      si.StartupInfo.hStdOutput = out;
      si.StartupInfo.hStdError = out;
      si.lpAttributeList = attrs;
    }
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, have_attrs ? TRUE : FALSE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS |
                                 (have_attrs ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             nullptr, cwd.c_str(), &si.StartupInfo, &pi);
    if (have_attrs) DeleteProcThreadAttributeList(attrs);
    if (have_out) CloseHandle(out);
    return ok != 0;
  }
  // -1 while running.
  int Poll(DWORD wait_ms = 0) {
    const DWORD wait = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait == WAIT_TIMEOUT) return -1;
    DWORD code = 1;
    if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(pi.hProcess, &code)) return 1;
    // Some NT exit codes have the sign bit set; never mistake a crashed tool for a live one.
    return code <= 0x7FFFFFFF ? static_cast<int>(code) : 1;
  }
  void Kill() {
    if (pi.hProcess && TerminateProcess(pi.hProcess, 1)) WaitForSingleObject(pi.hProcess, INFINITE);
  }
  ~Child() {
    if (pi.hProcess) {
      if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) Kill();
      CloseHandle(pi.hProcess);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
  }
};
#else
std::vector<std::string> NarrowArgs(const Args& args) {
  std::vector<std::string> out;
  for (const std::wstring& a : args) out.push_back(Narrow(a));
  return out;
}

std::vector<char*> Argv(std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (std::string& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  return argv;
}

struct Child {
  pid_t pid = -1;
  int status = -1;
  bool Start(const Args& args, const fs::path& cwd, const fs::path& log) {
#ifdef LOADER_NO_SPAWN
    (void)args, (void)cwd, (void)log;
    return false;
#endif
    std::vector<std::string> narrow = NarrowArgs(args);
    std::vector<char*> argv = Argv(narrow);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
#ifndef LOADER_NO_SPAWN
    posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
#endif
    int err = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) {
      pid = -1;
      return false;
    }
    // The install runs in the background, like BELOW_NORMAL_PRIORITY_CLASS on Windows.
    setpriority(PRIO_PROCESS, id_t(pid), 10);
    return true;
  }
  // -1 while running. wait_ms < 0 waits until it exits.
  int Poll(int wait_ms = 0) {
    if (pid < 0) return status;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(wait_ms, 0));
    for (;;) {
      int st = 0;
      pid_t r = waitpid(pid, &st, wait_ms < 0 ? 0 : WNOHANG);
      if (r == pid) {
        pid = -1;
        status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        return status;
      }
      if (r < 0 && errno != EINTR) return status = 1;
      if (wait_ms >= 0 && std::chrono::steady_clock::now() >= until) return -1;
      if (wait_ms >= 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  void Kill() {
    if (pid > 0) kill(pid, SIGKILL);
  }
  ~Child() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
    }
  }
};
constexpr int INFINITE = -1;
#endif

std::string Tail(const fs::path& p, size_t bytes) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  f.seekg(0, std::ios::end);
  auto size = size_t(f.tellg());
  size_t n = std::min(size, bytes);
  f.seekg(size - n);
  std::string s(n, '\0');
  f.read(s.data(), n);
  return s;
}

//-----------------------------------------------------------------------------
// GTAV.ipa: a zip of Payload/ (stored, no compression), written here so every desktop system
// makes the same one with nothing installed. Unix modes are recorded: iOS needs the executable
// bits on gtav and the dylibs.
//-----------------------------------------------------------------------------

uint32_t Crc32(uint32_t crc, const uint8_t* p, size_t n) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// The folder dir as a stored zip, every entry under prefix ("Payload/" for the ipa, "" for the
// Android pack). exec(leaf name, path) marks the files that get mode 755.
template <typename ExecFn>
bool WriteZip(const fs::path& dir, const std::string& prefix, const fs::path& ipa, ExecFn exec_fn) {
  struct Item {
    std::string name;  // zip path ('/' separators, directories end in '/')
    fs::path path;
    bool dir, exec;
  };
  std::vector<Item> items;
  if (!prefix.empty()) items.push_back({prefix, dir, true, false});
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    std::string rel = prefix + fs::relative(it->path(), dir).generic_string();
    const std::string leaf = it->path().filename().string();
    if (leaf.rfind("._", 0) == 0 || leaf == ".DS_Store") continue;  // AppleDouble files break signing
    if (it->is_directory()) {
      items.push_back({rel + "/", it->path(), true, false});
    } else if (it->is_regular_file()) {
      items.push_back({rel, it->path(), false, exec_fn(leaf, it->path())});
    }
  }
  if (ec) return false;
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
  fs::path tmp = ipa;
  tmp += ".part";
  std::ofstream z(tmp, std::ios::binary | std::ios::trunc);
  if (!z) return false;
  auto u16 = [&](std::string& b, uint16_t v) { b += char(v & 0xFF), b += char(v >> 8); };
  auto u32 = [&](std::string& b, uint32_t v) { u16(b, uint16_t(v)), u16(b, uint16_t(v >> 16)); };
  constexpr uint16_t kTime = 0, kDate = (46 << 9) | (1 << 5) | 1;  // 2026-01-01: same bytes every run
  std::string central;
  std::vector<uint8_t> buf(1 << 20);
  for (const Item& it : items) {
    const uint64_t offset = uint64_t(z.tellp());
    uint32_t crc = 0;
    uint64_t size = 0;
    if (!it.dir) {
      size = fs::file_size(it.path, ec);
      std::ifstream f(it.path, std::ios::binary);
      while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), buf.size());
        crc = Crc32(crc, buf.data(), size_t(f.gcount()));
      }
    }
    if (ec || size >= 0xFFFFFFFFull || offset >= 0xFFFFFFFFull) return false;  // (no zip64 needed: ~200 MB)
    std::string h;
    u32(h, 0x04034b50), u16(h, 20), u16(h, 0), u16(h, 0), u16(h, kTime), u16(h, kDate);
    u32(h, crc), u32(h, uint32_t(size)), u32(h, uint32_t(size)), u16(h, uint16_t(it.name.size())), u16(h, 0);
    z << h << it.name;
    if (!it.dir) {
      std::ifstream f(it.path, std::ios::binary);
      while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), buf.size());
        z.write(reinterpret_cast<const char*>(buf.data()), f.gcount());
      }
    }
    const uint32_t mode = it.dir ? 040755 : it.exec ? 0100755 : 0100644;
    std::string c;
    u32(c, 0x02014b50), u16(c, (3 << 8) | 20), u16(c, 20), u16(c, 0), u16(c, 0), u16(c, kTime), u16(c, kDate);
    u32(c, crc), u32(c, uint32_t(size)), u32(c, uint32_t(size)), u16(c, uint16_t(it.name.size()));
    u16(c, 0), u16(c, 0), u16(c, 0), u16(c, 0), u32(c, (mode << 16) | (it.dir ? 0x10 : 0)), u32(c, uint32_t(offset));
    central += c + it.name;
  }
  const uint64_t cd = uint64_t(z.tellp());
  std::string e;
  u32(e, 0x06054b50), u16(e, 0), u16(e, 0), u16(e, uint16_t(items.size())), u16(e, uint16_t(items.size()));
  u32(e, uint32_t(central.size())), u32(e, uint32_t(cd)), u16(e, 0);
  z << central << e;
  z.close();
  if (!z || items.size() > 0xFFFF) return false;
  fs::rename(tmp, ipa, ec);
  return !ec;
}

bool WriteIpa(const fs::path& payload, const fs::path& ipa) {
  return WriteZip(payload, "Payload/", ipa, [](const std::string& leaf, const fs::path& p) {
    return leaf == "gtav" || p.extension() == ".dylib";
  });
}

//-----------------------------------------------------------------------------
// The install
//-----------------------------------------------------------------------------

class Installer {
 public:
  Installer(fs::path root, fs::path home, const InstallInputs& in, InstallUi& ui)
      : root_(std::move(root)), home_(std::move(home)), in_(in), ui_(ui), data_(home_ / L"data") {}

  bool Run(std::wstring* error) {
    if (!BeginDesktop(error, L"install")) return Fail();
    if (!OpenInputs() || !CheckSourceExecutable() || !CheckSpace()) return Fail();
    LoadDone();

    copy_total_ = expected_.TotalBytes();
    LoadVerifiedData();
    if (!CopyDisc2() || !CopyTitleUpdate() || !CopyInstallPartitions() || !VerifyCodeOnDisk() ||
        !ReviewMismatches()) return Fail();
    SaveVerifiedData();
    // Once the copy has been checked, the compile only needs the local files.
    parts_.clear();
    tu_files_.clear();
    disc1_.reset();
    disc2_.reset();
    tu_.reset();
    return BuildDesktop();
  }

  bool Rebuild(std::wstring* error) {
    if (!BeginDesktop(error, L"recompile")) return Fail();
    if (!CheckSpace(false) || !VerifyLocalFiles()) return Fail();
    return BuildDesktop();
  }

  // Game data only (the iPhone app's setup screen: its game code and shaders were built on the
  // player's Mac): the same checked copies into <home>/data, no codegen.
  bool RunData(const fs::path& expected, std::wstring* error) {
    error_ = error;
    data_only_ = true;
    std::error_code ec;
    fs::create_directories(data_ / L"logs", ec);
    log_.open(data_ / L"logs" / L"install.log", std::ios::app);
    Log(L"---- game data install started");
    if (!expected_.Load(expected, error)) return false;
    if (!OpenInputs() || !CheckSourceExecutable() || !CheckSpace()) return Fail();
    LoadDone();
    copy_total_ = expected_.TotalBytes();
    if (!CopyDisc2() || !CopyTitleUpdate() || !CopyInstallPartitions()) return Fail();
    if (!VerifyCodeOnDisk() || !ReviewMismatches()) return Fail();
    std::ofstream(data_ / L"data_installed.txt") << kDataVersion << "\n";
    fs::remove(data_ / L"install_state.txt", ec);
    ui_.Progress(1.0);
    ui_.Stage(L"Done.");
    Log(L"---- game data install finished");
    return true;
  }

 private:
  // Progress budget: copying 0..0.55, codegen ..0.70, check ..0.72, compile ..0.97, link ..1.
  static constexpr double kCopyEnd = 0.55, kCodegenEnd = 0.70, kCheckEnd = 0.72,
                          kCompileEnd = 0.97;

  static constexpr char kDataVersion[] = "gtav-recomp data 1";
  bool data_only_ = false;  // RunData: copying is the whole progress bar

  bool Fail() {
    if (ui_.Cancelled() && error_->empty()) *error_ = L"Cancelled.";
    return false;
  }

  void Log(const std::wstring& s) {
    ui_.Log(s);
    if (log_) log_ << Narrow(s) << "\n" << std::flush;
  }

  bool SetError(const std::wstring& s) {
    *error_ = s;
    Log(L"error: " + s);
    return false;
  }

  bool CheckBundledBuildInputs() {
    const auto tools = root_ / "tools/game_dll";
    for (const char* name : {"codegen.partition.json", "fs/layout_check.h", "fs/game_dll.cpp",
                            "interface/gtav_hooks.h",
#if defined(_WIN32)
                            "interface/gtav_fast.def", "interface/rexruntime.def", "interface/ucrtbase.def",
#elif defined(__APPLE__)
                            "interface/gtav_game.exports",
#endif
#ifndef __APPLE__
                            "fs/fs_crt.cpp",
#endif
                           }) {
      std::error_code ec;
      if (!fs::is_regular_file(tools / name, ec))
        return SetError(L"The release is incomplete: tools/game_dll/" + Widen(name) +
                        L" is missing. Extract a fresh copy of the release.");
    }
    return true;
  }

  bool BeginDesktop(std::wstring* error, const std::wstring& action) {
    error_ = error;
    ui_.Progress(0);
    ui_.Stage(L"Checking the release and game folder...");
    if (ui_.Cancelled()) return false;
#ifdef __APPLE__
    if (!CheckGameFolder(home_, error)) return false;
#endif
    if (!LockBuild() || !Preflight(root_, error) || !CheckBundledBuildInputs()) return false;
    if (!ReadPackageBuildId(root_, &build_id_))
      return SetError(L"The release's game build ID is invalid. Extract a fresh copy of the release.");
    log_.open(data_ / "logs/install.log", std::ios::app);
    if (!log_) return SetError(L"Could not open data/logs/install.log for writing.");
    Log(L"---- " + action + L" started");
    return expected_.Load(root_ / "tools/game_dll/expected.txt", error);
  }

  bool LockBuild() {
    std::error_code ec;
    fs::create_directories(data_ / "logs", ec);
    if (ec) return SetError(L"Could not create the log folder: " + Widen(ec.message()));
    if (!lock_.Acquire(data_ / "setup.lock"))
      return SetError(L"Could not lock the game folder. Close the game or other setup and check write permissions.");
#ifdef _WIN32
    if (fs::exists(root_ / ".gtav-update/pending.txt", ec) || ec)
      return SetError(L"An update needs recovery. Open the launcher normally first.");
#endif
    return true;
  }

  fs::path BuildDir() const { return data_ / (ios_ ? "build-iphone" : android_ ? "build-android" : "build"); }
  bool IsDesktop() const { return !ios_ && !android_; }

  bool BuildDesktop() {
    if (!PrepareGenerated() || !Compile() || !Link()) return Fail();
    std::error_code ec;
    fs::remove(data_ / "install_state.txt", ec);
    ui_.Progress(1.0);
    ui_.Stage(L"Game build ready. Press Play.");
    Log(L"---- desktop game build finished");
    return true;
  }

  //--- inputs ----------------------------------------------------------------

  bool OpenInputs() {
    ui_.Stage(L"Checking Disc 1, Disc 2 and Title Update 27...");
    std::wstring err;
    disc2_ = OpenDisc(in_.disc2, &err);
    if (!disc2_) return SetError(L"Disc 2: " + err);
    disc1_ = OpenDisc(in_.disc1, &err);
    if (!disc1_) return SetError(L"Disc 1: " + err);

    bool disc2_has_play_data = disc2_->Find("xbox360b.rpf") && disc2_->Find("audio_rel.rpf");
    bool disc2_has_install = disc2_->Find(std::string(kInstallPackageDir) + kTitleId + "00000000");
    bool disc1_has_install = disc1_->Find(std::string(kInstallPackageDir) + kTitleId + "00000000");
    if (!disc2_has_play_data && disc2_has_install)
      return SetError(L"The file picked as Disc 2 is Disc 1 (Install). Swap the two discs.");
    if (!disc2_has_play_data || !disc2_->Find("default.xex"))
      return SetError(L"Disc 2 (Play) is missing game files. Pick the Disc 2 image or folder.");
    if (!disc1_has_install)
      return SetError(L"Disc 1 (Install) has no install packages (content\\0000000000000000\\"
                      L"545408A7\\00000002). Pick the Disc 1 image or folder.");

    // Title update: an STFS package file, or a folder with default.xexp + update.rpf.
    std::error_code ec;
    if (fs::is_directory(PathFromWide(in_.tu), ec)) {
      tu_ = OpenDisc(in_.tu, &err);
      if (!tu_) return SetError(L"Title update: " + err);
    } else {
      auto blob = OpenFileBlob(in_.tu, &err);
      if (!blob) return SetError(L"Title update: " + err);
      if (!LooksLikeStfs(*blob))
        return SetError(L"The title update file is not an Xbox 360 package. Pick the TU27 file "
                        L"itself (for example tu00000009_00000000), not a zip.");
      tu_ = OpenStfs(std::move(blob), &err);
      if (!tu_) return SetError(L"Title update: " + err);
    }
    for (const char* name : {"default.xexp", "update.rpf"}) {
      tu_files_[name] = nullptr;
      for (const char* prefix : {"disc002/", "", "disc001/"}) {
        if (auto* e = tu_->Find(std::string(prefix) + name)) {
          tu_files_[name] = e;
          break;
        }
      }
      if (!tu_files_[name])
        return SetError(L"The title update has no " + Widen(name) + L". Pick Title Update 27.");
    }
    // Reject incomplete source layouts before spending time copying large files.
    for (const auto& [path, expected] : expected_.disc2)
      if (!disc2_->Find(path)) return SetError(L"Disc 2 is missing " + Widen(path) + L".");
    for (char n = '0'; n <= '3'; ++n) {
      if (ui_.Cancelled()) return false;
      std::string pkg = std::string(kTitleId) + "0000000" + n;
      const Entry* e = disc1_->Find(kInstallPackageDir + pkg);
      if (!e) return SetError(L"Disc 1 is missing install package " + Widen(pkg) + L".");
      auto blob = disc1_->OpenEntry(*e, &err);
      if (!blob) return SetError(L"Disc 1: " + err);
      auto stfs = OpenStfs(std::move(blob), &err);
      if (!stfs) return SetError(L"Disc 1 package " + Widen(pkg) + L": " + err);
      const auto prefix = pkg + "/";
      for (const auto& [path, exp] : expected_.part) {
        if (path.starts_with(prefix) && !stfs->Find(path.substr(prefix.size())))
          return SetError(L"Disc 1 package is missing " + Widen(path) + L".");
      }
      parts_.emplace_back(std::move(pkg), std::move(stfs));
    }
    Log(L"Disc 1: " + disc1_->Describe());
    Log(L"Disc 2: " + disc2_->Describe());
    Log(L"Title update: " + in_.tu);
    return true;
  }

  bool CheckSourceExecutable() {
    ui_.Stage(L"Verifying the Disc 2 executable and TU27 patch...");
    auto check = [&](Source& src, const Entry& file, const Expected* exp, const wchar_t* hint) {
      if (!exp || file.size != exp->size) return SetError(hint);
      Sha256 sha;
      std::wstring err;
      if (!src.Read(file, [&](const uint8_t* bytes, size_t size) {
            if (ui_.Cancelled()) return false;
            sha.Update(bytes, size);
            return true;
          }, &err)) return ui_.Cancelled() ? false : SetError(err);
      return sha.Hex() == exp->sha256 || SetError(hint);
    };
    return check(*disc2_, *disc2_->Find("default.xex"), Find(expected_.disc2, "default.xex"),
                 L"Disc 2's default.xex is not the supported executable. Check your Disc 2 dump.") &&
           check(*tu_, *tu_files_.at("default.xexp"), Find(expected_.tu, "default.xexp"),
                 L"This title update is not the supported TU27. Select Title Update 27 for GTA V.");
  }

  bool CheckSpace(bool copying = true) {
    std::error_code ec;
    const uint64_t available = fs::space(data_, ec).available;
    if (ec) return true;
    uint64_t need = data_only_ ? (512ull << 20) : (3ull << 30);  // temporary work files
    if (copying) {
      // Files already in place (an interrupted earlier run) can be reused.
      VisitLocalFiles(expected_, data_, [&](const fs::path& p, const Expected& exp) {
        if (fs::file_size(p, ec) != exp.size || ec) need += exp.size;
        return true;
      });
    }
    Log(L"Disk space: ~" + Gb(need) + L" needed, " + Gb(available) + L" available.");
    if (available < need)
      return SetError(L"Not enough free space: " + Gb(need) + L" needed, " + Gb(available) + L" free.");
    return true;
  }

  //--- copying ---------------------------------------------------------------

  // Files verified by an earlier, interrupted run: "<sha256> <size> <dest path>".
  void LoadDone() {
    std::ifstream f(data_ / L"install_state.txt");
    std::string sha, path;
    uint64_t size;
    while (f >> sha >> size && std::getline(f >> std::ws, path)) done_[path] = {size, sha};
  }

  void MarkDone(const fs::path& dest, uint64_t size, const std::string& sha) {
    std::ofstream(data_ / L"install_state.txt", std::ios::app)
        << sha << " " << size << " " << Narrow(PathWide(dest)) << "\n";
    done_[Narrow(PathWide(dest))] = {size, sha};
  }

  // Only non-code assets use the fast path. An old import without this receipt is checked once;
  // missing, changed, or previously accepted non-reference assets are always checked again.
  struct VerifiedData {
    std::string digest;
    uint64_t size;
    int64_t mtime;
    bool operator==(const VerifiedData&) const = default;
  };

  fs::path VerifiedPath() const { return data_ / "verified-assets.txt"; }

  static bool IsCodeInput(const fs::path& path) {
    return path.filename() == "default.xex" || path.filename() == "default.xexp";
  }

  static std::optional<VerifiedData> Observe(const fs::path& path, const Expected& exp) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size != exp.size) return std::nullopt;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return std::nullopt;
    return VerifiedData{exp.sha256, size, static_cast<int64_t>(time.time_since_epoch().count())};
  }

  void LoadVerifiedData() {
    std::ifstream file(VerifiedPath());
    std::string line, path;
    if (!std::getline(file, line) || line != "gtav-recomp verified assets 1") return;
    VerifiedData entry;
    while (file >> entry.digest >> entry.size >> entry.mtime && std::getline(file >> std::ws, path))
      verified_data_.emplace(path, entry);
  }

  void SaveVerifiedData() {
    std::ostringstream out;
    out << "gtav-recomp verified assets 1\n";
    for (const auto& [path, entry] : verified_data_)
      out << entry.digest << ' ' << entry.size << ' ' << entry.mtime << ' ' << path << '\n';
    if (!WriteText(VerifiedPath(), out.str())) Log(L"Could not save the asset verification receipt.");
  }

  // Copies one entry to dest, hashing on the way. Records a mismatch against `exp`.
  bool CopyEntry(Source& src, const Entry& e, const fs::path& dest, const Expected* exp,
                 const std::wstring& what) {
    if (ui_.Cancelled()) return false;
    std::error_code ec;
    auto it = done_.find(Narrow(PathWide(dest)));
    // Re-copy the small code inputs, so a previously damaged local executable/patch can be
    // repaired even if the journal records a successful copy of the same size.
    if (dest.filename() != "default.xex" && dest.filename() != "default.xexp" &&
        it != done_.end() && it->second.size == e.size && (!exp || it->second.sha256 == exp->sha256) &&
        fs::file_size(dest, ec) == e.size && !ec) {
      copied_ += e.size;
      Report();
      return true;
    }
    ui_.Stage(L"Copying " + what + L" (" + Gb(e.size) + L")");
    OutFile out;
    if (!out.Open(dest, error_)) return false;
    Sha256 h;
    std::wstring err;
    bool ok = src.Read(e, [&](const uint8_t* d, size_t n) {
      if (ui_.Cancelled()) return false;
      h.Update(d, n);
      if (!out.Write(d, n)) {
        err = L"Write error on " + PathWide(dest) + L" (disk full?)";
        return false;
      }
      copied_ += n;
      Report();
      return true;
    }, &err);
    if (!ok) {
      if (!ui_.Cancelled()) SetError(err);
      return false;
    }
    if (!out.Commit()) return SetError(L"Could not write " + PathWide(dest));
    std::string sha = h.Hex();
    MarkDone(dest, e.size, sha);
    if (exp && (sha != exp->sha256 || e.size != exp->size)) {
      verified_data_.erase(NarrowGeneric(dest.lexically_relative(data_)));
      mismatches_.push_back(what);
      Log(L"differs from the known dump: " + what);
    } else if (exp && !data_only_ && !IsCodeInput(dest)) {
      if (auto verified = Observe(dest, *exp))
        verified_data_.insert_or_assign(NarrowGeneric(dest.lexically_relative(data_)), *verified);
    }
    return true;
  }

  void Report() {
    double f = copy_total_ ? double(copied_) / double(copy_total_) : 0;
    ui_.Progress(std::min(f, 1.0) * (data_only_ ? 1.0 : kCopyEnd));
  }

  bool CopyDisc2() {
    Log(L"Copying Disc 2 game files");
    for (auto& [path, exp] : expected_.disc2) {
      const Entry* e = disc2_->Find(path);
      if (!e) return SetError(L"Disc 2 is missing " + Widen(path) + L". The image is incomplete.");
      if (!CopyEntry(*disc2_, *e, data_ / L"game" / PathFromUtf8(path), &exp, L"Disc 2: " + Widen(path)))
        return false;
    }
    return true;
  }

  bool CopyTitleUpdate() {
    Log(L"Copying the title update");
    const Entry* xexp = tu_files_["default.xexp"];
    const Entry* rpf = tu_files_["update.rpf"];
    auto* exp_xexp = Find(expected_.tu, "default.xexp");
    auto* exp_rpf = Find(expected_.tu, "update.rpf");
    if (!CopyEntry(*tu_, *xexp, data_ / L"game" / L"default.xexp", exp_xexp, L"TU: default.xexp") ||
        !CopyEntry(*tu_, *rpf, data_ / L"update" / L"update.rpf", exp_rpf, L"TU: update.rpf"))
      return false;
    return true;
  }

  static const Expected* Find(const std::map<std::string, Expected>& m, const std::string& k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
  }

  bool CopyInstallPartitions() {
    Log(L"Unpacking the Disc 1 install partitions");
    for (const auto& [pkg, stfs] : parts_) {
      for (const Entry& f : stfs->entries()) {
        std::string rel = pkg + "/" + f.path;
        fs::path dest = PartitionPath(data_) / PathFromUtf8(rel);
        if (!CopyEntry(*stfs, f, dest, Find(expected_.part, rel), L"Disc 1: " + Widen(rel)))
          return false;
      }
    }
    return true;
  }

  bool VerifyCodeOnDisk() {
    auto check = [&](const char* file, const Expected* exp) {
      std::string sha;
      uint64_t size = 0;
      if (exp && HashFile(data_ / "game" / file, &sha, &size, &ui_) &&
          sha == exp->sha256 && size == exp->size) return true;
      return ui_.Cancelled() ? false : SetError(L"The local " + Widen(file) +
          L" is missing or does not match. Run Install & build with your original Disc 2 and TU27.");
    };
    return check("default.xex", Find(expected_.disc2, "default.xex")) &&
           check("default.xexp", Find(expected_.tu, "default.xexp"));
  }

  bool VerifyLocalFiles() {
    ui_.Stage(L"Checking existing game files (no discs needed)...");
    if (!VerifyCodeOnDisk()) return false;
    LoadVerifiedData();
    size_t reused = 0, checked = 0;
    copy_total_ = expected_.TotalBytes();
    if (!VisitLocalFiles(expected_, data_, [&](const fs::path& path, const Expected& exp) {
          if (ui_.Cancelled()) return false;
          std::error_code ec;
          if (!fs::is_regular_file(path, ec))
            return SetError(L"Missing game file: " + PathWide(path) + L". Run Install & build with your originals.");
          if (!IsCodeInput(path)) {
            const auto name = NarrowGeneric(path.lexically_relative(data_));
            const auto before = Observe(path, exp);
            const auto it = verified_data_.find(name);
            if (before && it != verified_data_.end() && it->second == *before) {
              ++reused;
            } else {
              verified_data_.erase(name);
              std::string sha;
              uint64_t size;
              ui_.Stage(L"Checking " + PathWide(path.filename()) + L" (" + Gb(exp.size) + L")...");
              if (!HashFile(path, &sha, &size, &ui_))
                return ui_.Cancelled() ? false : SetError(L"Could not read the local file " + PathWide(path));
              ++checked;
              if (size != exp.size || sha != exp.sha256) mismatches_.push_back(PathWide(path.filename()));
              else if (before && Observe(path, exp) == before) verified_data_.emplace(name, *before);
            }
          }
          copied_ += exp.size;
          Report();
          return true;
        })) return false;
    Log(L"Game assets: " + Num(reused) + L" unchanged, " + Num(checked) +
        L" rehashed. XEX and XEXP always rehashed.");
    if (!ReviewMismatches()) return false;
    SaveVerifiedData();
    return true;
  }

  bool ReviewMismatches() {
    if (mismatches_.empty()) {
      Log(L"All files match the known good dump.");
      return true;
    }
    std::wstring q = Num(mismatches_.size()) + L" file(s) differ from a known good dump of the "
                     L"game, for example:\n\n";
    for (size_t i = 0; i < mismatches_.size() && i < 5; ++i) q += L"  " + mismatches_[i] + L"\n";
    q += L"\nA damaged or modified dump can crash or misbehave. Continue anyway?";
    if (!ui_.Ask(q)) return SetError(L"Stopped: some game files differ from a known good dump.");
    return true;
  }

  //--- codegen ---------------------------------------------------------------

  bool PrepareGenerated() {
    const auto tools = root_ / "tools/game_dll";
    Sha256 key;
    KeyField(key, "gtav-codegen-cache-1");
    // The local pair has just passed strict hash verification. With the same recipe we can
    // reuse codegen only if *every* reference source/header still matches by content.
    KeyField(key, expected_.disc2.at("default.xex").sha256);
    KeyField(key, expected_.tu.at("default.xexp").sha256);
    if (!KeyFile(key, root_ / kCodegenExe, ui_) ||
        !KeyFile(key, tools / "gtav_manifest.toml", ui_) ||
        !KeyFile(key, tools / "codegen.partition.json", ui_))
      return ui_.Cancelled() ? false : SetError(L"Could not read the code generation inputs.");
    const std::string recipe = key.Hex();
    if (recipe.empty()) return SetError(L"Could not fingerprint the code generation inputs.");
    const auto stamp = BuildDir() / "codegen-cache.txt";
    std::string previous;
    std::ifstream(stamp) >> previous;
    const bool reuse = previous == recipe && CheckGenerated(false);
    if (ui_.Cancelled()) return false;
    if (reuse) {
      Log(L"Codegen: reused verified sources.");
    } else {
      if (!Codegen() || !CheckGenerated()) return false;
      if (!WriteText(stamp, recipe + "\n")) Log(L"Could not save the codegen cache key.");
    }
    // The code generator emits several auxiliary files. A cached include directory must not
    // pick up extra headers left over by another release or supplied by a damaged cache.
    const auto gen = BuildDir() / "generated/default";
    std::error_code ec;
    for (auto it = fs::directory_iterator(gen, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (!expected_.gen.contains(NarrowGeneric(it->path().filename()))) {
        if (fs::is_directory(it->symlink_status(ec)))
          return SetError(L"Unexpected directory in generated code: " + PathWide(it->path()));
        if (!ec) fs::remove(it->path(), ec);
        if (ec) break;
      }
    }
    if (ec) return SetError(L"Could not clean the generated include directory.");
    return true;
  }

  bool Codegen() {
    if (ui_.Cancelled()) return false;
    fs::path build = BuildDir();
    fs::path gen = build / L"generated" / L"default";
    fs::path tools = root_ / L"tools" / L"game_dll";
    std::error_code ec;
    fs::remove_all(build / L"generated", ec);
    if (ec) return SetError(L"Could not clear the old generated code: " + Widen(ec.message()));
    fs::create_directories(gen, ec);
    if (ec) return SetError(L"Could not prepare the generated code directory: " + Widen(ec.message()));
    struct StagedAssets {
      fs::path directory;
      ~StagedAssets() { std::error_code ignored; fs::remove_all(directory, ignored); }
    } assets{build / L"assets"};
    fs::create_directories(assets.directory, ec);
    if (ec) return SetError(L"Could not stage the codegen inputs: " + Widen(ec.message()));
    fs::copy_file(tools / L"gtav_manifest.toml", build / L"gtav_manifest.toml",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return SetError(L"Could not stage the manifest: " + Widen(ec.message()));
    // Seeds codegen's file partitioning, so every player gets the same source files.
    fs::copy_file(tools / L"codegen.partition.json", gen / L"codegen.partition.json",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return SetError(L"Could not stage the codegen partition seed: " + Widen(ec.message()));
    for (const wchar_t* f : {L"default.xex", L"default.xexp"}) {
      fs::copy_file(data_ / L"game" / f, build / L"assets" / f, fs::copy_options::overwrite_existing,
                    ec);
      if (ec) return SetError(L"Could not stage " + std::wstring(f) + L": " + Widen(ec.message()));
    }

    ui_.Stage(L"Recompiling the game executable to C++ (several minutes)...");
    Log(L"Running codegen");
    fs::path log = data_ / L"logs" / L"codegen.log";
    Child c;
    Args cmd = {PathWide(root_ / kCodegenExe), L"--log-level", L"info", L"codegen",
                L"gtav_manifest.toml"};
    if (!c.Start(cmd, build, log)) return SetError(L"Could not start " + std::wstring(kCodegenExe));
    size_t want = expected_.gen.size();
    for (;;) {
      int code = c.Poll(1000);
      if (code >= 0) {
        if (code != 0)
          return SetError(L"Codegen failed (exit " + Num(code) + L"). See " + PathWide(log) +
                          L":\n" + Widen(Tail(log, 600)));
        break;
      }
      if (ui_.Cancelled()) {
        c.Kill();
        return false;
      }
      size_t have = 0;
      for (auto it = fs::directory_iterator(gen, ec); !ec && it != fs::directory_iterator();
           it.increment(ec))
        ++have;
      double f = want ? std::min(1.0, double(have) / double(want)) : 0;
      ui_.Progress(kCopyEnd + f * (kCodegenEnd - kCopyEnd));
    }
    ui_.Progress(kCodegenEnd);
    return true;
  }

  bool CheckGenerated(bool report_error = true) {
    ui_.Stage(report_error ? L"Checking the generated code..." : L"Checking the cached generated code...");
    fs::path gen = BuildDir() / L"generated" / L"default";
    size_t n = 0, bad = 0;
    std::wstring first_bad;
    for (auto& [name, exp] : expected_.gen) {
      if (ui_.Cancelled()) return false;
      std::string sha;
      uint64_t size = 0;
      const auto file = gen / PathFromUtf8(name);
      std::error_code ec;
      if (!fs::is_regular_file(fs::symlink_status(file, ec)) || fs::file_size(file, ec) != exp.size || ec ||
          !HashFile(file, &sha, &size, &ui_) || size != exp.size || sha != exp.sha256) {
        if (ui_.Cancelled() || !report_error) return false;
        if (!bad++) first_bad = Widen(name);
      }
      if (++n % 50 == 0)
        ui_.Progress(kCodegenEnd + (kCheckEnd - kCodegenEnd) * double(n) / expected_.gen.size());
    }
    if (bad)
      return SetError(Num(bad) + L" generated file(s) differ from the reference build (first: " +
                      first_bad + L"). The codegen tool or your files differ from what this "
                      L"release was made with.");
    Log(L"Generated code matches the reference build (" + Num(n) + L" files).");
    return true;
  }

  //--- compile and link ------------------------------------------------------

  static int JobCount() {
    // GTAV_JOBS=N: at most N compilers at a time (a machine busy with something else).
    if (const char* env = getenv("GTAV_JOBS")) {
      int n = atoi(env);
      if (n >= 1) return std::min(n, Uncapped());
    }
    return Uncapped();
  }

  static int Uncapped() {
#ifdef __APPLE__
    // clang needs up to ~1 GB for the largest generated file; keep 3 GB for macOS and the app.
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    int64_t by_ram = (int64_t(mem >> 20) - 3072) / 1024;
    int jobs = int(std::min<int64_t>(sysconf(_SC_NPROCESSORS_ONLN), by_ram));
    return std::clamp(jobs, 1, 32);
#elif !defined(_WIN32)
    // MemAvailable from /proc/meminfo, in kB.
    int64_t avail_mb = 4096;
    std::ifstream mi("/proc/meminfo");
    std::string key;
    int64_t kb;
    while (mi >> key >> kb) {
      if (key == "MemAvailable:") {
        avail_mb = kb / 1024;
        break;
      }
      mi.ignore(64, '\n');
    }
    int64_t by_ram_mb = (avail_mb - 2048) / 1024;
    int jobs_mb = int(std::min<int64_t>(sysconf(_SC_NPROCESSORS_ONLN), by_ram_mb));
    return std::clamp(jobs_mb, 1, 32);
#endif
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    // clang needs up to ~1 GB for the largest generated file; keep 2 GB for everything else.
    int64_t by_ram = (int64_t(ms.ullAvailPhys >> 20) - 2048) / 1024;
    int jobs = int(std::min<int64_t>(si.dwNumberOfProcessors, by_ram));
    return std::clamp(jobs, 1, 32);
#endif
  }

  bool Compile() {
    fs::path gen = BuildDir() / L"generated" / L"default";
    fs::path obj = BuildDir() / L"obj";
    fs::path fsdir = root_ / L"tools" / L"game_dll" / L"fs";
    // iPhone / Android: the phone shell's own hook header (package ios/ or android/ interface).
    fs::path iface = ios_       ? root_ / L"ios" / L"interface"
                     : android_ ? root_ / L"android" / L"interface"
                                : root_ / L"tools" / L"game_dll" / L"interface";
    std::error_code ec;
    if (!IsDesktop()) fs::remove_all(obj, ec);
    if (!ec) fs::create_directories(obj, ec);
    if (ec) return SetError(L"Could not prepare the compiler work folder: " + Widen(ec.message()));
    for (auto [src, name] : {std::pair{fsdir, L"gtav_pch.h"}, {fsdir, L"layout_check.h"},
                             {iface, L"gtav_hooks.h"}}) {
      fs::copy_file(src / name, gen / name, fs::copy_options::overwrite_existing, ec);
      if (ec) return SetError(L"Could not stage " + std::wstring(name));
    }
    std::vector<fs::path> sources;
    for (auto& [name, exp] : expected_.gen) {
      if ((name.rfind("gtav_recomp.", 0) == 0 && name.size() > 4 &&
           name.substr(name.size() - 4) == ".cpp") || name == "gtav_init.cpp")
        sources.push_back(gen / PathFromUtf8(name));
    }
    sources.push_back(fsdir / L"game_dll.cpp");
#ifndef __APPLE__
    if (!ios_ && !android_) sources.push_back(fsdir / L"fs_crt.cpp");  // x86 memcpy & co.; arm64 takes the system's
#endif

    fs::path clang = root_ / L"tools" / L"llvm" / L"bin" / kClang;
    // Keep in sync with tools/game_dll/build_game_dll.py.
    Args flags;
    if (ios_) {
      // iPhone, from any host: arm64 iOS 17, A14 or newer. The SSE intrinsics map to NEON through
      // simde (MIT, package ios/include), as on the Mac.
      flags = {L"--target=arm64-apple-ios17.0", L"-nostdinc++", L"-nostdlibinc", L"-fno-stack-protector",
               L"-O2", L"-std=gnu++23", L"-mcpu=apple-a14", L"-fno-exceptions", L"-fno-rtti",
               L"-fno-threadsafe-statics", L"-fno-math-errno", L"-w", L"-isystem",
               PathWide(root_ / L"ios" / L"include")};
    } else if (android_) {
      // Android phones, from any host: aarch64, API 30, ARMv8.2 (LSE atomics; Snapdragon 845 and
      // newer). Freestanding like the others: no NDK headers; simde from the package's android/.
      // Keep in sync with tools/game_dll/build_game_dll.py (CXXFLAGS_ANDROID).
      flags = {L"--target=aarch64-linux-android30", L"-nostdinc++", L"-nostdlibinc", L"-fPIC",
               L"-fno-semantic-interposition", L"-fno-stack-protector", L"-O2", L"-std=gnu++23",
               L"-march=armv8.2-a+lse+rcpc+dotprod", L"-fno-exceptions", L"-fno-rtti",
               L"-fno-threadsafe-statics", L"-fno-math-errno", L"-w", L"-isystem",
               PathWide(root_ / L"android" / L"include")};
    } else {
#ifdef _WIN32
    flags = {L"--target=x86_64-pc-windows-msvc", L"-nostdinc++", L"-nostdlibinc", L"-O2",
                  L"-std=gnu++23", L"-march=x86-64-v3", L"-msse4.1", L"-fno-exceptions",
                  L"-fno-rtti", L"-fno-threadsafe-statics", L"-fno-math-errno", L"-w"};
#elif defined(__APPLE__)
    // fs/include also holds simde (SSE on NEON, the SDK's own headers).
    flags = {L"--target=arm64-apple-macos15", L"-nostdinc++", L"-nostdlibinc", L"-fno-stack-protector",
             L"-O2", L"-std=gnu++23", L"-mcpu=apple-m1", L"-fno-exceptions", L"-fno-rtti",
             L"-fno-threadsafe-statics", L"-fno-math-errno", L"-w"};
#else
    flags = {L"--target=x86_64-linux-gnu", L"-nostdinc++", L"-nostdlibinc", L"-fPIC",
                  L"-fno-semantic-interposition", L"-fno-stack-protector",
                  L"-fcf-protection=none", L"-O2", L"-std=gnu++23", L"-march=x86-64-v3",
                  L"-msse4.1", L"-fno-exceptions", L"-fno-rtti", L"-fno-threadsafe-statics",
                  L"-fno-math-errno", L"-w"};
#endif
    }
    flags.insert(flags.end(), {L"-isystem", PathWide(fsdir / L"include"), L"-I", PathWide(gen)});

    // Codegen emits <partition>.cpp + its own declaration header. Only gtav_init.cpp uses
    // gtav_init.h/gtav_funcs.h (global list). Compile keys include the relevant partition,
    // and all compiler, include and common-header inputs which can change emitted objects.
    struct Unit {
      fs::path source, object, record, staged, log;
      std::string key;
    };
    std::vector<Unit> pending;
    bool cache = IsDesktop();
    // These variables can add include paths outside the packaged, hashed trees. Do not produce
    // or reuse persistent objects with a non-hermetic compiler environment.
    for (const char* name : {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "SDKROOT"})
      if (const char* value = std::getenv(name); value && *value) cache = false;
    flags.push_back(L"--no-default-config");
    std::string common_key;
    if (cache) {
      ui_.Stage(L"Checking the toolchain and cached objects...");
      Sha256 key;
      KeyField(key, "gtav-compile-cache-1");
      for (const auto& flag : flags) KeyField(key, Narrow(flag));
      if (!KeyFile(key, clang, ui_) || !KeyTree(key, root_ / "tools/llvm/lib", ui_) ||
          !KeyTree(key, fsdir / "include", ui_))
        return ui_.Cancelled() ? false : SetError(L"Could not read the compiler or its headers.");
      for (const auto* header : {"gtav_pch.h", "layout_check.h", "gtav_hooks.h"})
        if (!KeyFile(key, gen / header, ui_))
          return ui_.Cancelled() ? false : SetError(L"Could not read the staged compiler headers.");
      common_key = key.Hex();
      if (common_key.empty()) return SetError(L"Could not fingerprint the compiler inputs.");
    }

    auto has_object = [&](const Unit& unit) {
      std::string key, digest, actual;
      uint64_t size = 0, actual_size = 0;
      std::ifstream record(unit.record);
      std::error_code ec;
      return fs::is_regular_file(fs::symlink_status(unit.object, ec)) &&
          record >> key >> digest >> size && key == unit.key && size != 0 &&
          fs::file_size(unit.object, ec) == size && !ec &&
          HashFile(unit.object, &actual, &actual_size, &ui_) && actual == digest && actual_size == size;
    };
    std::set<fs::path> outputs;
    for (const auto& src : sources) {
      if (ui_.Cancelled()) return false;
      const auto stem = PathWide(src.filename());
      const auto object = obj / (stem + kObjExt);
      Unit unit{src, object, obj / (stem + L".key"), obj / (stem + kObjExt + L".new"),
                obj / (stem + L".log"), {}};
      objects_.push_back(object);
      outputs.insert({unit.object, unit.record, unit.log});
      if (cache) {
        Sha256 key;
        KeyField(key, common_key);
        const auto name = NarrowGeneric(src.filename());
        KeyField(key, name);
        auto generated = [&](const std::string& path) {
          if (const auto it = expected_.gen.find(path); it != expected_.gen.end()) {
            KeyField(key, path);
            KeyField(key, it->second.sha256);
          }
        };
        if (src.parent_path() == gen) {
          generated(name);
          if (name == "gtav_init.cpp") {
            generated("gtav_init.h");
            generated("gtav_funcs.h");
          } else {
            generated("gtav_funcs." + name.substr(12, name.size() - 16) + ".h");
          }
        } else if (!KeyFile(key, src, ui_)) {
          return ui_.Cancelled() ? false : SetError(L"Could not read " + PathWide(src));
        }
        unit.key = key.Hex();
        if (unit.key.empty()) return SetError(L"Could not fingerprint a source file.");
        if (has_object(unit)) continue;
      }
      pending.push_back(std::move(unit));
    }
    // Drop obsolete partitions and any partial compiler output from a previous interruption.
    for (auto it = fs::directory_iterator(obj, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (!outputs.contains(it->path()) && !fs::is_directory(it->symlink_status(ec))) {
        if (!ec) fs::remove(it->path(), ec);
        if (ec) break;
      }
    }
    if (ec) return SetError(L"Could not clean the compiler work folder: " + Widen(ec.message()));
    const size_t reused = sources.size() - pending.size();
    Log(L"C++: " + Num(reused) + L" reused, " + Num(pending.size()) + L" to compile.");
    if (pending.empty()) {
      ui_.Progress(kCompileEnd);
      return !ui_.Cancelled();
    }
    const int jobs = JobCount();
    ui_.Stage(L"Compiling " + Num(pending.size()) + L" changed C++ files (" + Num(jobs) + L" jobs)...");
    struct Running {
      const Unit* unit;
      std::unique_ptr<Child> child;
    };
    struct Cleanup {
      const std::vector<Unit>& units;
      ~Cleanup() {
        for (const auto& unit : units) { std::error_code ignored; fs::remove(unit.staged, ignored); }
      }
    } cleanup{pending};
    std::vector<Running> running;
    size_t next = 0, finished = 0;
    while (finished < pending.size()) {
      if (ui_.Cancelled()) return false;  // Child destructors stop the tools before Cleanup runs.
      const size_t before = finished;
      while (!ui_.Cancelled() && running.size() < size_t(jobs) && next < pending.size()) {
        const auto& unit = pending[next++];
        Running r{&unit, std::make_unique<Child>()};
        Args cmd{PathWide(clang)};
        cmd.insert(cmd.end(), flags.begin(), flags.end());
        cmd.insert(cmd.end(), {L"-c", PathWide(unit.source), L"-o", PathWide(unit.staged)});
        if (!r.child->Start(cmd, gen, unit.log)) return SetError(L"Could not start " + std::wstring(kClang));
        running.push_back(std::move(r));
      }
      for (size_t i = 0; i < running.size() && !ui_.Cancelled();) {
        const int code = running[i].child->Poll(i == 0 ? 150 : 0);
        if (code < 0) { ++i; continue; }
        const auto& unit = *running[i].unit;
        if (code != 0)
          return SetError(L"Compiling " + PathWide(unit.source.filename()) + L" failed:\n" +
                          Widen(Tail(unit.log, 1500)));
        std::string digest;
        uint64_t size = 0;
        if (cache) {
          if (!HashFile(unit.staged, &digest, &size, &ui_) || size == 0)
            return ui_.Cancelled() ? false : SetError(L"The compiler did not produce a valid object file.");
        } else {
          size = fs::file_size(unit.staged, ec);
          if (ec || !size) return SetError(L"The compiler did not produce an object file.");
        }
        if (!cache) fs::remove(unit.record, ec);  // never attest a non-hermetic object
        if (ec || !ReplaceFile(unit.staged, unit.object))
          return SetError(L"Could not save " + PathWide(unit.object));
        if (cache && !WriteText(unit.record, unit.key + " " + digest + " " + std::to_string(size) + "\n"))
          Log(L"Could not save the cache record for " + PathWide(unit.object.filename()));
        fs::remove(unit.log, ec);
        ec.clear();
        running.erase(running.begin() + i);
        ++finished;
      }
      if (finished != before)
        ui_.Stage(L"Compiling C++: " + Num(finished) + L" / " + Num(pending.size()) + L" files");
      ui_.Progress(kCheckEnd + double(reused + finished) / double(sources.size()) * (kCompileEnd - kCheckEnd));
    }
    return true;
  }

  //--- iPhone ----------------------------------------------------------------
  // The iPhone build (docs/IOS_PORT.md, "Release"), on any of the three desktop systems: iOS can
  // neither compile code nor translate shaders, so this computer does both from the player's
  // installed game. Same codegen and reference check as the install, compiled with the bundled
  // clang for arm64 iOS and linked with the bundled lld (Mach-O flavour, against our
  // libSystem_ios.tbd: no Apple SDK) into gtav_game.dylib next to the shipped iOS shell (package
  // ios/, no game code); every shader the game has made here (and those the phone saved,
  // iphone/shader_cache) translated for the phone into the app; packed as iphone/GTAV.ipa,
  // unsigned (Sideloadly or AltStore sign it with the player's Apple ID). The phone installs the
  // game files itself from the player's discs (gtav/src/ios_setup.mm).
 public:
  bool RunIphone(std::wstring* error) {
    error_ = error;
    ios_ = true;
    std::error_code ec;
    if (!LockBuild()) return false;
    log_.open(data_ / L"logs" / L"iphone.log", std::ios::app);
    Log(L"---- iPhone build");
    if (GetInstallStatus(root_, home_) != InstallStatus::Ready)
      return SetError(L"Install or recompile the desktop game first, then build for iPhone.");
    const fs::path ios = root_ / L"ios", shell = ios / L"GTAV.app";
    for (const fs::path f : {shell / L"gtav", shell / L"librexruntime.dylib", shell / L"librexgpu-xenos.dylib",
                             shell / L"libMoltenVK.dylib", shell / L"Info.plist", ios / L"libSystem_ios.tbd",
                             ios / L"interface" / L"gtav_game.undefs", ios / L"include" / L"simde" / L"x86" / L"sse4.1.h"}) {
      if (!fs::exists(f, ec))
        return SetError(L"This download has no iPhone files (" + PathWide(fs::relative(f, root_)) + L" is missing).");
    }
#ifdef __APPLE__
    std::wstring err;
    if (!SyncRuntime(root_, home_, &err)) return SetError(err);  // the shader tools in <home>/tools
#endif
    const fs::path out = home_ / L"iphone", app = out / L"Payload" / L"GTAV.app";
    // Built before from this same package: only the shaders (new ones the phone saved) are redone
    // and the ipa repacked; reinstalling it keeps the game data in the app's Documents.
    std::string a, b;
    uint64_t sa = 0, sb = 0;
    const bool app_current = fs::exists(app / L"gtav_game.dylib", ec) && HashFile(app / L"gtav", &a, &sa) &&
                             HashFile(shell / L"gtav", &b, &sb) && a == b;
    if (app_current) {
      Log(L"The iPhone app is current: translating new shaders only");
    } else {
      if (!expected_.Load(root_ / L"tools" / L"game_dll" / L"expected.txt", error)) return false;
      if (!Codegen() || !CheckGenerated() || !Compile()) return Fail();
      fs::remove_all(out / L"Payload", ec);
      fs::create_directories(app, ec);
      fs::copy(shell, app, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
      if (ec) return SetError(L"Could not copy the iPhone app: " + Widen(ec.message()));
      if (!LinkIphone(app / L"gtav_game.dylib")) return Fail();
    }
    // Everything the phone needs but its game files is inside the app: shaders, the known-good
    // hashes its setup screen checks the discs against, and the default arguments.
    if (!PhoneShaders(app / L"spirv", false)) return Fail();
    fs::copy_file(root_ / L"tools" / L"game_dll" / L"expected.txt", app / L"expected.txt",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return SetError(L"Could not copy expected.txt into the iPhone app");
    std::ofstream(app / L"args.txt") << kIphoneArgs;
    fs::remove_all(out / L"Documents", ec);  // (earlier builds' layout)

    ui_.Stage(L"Packing GTAV.ipa...");
    if (!WriteIpa(out / L"Payload", out / L"GTAV.ipa")) return SetError(L"Could not write " + PathWide(out / L"GTAV.ipa"));
    fs::remove_all(BuildDir(), ec);
    Log(L"---- iPhone build finished: " + PathWide(out / L"GTAV.ipa"));
    ui_.Progress(1.0);
    ui_.Stage(L"Done: iphone/GTAV.ipa in the game folder. Install it with Sideloadly or AltStore.");
    return true;
  }

 private:
  // The app's own args.txt (the SDK reads it before Documents/args.txt). No data roots: the setup
  // screen installs the game data into Documents/data (gtav_app.h OnFinalizePaths).
  static constexpr char kIphoneArgs[] =
      "--gpu_plugin=xenos --headless=true\n"
      "--gtav_native=on --gpu_skip_rendering=true --gpu_skip_rendering_lag_ms=6\n"
      "--vsync=false --gtav_native_vsync=true --gtav_fps_limit=30\n"
      "--audio_device_frames=256\n"
      "--log_level=info --log_file=logs/gtav.log --log_max_file_size_mb=16 --log_max_files=2\n"
      "--gtav_native_texture_budget_mb=192\n"
      "--window_presenter=false\n"
      "--gtav_native_fetch_resolves=false\n";

  // The Android build: as the iPhone build, for aarch64 Android. The APK (public download, no game
  // code) dlopens libgtav_game.so from its private storage; this computer makes the game library
  // and the shaders from the player's installed game and packs them, with the known-good hashes
  // the phone's setup screen checks the discs against, into android/gtav-android-pack.zip. The
  // phone installs the game files itself from the player's discs.
 public:
  bool RunAndroid(std::wstring* error) {
    error_ = error;
    android_ = true;
    std::error_code ec;
    if (!LockBuild()) return false;
    log_.open(data_ / L"logs" / L"android.log", std::ios::app);
    Log(L"---- Android build");
    if (GetInstallStatus(root_, home_) != InstallStatus::Ready)
      return SetError(L"Install or recompile the desktop game first, then build for Android.");
    const fs::path pkg = root_ / L"android";
    for (const fs::path f : {pkg / L"link" / L"libgtav_fast.so", pkg / L"link" / L"librexruntime.so",
                             pkg / L"interface" / L"gtav_game.exports", pkg / L"interface" / L"gtav_hooks.h",
                             pkg / L"include" / L"simde" / L"x86" / L"sse4.1.h"}) {
      if (!fs::exists(f, ec))
        return SetError(L"This download has no Android files (" + PathWide(fs::relative(f, root_)) + L" is missing).");
    }
#ifdef __APPLE__
    std::wstring err;
    if (!SyncRuntime(root_, home_, &err)) return SetError(err);  // the shader tools in <home>/tools
#endif
    const fs::path out = home_ / L"android", pack = out / L"pack";
    // The pack belongs to one APK build: its libgtav_fast.so (the hooks the game library binds to).
    std::string shell_hash;
    uint64_t shell_size = 0;
    if (!HashFile(pkg / L"link" / L"libgtav_fast.so", &shell_hash, &shell_size))
      return SetError(L"Could not read android/link/libgtav_fast.so");
    std::string built;
    {
      std::ifstream m(pack / L"pack.txt");
      for (std::string line; std::getline(m, line);)
        if (line.rfind("shell ", 0) == 0) built = line.substr(6);
    }
    if (built == shell_hash && fs::exists(pack / L"libgtav_game.so", ec)) {
      Log(L"The Android game library is current: translating new shaders only");
    } else {
      if (!expected_.Load(root_ / L"tools" / L"game_dll" / L"expected.txt", error)) return false;
      fs::remove_all(pack, ec);
      fs::create_directories(pack, ec);
      if (!Codegen() || !CheckGenerated() || !Compile()) return Fail();
      if (!LinkAndroid(pack / L"libgtav_game.so")) return Fail();
    }
    if (!PhoneShaders(pack / L"spirv", true)) return Fail();
    fs::copy_file(root_ / L"tools" / L"game_dll" / L"expected.txt", pack / L"expected.txt",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return SetError(L"Could not copy expected.txt into the Android pack");
    std::ofstream(pack / L"pack.txt", std::ios::trunc) << "gtav-recomp android pack 1\nshell " << shell_hash << "\n";

    ui_.Stage(L"Packing gtav-android-pack.zip...");
    const fs::path zip = out / L"gtav-android-pack.zip";
    if (!WriteZip(pack, "", zip, [](const std::string&, const fs::path&) { return false; }))
      return SetError(L"Could not write " + PathWide(zip));
    // macOS: the package (with the APK) is inside GTAV.app; put the APK next to the pack, so the one
    // folder Finder shows holds both files for the phone. (Windows / Linux: it is that folder.)
    if (const fs::path apk = root_ / L"android" / L"GTAV.apk"; !fs::equivalent(root_, home_, ec)) {
      fs::copy_file(apk, out / L"GTAV.apk", fs::copy_options::overwrite_existing, ec);
      if (ec) return SetError(L"Could not copy GTAV.apk next to the pack: " + Widen(ec.message()));
    }
    fs::remove_all(BuildDir(), ec);
    Log(L"---- Android build finished: " + PathWide(zip));
    ui_.Progress(1.0);
    ui_.Stage(L"Done: android/gtav-android-pack.zip in the game folder. Copy it and your disc files to the "
              L"phone, then open GTA V there and press Install.");
    return true;
  }

 private:
  // As the Linux Link(), against the APK's own libgtav_fast.so (hooks, bridge) and
  // librexruntime.so (kernel imports): their DT_NEEDED entries put bionic (memcpy & co., log2f) in
  // the same dlopen'd tree. Every package's lld is one multi-format binary: -flavor gnu = ld.lld.
  bool LinkAndroid(const fs::path& out) {
    ui_.Stage(L"Linking libgtav_game.so for Android...");
    fs::path build = BuildDir();
    fs::path pkg = root_ / L"android";
    fs::path rsp = build / L"link_android.rsp";
    {
      std::ofstream f(rsp);
      for (auto& o : objects_) {
        std::u8string g = o.generic_u8string();  // lld reads UTF-8, GNU-style (a backslash escapes)
        f << "\"" << std::string(g.begin(), g.end()) << "\"\n";
      }
    }
    fs::path log = data_ / L"logs" / L"link_android.log";
#ifdef _WIN32
    const fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"lld-link.exe";
#elif defined(__APPLE__)
    const fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"ld64.lld";
#else
    const fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"ld.lld";
#endif
    auto gen = [](const fs::path& p) { return Widen(NarrowGeneric(p)); };
    fs::path tmp = out;
    tmp += ".new";
    Args cmd = {PathWide(lld), L"-flavor", L"gnu", L"-shared", L"-soname", L"libgtav_game.so", L"-Bsymbolic",
                L"-z", L"now", L"-z", L"relro", L"-z", L"noexecstack", L"-z", L"max-page-size=16384",
                L"--hash-style=gnu", L"-O1",
                L"--version-script=" + gen(pkg / L"interface" / L"gtav_game.exports"), L"-o", gen(tmp),
                L"@" + gen(rsp), gen(pkg / L"link" / L"libgtav_fast.so"), gen(pkg / L"link" / L"librexruntime.so")};
    Child c;
    if (!c.Start(cmd, build, log)) return SetError(L"Could not start ld.lld");
    if (c.Poll(INFINITE) != 0) return SetError(L"Linking failed:\n" + Widen(Tail(log, 1500)));
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) return SetError(L"Could not put libgtav_game.so in place: " + Widen(ec.message()));
    Log(L"Built libgtav_game.so for Android");
    return true;
  }

  bool LinkIphone(const fs::path& out) {
    ui_.Stage(L"Linking gtav_game.dylib for iPhone...");
    fs::path build = BuildDir();
    fs::path ios = root_ / L"ios";
    fs::path rsp = build / L"link_ios.rsp";
    {
      // Forward slashes: lld reads response files GNU-style (a backslash escapes).
      std::ofstream f(rsp);
      for (auto& o : objects_) {
        std::u8string g = o.generic_u8string();  // lld reads UTF-8
        f << "\"" << std::string(g.begin(), g.end()) << "\"\n";
      }
      std::ifstream u(ios / L"interface" / L"gtav_game.undefs");
      for (std::string line; std::getline(u, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) f << "-U " << line << "\n";
      }
    }
    fs::path log = data_ / L"logs" / L"link_ios.log";
    // As Link() for macOS, for iOS 17: libSystem_ios.tbd, the iOS shell's librexruntime.dylib. Every
    // package's lld is the same multi-format binary; -flavor darwin picks Mach-O whatever its name.
#ifdef _WIN32
    const fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"lld-link.exe";
#elif defined(__APPLE__)
    const fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"ld64.lld";
#else
    const fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"ld.lld";
#endif
    auto gen = [](const fs::path& p) { return Widen(NarrowGeneric(p)); };
    Args cmd = {PathWide(lld), L"-flavor", L"darwin", L"-arch", L"arm64", L"-platform_version", L"ios",
                L"17.0", L"17.0", L"-dylib", L"-install_name", L"@rpath/gtav_game.dylib", L"-dead_strip_dylibs",
                L"-O1", L"-exported_symbols_list", gen(ios / L"interface" / L"gtav_game.exports"),
                gen(ios / L"libSystem_ios.tbd"), gen(ios / L"GTAV.app" / L"librexruntime.dylib"), L"-o",
                gen(out), L"@" + gen(rsp)};
    Child c;
    if (!c.Start(cmd, build, log)) return SetError(L"Could not start ld64.lld");
    if (c.Poll(INFINITE) != 0) return SetError(L"Linking failed:\n" + Widen(Tail(log, 1500)));
    Log(L"Built gtav_game.dylib for iPhone");
    return true;
  }

  static std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  // Every shader in data/shader_cache (made by the game on this computer) and <phone>/shader_cache
  // (saved by the phone: copy its shader_cache there) -> SPIR-V for the phone, as
  // tools/ios/make_spirv.sh and tools/android/make_spirv.sh do with the same XenosRecomp and dxc the
  // desktop game uses. Android (Adreno) also: 1ull shifts made 32-bit in shader_common.h, every
  // shader's constants from uniform buffers, and 64-bit integers lowered to uvec2 afterwards
  // (adreno_shader_fix.cpp: byte-identical to the Python passes).
  bool PhoneShaders(const fs::path& spirv, bool android) {
    ui_.Stage(android ? L"Translating the shaders for Android..." : L"Translating the shaders for iPhone...");
#ifdef __APPLE__
    const fs::path tools = home_ / L"tools";  // SyncRuntime's copy
#else
    const fs::path tools = root_ / L"tools";
#endif
#ifdef _WIN32
    const wchar_t *kXr = L"XenosRecomp.exe", *kDxc = L"dxc.exe";
#else
    const wchar_t *kXr = L"XenosRecomp", *kDxc = L"dxc";
#endif
    const fs::path work = BuildDir() / (android ? L"spirv_android" : L"spirv_ios");
    const fs::path phone_cache = home_ / (android ? L"android" : L"iphone") / L"shader_cache";
    std::error_code ec;
    fs::create_directories(spirv, ec);
    fs::create_directories(work, ec);
    // Device-address float2 loads are split into scalars (UboConstants maps scalar loads only).
    std::string common = ReadText(tools / L"shader_common.h");
    static const std::regex f2(R"(vk::RawBufferLoad<float2>\((g_PushConstants\.SharedConstants \+ )(\d+)\))");
    common = std::regex_replace(common, f2, "float2(vk::RawBufferLoad<float>($1$2), vk::RawBufferLoad<float>($1$2 + 4))");
    if (common.empty() || common.find("RawBufferLoad<float2>") != std::string::npos)
      return SetError(L"Unexpected shader_common.h in " + PathWide(tools));
    if (android) {
      // The Adreno compiler asserts on 64-bit shifts (tfetchTexcoord).
      const std::string from = "(1ull << semanticIndex)";
      size_t at = common.find(from);
      if (at == std::string::npos) return SetError(L"Unexpected shader_common.h (no 1ull shift) in " + PathWide(tools));
      common.replace(at, from.size(), "(1u << semanticIndex)");
    }
    std::ofstream(work / L"shader_common.h", std::ios::binary) << common;

    std::map<std::string, fs::path> bins;  // name -> container
    for (const fs::path& dir : {phone_cache, data_ / L"shader_cache"}) {
      for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        // Shader containers only (ps_/vs_<hash>.bin): the phone's shader_cache also holds the renderer's
        // pipeline caches (pipelines.bin, vk_pipelines.bin) when the player copies the whole folder.
        if (const std::string stem = it->path().stem().string();
            it->path().extension() == ".bin" && (stem.rfind("ps_", 0) == 0 || stem.rfind("vs_", 0) == 0))
          bins[stem] = it->path();
      }
      ec.clear();
    }
    if (bins.empty())
      return SetError(L"No shaders yet: play on this computer for a while first (the game saves every "
                      L"shader it meets in data/shader_cache), then build for the phone again.");
    Log(L"Translating " + Num(bins.size()) + L" shaders");
    // The sky dome rendered white with uniform-buffer constants on the iPhone (cause not found);
    // Android takes every shader's constants from uniform buffers (make_spirv.sh).
    static const std::set<std::string> kNoUboIos = {"ps_6A340C638FB8D4C2", "vs_5338696E6825DEA1"};
    // The tile override (shader_compiler.cpp kVariants, backend.cpp TileOverride).
    static const std::string kTileShader = "ps_D258EBE32001470C", kTileSuffix = "_tiles2";
    static const std::pair<std::string, std::string> kTileEdits[] = {
        {"r0.y = select(r0.y > 0.0, c255.x, c255.y);", "r0.y = c255.x;"},
        {"oC0.z = select(r0.w > 0.0, r1.x, c255.z);", "oC0.z = select(r0.w > 0.0, max(r1.x, 0.0625), c255.z);"}};
    std::vector<std::pair<std::string, fs::path>> items(bins.begin(), bins.end());
    std::atomic<size_t> next{0}, done{0}, failed{0}, made{0};
    std::atomic<bool> stop{false};
    auto dxc = [&](const fs::path& hlsl, const fs::path& out, bool pixel, const fs::path& log) {
      Args cmd = {PathWide(tools / kDxc), L"-T", pixel ? L"ps_6_0" : L"vs_6_0", L"-HV", L"2021",
                  L"-all-resources-bound", L"-spirv", L"-fvk-use-dx-layout", L"-Wno-ignored-attributes",
                  L"-Qstrip_debug"};
      if (!pixel) cmd.push_back(L"-fvk-invert-y");
      fs::path tmp = out;
      tmp += ".tmp";
      cmd.insert(cmd.end(), {L"-Fo", PathWide(tmp), PathWide(hlsl)});
      Child c;
      if (!c.Start(cmd, work, log) || c.Poll(INFINITE) != 0) return false;
      std::error_code e;
      if (android) {
        // No shaderInt64 on Adreno (Vulkan 1.1): 64-bit integers -> uvec2.
        std::string bytes = ReadText(tmp);
        std::vector<uint32_t> words(bytes.size() / 4);
        if (!words.empty()) std::memcpy(words.data(), bytes.data(), words.size() * 4);
        std::string err;
        if (words.empty() || !gtav::adreno::LowerInt64(words, &err)) {
          std::ofstream(log, std::ios::app) << "lower_int64: " << err << "\n";
          return false;
        }
        std::ofstream(tmp, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
      }
      fs::rename(tmp, out, e);
      return !e;
    };
    auto one = [&](const std::string& name, const fs::path& bin) {
      const bool pixel = name.rfind("ps_", 0) == 0;
      const fs::path out = spirv / (name + ".spv"), hlsl = work / (name + ".hlsl"), log = work / (name + ".log");
      std::error_code e;
      if (fs::exists(out, e) && (name != kTileShader || fs::exists(spirv / (name + kTileSuffix + ".spv"), e)))
        return true;
      made++;
      Child xr;
      if (!xr.Start({PathWide(tools / kXr), PathWide(bin), PathWide(hlsl),
                     PathWide(work / L"shader_common.h")},
                    work, log) ||
          xr.Poll(INFINITE) != 0)
        return false;
      std::string text = ReadText(hlsl);
      if (android || !kNoUboIos.count(name)) {
        std::string err;
        if (!gtav::adreno::UboConstants(text, &err)) {
          std::ofstream(log, std::ios::app) << err << "\n";
          return false;
        }
      }
      std::ofstream(hlsl, std::ios::binary | std::ios::trunc) << text;
      if (!pixel && text.find("= vfetchIndirect(") != std::string::npos) std::ofstream(spirv / (name + ".indirect"));
      // Variant (shader_compiler.cpp kVariants): every sun-lighting tile takes the penumbra path,
      // and fully shadowed tiles still get the per-pixel sun draw (no light 16x16 squares).
      if (name == kTileShader) {
        std::string t = text;
        for (auto& [find, repl] : kTileEdits) {
          size_t at = t.find(find);
          if (at == std::string::npos) return false;
          t.replace(at, find.size(), repl);
        }
        fs::path th = work / (name + kTileSuffix + ".hlsl");
        std::ofstream(th, std::ios::binary) << t;
        if (!dxc(th, spirv / (name + kTileSuffix + ".spv"), true, log)) return false;
      }
      bool ok = dxc(hlsl, out, pixel, log);
      fs::remove(hlsl, e);
      if (ok) fs::remove(log, e);
      return ok;
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < std::max(1, JobCount()); i++)
      workers.emplace_back([&] {
        for (size_t i; !stop && (i = next++) < items.size();) {
          if (!one(items[i].first, items[i].second)) failed++;
          done++;
        }
      });
    while (done < items.size() && !stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (ui_.Cancelled()) stop = true;
      ui_.Progress(kCompileEnd + (1.0 - kCompileEnd) * double(done) / double(items.size()));
    }
    for (auto& w : workers) w.join();
    if (stop) return false;
    Log(L"Shaders: " + Num(items.size()) + L", " + Num(made - failed) + L" newly translated, " + Num(failed) +
        L" failed (logs in " + PathWide(work) + L")");
    if (failed > items.size() / 50) return SetError(Num(failed) + L" shaders could not be translated (see " + PathWide(work) + L").");
    return true;
  }

  int WaitForLink(Child& child) {
    while (!ui_.Cancelled()) {
      const int status = child.Poll(150);
      if (status >= 0) return status;
    }
    child.Kill();
    child.Poll(INFINITE);  // it must stop writing before we remove the staged output
    return 1;
  }

  bool LinkGame(const Args& cmd, const fs::path& build, const fs::path& log, const fs::path& out) {
    if (ui_.Cancelled()) return false;
    std::error_code ec;
    fs::remove(out, ec);
    if (ec) return SetError(L"Could not clear the old temporary game library.");
    Child c;
    if (!c.Start(cmd, build, log)) return SetError(L"Could not start the linker.");
    const int status = WaitForLink(c);
    if (status || ui_.Cancelled()) {
      fs::remove(out, ec);
      return ui_.Cancelled() ? false : SetError(L"Linking failed:\n" + Widen(Tail(log, 1500)));
    }
    const auto size = fs::file_size(out, ec);
    if (ec || size == 0) {
      fs::remove(out, ec);
      return SetError(L"The linker produced no game library.");
    }
    if (!CommitLibrary(out)) {
      fs::remove(out, ec);
      return false;
    }
    Log(L"Built " + std::wstring(kGameLib));
    return true;
  }

  bool CommitLibrary(const fs::path& library) {
    // The old game library is untouched until the new one is linked. Never leave a stamp for
    // a different ABI pointing at the new library, even if the launcher is interrupted here.
    OutFile stamp;
    const auto marker = home_ / kInstalledMarker;
    const std::string text = std::string(kInstallVersion) + "\n" + build_id_ + "\n";
    if (!stamp.Open(marker, error_)) return false;
    if (!stamp.Write(reinterpret_cast<const uint8_t*>(text.data()), text.size()))
      return SetError(L"Could not write the local game build stamp.");
    if (ui_.Cancelled()) return false;
    const auto old_id = InstalledBuildId(home_);
    std::error_code ec;
    if (old_id != build_id_) fs::remove(marker, ec);
    if (ec) return SetError(L"Could not clear the old game build stamp: " + Widen(ec.message()));
    if (!ReplaceFile(library, home_ / kGameLib)) {
      auto original = marker;
      original += ".restore";
      if (!old_id.empty() && old_id != build_id_ &&
          WriteText(original, std::string(kInstallVersion) + "\n" + old_id + "\n"))
        ReplaceFile(original, marker);
      return SetError(L"Could not replace the game library. Close the game and check the folder permissions.");
    }
    if (!stamp.Commit()) return SetError(L"Could not save the local game build stamp.");
    return true;
  }

#ifdef __APPLE__
  bool Link() {
    ui_.Stage(L"Linking gtav_game.dylib...");
    fs::path build = BuildDir();
    fs::path gd = root_ / L"tools" / L"game_dll";
    fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"ld64.lld";
    fs::path rsp = build / L"link.rsp";
    {
      std::ofstream f(rsp);
      for (auto& o : objects_) f << "\"" << o.string() << "\"\n";
      // Hooks and bridge: bound to gtav_fast at load time (Mach-O cannot link against an exe).
      std::ifstream u(gd / L"interface" / L"gtav_game.undefs");
      for (std::string line; std::getline(u, line);)
        if (!line.empty()) f << "-U " << line << "\n";
    }
    fs::path out = home_ / L"gtav_game.dylib.new";
    fs::path log = data_ / L"logs" / L"link.log";
    // Keep in sync with tools/game_dll/build_game_dll.py. Kernel imports bind to
    // librexruntime.dylib, memcpy & co. and log2f to libSystem (libSystem.tbd: names only).
    Args cmd = {PathWide(lld), L"-arch", L"arm64", L"-platform_version", L"macos", L"15.0", L"15.0",
                L"-dylib", L"-install_name", L"@rpath/gtav_game.dylib", L"-dead_strip_dylibs",
                L"-O1", L"-exported_symbols_list", PathWide(gd / L"interface" / L"gtav_game.exports"),
                PathWide(gd / L"libSystem.tbd"), PathWide(root_ / L"librexruntime.dylib"), L"-o",
                PathWide(out), L"@" + PathWide(rsp)};
    return LinkGame(cmd, build, log, out);
  }
#elif !defined(_WIN32)
  bool Link() {
    ui_.Stage(L"Linking gtav_game.so...");
    fs::path build = BuildDir();
    fs::path iface = root_ / L"tools" / L"game_dll" / L"interface";
    fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"ld.lld";
    fs::path rsp = build / L"link.rsp";
    {
      std::ofstream f(rsp);
      for (auto& o : objects_) f << "\"" << o.string() << "\"\n";
    }
    fs::path out = home_ / L"gtav_game.so.new";
    fs::path log = data_ / L"logs" / L"link.log";
    // Keep in sync with tools/game_dll/build_game_dll.py. -Bsymbolic keeps calls between game
    // functions direct; the hooks, bridge, kernel imports and log2f bind to gtav_fast,
    // librexruntime.so and libm at load time (-z now: at once, so a missing one fails at start).
    Args cmd = {PathWide(lld), L"-shared", L"-soname", L"gtav_game.so", L"-Bsymbolic", L"-z", L"now",
                L"-z", L"relro", L"-z", L"noexecstack", L"--hash-style=gnu", L"-O1",
                L"--version-script=" + PathWide(iface / L"gtav_game.exports"), L"-o",
                PathWide(out), L"@" + PathWide(rsp)};
    return LinkGame(cmd, build, log, out);
  }
#else
  bool Link() {
    ui_.Stage(L"Linking gtav_game.dll...");
    fs::path build = BuildDir();
    fs::path iface = root_ / L"tools" / L"game_dll" / L"interface";
    fs::path lld = root_ / L"tools" / L"llvm" / L"bin" / L"lld-link.exe";
    std::vector<fs::path> libs;
    for (const wchar_t* name : {L"gtav_fast", L"rexruntime", L"ucrtbase"}) {
      fs::path lib = build / (std::wstring(name) + L".lib");
      Child c;
      Args cmd = {PathWide(lld), L"/lib", L"/machine:x64",
                  L"/def:" + PathWide(iface / (std::wstring(name) + L".def")),
                  L"/out:" + PathWide(lib)};
      fs::path log = data_ / L"logs" / L"implib.log";
      if (ui_.Cancelled()) return false;
      if (!c.Start(cmd, build, log) || WaitForLink(c) != 0)
        return ui_.Cancelled() ? false : SetError(L"Could not build an import library:\n" + Widen(Tail(log, 800)));
      libs.push_back(lib);
    }
    fs::path rsp = build / L"link.rsp";
    {
      std::ofstream f(rsp);
      for (auto& o : objects_) f << "\"" << Narrow(PathWide(o)) << "\"\n";
      for (auto& l : libs) f << "\"" << Narrow(PathWide(l)) << "\"\n";
    }
    fs::path out = home_ / L"gtav_game.dll.new";
    fs::path log = data_ / L"logs" / L"link.log";
    Args cmd = {PathWide(lld), L"/dll", L"/noentry", L"/nodefaultlib", L"/machine:x64",
                L"/debug:none", L"/def:" + PathWide(iface / L"gtav_game.def"),
                L"/out:" + PathWide(out), L"@" + PathWide(rsp)};
    return LinkGame(cmd, build, log, out);
  }
#endif

  fs::path root_, home_;
  InstallInputs in_;
  InstallUi& ui_;
  fs::path data_;
  std::wstring* error_ = nullptr;
  std::ofstream log_;
  ExpectedSet expected_;
  SetupLock lock_;
  std::string build_id_;
  std::unique_ptr<Source> disc1_, disc2_, tu_;
  std::vector<std::pair<std::string, std::unique_ptr<Source>>> parts_;
  std::map<std::string, const Entry*> tu_files_;
  std::map<std::string, Expected> done_;
  std::map<std::string, VerifiedData> verified_data_;
  std::vector<std::wstring> mismatches_;
  std::vector<fs::path> objects_;
  uint64_t copy_total_ = 0, copied_ = 0;
  bool ios_ = false;      // RunIphone: Compile for arm64 iOS
  bool android_ = false;  // RunAndroid: Compile for aarch64 Android
};

}  // namespace

bool RunDataInstall(const fs::path& home, const fs::path& expected, const InstallInputs& in, InstallUi& ui,
                    std::wstring* error) {
  Installer inst(home, home, in, ui);
  return inst.RunData(expected, error);
}

bool IsDataInstalled(const fs::path& home) {
  std::ifstream f(home / "data" / "data_installed.txt");
  std::string line;
  std::error_code ec;
  return std::getline(f, line) && line == "gtav-recomp data 1" &&
         fs::exists(home / "data" / "game" / "default.xex", ec);
}

InstallStatus GetInstallStatus(const fs::path& root, const fs::path& home) {
  std::error_code ec;
  const auto data = home / "data";
  if (!fs::is_regular_file(data / "game/default.xex", ec) ||
      !fs::is_regular_file(data / "game/default.xexp", ec)) return InstallStatus::NeedsSources;
  ExpectedSet files;
  std::wstring error;
  if (!files.Load(root / "tools/game_dll/expected.txt", &error) ||
      !VisitLocalFiles(files, data, [&](const fs::path& path, const Expected&) {
        return fs::is_regular_file(path, ec);
      })) return InstallStatus::NeedsSources;
  std::string target;
  if (!fs::is_regular_file(home / kGameLib, ec) || fs::file_size(home / kGameLib, ec) == 0 || ec ||
      !ReadPackageBuildId(root, &target) || InstalledBuildId(home) != target) return InstallStatus::NeedsRebuild;
  return InstallStatus::Ready;
}

const wchar_t* InstallStatusText(InstallStatus status) {
  switch (status) {
    case InstallStatus::Ready: return L"Ready to play. Recompile only if you want to rebuild the local game library.";
    case InstallStatus::NeedsRebuild: return L"Game files found. Choose Recompile to build for this release without the discs.";
    default: return L"Choose the three sources, then select Install & build.";
  }
}

#ifdef __APPLE__
bool Preflight(const fs::path& root, std::wstring* error) {
  // The package is arm64 only and its shader compiler (dxc) needs macOS 15.
  char ver[32] = {};
  size_t len = sizeof(ver);
  sysctlbyname("kern.osproductversion", ver, &len, nullptr, 0);
  if (atoi(ver) < 15) {
    *error = L"This needs macOS 15 (Sequoia) or newer; this Mac runs macOS " + Widen(ver) + L".";
    return false;
  }
  for (const wchar_t* f :
       {L"gtav_fast", L"librexruntime.dylib", L"librexgpu-xenos.dylib", L"rexglue",
        L"vulkan/lib/libMoltenVK.dylib", L"tools/llvm/bin/clang++", L"tools/llvm/bin/ld64.lld",
        L"tools/game_dll/expected.txt", L"tools/game_dll/gtav_manifest.toml",
        L"tools/game_dll/fs/gtav_pch.h", L"tools/game_dll/fs/include/simde/x86/sse4.1.h",
        L"tools/game_dll/libSystem.tbd", L"tools/game_dll/interface/gtav_game.undefs"}) {
    std::error_code ec;
    if (!fs::exists(root / f, ec)) {
      *error = std::wstring(L"The app is incomplete: ") + f + L" is missing. Download it again.";
      return false;
    }
  }
  return true;
}

bool CheckGameFolder(const fs::path& home, std::wstring* error) {
  std::error_code ec;
  fs::create_directories(home, ec);
  if (ec) {
    *error = L"Could not create " + PathWide(home) + L": " + Widen(ec.message());
    return false;
  }
  // Xbox paths ignore case: the game data must be on a case-insensitive volume (macOS default).
  if (pathconf(home.c_str(), _PC_CASE_SENSITIVE) == 1) {
    *error = PathWide(home) + L" is on a case-sensitive volume. Pick a folder on a normal "
             L"(case-insensitive) APFS or Mac OS Extended volume.";
    return false;
  }
  return true;
}

// The runtime files gtav_fast needs next to it in the game folder: gtav_fast must find the
// gtav_game.dylib made there (@rpath = its own folder), and the app bundle itself stays read-only.
namespace {

bool SyncRuntime(const fs::path& root, const fs::path& home, std::wstring* error) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(root, ec)) {
    auto name = e.path().filename().string();
    if (name == "gtav_fast" || (e.path().extension() == ".dylib" && e.path().filename() != kGameLib))
      files.push_back(e.path().filename());
  }
  for (const char* dir : {"vulkan", "tools/shaders"}) {
    for (auto it = fs::recursive_directory_iterator(root / dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file()) files.push_back(fs::relative(it->path(), root));
    }
  }
  for (const fs::path& rel : files) {
    fs::path src = root / rel, dst = home / rel;
    if (rel.begin()->string() == "tools") dst = home / "tools" / rel.lexically_relative("tools/shaders");
    std::error_code e1, e2;
    if (fs::exists(dst, e1) && fs::file_size(dst, e1) == fs::file_size(src, e2) &&
        fs::last_write_time(dst, e1) == fs::last_write_time(src, e2))
      continue;
    fs::create_directories(dst.parent_path(), e1);
    fs::path tmp = dst;
    tmp += ".new";
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, e1);
    if (!e1) fs::last_write_time(tmp, fs::last_write_time(src, e2), e2);
    if (e1 || rename(tmp.c_str(), dst.c_str()) != 0) {
      *error = L"Could not copy " + PathWide(rel) + L" into the game folder";
      return false;
    }
  }
  return true;
}

}  // namespace
#elif !defined(_WIN32) && !defined(__ANDROID__)
bool Preflight(const fs::path& root, std::wstring* error) {
  // gtav_fast and the game code are built for x86-64-v3 (AVX2, BMI2, FMA, MOVBE).
  __builtin_cpu_init();
  if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("bmi2") ||
      !__builtin_cpu_supports("fma")) {
    *error = L"This processor has no AVX2. The game needs an x86-64 CPU from about 2013 or later "
             L"(Intel Haswell, AMD Excavator / Zen or newer).";
    return false;
  }
  if (void* vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)) {
    dlclose(vk);
  } else {
    *error = L"The Vulkan loader (libvulkan.so.1) is not installed. Install your distribution's "
             L"Vulkan package (for example libvulkan1 and mesa-vulkan-drivers, or the NVIDIA "
             L"driver) and try again.";
    return false;
  }
  for (const wchar_t* f :
       {L"gtav_fast", L"librexruntime.so", L"librexgpu-xenos.so", L"rexglue",
        L"tools/llvm/bin/clang++", L"tools/llvm/bin/ld.lld", L"tools/game_dll/expected.txt",
        L"tools/game_dll/gtav_manifest.toml", L"tools/game_dll/fs/gtav_pch.h",
        L"tools/game_dll/interface/gtav_game.exports"}) {
    std::error_code ec;
    if (!fs::exists(root / f, ec)) {
      *error = std::wstring(L"The release folder is incomplete: ") + f + L" is missing. "
               L"Unpack the whole release archive again.";
      return false;
    }
  }
  return true;
}
#elif defined(__ANDROID__)
// Android: the setup screen (libgtav_setup.so) uses RunDataInstall only; nothing to check.
bool Preflight(const fs::path&, std::wstring*) { return true; }
#else
bool Preflight(const fs::path& root, std::wstring* error) {
  // gtav_fast.exe and the game code are built for x86-64-v3 (AVX2, BMI2, FMA, MOVBE).
  if (!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) {
    *error = L"This processor has no AVX2. The game needs an x86-64 CPU from about 2013 or later "
             L"(Intel Haswell, AMD Excavator / Zen or newer).";
    return false;
  }
  // The runtime DLLs use the Visual C++ runtime. Packages since 0.3.0 carry it next to the exes
  // (make_release.py); an installed redistributable works too.
  for (const wchar_t* dll : {L"msvcp140.dll", L"msvcp140_atomic_wait.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
    HMODULE m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) {
      *error = std::wstring(L"The Microsoft Visual C++ 2015-2022 Redistributable (x64) is not "
                            L"installed (") + dll + L" is missing). Install it from Microsoft: "
                            L"https://aka.ms/vs/17/release/vc_redist.x64.exe";
      return false;
    }
    FreeLibrary(m);
  }
  for (const wchar_t* f :
       {L"gtav_fast.exe", L"rexruntimerd.dll", L"SDL3rd.dll", L"rexgluerd.exe",
        L"tools\\llvm\\bin\\clang++.exe",
        L"tools\\llvm\\bin\\lld-link.exe", L"tools\\game_dll\\expected.txt",
        L"tools\\game_dll\\gtav_manifest.toml", L"tools\\game_dll\\fs\\gtav_pch.h",
        L"tools\\game_dll\\interface\\gtav_game.def"}) {
    std::error_code ec;
    if (!fs::exists(root / f, ec)) {
      *error = std::wstring(L"The launcher folder is incomplete: ") + f + L" is missing. "
               L"Unpack the whole release zip again.";
      return false;
    }
  }
  return true;
}
#endif

bool RunInstall(const fs::path& root, const InstallInputs& in, InstallUi& ui, std::wstring* error) {
  Installer inst(root, root, in, ui);
  return inst.Run(error);
}

bool RunInstall(const fs::path& root, const fs::path& home, const InstallInputs& in, InstallUi& ui,
                std::wstring* error) {
  Installer inst(root, home, in, ui);
  return inst.Run(error);
}

bool RunRebuild(const fs::path& root, const fs::path& home, InstallUi& ui, std::wstring* error) {
  Installer inst(root, home, InstallInputs{}, ui);
  return inst.Rebuild(error);
}

bool RunIphoneBuild(const fs::path& root, const fs::path& home, InstallUi& ui, std::wstring* error) {
  Installer inst(root, home, InstallInputs{}, ui);
  return inst.RunIphone(error);
}

bool RunAndroidBuild(const fs::path& root, const fs::path& home, InstallUi& ui, std::wstring* error) {
  Installer inst(root, home, InstallInputs{}, ui);
  return inst.RunAndroid(error);
}

#ifdef _WIN32
bool LaunchWindowsGame(const fs::path& root, std::wstring* error, const std::vector<std::wstring>& extra,
                       win::Handle* process) {
#elif defined(__APPLE__)
bool LaunchMacGame(const fs::path& package, const fs::path& root, std::wstring* error,
                   const std::vector<std::wstring>& extra) {
#else
bool LaunchGame(const fs::path& root, std::wstring* error, const std::vector<std::wstring>& extra) {
#endif
  fs::path data = root / L"data";
  std::error_code ec;
  fs::create_directories(data, ec);
  if (ec) { *error = L"Could not create the game data folder: " + Widen(ec.message()); return false; }
#ifdef _WIN32
  // Keep setup/update out of this installation until the game exits, even if this launcher closes.
  win::Handle lock(win::OpenSetupLock(data / "setup.lock", true));
  if (!lock || fs::exists(root / ".gtav-update/pending.txt", ec) || ec ||
      GetInstallStatus(root, root) != InstallStatus::Ready) {
    *error = L"The installation is not ready or is being used by another game, update or setup.";
    return false;
  }
#else
  SetupLock lock;
#ifdef __APPLE__
  const auto& package_root = package;
#else
  const auto& package_root = root;
#endif
  if (ec || !lock.Acquire(data / "setup.lock") || GetInstallStatus(package_root, root) != InstallStatus::Ready) {
    *error = L"The installation is not ready or is being used by another game, update or setup.";
    return false;
  }
#ifdef __APPLE__
  if (!SyncRuntime(package, root, error)) return false;
#endif
#endif
  for (const auto* dir : {"spirv", "logs", "user"}) {
    fs::create_directories(data / dir, ec);
    if (ec) { *error = L"Could not prepare the game data folder: " + Widen(ec.message()); return false; }
  }
  // Read the exe and DLLs once so they are in the file cache: slow first page-ins during boot
  // were seen to hang it (see play.ps1).
  std::vector<uint8_t> buf(8u << 20);
  for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    auto ext = Lower(Narrow(PathWide(it->path().extension())));
    if (ext != ".dll" && ext != ".so" && ext != ".dylib" && it->path().filename() != kGameExe)
      continue;
    std::wstring err;
    if (auto b = OpenFileBlob(PathWide(it->path()), &err)) {
      for (uint64_t off = 0; off < b->size(); off += buf.size())
        b->ReadAt(off, buf.data(), size_t(std::min<uint64_t>(buf.size(), b->size() - off)));
    }
  }
  const auto install_root = fs::absolute(root, ec);
  if (ec) { *error = L"Could not resolve the installed game folder: " + Widen(ec.message()); return false; }
  const auto absolute_data = install_root / L"data";
  auto arg = [](const std::wstring& name, const fs::path& p) {
    return L"--" + name + L"=" + PathWide(p);
  };
  Args args = {PathWide(install_root / kGameExe)};
  const Args defaults = {arg(L"game_data_root", absolute_data / L"game"),
      arg(L"user_data_root", absolute_data / L"user"), arg(L"update_data_root", absolute_data / L"update"),
      arg(L"log_file", absolute_data / L"logs" / L"gtav.log"),
      arg(L"gtav_native_spirv_dir", absolute_data / L"spirv"),
      arg(L"gtav_settings_path", absolute_data / L"config" / L"graphics.ini"), L"--gpu_plugin=xenos",
      L"--headless=true", L"--log_level=info", L"--log_max_file_size_mb=256", L"--log_max_files=2",
#ifdef __APPLE__
      // The runtime's own window would open in a fullscreen Space of its own; the native renderer
      // hides it, leaving the player looking at an empty black Space.
      L"--fullscreen=false",
#endif
      L"--gtav_native=on", L"--gpu_skip_rendering=true", L"--vsync=false", L"--gtav_mode=menu"};
  // CLI11 rejects duplicated value options. A caller can replace any launcher default in
  // --name=value or --name value form (including the SDK's --no-name boolean alias).
  auto name = [](std::wstring_view opt) {
    if (!opt.starts_with(L"--")) return std::wstring_view{};
    opt.remove_prefix(2);
    return opt.substr(0, opt.find(L'='));
  };
  for (const auto& option : defaults) {
    const auto key = name(option);
    const bool overridden = std::any_of(extra.begin(), extra.end(), [&](const auto& opt) {
      const auto explicit_key = name(opt);
      return explicit_key == key || (explicit_key.starts_with(L"no-") && explicit_key.substr(3) == key);
    });
    if (!overridden) args.push_back(option);
  }
  args.insert(args.end(), extra.begin(), extra.end());
#ifndef _WIN32
  if (!lock.KeepInChild()) {
    *error = L"Could not pass the game folder lock to the runtime.";
    return false;
  }
#endif
#ifdef __APPLE__
  // The launcher app quits; the game runs on in its own session, its output in data/logs.
  std::vector<std::string> narrow = NarrowArgs(args);
  std::vector<char*> argv = Argv(narrow);
  fs::path out = data / L"logs" / L"gtav_stdout.txt";
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
#ifndef LOADER_NO_SPAWN
  posix_spawn_file_actions_addchdir_np(&fa, root.c_str());
#endif
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
  pid_t pid;
  int err = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (err != 0) {
    *error = L"Could not start " + args[0] + L": " + Widen(strerror(err));
    return false;
  }
  return true;
#elif !defined(_WIN32)
  // The launcher becomes the game: it keeps the terminal and the exit code.
  if (chdir(root.c_str()) != 0) {
    *error = L"Could not enter " + PathWide(root);
    return false;
  }
  std::vector<std::string> narrow = NarrowArgs(args);
  std::vector<char*> argv = Argv(narrow);
  execv(argv[0], argv.data());
  *error = L"Could not start " + args[0] + L": " + Widen(strerror(errno));
  return false;
#else
  HANDLE inherit[] = {lock.Get()};
  if (!win::Spawn(install_root / kGameExe, win::CommandLine(args), install_root, 0, process, inherit)) {
    *error = L"Could not start gtav_fast.exe";
    return false;
  }
  return true;
#endif
}

#ifdef _WIN32
bool LaunchGame(const fs::path& root, std::wstring* error, const std::vector<std::wstring>& extra) {
  return LaunchWindowsGame(root, error, extra, nullptr);
}

bool HashUpdateArchive(const fs::path& path, std::string* digest, uint64_t* size, InstallUi& ui) {
  return HashFile(path, digest, size, &ui);
}
#endif

}  // namespace loader
