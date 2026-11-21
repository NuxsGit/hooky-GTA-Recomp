#include "AdminCheatApp.h"

#include "../sandbox/CreativeRuntime.h"

#include <imgui.h>

#include <array>
#include <cstdio>

namespace gtav::workshop {
namespace {
struct Panel {
  unsigned tab{};
  PlayerId player = kLocalOwner;
  int weapon{}, dest{}, weather{}, hour = 12;
  uint32_t vehicle = ModelHash("sultan");
  bool favorites{}, warp = true;
  char reason[161]{};
};
void Button(const char* label, bool enabled, Command cmd) {
  ImGui::BeginDisabled(!enabled);
  if (ImGui::Button(label)) Submit(std::move(cmd));
  ImGui::EndDisabled();
}
void GtaButton(const char* label, bool enabled, Command cmd, ImVec2 size = {}) {
  ImGui::BeginDisabled(!enabled);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.13f, 0.14f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.85f, 0.45f, 0.30f));
  if (ImGui::Button(label, size)) Submit(std::move(cmd));
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
}
void Check(const char* label, bool enabled, bool value, Command cmd) {
  ImGui::BeginDisabled(!enabled);
  if (ImGui::Checkbox(label, &value)) { cmd.on = value; Submit(std::move(cmd)); }
  ImGui::EndDisabled();
}
void PickMode(const char* label, const PlayerView& p, bool enabled) {
  int selected = p.override_mode ? int(*p.override_mode) : 2;
  ImGui::BeginDisabled(!enabled);
  if (ImGui::Combo(label, &selected, "Creative\0Survival\0Map default\0"))
    Submit({Op::PlayerMode, p.id, unsigned(selected)});
  ImGui::EndDisabled();
}
const PlayerView& Target(Panel& ui, const DesktopView& v, const PlayerView& local) {
  auto pick = std::find_if(v.players.begin(), v.players.end(), [&](const PlayerView& p) { return p.id == ui.player; });
  if (pick == v.players.end() || !PermissionManager::Target(local.role, pick->role, pick->id == local.id)) {
    ui.player = local.id;
    pick = std::find_if(v.players.begin(), v.players.end(), [&](const PlayerView& p) { return p.id == local.id; });
  }
  if (ImGui::BeginCombo("Target", pick == v.players.end() ? "Self" : pick->name.c_str())) {
    for (auto it = v.players.begin(); it != v.players.end(); ++it) {
      ImGui::PushID(int(it->id));
      const bool allowed = PermissionManager::Target(local.role, it->role, it->id == local.id);
      ImGui::BeginDisabled(!allowed);
      char name[90];
      std::snprintf(name, sizeof name, "%s / %s%s###seat", it->name.c_str(), Label(it->role), it->seat == Seat::Simulated ? " (sim)" : "");
      if (ImGui::Selectable(name, it == pick)) { ui.player = it->id; pick = it; ui.reason[0] = 0; }
      ImGui::EndDisabled(); ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return pick == v.players.end() ? local : *pick;
}
void MapPage(const DesktopView& v, const PlayerView& local) {
  PickMode("Personal mode", local, true);
  int mode = int(v.map.mode);
  if (ImGui::Combo("Map default", &mode, "Creative\0Survival\0")) Submit({.op = Op::MapMode, .value = unsigned(mode)});
  ImGui::TextWrapped("Survival closes Build and clears Creative movement effects. Explicit player modes are kept when the map default changes.");
  ImGui::SeparatorText("Builder policy");
  Check("Allow Players to build in Creative", true, v.players_build, {.op = Op::BuildRule, .value = v.limit});
  static int limit;
  static bool dragging;
  if (!dragging) limit = int(v.limit);
  const bool changed = ImGui::SliderInt("Pieces / player", &limit, 1, 128);
  dragging = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit() || (changed && !dragging))
    Submit({.op = Op::BuildRule, .value = unsigned(limit), .on = v.players_build});
  ImGui::SeparatorText("Local map rules");
  constexpr const char* rules[]{"PvP", "Fall damage", "Traffic", "Pedestrians", "Police dispatch"};
  for (unsigned i = 0; i < std::size(rules); ++i)
    Check(rules[i], v.caps.Has(ForRule(Rule(i))), v.map.Get(Rule(i)), {.op = Op::MapRule, .value = i});
  ImGui::TextDisabled("PvP and fall-damage authority not wired to TU27.");
}
void MovementPage(const DesktopView& v, const PlayerView& local) {
  const bool can = v.playable && local.ready;
  constexpr const char* flags[]{"Noclip", "Fly (collisions stay on)", "God mode", "Unlimited stamina", "Super jump", "Fast run (1.49x)", "Never wanted"};
  for (unsigned i = 0; i < std::size(flags); ++i)
    Check(flags[i], can && v.caps.Has(Effect(i)), v.cheats.Has(Effect(i)), {.op = Op::Effect, .value = i});
  constexpr unsigned speeds[]{1, 3, 8, 15};
  int speed = int(std::find(std::begin(speeds), std::end(speeds), v.cheats.speed) - std::begin(speeds));
  ImGui::BeginDisabled(!can);
  if (ImGui::Combo("Flight speed", &speed, "x1\0x3\0x8\0x15\0")) Submit({.op = Op::Speed, .value = speeds[speed]});
  ImGui::EndDisabled();
  if (!v.caps.Has(Effect::Noclip)) ImGui::TextDisabled("Noclip needs the extended TU27 ABI opt-in.");
  ImGui::TextWrapped("Close cheat panel (F5) to steer: ZQSD (WASD positions), Space up, Ctrl down. On foot only. Works in all modes.");
  if (local.mode != Mode::Creative) ImGui::TextDisabled("Dev cheat active in all modes (host-only by default, grantable) — no longer Creative-only.");
  GtaButton("Health 200 + armor 100", can && v.caps.Has(Tool::Heal), {Op::Heal});
  GtaButton("Clear blood and weapon damage", can && v.caps.Has(Tool::Damage), {Op::Damage});
}
void GivePage(Panel& ui, const DesktopView& v, const PlayerView& local) {
  const auto& target = Target(ui, v, local);
  const bool can = v.playable && target.ready;
  ImGui::SeparatorText("Armory");
  const auto weapons = Weapons();
  if (ImGui::BeginCombo("Weapon", weapons[size_t(ui.weapon)].name.data())) {
    ImGuiListClipper clipper;
    clipper.Begin(int(weapons.size()));
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      if (ImGui::Selectable(weapons[size_t(i)].name.data(), ui.weapon == i)) ui.weapon = i;
    }
    ImGui::EndCombo();
  }
  GtaButton("Give selected", can && v.caps.Has(Tool::Weapon), {Op::Weapon, target.id, weapons[size_t(ui.weapon)].hash});
  ImGui::SameLine(); GtaButton("Give armory", can && v.caps.Has(Tool::Weapon), {Op::Armory, target.id});
  ImGui::TextWrapped("Only weapons available in the loaded TU27 build are given. Bulk give streams one request per frame.");
  Check("Infinite clip (self)", v.playable && v.caps.Has(Effect::InfiniteAmmo),
        v.cheats.Has(Effect::InfiniteAmmo), {.op = Op::Effect, .value = unsigned(Effect::InfiniteAmmo)});

  ImGui::SeparatorText("Quick vehicle (self)");
  ImGui::Checkbox("Favorites only", &ui.favorites);
  const auto* picked = FindModel(ui.vehicle);
  if (ImGui::BeginCombo("Vehicle", picked ? picked->name.data() : "Select")) {
    for (const auto& m : Models()) if (m.kind == Kind::Vehicle &&
        (!ui.favorites || std::find(v.favorites.begin(), v.favorites.end(), m.hash) != v.favorites.end())) {
      if (ImGui::Selectable(m.name.data(), ui.vehicle == m.hash)) ui.vehicle = m.hash;
    }
    ImGui::EndCombo();
  }
  ImGui::BeginDisabled(!v.caps.Has(Tool::Warp)); ImGui::Checkbox("Driver seat + start engine", &ui.warp); ImGui::EndDisabled();
  GtaButton("Spawn in front", v.playable && v.caps.Has(Feature::Vehicle) && picked && !v.busy,
         {.op = Op::QuickVehicle, .value = ui.vehicle, .on = ui.warp && v.caps.Has(Tool::Warp)});
  GtaButton("Repair current vehicle", v.playable && v.caps.Has(Tool::Repair), {Op::Repair});
  ImGui::SameLine(); GtaButton("Clean", v.playable && v.caps.Has(Tool::Clean), {Op::Clean});

  ImGui::SeparatorText("Workshop wallet");
  ImGui::Text("Balance: %u credits (not GTA story cash)", target.wallet);
  GtaButton("+1,000", true, {Op::Cash, target.id, 1000}); ImGui::SameLine();
  GtaButton("+50,000", true, {Op::Cash, target.id, 50000}); ImGui::SameLine();
  GtaButton("+1,000,000", true, {Op::Cash, target.id, 1000000});
  GtaButton("Survival kit", can && v.caps.Has(Tool::Heal) && v.caps.Has(Tool::Weapon), {Op::Kit, target.id});
}
void WorldPage(Panel& ui, const DesktopView& v) {
  const auto dest = Destinations();
  GtaButton("Travel to waypoint", v.playable && v.caps.Has(Feature::Waypoint), {Op::Waypoint});
  if (ImGui::BeginCombo("Landmark", dest[size_t(ui.dest)].name)) {
    for (size_t i = 0; i < dest.size(); ++i) if (ImGui::Selectable(dest[i].name, int(i) == ui.dest)) ui.dest = int(i);
    ImGui::EndCombo();
  }
  GtaButton("Travel to landmark", v.playable && v.caps.Has(Feature::Teleport) && v.caps.Has(Feature::Ground), {.op = Op::Landmark, .value = unsigned(ui.dest)});
  ImGui::TextWrapped("Checks collision and ground before moving. Drivers take their vehicle; passengers can't travel this way.");
  ImGui::SeparatorText("World");
  const auto weather = WeatherNames();
  ImGui::Combo("Weather", &ui.weather, weather.data(), int(weather.size()));
  GtaButton("Apply weather", v.playable && v.caps.Has(Tool::Weather), {.op = Op::Weather, .value = unsigned(ui.weather)});
  ImGui::SliderInt("Hour", &ui.hour, 0, 23);
  GtaButton("Set clock", v.playable && v.caps.Has(Tool::Clock), {.op = Op::Time, .value = unsigned(ui.hour)});
  Check("Freeze clock", v.playable && v.caps.Has(Tool::PauseClock), v.clock_frozen, {Op::PauseClock});
  Check("Lunar gravity", v.playable && v.caps.Has(Tool::Gravity), v.low_gravity, {Op::Gravity});
  if (!v.caps.Has(Feature::Ground)) ImGui::TextDisabled("Ground streaming / weather require extended TU27 ABI opt-in.");
}
void PlayersPage(Panel& ui, const DesktopView& v, const PlayerView& local) {
  if (v.practice) {
    GtaButton("Load / respawn three practice players", v.playable && v.caps.Has(Feature::Ped), {Op::Practice});
    ImGui::TextDisabled("Simulated peds, not online accounts.");
  } else ImGui::TextWrapped("Private v2 peers are in F6. The v2 link doesn't authenticate a durable player identity.");
  const auto& p = Target(ui, v, local);
  ImGui::Text("%s / %s%s", Label(p.role), Label(p.mode), p.frozen ? " / anchored" : "");
  ImGui::TextDisabled("%s%s", p.seat == Seat::Simulated ? "Simulated / " : "", p.ready ? "entity ready" : "no live entity");
  PickMode("Mode override", p, true);
  const bool others = local.id != p.id, ready = v.playable && p.ready;
  const bool moderate = PermissionManager::Moderate(local.role, p.role, !others);
  GtaButton("Go to player", ready && others && v.caps.Has(Feature::Teleport), {Op::GoTo, p.id});
  ImGui::SameLine(); GtaButton("Bring player", ready && others && v.caps.Has(Feature::Teleport), {Op::Bring, p.id});
  Check("Anchor / freeze", ready && moderate && v.caps.Has(Feature::Anchor), p.frozen, {Op::Freeze, p.id});
  GtaButton("Heal + armor", ready && v.caps.Has(Tool::Heal), {Op::Heal, p.id});
  ImGui::SameLine(); GtaButton("Revive", ready && others && v.caps.Has(Tool::Revive), {Op::Revive, p.id});
  GtaButton("Give survival kit", ready && v.caps.Has(Tool::Heal) && v.caps.Has(Tool::Weapon), {Op::Kit, p.id});
  if (local.role == Role::Owner && others) {
    GtaButton(p.role == Role::Admin ? "Demote to Player" : "Promote to Admin", p.role != Role::Owner,
           {.op = Op::Promote, .target = p.id, .on = p.role != Role::Admin});
  }
  ImGui::SeparatorText("Moderation");
  ImGui::BeginDisabled(!moderate);
  ImGui::InputTextWithHint("##moderation-reason", "Required reason", ui.reason, sizeof ui.reason);
  GtaButton("Kick", ValidText(Trim(ui.reason), 160), {.op = Op::Kick, .target = p.id, .text = ui.reason});
  ImGui::SameLine(); GtaButton("Ban", ValidText(Trim(ui.reason), 160), {.op = Op::Ban, .target = p.id, .text = ui.reason});
  ImGui::EndDisabled();
  if (ImGui::CollapsingHeader("Persistent ban list")) {
    for (size_t i = 0; i < v.bans.size(); ++i) {
      const auto& b = v.bans[i]; ImGui::PushID(int(i));
      ImGui::TextUnformatted(b.name.c_str()); ImGui::SameLine(); ImGui::TextDisabled("%s", b.utc.c_str());
      ImGui::TextWrapped("%s", b.reason.c_str());
      GtaButton("Unban", true, {.op = Op::Unban, .text = b.identity});
      ImGui::PopID();
    }
    if (v.bans.empty()) ImGui::TextDisabled("No workshop bans");
  }
}
}  // namespace

void DrawAdminCheatApp(const DesktopView& v, const PlayerView& local, float scale) {
  const bool authority = v.admin && LocalAuthority() && PermissionManager::AdminOS(local.role);
  if (!authority) {
    // Locked — never render a blank page. The panel stays open so the user sees why it is
    // locked and what they need (owner/admin role in an authoritative/solo/host session).
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.35f, 1.f));
    ImGui::BeginDisabled();
    ImGui::Button("Cheats locked", ImVec2(-1.f, 32.f * scale));
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::TextWrapped("Admin OS needs the owner or admin role in an authoritative session (solo or a host/home session).");
    ImGui::TextWrapped("You are %s%s.", Label(local.role), !v.admin ? " (not admin)" : "");
    ImGui::TextDisabled("Run solo, host a local session, or join with an owner/admin credential to unlock this panel.");
    ImGui::TextDisabled("Cheats execute through the authoritative core, never over the network.");
    return;
  }
  static Panel ui;
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * scale);
  constexpr const char* tabs[]{"MAP", "MOVE", "GIVE", "WORLD", "PLAYERS"};
  constexpr const char* tab_labels[]{"Gamemode & Map", "Movement", "Give", "Teleport & World", "Players & Moderation"};
  for (unsigned i = 0; i < std::size(tabs); ++i) {
    if (i) ImGui::SameLine(0, 6);
    const bool active = ui.tab == i;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.85f, 0.45f, 1.f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.f));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.14f, 0.15f, 1.f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.86f, 1.f));
    }
    if (ImGui::Button(tabs[i])) ui.tab = i;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tab_labels[i]);
    ImGui::PopStyleColor(2);
  }
  ImGui::PopStyleVar();
  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.18f, 0.85f, 0.45f, 1.f), "%s", tab_labels[ui.tab]);
  ImGui::TextDisabled("%s", v.build ? "Build allowed" : "Build disabled");
  ImGui::SameLine();
  ImGui::TextDisabled("%u/%u pieces", v.count, v.total);
  ImGui::Separator();
  if (ImGui::BeginChild("admin-scroll", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing() * 1.25f))) {
    if (!v.playable) ImGui::TextDisabled("Waiting for a controllable TU27 player in free roam.");
    if (v.storage_error) ImGui::TextColored({1.f, .65f, .27f, 1.f}, "Config save error / incompatible version");
    switch (ui.tab) {
      case 0: MapPage(v, local); break;
      case 1: MovementPage(v, local); break;
      case 2: GivePage(ui, v, local); break;
      case 3: WorldPage(ui, v); break;
      case 4: PlayersPage(ui, v, local); break;
    }
  }
  ImGui::EndChild();
  ImGui::TextDisabled("%s", Label(v.result));
}
}  // namespace gtav::workshop
