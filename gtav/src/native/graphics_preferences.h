// Player choices, independent of ReXGlue and the renderer. The startup hub and F1 share
// one instance; the app transfers ownership from the UI thread to the render thread at boot.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gtav::native::settings {

enum class Key {
  FrameLimit, Vsync, Counter, SunShadows, AsyncPipelines, GuestGpuLag, ExactCache,
  TextureRecheck, Touch, Output, SceneScale, Fsr1, ShortQueue, AmdAntiLag, FsrSharpen, Count
};

constexpr size_t Index(Key key) { return static_cast<size_t>(key); }
struct Setting {
  std::string_view name;
  bool restart = false;
  bool windows_only = false;
};
inline constexpr std::array<Setting, Index(Key::Count)> kSettings{{
    {"gtav_fps_limit"},
    {"gtav_native_vsync"},
    {"gtav_native_fps"},
    {"gtav_native_tile_penumbra_all"},
    {"gtav_native_async_pipelines"},
    {"gpu_skip_rendering_lag_ms"},
    {"gtav_native_exact_cache"},
    {"gtav_native_texture_recheck_frames"},
    {"gtav_touch_controls"},
    {"gtav_native_output", true, true},
    {"gtav_native_render_scale", true, true},
    {"gtav_native_fsr1", false, true},
    {"gtav_native_short_queue", true, true},
    {"gtav_native_amd_anti_lag", true, true},
    {"gtav_native_fsr1_sharpen", false, true},
}};
constexpr const Setting& Spec(Key key) { return kSettings[Index(key)]; }
std::optional<Key> FindKey(std::string_view name);
std::string CanonicalValue(Key key, std::string_view value);  // empty = invalid

using Locks = std::array<bool, Index(Key::Count)>;
enum class SaveState { Unchanged, Saved, Ignored, Failed };

class Preferences {
 public:
  using Get = std::function<std::string(Key)>;
  using Set = std::function<bool(Key, std::string_view)>;

  Preferences(std::filesystem::path path, std::filesystem::path legacy, bool ignore_file,
              bool windows, Locks forced, Get get, Set set);
  // Applies valid saved values once. No writes on startup, even when importing the legacy file.
  void Load();
  // Before starting guest work: capture the startup values, then stage edits of startup flags
  // without changing cvars the current renderer has already sampled (or will sample lazily).
  void BeginRun();

  bool Available(Key key) const;
  bool Forced(Key key) const { return forced_[Index(key)]; }
  bool CanEdit(Key key) const;
  std::string Value(Key key) const;
  std::string_view BootValue(Key key) const { return boot_[Index(key)]; }
  bool Pending(Key key) const;
  bool PendingRestart() const;
  bool Edit(Key key, std::string_view value);
  SaveState Save();

  const std::filesystem::path& Path() const { return path_; }
  const std::filesystem::path& LoadedPath() const { return loaded_path_; }
  bool Ignored() const { return ignored_; }
  bool ReadFailed() const { return read_failed_; }
  unsigned InvalidEntries() const { return invalid_entries_; }
  SaveState LastSave() const { return save_state_; }

 private:
  using Values = std::array<std::string, Index(Key::Count)>;
  std::filesystem::path path_, legacy_, loaded_path_;
  bool ignored_, windows_;
  Locks forced_;
  Get get_;
  Set set_;
  Values saved_{}, boot_{};
  bool loaded_ = false, running_ = false, dirty_ = false, read_failed_ = false;
  unsigned invalid_entries_ = 0;
  SaveState save_state_ = SaveState::Unchanged;
};

}  // namespace gtav::native::settings
