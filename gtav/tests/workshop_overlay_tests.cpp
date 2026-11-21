// Headless test of the actual ImGui panels, using the SDK's pinned ImGui sources.
#include <imgui.h>
#include <imgui_internal.h>
#include "sandbox/BuildMenuUI.h"
#include "sandbox/CreativeRuntime.h"
#include "sandbox/desktop.h"
#include "phone/AdminCheatApp.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)
using namespace gtav::workshop;
static DesktopView v;
static std::vector<Command> sent;
static bool owner = true;
static Preview preview;
static std::map<ImGuiID, ImRect> rects;
static std::map<std::string, ImGuiID, std::less<>> labels;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) { rects.insert_or_assign(id, rect); }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) { if (label) labels.insert_or_assign(label, id); }
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return "fixture"; }
namespace gtav::workshop {
bool BuildAllowed() { return owner && v.build; }
bool LocalAuthority() { return owner; }
bool Fresh(const DesktopView&) { return true; }
bool Submit(Command cmd, bool) { sent.push_back(std::move(cmd)); return true; }
void SetPreview(uint32_t hash, Placement p) { preview = {hash, p, 1}; }
}
namespace gtav::sandbox {
Mode CurrentMode() { return Mode::Solo; }
Session& Party() { static Session party; return party; }
Session::~Session() = default;
SessionView Session::View() const { return {}; }
}

static bool admin = false;
void Frame() {
  labels.clear(); rects.clear();
  auto& io = ImGui::GetIO();
  ImGui::NewFrame();
  if (admin) {
    ImGui::SetNextWindowSize(ImVec2(760, 565), ImGuiCond_Always);
    ImGui::Begin("Admin fixture");
    const auto self = std::find_if(v.players.begin(), v.players.end(), [](const PlayerView& p) { return p.id == kLocalOwner; });
    CHECK(self != v.players.end());
    DrawAdminCheatApp(v, *self, 1.f);
    ImGui::End();
  } else CHECK(DrawBuildMenu(v, 1.f, {0, 0, {unsigned(io.DisplaySize.x), unsigned(io.DisplaySize.y)}}));
  ImGui::Render();
  if (!admin || owner) CHECK(ImGui::GetDrawData()->TotalVtxCount > 200);
}
ImVec2 At(const char* label) {
  if (!labels.contains(label)) { for (const auto& [s, _] : labels) std::fprintf(stderr, "[%s]\n", s.c_str()); }
  CHECK(labels.contains(label) && rects.contains(labels[label]));
  return rects.at(labels.at(label)).GetCenter();
}
void Move(ImVec2 pt) { ImGui::GetIO().AddMousePosEvent(pt.x, pt.y); Frame(); }
void Click(ImVec2 pt, int button = 0) {
  Move(pt);
  ImGui::GetIO().AddMouseButtonEvent(button, true); Frame();
  ImGui::GetIO().AddMouseButtonEvent(button, false); Frame();
}
void Click(const char* label) { Click(At(label)); }

int main() {
  ImGui::CreateContext();
  ImGui::GetCurrentContext()->TestEngineHookItems = true;
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1280, 720);
  unsigned char* font; int fw, fh;
  io.Fonts->GetTexDataAsRGBA32(&font, &fw, &fh);
  v.authority = v.playable = v.build = v.admin = v.practice = true;
  v.caps = {0xffffffffu, 0xffffffffu, 0xffffffffu};
  v.limit = 48;
  v.undo = v.redo = true;
  v.players.push_back({kLocalOwner, "You", Seat::Local, Role::Owner, Mode::Creative, {}, true});
  v.hour = 13;
  Frame(); Frame();
  Click("##model-search");
  io.AddInputCharactersUTF8("roadcone"); Frame();
  CHECK(labels.contains("Traffic cone   /   Barriers & roads"));
  CHECK(!labels.contains("Concrete wall   /   Walls & floors"));
  // Press Z while the real search input has keyboard focus: it must not queue Undo.
  io.AddKeyEvent(ImGuiKey_Z, true); Frame();
  io.AddKeyEvent(ImGuiKey_Z, false); Frame();
  CHECK(sent.empty());
  Click("Traffic cone   /   Barriers & roads");
  CHECK(preview.model == ModelHash("prop_roadcone02a"));
  Move({975, 375});
  io.AddMouseWheelEvent(0.f, 1.f); Frame(); CHECK(preview.opts.yaw == 15.f);
  Click({975, 375});
  CHECK(sent.size() == 1 && sent.back().op == Op::Place && sent.back().placement->yaw == 15.f);
  Click({975, 375}, 1); CHECK(preview.model == 0);
  io.AddKeyEvent(ImGuiKey_Z, true); Frame();
  io.AddKeyEvent(ImGuiKey_Z, false); Frame(); CHECK(sent.back().op == Op::Undo);
  // Cheats is a first-class workshop panel, not a page of a phone.
  admin = true; Frame(); Frame();
  CHECK(labels.contains("MAP"));
  Click("MAP");
  CHECK(labels.contains("Allow Players to build in Creative") && labels.contains("Pieces / player"));
  owner = false; Frame();
  CHECK(labels.contains("Cheats locked") && !labels.contains("Allow Players to build in Creative"));
  ImGui::DestroyContext();
  std::puts("ImGui: filter, Z typing guard, placement wheel/click/cancel, cheats panel and live revocation passed");
}
