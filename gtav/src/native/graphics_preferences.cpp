#include "graphics_preferences.h"
#include "graphics_policy.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gtav::native::settings {
namespace fs = std::filesystem;
namespace {
constexpr size_t kMaxBytes = 16 * 1024;

std::string_view Trim(std::string_view s) {
  constexpr std::string_view kWhitespace = " \t\r";
  const auto first = s.find_first_not_of(kWhitespace);
  if (first == s.npos) return {};
  return s.substr(first, s.find_last_not_of(kWhitespace) - first + 1);
}

std::optional<int> Int(std::string_view s) {
  if (s.empty()) return {};
  int result = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
  if (ec != std::errc{} || end != s.data() + s.size()) return {};
  return result;
}

std::string Range(std::string_view s, int low, int high) {
  const auto number = Int(s);
  return number && *number >= low && *number <= high ? std::to_string(*number) : std::string{};
}

enum class ReadResult { Missing, Ready, Failed };
ReadResult ReadFile(const fs::path& path, std::string& bytes) {
  if (path.empty()) return ReadResult::Missing;
  std::error_code ec;
  const auto status = fs::status(path, ec);
  if (status.type() == fs::file_type::not_found) return ReadResult::Missing;
  if (ec || !fs::is_regular_file(status)) return ReadResult::Failed;
  std::ifstream in(path, std::ios::binary);
  if (!in) return ReadResult::Failed;
  bytes.resize(kMaxBytes + 1);
  in.read(bytes.data(), std::streamsize(bytes.size()));
  if (in.bad() || !in.eof() || size_t(in.gcount()) > kMaxBytes) return ReadResult::Failed;
  bytes.resize(size_t(in.gcount()));
  return ReadResult::Ready;
}

bool ReplaceFile(const fs::path& path, const std::string& contents) {
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  if (ec) return false;
  // Exclusive creation protects a concurrent writer's temporary file. The launcher already
  // serializes games using this home. A timestamped suffix also avoids old interrupted writes.
  auto tmp = path;
  tmp += ".new-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
  FILE* out = _wfopen(tmp.c_str(), L"wbx");
#else
  FILE* out = std::fopen(tmp.c_str(), "wbx");
#endif
  if (!out) return false;
  const bool written = std::fwrite(contents.data(), 1, contents.size(), out) == contents.size();
  const bool closed = std::fclose(out) == 0;
  if (written && closed) {
#ifdef _WIN32
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
    fs::rename(tmp, path, ec);
    if (!ec) return true;
#endif
  }
  fs::remove(tmp, ec);
  return false;
}
}  // namespace

std::optional<Key> FindKey(std::string_view name) {
  for (size_t i = 0; i < kSettings.size(); ++i) if (kSettings[i].name == name) return Key(i);
  return {};
}

std::string CanonicalValue(Key key, std::string_view value) {
  value = Trim(value);
  if (value.empty() || value.size() > 32) return {};
  switch (key) {
    case Key::FrameLimit: return Range(value, 0, 1000);
    case Key::GuestGpuLag: return Range(value, 0, 64);
    case Key::TextureRecheck: return Range(value, 1, 30);
    case Key::ContactRays: return Range(value, 0, 3);
    case Key::ContactStrength: return Range(value, 0, 100);
    case Key::MaterialAnisotropy: {
      const auto n = Int(value);
      if (n == 0 || n == 1 || n == 2 || n == 4 || n == 8 || n == 16) return std::to_string(*n);
      return {};
    }
    case Key::Output: {
      const auto n = Int(value);
      if (n == -1 || n == 0 || n == 540 || n == 720 || n == 1080 || n == 1440 || n == 2160) return std::to_string(*n);
      return {};
    }
    case Key::SceneScale: {
      // Locale independent; also works on libc++ versions without floating-point from_chars.
      // Allow Extra Potato down to 0.333 (GTA IV faces on ultra-low hardware); Potato stays at 0.5.
      std::istringstream in{std::string(value)};
      in.imbue(std::locale::classic());
      double scale;
      if (!(in >> std::noskipws >> scale) || !in.eof() || !std::isfinite(scale) || scale < double(kMinSceneScale) - 1e-6 || scale > kUltraSceneScale)
        return {};
      std::ostringstream out;
      out.imbue(std::locale::classic());
      out << std::setprecision(6) << scale;
      return out.str();
    }
    case Key::Count: return {};
    default:
      if (value == "true" || value == "1") return "true";
      if (value == "false" || value == "0") return "false";
      return {};
  }
}

Preferences::Preferences(fs::path path, fs::path legacy, bool ignore_file, Platform platform,
                         Locks forced, Get get, Set set)
    : path_(std::move(path)), legacy_(std::move(legacy)), ignored_(ignore_file), platform_(platform),
      forced_(forced), get_(std::move(get)), set_(std::move(set)) {}

void Preferences::Load() {
  if (std::exchange(loaded_, true)) return;
  if (ignored_) { save_state_ = SaveState::Ignored; return; }
  std::string data;
  const fs::path* chosen = &path_;
  auto result = ReadFile(*chosen, data);
  if (result == ReadResult::Missing && path_ != legacy_) {
    chosen = &legacy_;
    result = ReadFile(*chosen, data);
  }
  if (result == ReadResult::Failed) { read_failed_ = true; return; }
  if (result == ReadResult::Missing) return;
  loaded_path_ = *chosen;
  std::string_view lines = data;
  if (lines.starts_with("\xef\xbb\xbf")) lines.remove_prefix(3);
  while (!lines.empty()) {
    const auto end = lines.find('\n');
    auto line = Trim(lines.substr(0, end));
    lines.remove_prefix(end == lines.npos ? lines.size() : end + 1);
    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
    const auto eq = line.find('=');
    if (eq == line.npos) { ++invalid_entries_; continue; }
    const auto key = FindKey(Trim(line.substr(0, eq)));
    if (!key) continue;  // no arbitrary cvars from a graphics preference file
    auto value = CanonicalValue(*key, line.substr(eq + 1));
    if (value.empty()) { ++invalid_entries_; continue; }
    saved_[Index(*key)] = std::move(value);
  }
  for (size_t i = 0; i < saved_.size(); ++i) {
    const auto key = Key(i);
    if (Available(key) && !Forced(key) && !saved_[i].empty() && !set_(key, saved_[i])) {
      ++invalid_entries_;
      saved_[i].clear();
    }
  }
}

void Preferences::BeginRun() {
  if (std::exchange(running_, true)) return;
  for (size_t i = 0; i < boot_.size(); ++i)
    if (Available(Key(i)) && Spec(Key(i)).restart) boot_[i] = CanonicalValue(Key(i), get_(Key(i)));
}

bool Preferences::Available(Key key) const { return platform_ >= Spec(key).minimum; }

bool Preferences::CanEdit(Key key) const {
  return Available(key) && !Forced(key) && !(ignored_ && running_ && Spec(key).restart);
}

std::string Preferences::Value(Key key) const {
  const auto& saved = saved_[Index(key)];
  if (running_ && Spec(key).restart && !Forced(key) && !saved.empty()) return saved;
  return get_(key);
}

bool Preferences::Pending(Key key) const {
  const auto& saved = saved_[Index(key)];
  return running_ && Available(key) && Spec(key).restart && !Forced(key) &&
         !saved.empty() && saved != boot_[Index(key)];
}

bool Preferences::PendingRestart() const {
  for (size_t i = 0; i < saved_.size(); ++i) if (Pending(Key(i))) return true;
  return false;
}

bool Preferences::Edit(Key key, std::string_view value) {
  if (!CanEdit(key)) return false;
  auto canonical = CanonicalValue(key, value);
  if (canonical.empty()) return false;
  if (!(running_ && Spec(key).restart) && !set_(key, canonical)) return false;
  if (!ignored_ && saved_[Index(key)] != canonical) {
    saved_[Index(key)] = std::move(canonical);
    dirty_ = true;
  }
  return true;
}

SaveState Preferences::Save() {
  if (ignored_ || !dirty_) return save_state_;
  // A vanished directory can be recreated, but never overwrite a file we did not manage to read
  // at startup. The UI explains when the user needs to check the file and relaunch first.
  if (read_failed_) return save_state_ = SaveState::Failed;
  std::string data = "# GTA V Recomp graphics preferences. Explicit CLI and environment overrides win.\n";
  for (size_t i = 0; i < saved_.size(); ++i) {
    if (!saved_[i].empty()) {
      data += kSettings[i].name;
      data += '=';
      data += saved_[i];
      data += '\n';
    }
  }
  if (!ReplaceFile(path_, data)) return save_state_ = SaveState::Failed;
  dirty_ = false;
  return save_state_ = SaveState::Saved;
}

}  // namespace gtav::native::settings
