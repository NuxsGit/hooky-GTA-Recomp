// Windows packaging smoke test ONLY. No game data/code and never shipped with the launcher.
#include <windows.h>
#include <rex/image_info.h>

extern "C" const rex::PPCImageInfo* gtav_game_image_config() {
  static constexpr rex::PPCImageInfo empty{};
  return &empty;
}

extern "C" void unexpected_guest_call() { ExitProcess(86); }
