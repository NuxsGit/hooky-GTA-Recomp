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
    ImGui::SetNextWindowSize({680.f, std::min(550.f, io.DisplaySize.y - 60.f)});
    CHECK(ImGui::Begin("Graphics fixture"));
    if (gfx::DrawGraphicsMenu(prefs, {1920, 1080}, 60, 16.7, info, 1)) CHECK(prefs.Save() == gfx::SaveState::Saved);
    ImGui::End();
    ImGui::Render();
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 200);
  };
  auto click_at = [&](ImVec2 pt) {
    io.AddMousePosEvent(pt.x, pt.y); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    frame();
  };
  auto click = [&](const char* name) {
    CHECK(labels.contains(name) && rects.contains(labels[name]));
    click_at(rects.at(labels.at(name)).GetCenter());
  };
  auto combo = [&](Key key, const char* name, const char* choice) {
    // BeginCombo registers its rectangle but doesn't send a label to the test engine.
    const int id = int(key);
    const auto seed = ImHashData(&id, sizeof(id), labels.at("Realism"));
    const auto r = rects.at(ImHashStr(name, 0, seed));
    click_at({r.Min.x + 50, r.GetCenter().y});
    click(choice);
  };
  frame(); frame();
  click("FPS counter");
  CHECK(flags[Key::Counter] == "true" && Read(path).find("gtav_native_fps=true\n") != std::string::npos);
  click("Performance");
  click("Balanced");
  CHECK(flags[Key::Fsr1] == "true" && flags[Key::SceneScale] == "1.000000" && prefs.PendingRestart());
  CHECK(fresh.Open(path).Value(Key::SceneScale) == "0.875");
  click("Ultra");
  CHECK(prefs.PendingRestart() && prefs.Value(Key::SceneScale) == "1.5");
  CHECK(prefs.Value(Key::MaterialFiltering) == "true" && flags[Key::MaterialFiltering] == "false");
  click("Potato");
  CHECK(prefs.PendingRestart() && prefs.Value(Key::LowMemory) == "true" && flags[Key::LowMemory] == "false");
  CHECK(fresh.Open(path).Value(Key::SceneScale) == "0.5" && fresh[Key::MaterialFiltering] == "false");
  click("Extra Potato");
  CHECK(prefs.PendingRestart() && prefs.Value(Key::SceneScale) == "0.333333" && prefs.Value(Key::ExtraPotato) == "true");
  CHECK(fresh.Open(path).Value(Key::SceneScale) == "0.333333" && fresh[Key::ExtraPotato] == "true" && fresh[Key::LowMemory] == "true");
  click("Standard");
  CHECK(!prefs.PendingRestart() && flags[Key::Fsr1] == "false" && flags[Key::FrameLimit] == "30");
  CHECK(fresh.Open(path).Value(Key::LowMemory) == "false");
  click("Exact texture and buffer checks");
  CHECK(flags[Key::ExactCache] == "true" && fresh.Open(path).Value(Key::ExactCache) == "true");
  click("Latency & support");
  io.DisplaySize = {960, 540}; frame();
  CHECK(labels.contains("Display") && labels.contains("Performance") && labels.contains("Latency & support"));

  // The read-only tab fits Potato's Auto window. Render both an unavailable snapshot and a
  // real one, including a soft-target overage; neither should touch the preferences.
  const auto saved = Read(path);
  click("Memory");
  const int waiting = ImGui::GetDrawData()->TotalVtxCount;
  info.lean_memory = true;
  info.memory = GraphicsMemoryStats{{200 * kMiB, 192 * kMiB}, {54 * kMiB, 64 * kMiB}, 16 * kMiB, 48 * kMiB};
  frame();
  CHECK(ImGui::GetDrawData()->TotalVtxCount > waiting && Read(path) == saved && !prefs.PendingRestart());
  info.lean_memory = false;
  info.memory->converted_buffers.target = 0;
  frame();
  CHECK(ImGui::GetDrawData()->TotalVtxCount > waiting && Read(path) == saved);
  click("Realism");
  info.realism = {8, 16, ContactRayStatus::NoAlignedDepth};
  frame();
  combo(Key::MaterialAnisotropy, "Material filtering (restart)", "16x minimum");
  CHECK(flags[Key::MaterialAnisotropy] == "0" && prefs.Pending(Key::MaterialAnisotropy));
  CHECK(fresh.Open(path).Value(Key::MaterialAnisotropy) == "16");
  combo(Key::ContactRays, "Contact rays", "Medium / 20 steps");
  CHECK(flags[Key::ContactRays] == "2" && !prefs.Pending(Key::ContactRays));
  const auto strength = rects.at(labels.at("Contact strength"));
  click_at({strength.Min.x + 206, strength.GetCenter().y});
  CHECK(std::stoi(flags[Key::ContactStrength]) > 50 && flags[Key::ContactStrength] == fresh.Open(path).Value(Key::ContactStrength));
  for (auto status : {ContactRayStatus::Unavailable, ContactRayStatus::Recorded}) {
    info.realism.contact_rays = status;
    frame();
  }
  combo(Key::ContactRays, "Contact rays", "Off");
  CHECK(flags[Key::ContactRays] == "0" && fresh.Open(path).Value(Key::ContactRays) == "0");
  ImGui::DestroyContext();
  std::puts("F1: saved live options, realism controls, restart presets and read-only memory counters passed");
}
