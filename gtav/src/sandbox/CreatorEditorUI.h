#pragma once

#include "CreatorManager.h"
#include "CreativeRuntime.h"

#include <imgui.h>

namespace gtav::sandbox {

struct CreatorEditorView {
  CreatorManager* manager = nullptr;
  workshop::DesktopView workshop_view{};
  float scale = 1.f;
  bool is_playing = false;
};

// Unity/Unreal style editor: hierarchy | viewport | inspector + asset browser + toolbar
// Returns false if user closes the editor window
bool DrawCreatorEditor(const CreatorEditorView& view);
void UpdateCreatorPreview(uint32_t model_hash, workshop::Placement placement);

} // namespace gtav::sandbox
