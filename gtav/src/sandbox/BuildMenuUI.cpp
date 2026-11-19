#include "BuildMenuUI.h"

#include "CreativeRuntime.h"

#include <imgui.h>

#include <array>
#include <cstdio>

namespace gtav::workshop {
namespace {
struct Panel {
  char search[96]{}, preset_name[41]{};
  uint32_t model{};
  PieceId duplicate{};
  std::string preset;
  Placement place;
};

void Rows(Panel& ui, const DesktopView& v, std::span<const Model* const> rows, float height) {
  if (ImGui::BeginChild("model-list", {0, height}, ImGuiChildFlags_Borders)) {
    ImGuiListClipper clipper;
    clipper.Begin(int(rows.size()));
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const auto& m = *rows[size_t(i)];
      ImGui::PushID(i);
      ImGui::BeginDisabled(!v.caps.Has(ForKind(m.kind)));
      char label[144];
      const bool star = std::find(v.favorites.begin(), v.favorites.end(), m.hash) != v.favorites.end();
      std::snprintf(label, sizeof label, "%s%s   /   %s", star ? "* " : "", m.name.data(), m.category.data());
      if (ImGui::Selectable(label, ui.model == m.hash && ui.preset.empty())) {
        ui.model = m.hash;
        ui.duplicate = 0;
        ui.preset.clear();
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s / 0x%08X", m.internal.data(), unsigned(m.hash));
      ImGui::PopID();
    }
    if (rows.empty()) ImGui::TextDisabled("No matches");
  }
  ImGui::EndChild();
}

void Presets(Panel& ui, const DesktopView& v, float scale) {
  ImGui::TextWrapped("Aim at one of your pieces. Save it and nearby pieces (25 m, max 16) relative to that anchor.");
  ImGui::SetNextItemWidth(154.f * scale);
  ImGui::InputTextWithHint("##preset-name", "Preset name", ui.preset_name, sizeof ui.preset_name);
  ImGui::SameLine();
  ImGui::BeginDisabled(!v.aimed || !*ui.preset_name);
  if (ImGui::Button("Save aimed group")) Submit({.op = Op::SavePreset, .piece = v.aimed, .text = ui.preset_name}, true);
  ImGui::EndDisabled();
  if (ImGui::BeginChild("presets", {0, 106.f * scale}, ImGuiChildFlags_Borders)) {
    for (const auto& p : v.presets) {
      ImGui::PushID(p.name.c_str());
      char label[90];
      std::snprintf(label, sizeof label, "%s   /   %u pieces###preset-row", p.name.c_str(), unsigned(p.pieces.size()));
      if (ImGui::Selectable(label, ui.preset == p.name)) { ui.preset = p.name; ui.model = p.pieces.front().model; ui.duplicate = 0; }
      ImGui::PopID();
    }
    if (v.presets.empty()) ImGui::TextDisabled("No saved groups");
  }
  ImGui::EndChild();
  if (!ui.preset.empty() && ImGui::Button("Remove selected preset")) {
    if (Submit({.op = Op::DeletePreset, .text = ui.preset}, true)) { ui.preset.clear(); ui.model = 0; }
  }
}
}  // namespace

bool DrawBuildMenu(const DesktopView& v, float scale, native::OutputRect area) {
  static Panel ui;
  bool open = true;
  const auto screen = ImGui::GetIO().DisplaySize;
  const ImVec4 accent(.65f, .62f, 1.f, 1.f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.059f, .066f, .104f, .97f));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(.22f, .20f, .43f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.19f, .18f, .34f, 1.f));
  ImGui::SetNextWindowPos({15.f * scale, 16.f * scale}, ImGuiCond_Always);
  ImGui::SetNextWindowSize({std::min(615.f * scale, screen.x * .62f), std::min(650.f * scale, screen.y - 31.f * scale)}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(.97f);
  if (ImGui::Begin("Workshop / Build  [F8]", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextColored(accent, "CREATIVE   /   BUILD TOOLKIT");
    ImGui::Text("%u / %u yours   |   %u total active", v.count, v.limit, v.total);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##model-search", "Search a model, name or 0x hash", ui.search, sizeof ui.search);
    if (ImGui::BeginTabBar("build-tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      constexpr const char* labels[]{"Props & architecture", "Vehicles", "NPCs & entities", "Favorites / recent", "Presets"};
      for (int tab = 0; tab < int(std::size(labels)); ++tab) {
        if (!ImGui::BeginTabItem(labels[tab])) continue;
        if (tab == 4) Presets(ui, v, scale);
        else {
          std::vector<const Model*> list;
          const auto push = [&](const Model* model) { if (model && Matches(*model, ui.search)) list.push_back(model); };
          if (tab == 3) {
            for (const auto h : v.favorites) push(FindModel(h));
            ImGui::TextDisabled("Favorites (%zu), then last %zu spawned", v.favorites.size(), v.recent.size());
            for (const auto h : v.recent) push(FindModel(h));
          } else for (const auto& m : Models()) {
            const bool interactive = m.category == "Interactive models";
            if ((tab == 0 && m.kind == Kind::Prop && !interactive) || (tab == 1 && m.kind == Kind::Vehicle) ||
                (tab == 2 && (m.kind == Kind::Ped || interactive))) push(&m);
          }
          Rows(ui, v, list, std::clamp(screen.y * .24f, 90.f * scale, 205.f * scale));
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    if (const auto* model = FindModel(ui.model)) {
      ImGui::TextColored(accent, "%s%s", ui.preset.empty() ? "Selected: " : "Preset: ",
                         ui.preset.empty() ? model->name.data() : ui.preset.c_str());
      ImGui::TextDisabled("%s   /   0x%08X", model->internal.data(), unsigned(model->hash));
      if (ImGui::SmallButton("Toggle favorite")) Submit({.op = Op::Favorite, .value = ui.model}, true);
      if (!ui.preset.empty()) { ImGui::SameLine(); ImGui::TextDisabled("Anchor preview only"); }
    } else ImGui::TextDisabled("Choose a model or a preset to preview.");
    ImGui::SeparatorText("Placement");
    const char* grids[]{"Off", "0.25 m", "0.5 m", "1 m", "2 m"};
    constexpr unsigned grid_values[]{0, 25, 50, 100, 200};
    int grid = int(std::find(std::begin(grid_values), std::end(grid_values), ui.place.grid_cm) - std::begin(grid_values));
    ImGui::SetNextItemWidth(72.f * scale);
    if (ImGui::Combo("Grid", &grid, grids, IM_ARRAYSIZE(grids))) ui.place.grid_cm = grid_values[grid];
    ImGui::SameLine();
    int angle = ui.place.angle == 15 ? 0 : ui.place.angle == 45 ? 1 : 2;
    ImGui::SetNextItemWidth(75.f * scale);
    if (ImGui::Combo("Angle", &angle, "15 degrees\0" "45 degrees\0" "90 degrees\0"))
      ui.place.angle = std::array{15u, 45u, 90u}[unsigned(angle)];
    if (!v.caps.Has(Feature::Anchor)) ui.place.lock = false;
    if (!v.caps.Has(Feature::Ground)) ui.place.ground = false;
    ImGui::BeginDisabled(!v.caps.Has(Feature::Anchor)); ImGui::Checkbox("Auto-anchor", &ui.place.lock); ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.caps.Has(Feature::Ground)); ImGui::Checkbox("Ground snap", &ui.place.ground); ImGui::EndDisabled();
    ImGui::TextDisabled("%.1f m out   /   %+.1f m high   /   %.0f deg", ui.place.distance, ui.place.height, ui.place.yaw);
    ImGui::TextWrapped("Aim outside this panel. Left-click place, right-click cancel. Wheel rotates; Shift adjusts range; Ctrl+Shift adjusts height.");
    ImGui::SeparatorText("Edit");
    ImGui::BeginDisabled(v.busy || !v.undo);
    if (ImGui::Button("Undo [Z]")) Submit({Op::Undo}, true);
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(v.busy || !v.redo);
    if (ImGui::Button("Redo [Shift+Z]")) Submit({Op::Redo}, true);
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!v.count || v.busy);
    if (ImGui::Button("Clean my build...")) ImGui::OpenPopup("Confirm cleanup");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Confirm cleanup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("Remove your Workshop pieces and their undo history?");
      if (ImGui::Button("Remove my pieces")) { Submit({Op::Cleanup}, true); ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    const auto* aimed = FindModel(v.aimed_model);
    ImGui::TextDisabled("Aimed: %s", aimed ? aimed->name.data() : "no owned piece");
    ImGui::BeginDisabled(!v.aimed || v.busy);
    ImGui::BeginDisabled(!v.caps.Has(Feature::Anchor));
    if (ImGui::SmallButton(v.aimed_locked ? "Unanchor" : "Anchor")) Submit({.op = Op::Anchor, .piece = v.aimed, .on = !v.aimed_locked}, true);
    ImGui::EndDisabled(); ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate to cursor") && aimed) {
      ui.model = aimed->hash; ui.duplicate = v.aimed; ui.preset.clear(); ui.place.lock = v.aimed_locked;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) Submit({.op = Op::Delete, .piece = v.aimed}, true);
    ImGui::EndDisabled();
    if (v.storage_error) ImGui::TextColored({1.f, .65f, .27f, 1.f}, "Some config files are not writable or compatible.");
    ImGui::TextColored(accent, "%s", Label(v.result));
    ImGui::TextDisabled("Camera projection / tracked-piece targeting (no TU27 surface raycast)");
  }
  ImGui::End();
  ImGui::PopStyleColor(3);

  const auto& io = ImGui::GetIO();
  const bool text = io.WantTextInput || ImGui::IsAnyItemActive();
  if (!text && !v.busy) {
    if (v.redo && ((io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))))
      Submit({Op::Redo}, true);
    else if (!io.KeyShift && v.undo && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Submit({Op::Undo}, true);
  }
  if (!open) return false;
  const auto m = io.MousePos;
  if (!area.size.Empty() && !io.WantCaptureMouse && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
      m.x >= area.x && m.y >= area.y && m.x < area.x + area.size.width && m.y < area.y + area.size.height) {
    ui.place.lens = {2.f * (m.x - area.x) / float(area.size.width) - 1.f,
                    1.f - 2.f * (m.y - area.y) / float(area.size.height), float(area.size.width) / float(area.size.height)};
    if (io.KeyShift && io.KeyCtrl) ui.place.height = std::clamp(ui.place.height + io.MouseWheel * .25f, -12.f, 12.f);
    else if (io.KeyShift) ui.place.distance = std::clamp(ui.place.distance + io.MouseWheel, 2.f, 50.f);
    else ui.place.yaw = std::remainder(ui.place.yaw + io.MouseWheel * float(ui.place.angle), 360.f);
    auto* fg = ImGui::GetForegroundDrawList();
    const ImU32 color = v.ghost ? IM_COL32(140, 248, 201, 220) : IM_COL32(185, 182, 250, 215);
    fg->AddCircle(m, 9.f * scale, color, 20, 1.5f * scale);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { ui.model = 0; ui.duplicate = 0; ui.preset.clear(); }
    else if (ui.model && !v.busy && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const auto op = !ui.preset.empty() ? Op::LoadPreset : ui.duplicate ? Op::Duplicate : Op::Place;
      if (Submit({.op = op, .value = ui.model, .piece = ui.duplicate, .text = ui.preset, .placement = ui.place}, true)) ui.duplicate = 0;
    }
  }
  SetPreview(ui.model, ui.place);
  return true;
}
}  // namespace gtav::workshop
