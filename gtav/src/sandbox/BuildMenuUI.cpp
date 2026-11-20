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
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.85f, 0.45f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f,0.05f,0.05f,1.f));
  ImGui::BeginDisabled(!v.aimed || !*ui.preset_name);
  if (ImGui::Button("Save aimed group")) Submit({.op = Op::SavePreset, .piece = v.aimed, .text = ui.preset_name}, true);
  ImGui::EndDisabled();
  ImGui::PopStyleColor(2);
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
  if (!ui.preset.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f,0.25f,0.25f,1.f));
    if (ImGui::Button("Remove selected preset")) {
      if (Submit({.op = Op::DeletePreset, .text = ui.preset}, true)) { ui.preset.clear(); ui.model = 0; }
    }
    ImGui::PopStyleColor();
  }
}
}  // namespace

bool DrawBuildMenu(const DesktopView& v, float scale, native::OutputRect area) {
  static Panel ui;
  bool open = true;
  const auto screen = ImGui::GetIO().DisplaySize;
  // GTA palette — noir profond, vert néon, jaune Build
  const ImVec4 kGtaBlack{0.06f, 0.06f, 0.07f, 0.98f};
  const ImVec4 kGtaGreen{0.18f, 0.85f, 0.45f, 1.f};
  const ImVec4 kGtaYellow{1.f, 0.85f, 0.08f, 1.f};
  const ImVec4 kGtaDark{0.09f, 0.09f, 0.10f, 1.f};
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kGtaBlack);
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.85f, 0.45f, 0.18f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.85f, 0.45f, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.13f, 0.14f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.85f, 0.45f, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.13f, 0.14f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.12f,0.12f,0.13f,1.f));
  ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.18f,0.85f,0.45f,0.22f));
  ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.18f,0.85f,0.45f,0.45f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 8.f * scale);
  ImGui::SetNextWindowPos({15.f * scale, 16.f * scale}, ImGuiCond_Always);
  ImGui::SetNextWindowSize({std::min(615.f * scale, screen.x * .62f), std::min(650.f * scale, screen.y - 31.f * scale)}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(.98f);
  if (ImGui::Begin("BUILD  //  ATELIER CREATIF  [F8]", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextColored(kGtaGreen, "HOOKY  BUILD  —  LOS SANTOS WORKSHOP");
    ImGui::TextColored(kGtaYellow, "%u / %u  pieces  //  %u  actifs", v.count, v.limit, v.total);
    if (v.storage_error) ImGui::TextColored(ImVec4(1.f,.65f,.27f,1.f), "Config non inscriptible.");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kGtaDark);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##model-search", "Rechercher un modele, nom ou hash 0x...", ui.search, sizeof ui.search);
    ImGui::PopStyleColor();
    if (ImGui::BeginTabBar("build-tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      constexpr const char* labels[]{"Props & architecture", "Vehicles", "NPCs & entities", "Favorites / recent", "Presets"};
      constexpr const char* icons[]{"◧", "◈", "☺", "★", "⬢"};
      for (int tab = 0; tab < int(std::size(labels)); ++tab) {
        char tab_label[64]; std::snprintf(tab_label, sizeof tab_label, "%s  %s", icons[tab], labels[tab]);
        if (!ImGui::BeginTabItem(tab_label)) continue;
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
      ImGui::Separator();
      ImGui::TextColored(kGtaYellow, "%s%s", ui.preset.empty() ? "Selection : " : "Preset : ",
                         ui.preset.empty() ? model->name.data() : ui.preset.c_str());
      ImGui::TextDisabled("%s   /   0x%08X", model->internal.data(), unsigned(model->hash));
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f,0.85f,0.45f,1.f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f,0.05f,0.05f,1.f));
      if (ImGui::SmallButton("Toggle favorite")) Submit({.op = Op::Favorite, .value = ui.model}, true);
      ImGui::PopStyleColor(2);
      if (!ui.preset.empty()) { ImGui::SameLine(); ImGui::TextDisabled("Apercu ancrage seulement"); }
    } else ImGui::TextDisabled("Choisis un modele ou un preset pour previsualiser.");
    ImGui::SeparatorText("Placement");
    const char* grids[]{"Off", "0.25 m", "0.5 m", "1 m", "2 m"};
    constexpr unsigned grid_values[]{0, 25, 50, 100, 200};
    int grid = int(std::find(std::begin(grid_values), std::end(grid_values), ui.place.grid_cm) - std::begin(grid_values));
    ImGui::SetNextItemWidth(72.f * scale);
    if (ImGui::Combo("Grid", &grid, grids, IM_ARRAYSIZE(grids))) ui.place.grid_cm = grid_values[grid];
    ImGui::SameLine();
    int angle = ui.place.angle == 15 ? 0 : ui.place.angle == 45 ? 1 : 2;
    ImGui::SetNextItemWidth(75.f * scale);
    if (ImGui::Combo("Angle", &angle, "15 degrees\000" "45 degrees\000" "90 degrees\000"))
      ui.place.angle = std::array{15u, 45u, 90u}[unsigned(angle)];
    if (!v.caps.Has(Feature::Anchor)) ui.place.lock = false;
    if (!v.caps.Has(Feature::Ground)) ui.place.ground = false;
    ImGui::BeginDisabled(!v.caps.Has(Feature::Anchor)); ImGui::Checkbox("Auto-anchor", &ui.place.lock); ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.caps.Has(Feature::Ground)); ImGui::Checkbox("Ground snap", &ui.place.ground); ImGui::EndDisabled();
    ImGui::TextColored(kGtaYellow, "%.1f m  /  %+.1f m haut  /  %.0f°", ui.place.distance, ui.place.height, ui.place.yaw);
    ImGui::TextWrapped("Vise hors du panneau. Clic gauche place, clic droit annule. Molette tourne ; Shift ajuste la distance ; Ctrl+Shift la hauteur.");
    ImGui::SeparatorText("Edit");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f,0.85f,0.45f,1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f,0.05f,0.05f,1.f));
    ImGui::BeginDisabled(v.busy || !v.undo);
    if (ImGui::Button("Undo [Z]")) Submit({Op::Undo}, true);
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(v.busy || !v.redo);
    if (ImGui::Button("Redo [Shift+Z]")) Submit({Op::Redo}, true);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f,0.25f,0.25f,1.f));
    ImGui::BeginDisabled(!v.count || v.busy);
    if (ImGui::Button("Clean my build...")) ImGui::OpenPopup("Confirm cleanup");
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    // GTA styled popup
    if (ImGui::BeginPopupModal("Confirm cleanup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(kGtaYellow, "Supprimer tes pieces Workshop ?");
      ImGui::TextUnformatted("Tes pieces et l'historique undo seront effaces.");
      if (ImGui::Button("Supprimer")) { Submit({Op::Cleanup}, true); ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Annuler")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    const auto* aimed = FindModel(v.aimed_model);
    ImGui::TextDisabled("Vise : %s", aimed ? aimed->name.data() : "aucune piece a toi");
    ImGui::BeginDisabled(!v.aimed || v.busy);
    ImGui::BeginDisabled(!v.caps.Has(Feature::Anchor));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f,0.13f,0.14f,1.f));
    if (ImGui::SmallButton(v.aimed_locked ? "Unanchor" : "Anchor")) Submit({.op = Op::Anchor, .piece = v.aimed, .on = !v.aimed_locked}, true);
    ImGui::PopStyleColor();
    ImGui::EndDisabled(); ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate to cursor") && aimed) {
      ui.model = aimed->hash; ui.duplicate = v.aimed; ui.preset.clear(); ui.place.lock = v.aimed_locked;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f,0.25f,0.25f,0.6f));
    if (ImGui::SmallButton("Delete")) Submit({.op = Op::Delete, .piece = v.aimed}, true);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (v.storage_error) ImGui::TextColored({1.f, .65f, .27f, 1.f}, "Certains fichiers config ne sont pas inscriptibles.");
    ImGui::TextColored(kGtaGreen, "%s", Label(v.result));
    ImGui::TextDisabled("Projection camera / ciblage piece suivie (pas de raycast surface TU27)");
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(9);

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
    const ImU32 color = v.ghost ? IM_COL32(24, 217, 115, 220) : IM_COL32(255, 217, 20, 215);
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
