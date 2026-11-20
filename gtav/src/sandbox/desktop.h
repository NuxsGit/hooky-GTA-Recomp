#pragma once

#ifdef GTAV_DESKTOP_WORKSHOP
#include "../frontend/modes.h"
#include "paths.h"
#include "session.h"
#include "scene.h"
#include "campaign.h"

namespace rex { struct PathConfig; }

namespace gtav::sandbox {
using frontend::Mode;
Mode CurrentMode();
bool Active();
// Runs on the main UI thread before the retail module opens any saves.
bool Configure(rex::PathConfig& paths, std::filesystem::path& installed_content_root);
void Shutdown();
Session& Party();

// Bridge between the native UI and the guarded TU27 guest tick; one local request at a time.
enum class Guest { Loading, Ready, Busy, Unavailable };
void ReportGuest(Guest state);
struct VehicleRequest { unsigned preset; int64_t deadline; };
std::optional<VehicleRequest> TakeVehicleRequest();
bool SceneEnabled();
void ReportSceneUnavailable();
void ReportVehicle(const char* message);

// Render thread.
void DrawWorkshop();
void DrawHud();
}  // namespace gtav::sandbox
#endif
