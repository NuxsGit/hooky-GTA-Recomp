#pragma once

#include "../core/WorkshopCore.h"

#include <filesystem>
#include <memory>

namespace gtav::workshop {
// Desktop bridge. The render/input threads see only immutable views and push bounded requests;
// the guest script thread owns all authority/effects, and the disk worker writes config files.
struct RuntimeLog {
  std::function<void(std::string_view)> audit;   // guest thread
  std::function<void(uint8_t)> storage_failed;  // disk worker, document mask
};
void Start(const std::filesystem::path& profile, bool practice, RuntimeLog log = {});
void Stop();
struct DesktopView : CoreView {
  int64_t sampled_at{};
  uint8_t storage_error{};
};
std::shared_ptr<const DesktopView> Snapshot();
bool Fresh(const DesktopView&);
bool LocalAuthority();
bool IsCheatAllowed(const DesktopView& v, const PlayerView& local);
bool BuildAllowed();
bool Submit(Command, bool build_panel = false);
void SetCapture(bool build, bool admin_panel, bool other = false);
bool ActiveCapture();
void SetPreview(uint32_t model, Placement);
bool FlightKey(unsigned bit, bool down);
void ResetInput();
bool WantsFrame();
// Called only by the existing guarded native timer hook. Return whether model streaming is held.
bool GuestFrame(WorkshopGame&, Entity local, uint64_t frame, bool can_stream);
}  // namespace gtav::workshop
