#include "CreatorEditorUI.h"

#include "../core/WorkshopCore.h"
#include "../phone/AdminCheatApp.h"
#include "BuildManager.h"
#include "CreativeRuntime.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace gtav::sandbox {
namespace {
struct EditorState {
  int selected_object = -1;
  int selected_trigger = -1;
  char map_name[64] = "Untitled";
  char search[96] = "";
  int asset_kind = 0; // 0 All,1 Prop,2 Vehicle,3 Ped,4 Trigger
  workshop::Placement placement{};
  bool show_gizmos = true;
  char trigger_name[48] = "Checkpoint";
  int trigger_type = 0;
  float trigger_radius = 5.f;
};

const char* TriggerLabel(TriggerType t){
  switch(t){
    case TriggerType::Checkpoint: return "Checkpoint";
    case TriggerType::SpawnPlayer: return "Spawn Player";
    case TriggerType::SpawnVehicle: return "Spawn Vehicle";
    case TriggerType::WinZone: return "Win Zone";
    case TriggerType::LoseZone: return "Lose Zone";
    case TriggerType::Script: return "Script";
    default: return "?";
  }
}
} // anon

static EditorState* g_state = nullptr;

void UpdateCreatorPreview(uint32_t model_hash, workshop::Placement p){
  if(!g_state) return;
  g_state->placement = p;
  workshop::SetPreview(model_hash, p);
}

namespace {

void DrawHierarchy(CreatorManager& mgr, EditorState& st){
  ImGui::TextColored(ImVec4(1.f,0.85f,0.08f,1.f), "Hierarchy  //  %zu objects", mgr.Current().objects.size());
  ImGui::Separator();
  if(ImGui::BeginChild("creator-hierarchy", ImVec2(0,220), ImGuiChildFlags_Borders)){
    for(size_t i=0;i<mgr.Current().objects.size();++i){
      const auto& o = mgr.Current().objects[i];
      const auto* m = workshop::FindModel(o.model);
      char label[160];
      std::snprintf(label,sizeof label, "%zu: %s  [%s]%s", i, o.name.empty()?(m?m->name.data():"?"):o.name.c_str(), m?m->internal.data():"?", o.locked?" [L]":"");
      const bool sel = int(i)==st.selected_object;
      if(ImGui::Selectable(label, sel)){
        st.selected_object=int(i); st.selected_trigger=-1;
      }
      if(ImGui::IsItemHovered()) ImGui::SetTooltip("Pose: %.1f %.1f %.1f  yaw %.0f  hash 0x%08X", o.pose.at.x, o.pose.at.y, o.pose.at.z, o.pose.yaw, unsigned(o.model));
    }
    if(mgr.Current().objects.empty()) ImGui::TextDisabled("No objects — add from Asset Browser below");
  }
  ImGui::EndChild();
  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.6f,0.85f,1.f,1.f), "Triggers / Scenario  //  %zu", mgr.Current().triggers.size());
  if(ImGui::BeginChild("creator-triggers", ImVec2(0,120), ImGuiChildFlags_Borders)){
    for(size_t i=0;i<mgr.Current().triggers.size();++i){
      const auto& t=mgr.Current().triggers[i];
      char label[120]; std::snprintf(label,sizeof label, "%zu: %s  [%s] r=%.0f", i, t.name.c_str(), TriggerLabel(t.type), t.radius);
      if(ImGui::Selectable(label, int(i)==st.selected_trigger)){
        st.selected_trigger=int(i); st.selected_object=-1;
      }
    }
    if(mgr.Current().triggers.empty()) ImGui::TextDisabled("No triggers — add Checkpoint/Win/Lose below");
  }
  ImGui::EndChild();
}

void DrawInspector(CreatorManager& mgr, EditorState& st, const workshop::DesktopView& wv){
  ImGui::TextColored(ImVec4(0.18f,0.85f,0.45f,1.f), "Inspector");
  ImGui::Separator();
  if(st.selected_object>=0 && st.selected_object < int(mgr.Current().objects.size())){
    auto& o = mgr.Mutable().objects[size_t(st.selected_object)];
    const auto* m = workshop::FindModel(o.model);
    ImGui::Text("%s", o.name.c_str());
    ImGui::TextDisabled("%s  0x%08X  %s", m?m->internal.data():"?", unsigned(o.model), m?m->category.data():"");
    char name[64]; std::snprintf(name,sizeof name, "%s", o.name.c_str());
    if(ImGui::InputText("Name", name, sizeof name)) { o.name=name; }
    bool changed=false;
    changed |= ImGui::DragFloat3("Position", &o.pose.at.x, 0.1f);
    changed |= ImGui::DragFloat("Yaw", &o.pose.yaw, 0.5f, 0.f, 360.f);
    changed |= ImGui::DragFloat3("Scale", &o.scale.x, 0.02f, 0.1f, 5.f);
    ImGui::Checkbox("Locked (anchor)", &o.locked);
    if(changed){
      o.pose.yaw = std::fmod(o.pose.yaw,360.f); if(o.pose.yaw<0) o.pose.yaw+=360.f;
    }
    ImGui::Separator();
    if(ImGui::Button("Duplicate")) mgr.DuplicateObject(size_t(st.selected_object));
    ImGui::SameLine();
    if(ImGui::Button("Delete")){ mgr.RemoveObject(size_t(st.selected_object)); st.selected_object=-1; }
    ImGui::SameLine();
    ImGui::BeginDisabled(!wv.caps.Has(workshop::ForKind(o.kind)));
    if(ImGui::Button("Snap to ghost")){ }
    ImGui::EndDisabled();
    if(!wv.caps.Has(workshop::ForKind(o.kind))) ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "Kind %d unavailable TU27", int(o.kind));
  } else if(st.selected_trigger>=0 && st.selected_trigger < int(mgr.Current().triggers.size())){
    auto& t = mgr.Mutable().triggers[size_t(st.selected_trigger)];
    char name[48]; std::snprintf(name,sizeof name, "%s", t.name.c_str());
    if(ImGui::InputText("Name", name, sizeof name)) t.name=name;
    ImGui::DragFloat3("Position", &t.pos.x, 0.2f);
    ImGui::DragFloat("Radius", &t.radius, 0.1f, 0.5f, 200.f);
    int type=int(t.type);
    if(ImGui::Combo("Type", &type, "Checkpoint\0SpawnPlayer\0SpawnVehicle\0WinZone\0LoseZone\0Script\0")) t.type=TriggerType(type);
    char script[128]; std::snprintf(script,sizeof script, "%s", t.script.c_str());
    if(ImGui::InputText("Script", script, sizeof script)) t.script=script;
    if(ImGui::Button("Delete trigger")){ mgr.RemoveTrigger(size_t(st.selected_trigger)); st.selected_trigger=-1; }
  } else {
    ImGui::TextDisabled("No selection — click an object or trigger in Hierarchy");
    ImGui::Separator();
    ImGui::TextWrapped("Tips: Left-click in viewport outside panels places the previewed asset. Wheel rotates, Shift+wheel distance, Ctrl+Shift height — same as F8 Build.");
    ImGui::Checkbox("Show gizmos", &st.show_gizmos);
  }
}

void DrawAssetBrowser(CreatorManager& mgr, EditorState& st){
  ImGui::TextColored(ImVec4(0.18f,0.85f,0.45f,1.f), "Asset Browser  //  Models + Triggers");
  const char* kinds[]={"All","Props","Vehicles","Peds","Triggers"};
  ImGui::Combo("Filter", &st.asset_kind, kinds, IM_ARRAYSIZE(kinds));
  ImGui::SameLine(); ImGui::InputTextWithHint("##asset-search","Search name/hash", st.search, sizeof st.search);
  if(st.asset_kind==4){
    ImGui::InputText("New trigger name", st.trigger_name, sizeof st.trigger_name);
    ImGui::Combo("Trigger type", &st.trigger_type, "Checkpoint\0SpawnPlayer\0SpawnVehicle\0WinZone\0LoseZone\0Script\0");
    ImGui::DragFloat("Radius", &st.trigger_radius, 0.5f, 1.f, 200.f);
    if(ImGui::Button("Add trigger at player (6m ahead)")){
      CreatorTrigger t; t.name=st.trigger_name; t.type=TriggerType(st.trigger_type); t.radius=st.trigger_radius;
      t.pos = workshop::Vec3{mgr.Current().player_start.x + 6.f, mgr.Current().player_start.y, mgr.Current().player_start.z};
      mgr.AddTrigger(t);
    }
    ImGui::Separator();
    ImGui::TextDisabled("Triggers are invisible volumes — enter to fire scenario logic (win/lose/spawn).");
  } else {
    std::vector<const workshop::Model*> filtered;
    for(const auto& m: workshop::Models()){
      if(st.asset_kind==1 && m.kind!=workshop::Kind::Prop) continue;
      if(st.asset_kind==2 && m.kind!=workshop::Kind::Vehicle) continue;
      if(st.asset_kind==3 && m.kind!=workshop::Kind::Ped) continue;
      if(!workshop::Matches(m, st.search)) continue;
      filtered.push_back(&m);
    }
    if(ImGui::BeginChild("asset-list", ImVec2(0,150), ImGuiChildFlags_Borders)){
      ImGuiListClipper clipper; clipper.Begin(int(filtered.size()));
      while(clipper.Step()) for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i){
        const auto* m=filtered[size_t(i)];
        char label[180]; std::snprintf(label,sizeof label, "%s  /  %s  [%s] 0x%08X", m->name.data(), m->category.data(), m->kind==workshop::Kind::Prop?"Prop":m->kind==workshop::Kind::Vehicle?"Veh":"Ped", unsigned(m->hash));
        ImGui::PushID(i);
        if(ImGui::Selectable(label)){
          CreatorObject obj; obj.model=m->hash; obj.kind=m->kind; obj.pose=workshop::Pose{{0,6,0.5f},0};
          obj.name=std::string(m->name); obj.category=std::string(m->category);
          mgr.AddObject(obj);
          st.selected_object=int(mgr.Current().objects.size())-1;
          UpdateCreatorPreview(m->hash, st.placement);
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s  %s", m->internal.data(), m->name.data());
        ImGui::PopID();
      }
      if(filtered.empty()) ImGui::TextDisabled("No matches");
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%zu assets — click to add to Hierarchy (spawns on Save&Play).", filtered.size());
  }
}

} // anon namespace wrapper for static helpers (second anon closed)

bool DrawCreatorEditor(const CreatorEditorView& view){
  if(!view.manager) return false;
  static EditorState state;
  g_state=&state;
  if(std::string(state.map_name)!=view.manager->Current().name){
    std::snprintf(state.map_name,sizeof state.map_name,"%s",view.manager->Current().name.c_str());
  }
  const float scale = view.scale;
  const auto screen = ImGui::GetIO().DisplaySize;
  if(screen.x<=1 || screen.y<=1) return true;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f*scale);
  ImGui::SetNextWindowPos(ImVec2(12*scale, 12*scale), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(screen.x-24*scale, 42*scale), ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f,0.08f,0.09f,0.97f));
  bool toolbar_open=true;
  if(ImGui::Begin("CREATOR MAP // TOOLBAR", &toolbar_open, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoCollapse)){
    if(ImGui::Button("New")){ std::string n = "Untitled"; view.manager->New(n); }
    ImGui::SameLine();
    if(ImGui::Button("Open")){
      auto list = ListCreatorMaps(view.manager->MapsRoot());
      if(!list.empty()){ std::string err; view.manager->Open(list.front(), err); }
    }
    ImGui::SameLine();
    std::string err;
    const bool dirty=view.manager->Dirty();
    if(ImGui::Button(dirty?"Save *":"Save")){
      view.manager->Save(err);
    }
    if(!err.empty()) ImGui::SetTooltip("%s", err.c_str());
    ImGui::SameLine();
    if(ImGui::Button("Save As...")){
      char buf[64]; std::snprintf(buf,sizeof buf, "%s_copy", view.manager->Current().name.c_str());
      view.manager->SaveAs(buf, err);
    }
    ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, view.is_playing? ImVec4(0.95f,0.25f,0.25f,1.f):ImVec4(0.18f,0.85f,0.45f,1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, view.is_playing? ImVec4(1.f,1.f,1.f,1.f):ImVec4(0.05f,0.05f,0.05f,1.f));
    if(ImGui::Button(view.is_playing?" \u25A0 Stop":" \u25B6 Play")){
      if(!view.is_playing){
        std::string why; if(view.manager->ValidForPlay(why)){
          for(const auto& obj: view.manager->Current().objects){
            workshop::Command cmd; cmd.op = workshop::Op::Place; cmd.value = obj.model; cmd.proposed = obj.pose; cmd.on = obj.locked;
            workshop::Submit(std::move(cmd), true);
          }
        }
      } else {
        // Stop: cleanup via workshop cleanup? For now just close build queue via Undo? We use Cleanup op
        // workshop::Submit({workshop::Op::Cleanup}, true);
      }
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::InputTextWithHint("##mapname", "Map name", state.map_name, sizeof state.map_name);
    ImGui::SameLine();
    if(ImGui::Button("Rename")){ view.manager->Mutable().name = state.map_name; }
    ImGui::SameLine();
    ImGui::TextColored(dirty? ImVec4(1.f,0.85f,0.08f,1.f):ImVec4(0.5f,0.5f,0.55f,1.f), dirty?"* unsaved":"saved");
    ImGui::SameLine();
    ImGui::TextDisabled("Objects %zu / Triggers %zu", view.manager->Current().objects.size(), view.manager->Current().triggers.size());
  }
  ImGui::End();
  ImGui::PopStyleColor(); ImGui::PopStyleVar();

  const float top = 58*scale;
  const float avail_h = screen.y - top - 22*scale;
  const float left_w = std::min(320.f*scale, screen.x*0.26f);
  const float right_w = std::min(360.f*scale, screen.x*0.32f);
  const float center_w = screen.x - left_w - right_w - 24*scale;

  ImGui::SetNextWindowPos(ImVec2(12*scale, top), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(left_w, avail_h*0.75f), ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f,0.07f,0.08f,0.98f));
  if(ImGui::Begin("Hierarchy", nullptr, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse)){
    DrawHierarchy(*view.manager, state);
  }
  ImGui::End();

  ImGui::SetNextWindowPos(ImVec2(12*scale+left_w+4*scale, top), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(center_w, avail_h), ImGuiCond_Always);
  if(ImGui::Begin("Viewport & Assets", nullptr, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse)){
    ImGui::TextColored(ImVec4(0.6f,0.85f,1.f,1.f), "Viewport  //  Free roam ghost preview");
    ImGui::TextWrapped("Vise hors des panneaux. Clic gauche place le preview, molette = yaw, Shift+molette = distance, Ctrl+Shift+molette = hauteur. Grille/snap ci-dessous.");
    ImGui::SeparatorText("Placement (Unity gizmo)");
    const char* grids[]={"Off","0.25m","0.5m","1m","2m"};
    constexpr unsigned gv[]={0,25,50,100,200};
    int grid=int(std::find(std::begin(gv), std::end(gv), state.placement.grid_cm)-std::begin(gv));
    ImGui::SetNextItemWidth(90*scale); if(ImGui::Combo("Grid", &grid, grids, IM_ARRAYSIZE(grids))) state.placement.grid_cm=gv[grid];
    ImGui::SameLine();
    int angle = state.placement.angle==15?0:state.placement.angle==45?1:2;
    ImGui::SetNextItemWidth(90*scale); if(ImGui::Combo("Angle", &angle, "15\0""45\0""90\0")) state.placement.angle=std::array{15u,45u,90u}[unsigned(angle)];
    ImGui::SameLine(); ImGui::Checkbox("Ground snap", &state.placement.ground);
    ImGui::SameLine(); ImGui::Checkbox("Auto-anchor", &state.placement.lock);
    ImGui::TextDisabled("Dist %.1fm  Height %+.1fm  Yaw %.0f deg", state.placement.distance, state.placement.height, state.placement.yaw);
    uint32_t preview_model = 0;
    if(state.selected_object>=0 && state.selected_object<int(view.manager->Current().objects.size())) preview_model = view.manager->Current().objects[size_t(state.selected_object)].model;
    UpdateCreatorPreview(preview_model, state.placement);
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.f,0.85f,0.08f,1.f), "Maps on disk  //  %s", view.manager->MapsRoot().string().c_str());
    auto list = ListCreatorMaps(view.manager->MapsRoot());
    if(ImGui::BeginChild("maps-list", ImVec2(0,90), ImGuiChildFlags_Borders)){
      for(auto &n: list){
        bool sel = n==view.manager->Current().name;
        if(ImGui::Selectable(n.c_str(), sel)){
          std::string e; view.manager->Open(n,e);
          if(!e.empty()) ImGui::SetTooltip("%s", e.c_str());
          state.selected_object=-1;
        }
      }
      if(list.empty()) ImGui::TextDisabled("No maps yet — Save to create one.");
    }
    ImGui::EndChild();
    if(!list.empty()){
      ImGui::SameLine();
      if(ImGui::Button("Delete selected map")){
        DeleteCreatorMap(view.manager->MapsRoot(), view.manager->Current().name);
      }
    }
    ImGui::Separator();
    DrawAssetBrowser(*view.manager, state);
    ImGui::Separator();
    std::string why; bool can_play = view.manager->ValidForPlay(why);
    ImGui::BeginDisabled(!can_play);
    if(ImGui::Button("\u25B6 Test Play (spawn all)")){
      // Spawn each object at its stored pose via proposed (Creator precise spawn)
      int spawned=0;
      for(const auto& obj: view.manager->Current().objects){
        workshop::Command cmd; cmd.op = workshop::Op::Place; cmd.value = obj.model; cmd.proposed = obj.pose; cmd.on = obj.locked;
        if(workshop::Submit(std::move(cmd), true)) ++spawned;
      }
      why = std::to_string(spawned) + " spawn requests queued (90m limit, TU27 filter via IS_VEHICLE_MODEL etc).";
    }
    ImGui::EndDisabled();
    if(!can_play) ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "%s", why.c_str());
    else if(!why.empty()) ImGui::TextColored(ImVec4(0.18f,0.85f,0.45f,1.f), "%s", why.c_str());
    else ImGui::TextDisabled("Play spawns %zu objects + %zu triggers, then you roam & test scenario logic.", view.manager->Current().objects.size(), view.manager->Current().triggers.size());
  }
  ImGui::End();

  ImGui::SetNextWindowPos(ImVec2(screen.x - right_w - 12*scale, top), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(right_w, avail_h), ImGuiCond_Always);
  if(ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse)){
    DrawInspector(*view.manager, state, view.workshop_view);
    ImGui::SeparatorText("Scenario");
    static char scen_name[64]={}; static bool init=false;
    if(!init){ std::snprintf(scen_name,sizeof scen_name,"%s", view.manager->Current().scenario.name.c_str()); init=true; }
    if(ImGui::InputText("Scenario", scen_name, sizeof scen_name)) view.manager->Mutable().scenario.name=scen_name;
    static char scen_desc[128]={};
    if(ImGui::InputTextMultiline("Description", scen_desc, sizeof scen_desc, ImVec2(0,40))) view.manager->Mutable().scenario.description=scen_desc;
    int win=int(view.manager->Current().scenario.win);
    if(ImGui::Combo("Win", &win, "All Checkpoints\0Win Zone\0Script\0")) view.manager->Mutable().scenario.win=CreatorScenario::WinCondition(win);
    ImGui::TextDisabled("Add triggers as checkpoints — entering them counts for win.");
  }
  ImGui::End();
  ImGui::PopStyleColor();

  ImGui::SetNextWindowPos(ImVec2(12*scale, screen.y-18*scale), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(screen.x-24*scale, 16*scale), ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f,0.06f,0.07f,0.85f));
  if(ImGui::Begin("##creator-status", nullptr, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoScrollbar)){
    ImGui::TextDisabled("Creator MAP  |  F7 toggle editor  |  F8 Build still available  |  F5 Cheats  |  isole dans data/user/creator/");
    ImGui::SameLine();
    std::string why; bool ok=view.manager->ValidForPlay(why);
    ImGui::TextColored(ok? ImVec4(0.18f,0.85f,0.45f,1.f):ImVec4(1.f,0.55f,0.35f,1.f), ok?"Ready to play":" %s", ok?"":why.c_str());
  }
  ImGui::End(); ImGui::PopStyleColor();

  return true;
}

} // namespace gtav::sandbox
