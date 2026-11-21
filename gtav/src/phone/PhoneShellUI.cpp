#include "PhoneShellUI.h"

#include "../sandbox/CreativeRuntime.h"
#include "../sandbox/desktop.h"

#include <imgui.h>
#include <algorithm>

namespace gtav::workshop {
namespace {
enum class Page { Home, Status, Campaign };
Page page = Page::Home;

// GTA V palette — sombre, vert néon, jaune sélection
const ImVec4 kGtaBlack{0.06f, 0.06f, 0.07f, 0.98f};
const ImVec4 kGtaDark{0.09f, 0.09f, 0.10f, 1.00f};
const ImVec4 kGtaGreen{0.18f, 0.85f, 0.45f, 1.00f};
const ImVec4 kGtaGreenDim{0.12f, 0.55f, 0.32f, 1.00f};
const ImVec4 kGtaYellow{1.00f, 0.85f, 0.08f, 1.00f};
const ImVec4 kGtaGray{0.55f, 0.57f, 0.60f, 1.00f};

void PushGtaPhoneStyle(float scale) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.f * scale, 10.f * scale});
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 8.f * scale);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kGtaBlack);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kGtaDark);
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.14f, 0.14f, 0.15f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.85f, 0.45f, 0.22f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.13f, 0.14f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.85f, 0.45f, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.85f, 0.45f, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.96f, 1.f));
}
void PopGtaPhoneStyle() {
  ImGui::PopStyleColor(8);
  ImGui::PopStyleVar(4);
}

void DrawGtaHeader(const char* subtitle, float scale) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 1.f));
  if (ImGui::BeginChild("##gta-header", ImVec2(0, 24.f * scale), false, ImGuiWindowFlags_NoScrollbar)) {
    ImGui::TextColored(kGtaGreen, "  WORKSHOP");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 70.f * scale);
    ImGui::TextColored(kGtaGray, "LOS SANTOS");
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::TextColored(kGtaGreen, "  %s", subtitle);
  ImGui::Separator();
}

bool GtaAppTile(const char* label, const char* icon, const ImVec4& accent, float scale, ImVec2 tile) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.11f, 0.12f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x, accent.y, accent.z, 0.28f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x, accent.y, accent.z, 0.45f));
  bool pressed = ImGui::Button(label, tile);
  ImGui::PopStyleColor(3);
  ImVec2 min = ImGui::GetItemRectMin();
  ImVec2 center{min.x + tile.x * 0.5f, min.y + tile.y * 0.38f};
  float radius = 18.f * scale;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled(center, radius, ImGui::GetColorU32(accent), 24);
  dl->AddText(ImVec2(center.x - ImGui::CalcTextSize(icon).x * 0.5f, center.y - 7.f * scale), ImGui::GetColorU32(ImVec4(0.05f,0.05f,0.05f,1.f)), icon);
  return pressed;
}

const char* CampaignLabelForSlot(size_t i) {
  switch (i % 3) { case 0: return "Michael"; case 1: return "Franklin"; case 2: return "Trevor"; default: return "Michael"; }
}
} // namespace

void ResetPhone() { page = Page::Home; }

PhoneAction DrawPhone(const DesktopView& v, unsigned fps, float scale) {
  const bool isCampaign = sandbox::CurrentMode() == sandbox::Mode::Campaign;
  const bool isCampaignCoop = sandbox::CurrentMode() == sandbox::Mode::CampaignCoop;
  const bool isWorkshop = !isCampaign && !isCampaignCoop;
  const auto view = isWorkshop && sandbox::CurrentMode() == sandbox::Mode::Solo ? sandbox::SessionView{} : sandbox::Party().View();
  PlayerView unknown{kLocalOwner, "You", Seat::Local, Role::Player, v.map.mode, {}, false, false, 0};
  const auto self = std::find_if(v.players.begin(), v.players.end(), [](const PlayerView& p) { return p.id == kLocalOwner; });
  const auto& local = self != v.players.end() ? *self : unknown;
  const bool build = isWorkshop && v.build && BuildAllowed();
  // Local campaign assignment (avoid linking campaign.cpp in overlay tests)
  struct CampInfo { std::optional<std::string> self_label; std::vector<std::pair<uint32_t,std::string>> assignments; std::string invite; };
  CampInfo camp; camp.invite = view.invite;
  if (isCampaignCoop) {
    std::vector<uint32_t> ids; ids.reserve(view.world.players.size());
    for (auto &p : view.world.players) ids.push_back(p.id);
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size() && i < 3; ++i) {
      const char* lab = CampaignLabelForSlot(i);
      camp.assignments.emplace_back(ids[i], lab);
      if (ids[i] == view.world.self) camp.self_label = lab;
    }
  }

  if (page != Page::Home && page == Page::Campaign && !isCampaignCoop) page = Page::Home;
  if (isCampaign && page == Page::Campaign) page = Page::Home;

  const auto screen = ImGui::GetIO().DisplaySize;
  const float margin = 12.f * scale;
  const ImVec2 size{std::min(360.f * scale, screen.x - margin * 2), std::min(560.f * scale, screen.y - margin * 2)};
  ImGui::SetNextWindowPos({screen.x - margin, screen.y - margin}, ImGuiCond_Always, {1, 1});
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  PushGtaPhoneStyle(scale);
  bool open = true;
  PhoneAction action = PhoneAction::None;
  if (ImGui::Begin("Workshop Phone###phone-shell", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)) {
    if (isCampaignCoop) DrawGtaHeader("CAMPAGNE CO-OP  /  3 JOUEURS", scale);
    else if (isCampaign) DrawGtaHeader("WORKSHOP  /  HISTOIRE", scale);
    else DrawGtaHeader("WORKSHOP  /  LOS SANTOS", scale);

    {
      const auto latency = view.rtt_ms ? std::to_string(*view.rtt_ms) + " ms" :
          (view.link == sandbox::Link::Joined || view.link == sandbox::Link::Connecting ? "RTT --" : "Hors ligne");
      ImGui::TextColored(kGtaGray, "%s  \xC2\xB7  %u FPS  \xC2\xB7  %s", isCampaign ? "Histoire" : "Workshop", fps, latency.c_str());
      if (isCampaignCoop && camp.self_label) {
        ImGui::SameLine();
        ImGui::TextColored(kGtaGreen, "  |  %s", camp.self_label->c_str());
      }
      ImGui::Separator();
    }

    if (page != Page::Home) {
      if (ImGui::SmallButton("<  RETOUR")) page = Page::Home;
      ImGui::SameLine();
      const char* title = page == Page::Campaign ? "CAMPAGNE" : "ETAT DE SESSION";
      ImGui::TextColored(kGtaYellow, "%s", title);
      ImGui::Spacing();
    }

    if (page == Page::Campaign) {
      ImGui::TextColored(kGtaGreen, "CAMPAGNE CO-OP");
      ImGui::TextWrapped("Sauvegarde isolee : data/user/campaign_coop (solo intact). Chaque protagoniste est verrouille.");
      if (camp.self_label) ImGui::TextColored(kGtaYellow, "Tu es : %s", camp.self_label->c_str());
      else ImGui::TextDisabled("Attribution en cours...");
      ImGui::SeparatorText("Equipe");
      for (auto [id, label] : camp.assignments) {
        const char* name = "?";
        for (auto &p : view.world.players) if (p.id == id) { name = p.name.c_str(); break; }
        bool is_self = id == view.world.self;
        ImGui::BulletText("%s : %s%s", label.c_str(), name, is_self ? " (toi)" : "");
      }
      if (camp.assignments.empty()) ImGui::TextDisabled("En attente de joueurs...");
      ImGui::Spacing();
      ImGui::TextDisabled("IP/Port editables dans le launcher (Accueil > Campagne Co-op).");
      if (ImGui::Button("Ouvrir Session (F6)", ImVec2(-1, 36 * scale))) action = PhoneAction::Session;
    } else if (page == Page::Status) {
      if (view.link == sandbox::Link::Joined) ImGui::TextColored(kGtaGreen, "Connecte \xE2\x80\x94 salon prive");
      else if (view.link == sandbox::Link::Hosting) ImGui::TextColored(kGtaGreen, "Hebergeur \xE2\x80\x94 %u joueurs", (unsigned)view.world.players.size());
      else ImGui::Text("%s  /  %s", Label(local.mode), v.playable ? "Monde ouvert" : "Chargement...");
      ImGui::SeparatorText("Salon");
      if (!view.world.players.empty()) {
        for (const auto& p : view.world.players) ImGui::BulletText("%s%s", p.name.c_str(), p.id == view.world.self ? " (toi)" : "");
        ImGui::TextWrapped("RTT estime local. Les actions AdminOS ne sont pas repliquees en v2.");
      } else ImGui::TextWrapped("Aucun salon. La campagne solo et le Workshop utilisent des sauvegardes separees.");
      if (isWorkshop) ImGui::TextDisabled("Workshop Phone O/F7/Y | Esc fermer (GTA natif).");
    } else {
      const char* header = isCampaignCoop ? "CAMPAGNE  /  CO-OP" : isCampaign ? "WORKSHOP  /  HISTOIRE" : "WORKSHOP  /  LOS SANTOS";
      ImGui::TextColored(kGtaYellow, "%s", header);
      ImGui::TextDisabled("%s", isCampaign ? "Outils histoire disponibles" : isCampaignCoop ? "3 protagonistes verrouilles" : "Maintenance Workshop");
      ImGui::Spacing();
      const float tileW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
      const ImVec2 tile{tileW, 68.f * scale};

      if (isCampaignCoop) {
        if (GtaAppTile("CAMPAGNE\nEtat", "\xE2\x98\x85", kGtaGreen, scale, tile)) page = Page::Campaign;
        ImGui::SameLine();
        if (GtaAppTile("SESSION\nWorkshop F6", "\xE2\x97\x8F", kGtaGreen, scale, tile)) action = PhoneAction::Session;
      } else if (isCampaign) {
        if (GtaAppTile("CAMPAGNE\nHistoire", "\xE2\x98\x85", kGtaGreen, scale, tile)) page = Page::Campaign;
        ImGui::SameLine();
        if (GtaAppTile("STATUS\nWorld & room", "\xE2\x97\x8F", kGtaGray, scale, tile)) page = Page::Status;
      } else {
        if (GtaAppTile("SESSION\nWorkshop F6", "\xE2\x97\x8F", kGtaGreen, scale, tile)) action = PhoneAction::Session;
        ImGui::SameLine();
        if (GtaAppTile("STATUS\nWorld & room", "\xE2\x97\x8F", ImVec4(0.35f,0.55f,0.95f,1.f), scale, tile)) page = Page::Status;
      }

      if (isWorkshop && build) {
        ImGui::Spacing();
        if (GtaAppTile("BUILD\nCreative tools", "\xE2\x9F\xA6", kGtaYellow, scale, tile)) action = PhoneAction::Build;
      } else if (isCampaignCoop) {
        ImGui::Spacing();
        if (GtaAppTile("INVITE\nCopy code", "\xE2\x97\x8F", ImVec4(0.95f,0.55f,0.10f,1.f), scale, tile)) {
          if (!camp.invite.empty()) ImGui::SetClipboardText(camp.invite.c_str());
        }
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Workshop Phone O/F7/Y  \xC2\xB7  Esc / B fermer");
      ImGui::TextDisabled("Cheats via F5 (panel client) - pas d'AdminOS dans le phone.");
      if (isCampaignCoop) ImGui::TextDisabled("Sauvegarde campagne isolee \xE2\x80\x94 solo intacte.");
      else if (isWorkshop) ImGui::TextDisabled("Build / AdminOS locaux, non repliques en v2.");
      else ImGui::TextDisabled("Telephone histoire \xE2\x80\x94 disponible des le debut.");
    }
  }
  ImGui::End();
  PopGtaPhoneStyle();
  return open ? action : PhoneAction::Close;
}
}  // namespace gtav::workshop
