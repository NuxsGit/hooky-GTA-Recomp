#pragma once

#include "native/graphics_preferences.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); std::exit(1); \
} } while (false)

namespace graphics_fixture {
namespace gfx = gtav::native::settings;
namespace fs = std::filesystem;
using gfx::Key;
using gfx::Index;

struct Temp {
  fs::path root = fs::temp_directory_path() / (fs::path(u8"GTAV Recomp réglages ").native() +
      fs::path(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())).native());
  Temp() { CHECK(fs::create_directory(root)); }
  ~Temp() { std::error_code ec; fs::remove_all(root, ec); }
};

inline void Write(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary);
  f.write(text.data(), std::streamsize(text.size()));
  f.close();
  CHECK(f);
}

inline std::string Read(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  CHECK(f);
  return {std::istreambuf_iterator<char>(f), {}};
}

struct Flags {
  std::array<std::string, Index(Key::Count)> values{
      "30", "false", "false", "true", "true", "4", "false", "30", "false", "-1", "1.000000", "false", "false", "false", "true",
      "false", "false", "0", "0", "50", "false", "false", "false", "0"};
  std::optional<Key> reject;
  std::string& operator[](Key key) { return values[Index(key)]; }
  gfx::Preferences Open(const fs::path& path, const fs::path& legacy = {}, gfx::Locks forced = {},
                        bool ignore = false, gfx::Platform platform = gfx::Platform::Windows) {
    gfx::Preferences prefs(path, legacy, ignore, platform, forced, [this](Key key) { return (*this)[key]; },
        [this](Key key, std::string_view value) {
          if (reject == key) return false;
          (*this)[key] = value;
          return true;
        });
    prefs.Load();
    return prefs;
  }
};
}  // namespace graphics_fixture
