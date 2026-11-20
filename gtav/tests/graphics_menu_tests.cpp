// Real F1 widgets with the pinned SDK ImGui, a temporary preference file and fake cvars.
#include "graphics_fixture.h"
#include "native/graphics_menu_ui.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <map>

using namespace gtav::native;
using namespace graphics_fixture;
static std::map<ImGuiID, ImRect> rects;
static std::map<std::string, ImGuiID, std::less<>> labels;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) { rects.insert_or_assign(id, rect); }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) { if (label) labels.insert_or_assign(label, id); }
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return "graphics fixture"; }

int main() {
  Temp dir;
  Flags flags;
  auto path = dir.root / "data/config/graphics.ini";
  Flags fresh;
  gfx::Locks locks{};
  locks[Index(Key::FrameLimit)] = true;
  auto prefs = flags.Open(path, {}, locks);
  prefs.BeginRun();
  ImGui::CreateContext();
  ImGui::GetCurrentContext()->TestEngineHookItems = true;
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = {1280, 720};
  unsigned char* font; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&font, &w, &h);
  PresentationInfo info{{1280, 720}, {1920, 1080}, UpscaleStatus::kFsr1};
  info.adapter = "Fixture Vulkan GPU";
  info.anti_lag = AntiLagStatus::Unsupported;
  auto frame = [&] {
    labels.clear(); rects.clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({40.f, 40.f});
    ImGui::SetNextWindowSize({680.f, 550.f});
    CHECK(ImGui::Begin("Graphics fixture"));
    if (gfx::DrawGraphicsMenu(prefs, {1920, 1080}, 60, 16.7, info, 1)) CHECK(prefs.Save() == gfx::SaveState::Saved);
    ImGui::End();
    ImGui::Render();
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 200);
  };
  auto click = [&](const char* name) {
    CHECK(labels.contains(name) && rects.contains(labels[name]));
    const auto pt = rects.at(labels.at(name)).GetCenter();
    io.AddMousePosEvent(pt.x, pt.y); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    frame();
  };
  frame(); frame();
  click("FPS counter");
  CHECK(flags[Key::Counter] == "true" && Read(path).find("gtav_native_fps=true\n") != std::string::npos);
  click("Performance");
  click("Balanced");
  CHECK(flags[Key::Fsr1] == "true" && flags[Key::SceneScale] == "1.000000" && prefs.PendingRestart());
  CHECK(fresh.Open(path).Value(Key::SceneScale) == "0.875");
  click("Best image");
  CHECK(!prefs.PendingRestart() && flags[Key::Fsr1] == "false" && flags[Key::FrameLimit] == "30");
  click("Exact texture and buffer checks");
  CHECK(flags[Key::ExactCache] == "true" && fresh.Open(path).Value(Key::ExactCache) == "true");
  click("Latency & support");
  io.DisplaySize = {800, 580}; frame();
  CHECK(labels.contains("Display") && labels.contains("Performance") && labels.contains("Latency & support"));
  ImGui::DestroyContext();
  std::puts("F1: real ImGui tab changes, saved live options and reversible restart preset passed");
}
