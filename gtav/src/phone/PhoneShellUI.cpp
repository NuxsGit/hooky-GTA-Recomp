#include "PhoneShellUI.h"

#include "AdminCheatApp.h"
#include "../sandbox/CreativeRuntime.h"
#include "../sandbox/desktop.h"

#include <imgui.h>

namespace gtav::workshop {
namespace {
enum class Page { Home, Admin, Status };
Page page = Page::Home;
}
void ResetPhone() { page = Page::Home; }
PhoneAction DrawPhone(const DesktopView& v, unsigned fps, float scale) {
  const auto view = sandbox::CurrentMode() == sandbox::Mode::Solo ? sandbox::SessionView{} : sandbox::Party().View();
  PlayerView unknown{kLocalOwner, "You", Seat::Local, Role::Player, v.map.mode, {}, false, false, 0};
  const auto self = std::find_if(v.players.begin(), v.players.end(), [](const PlayerView& p) { return p.id == kLocalOwner; });
  const auto& local = self != v.players.end() ? *self : unknown;
  const bool authority = Fresh(v) && v.admin && LocalAuthority() && PermissionManager::AdminOS(local.role);
  const bool build = v.build && BuildAllowed();
  if (!authority && page == Page::Admin) page = Page::Home;
  const auto screen = ImGui::GetIO().DisplaySize;
  const float margin = 14.f * scale;
  const ImVec2 size{std::min(430.f * scale, screen.x - margin * 2), std::min(570.f * scale, screen.y - margin * 2)};
  const ImVec4 mint(.40f, .96f, .76f, 1.f);
  ImGui::SetNextWindowPos({screen.x - margin, screen.y - margin}, ImGuiCond_Always, {1, 1});
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 17.f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.f * scale, 12.f * scale});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, {.035f, .068f, .086f, .98f});
  ImGui::PushStyleColor(ImGuiCol_Button, {.083f, .21f, .22f, 1.f});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.10f, .31f, .30f, 1.f});
  bool open = true;
  PhoneAction action = PhoneAction::None;
  if (ImGui::Begin("Workshop phone###phone-shell", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    if (v.hour) ImGui::TextColored(mint, "%02u h", *v.hour); else ImGui::TextColored(mint, "-- h");
    ImGui::SameLine();
    const auto latency = view.rtt_ms ? std::to_string(*view.rtt_ms) + " ms TCP" :
        view.link == sandbox::Link::Joined || view.link == sandbox::Link::Connecting ? "RTT --" : "Local";
    ImGui::Text("%s   /   %u FPS   /   %s", authority ? Label(local.role) : "Player", fps, latency.c_str());
    ImGui::Separator();
    if (page != Page::Home) {
      if (ImGui::SmallButton("< Home")) page = Page::Home;
      ImGui::SameLine(); ImGui::TextColored(mint, "%s", page == Page::Admin ? "ADMIN OS" : "SESSION STATUS");
      ImGui::Spacing();
    }
    if (page == Page::Admin) DrawAdminCheatApp(v, local);
    else if (page == Page::Status) {
      if (view.link == sandbox::Link::Joined) ImGui::TextUnformatted("Joined private room");
      else ImGui::Text("%s   /   %s", Label(local.mode), v.playable ? "Free roam" : "Waiting for free roam");
      if (!view.world.players.empty()) {
        ImGui::SeparatorText("Private room / pose sync only");
        for (const auto& p : view.world.players) ImGui::BulletText("%s%s", p.name.c_str(), p.id == view.world.self ? " (you)" : "");
        ImGui::TextWrapped("RTT is the local TCP estimate when available. AdminOS actions are not replicated over v2.");
      } else ImGui::TextWrapped("Workshop uses its own saves. There is no connection to GTA Online.");
    } else {
      ImGui::TextColored(mint, "WORKSHOP   /   HOME");
      ImGui::TextUnformatted("Los Santos toolkit");
      if (view.link == sandbox::Link::Joined) ImGui::TextDisabled("Private room / pose sync only");
      else ImGui::TextDisabled("%s world", Label(local.mode));
      ImGui::Spacing();
      const float tile = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
      if (ImGui::Button("SESSION\nWorkshop F6", {tile, 67.f * scale})) action = PhoneAction::Session;
      ImGui::SameLine();
      if (ImGui::Button("STATUS\nWorld & room", {tile, 67.f * scale})) page = Page::Status;
      if (build && ImGui::Button("BUILD\nCreative tools", {tile, 67.f * scale})) action = PhoneAction::Build;
      if (authority) {
        if (build) ImGui::SameLine();
        if (ImGui::Button("ADMIN OS\nHost controls", {tile, 67.f * scale})) page = Page::Admin;
      }
      ImGui::Spacing();
      ImGui::TextDisabled("P / Up to open   |   Esc / B to put away");
      if (sandbox::CurrentMode() == sandbox::Mode::Multiplayer)
        ImGui::TextWrapped("Build / AdminOS on this PC do not sync with v2 guests.");
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);
  return open ? action : PhoneAction::Close;
}
}  // namespace gtav::workshop
