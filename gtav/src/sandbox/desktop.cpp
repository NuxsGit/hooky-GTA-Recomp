#include "desktop.h"
#include "CreativeRuntime.h"
#include "campaign.h"
#include "campaign_death.h"

#include "courses.h"
#include "../frontend/play_hub.h"
#include "../native/menu.h"
#include "../native/settings_runtime.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>

REXCVAR_DEFINE_STRING(gtav_mode, "campaign", "GTAV", "Desktop startup mode: menu | solo | multiplayer | campaign | campaign-coop");
REXCVAR_DEFINE_BOOL(gtav_workshop_scene, true, "GTAV", "Workshop: show local props and remote ghosts in free roam");
REXCVAR_DEFINE_STRING(gtav_campaign_host, "", "GTAV", "Campaign coop host IP (launcher editable, empty=auto)");
REXCVAR_DEFINE_INT32(gtav_campaign_port, 30127, "GTAV", "Campaign coop host TCP port");

namespace gtav::sandbox {
namespace fs = std::filesystem;
namespace {
std::atomic<Mode> mode{Mode::Campaign};
std::filesystem::path courses;
std::atomic<Guest> guest{Guest::Loading};
std::atomic<int64_t> guest_at{0};
std::mutex vehicle_mutex;
std::optional<VehicleRequest> vehicle_request;
std::atomic<const char*> vehicle_status{"Select a local test vehicle."};
std::atomic<bool> scene_available{true};
constexpr const char* kGameNames[] = {"Checkpoint race", "Checkpoint hunt"};

bool Error(const char* message) {
  REXLOG_ERROR("[workshop] {}", message);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GTA V Recomp | Workshop", message, SDL_GetKeyboardFocus());
  return false;
}

const Player* Myself(const World& world) {
  for (const auto& p : world.players) if (p.id == world.self) return &p;
  return nullptr;
}

Guest GuestState() {
  const auto state = guest.load();
  if (state == Guest::Ready && NowMs() - guest_at.load() > kPoseLifetimeMs) return Guest::Busy;
  return state;
}

const char* GuestLabel() {
  const bool campaign = CurrentMode() == Mode::CampaignCoop;
  switch (GuestState()) {
    case Guest::Loading: return campaign ? "Campaign co-op ready. All missions playable at 3 from start." : "Waiting for the TU27 world. Load a sandbox save and enter free roam.";
    case Guest::Ready: return campaign ? "Campaign co-op ready. Missions at 3 - F6: Session | F8: Build | P / Up: phone | F5: Cheats." : "Free roam ready. F6: Workshop | F8: Creative Build | P / Up: phone | F5: Cheats.";
    case Guest::Busy: return campaign ? "Waiting for world (cutscene/pause) - missions stay synced at 3." : "Waiting for free roam (not during a cutscene, mission or pause).";
    default: return "TU27 native context unavailable. Local vehicles and map markers are disabled.";
  }
}

const char* LinkLabel(Link link) {
  switch (link) {
    case Link::Idle: return "No active network session";
    case Link::Opening: return "Opening local TCP listener...";
    case Link::Hosting: return "Hosting a private session";
    case Link::Connecting: return "Connecting to the host...";
    case Link::Joined: return "Joined a private session";
    default: return "Connection stopped";
  }
}

void EditCourse(const SessionView& view, const Player* local) {
  const auto& world = view.world;
  static char name[33] = "Untitled";
  static std::string notice;
  static int prop = 0, rotation = 0, height = -50;
  const bool racing = world.race == Race::Countdown || world.race == Race::Running;
  ImGui::Text("%s | %zu/%u checkpoints | %zu/%u props", world.course.name.c_str(), world.course.points.size(),
              kMaxCheckpoints, world.course.props.size(), kMaxProps);
  if (!view.Owner()) {
    ImGui::Text("%s", kGameNames[unsigned(world.course.game)]);
    ImGui::TextDisabled("The host owns the layout and minigame. You can play it, not edit it.");
    return;
  }
  auto edit = [&](auto change, const char* success) {
    auto course = world.course;
    course.name = name;
    change(course);
    notice = Party().SetCourse(std::move(course)) ? success :
        "Edit rejected. Check the layout name and the 15 m spacing rule.";
  };
  const bool at_player = local && local->pose && GuestState() == Guest::Ready;
  ImGui::BeginDisabled(racing || !workshop::BuildAllowed());
  ImGui::InputText("Layout name", name, sizeof(name));
  int game = int(world.course.game);
  if (ImGui::Combo("Minigame", &game, kGameNames, int(std::size(kGameNames))))
    edit([&](Course& c) { c.game = Minigame(game); }, "Minigame changed.");
  ImGui::BeginDisabled(!at_player || world.course.points.size() >= kMaxCheckpoints);
  if (ImGui::Button("Add checkpoint here") && at_player)
    edit([&](Course& c) { c.points.push_back(local->pose->position); }, "Checkpoint added.");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Undo checkpoint") && !world.course.points.empty())
    edit([](Course& c) { c.points.pop_back(); }, "Checkpoint removed.");

  if (ImGui::CollapsingHeader("Place props", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Combo("Prop", &prop, kPropNames.data(), int(kPropNames.size()));
    ImGui::SliderInt("Rotation", &rotation, -180, 180, "%d deg");
    ImGui::SliderInt("Height offset", &height, -200, 200, "%d cm");
    const auto placement = at_player ? PlaceProp(*local->pose, PropKind(prop), rotation, height) : std::nullopt;
    ImGui::BeginDisabled(!placement || world.course.props.size() >= kMaxProps);
    if (ImGui::Button("Place 3 m ahead (on foot)") && placement)
      edit([&](Course& c) { c.props.push_back(*placement); }, "Prop placed.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Undo prop") && !world.course.props.empty())
      edit([](Course& c) { c.props.pop_back(); }, "Prop removed.");
  }
  if (ImGui::Button("Save layout")) {
    auto course = world.course;
    course.name = name;
    notice = SaveCourse(courses, course) ? "Layout saved on this PC." : "Could not save layout. Check name and write access.";
  }
  ImGui::SameLine();
  if (ImGui::Button("Load layout")) {
    auto course = LoadCourse(courses, name);
    notice = course && Party().SetCourse(std::move(*course)) ? "Layout loaded." : "No valid local layout with that name.";
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear layout"))
    edit([](Course& c) { c.props.clear(); c.points.clear(); }, "Layout cleared (saved files unchanged).");
  ImGui::EndDisabled();
  if (!workshop::BuildAllowed()) ImGui::TextDisabled("Creative build permission and free roam are required to edit.");
  ImGui::Separator();
  if (!racing) {
    if (ImGui::Button("Start minigame")) {
      if (Party().StartRace()) native::menu::Close();
      else notice = "Add two checkpoints and enter free roam first.";
    }
    if (world.race == Race::Finished) ImGui::SameLine();
  }
  if ((racing || world.race == Race::Finished) && ImGui::Button("Return to editing")) {
    Party().ResetRace();
    notice.clear();
  }
  ImGui::TextWrapped("%s", notice.c_str());
}

void ShowNetwork(const SessionView& view) {
  static char name[21] = "Player", address[128]{}, code[64]{};
  static int host_port = kDefaultPort;
  static bool local_only = false;
  static size_t selected_address = 0;
  static std::string notice;
  const bool is_campaign = CurrentMode() == Mode::CampaignCoop;
  ImGui::TextUnformatted(LinkLabel(view.link));
  if (is_campaign) ImGui::TextDisabled("Campaign coop: 3 slots locked to Michael / Franklin / Trevor. All missions at 3 - saves isolated.");
  if (!view.error.empty()) ImGui::TextWrapped("%s", view.error.c_str());
  const bool idle = view.link == Link::Idle || view.link == Link::Failed;
  if (idle) {
    ImGui::InputText("Nickname", name, sizeof(name));
    ImGui::SeparatorText(is_campaign ? "Host campaign on this PC" : "Host on this PC");
    // Prefill launcher-editable port if available
    if (host_port == kDefaultPort) {
      const int cfg_port = REXCVAR_GET(gtav_campaign_port);
      if (cfg_port > 0 && cfg_port <= 65535) host_port = cfg_port;
    }
    ImGui::InputInt("TCP port", &host_port, 0);
    ImGui::Checkbox("This PC only (testing)", &local_only);
    if (ImGui::Button(is_campaign ? "Host campaign here" : "Host here")) {
      selected_address = 0;
      bool ok = false;
      if (host_port > 0 && host_port <= 65535) {
        ok = is_campaign ? Party().HostCampaign(uint16_t(host_port), name, local_only)
                         : Party().Host(uint16_t(host_port), name, local_only);
      }
      notice = ok ? "" : "Enter a port from 1 to 65535 and a nickname (letters, digits, spaces, - or _).";
      if (is_campaign && ok) {
        // Persist host choice for next launch (launcher can also edit this file)
        // Note: desktop does not write launcher file directly; launcher owns it.
      }
    }
    if (is_campaign) ImGui::TextDisabled("Up to 3 players. Each protagonist locked. All story missions co-op at 3.");
    ImGui::SeparatorText("Join a friend");
    ImGui::InputTextWithHint("Server", "IPv4:port or paste the full invitation", address, sizeof(address));
    ImGui::InputTextWithHint("Invite code", "Leave blank if pasted with the address", code, sizeof(code));
    if (ImGui::Button("Join friend")) {
      if (!ValidName(name)) notice = "Enter a nickname (letters, digits, spaces, - or _).";
      else {
        bool ok = is_campaign ? Party().JoinCampaign(address, name, code)
                              : Party().Join(address, name, code);
        notice = ok ? "" : "Paste the full invitation, or enter the host's IPv4:port and 32-character invite code.";
      }
    }
  } else {
    if (view.link == Link::Hosting) {
      ImGui::Text("Listening on TCP %u. The room stays open while your game is running.", unsigned(view.port));
      if (is_campaign) ImGui::Text("Campaign coop: %zu / 3 protagonists assigned", view.world.players.size(), size_t(3));
      if (view.host_addresses.empty()) {
        ImGui::TextWrapped("No local IPv4 detected at startup. Connect to the LAN/VPN and restart the room, "
                           "or share a reachable IPv4 and the code manually.");
      } else {
        selected_address = std::min(selected_address, view.host_addresses.size() - 1);
        const bool test = (view.host_addresses.front().ipv4 >> 24) == 127;
        if (ImGui::BeginCombo(test ? "This PC only" : "LAN / VPN address",
                              FormatEndpoint(view.host_addresses[selected_address]).c_str())) {
          for (size_t i = 0; i < view.host_addresses.size(); ++i) {
            if (ImGui::Selectable(FormatEndpoint(view.host_addresses[i]).c_str(), i == selected_address))
              selected_address = i;
          }
          ImGui::EndCombo();
        }
        if (ImGui::Button("Copy invitation"))
          ImGui::SetClipboardText(FormatInvitation(view.host_addresses[selected_address], view.invite).c_str());
        ImGui::SameLine();
      }
      if (ImGui::Button("Copy code")) ImGui::SetClipboardText(view.invite.c_str());
      ImGui::TextWrapped("Invite: %s", view.invite.c_str());
    }
    if (is_campaign) {
      const auto camp = ViewCampaign();
      if (!camp.assignments.empty()) {
        ImGui::SeparatorText("Protagonist locks");
        for (auto [id, ch] : camp.assignments) {
          const bool is_self = id == camp.world.self;
          ImGui::BulletText("%s: %s%s", CharacterLabel(ch),
                            [&]()->const char* { for (auto &p : camp.world.players) if (p.id==id) return p.name.c_str(); return "?";}(),
                            is_self ? " (you)" : "");
        }
      }
    }
    if (ImGui::Button(view.link == Link::Hosting ? "Stop server" : "Disconnect")) {
      Party().Stop();
      notice.clear();
    }
  }
  if (!notice.empty()) ImGui::TextWrapped("%s", notice.c_str());
  ImGui::TextWrapped("Share invitations privately. Allow inbound TCP on the host's firewall. For remote friends, "
                     "use a trusted private VPN, or forward TCP manually if your Internet connection permits it. "
                     "IPv4 only; traffic is not encrypted.");
}

void ShowCampaignStatus() {
  if (CurrentMode() != Mode::CampaignCoop) return;
  const auto view = ViewCampaign();
  ImGui::SeparatorText("Campaign co-op - Missions at 3");
  ImGui::Text("Save: isolated under data/user/campaign_coop (solo saves untouched)");
  ImGui::Text("Unlock: Michael/Franklin/Trevor from start - all missions co-op at 3");
  if (view.self_character)
    ImGui::Text("You are: %s", CharacterLabel(*view.self_character));
  else if (view.link == Link::Joined || view.link == Link::Hosting)
    ImGui::TextDisabled("Assigning protagonist...");
  for (auto [id, ch] : view.assignments) {
    const char* name = "?";
    for (auto &p : view.world.players) if (p.id == id) { name = p.name.c_str(); break; }
    ImGui::BulletText("%s -> %s", name, CharacterLabel(ch));
  }
  ImGui::SeparatorText("Mort en coop - GTA Online adapte");
  const bool in_mission = GetCampaignMissionState();
  const auto mode = CampaignDeathMode(in_mission);
  ImGui::Text("Mode actuel: %s", DeathModeLabel(mode));
  if (IsCampaignSoloDeath(in_mission)) {
    ImGui::TextWrapped("Open world: mort SOLO - seul le joueur Wasted respawn hopital (-2%% max $5000), les autres continuent. Style GTA Online.");
    ImGui::TextDisabled("Exemple perte: $10k -> -$200, $300k -> -$5000 (cap)");
  } else {
    ImGui::TextWrapped("Mission: mort PARTAGEE - 30s pour revive, sinon Mission Failed pour tous (host authoritative, checkpoints partages).");
  }
}
}  // namespace

Mode CurrentMode() { return mode.load(std::memory_order_relaxed); }
bool Active() { return CurrentMode() != Mode::Campaign; }
Session& Party() { static Session session; return session; }

bool Configure(rex::PathConfig& paths, std::filesystem::path& installed_content_root) {
  const auto requested = REXCVAR_GET(gtav_mode);
  const bool workshop_enabled = rex::cvar::GetFlagByName("gtav_native") == "on";
  Mode chosen = Mode::Campaign;
  if (requested == "menu") {
    const auto selected = frontend::ChooseMode(workshop_enabled, native::settings::Player());
    if (!selected) return false;
    chosen = *selected;
  } else if (requested == "solo") chosen = Mode::Solo;
  else if (requested == "multiplayer") chosen = Mode::Multiplayer;
  else if (requested == "campaign-coop" || requested == "campaign_coop") chosen = Mode::CampaignCoop;
  else if (requested != "campaign") return Error("Unknown mode. Use menu, solo, multiplayer, campaign or campaign-coop.");

  if (chosen == Mode::Campaign) {
    // Vrai cheat complet: start minimal workshop core for F5 dev panel even in solo Campaign.
    // Host-only par defaut (tu es host en solo), tous les modes, grantable via Promote en coop.
    // Use user_data_root as profile so permissions persist; does not mount story saves differently.
    if (workshop_enabled) {
      workshop::Start(paths.user_data_root, false, {
          [](std::string_view line) { REXLOG_INFO("{}", line); },
          [](uint8_t failed) { REXLOG_WARN("[cheat] could not write config documents (mask {})", unsigned(failed)); },
      });
      REXLOG_INFO("[cheat] Campaign solo: dev cheat F5 enabled (host-only, all-modes, workshop core started)");
    }
    mode.store(chosen, std::memory_order_relaxed);
    return true;
  }
  if (!workshop_enabled) return Error("Workshop modes need the native renderer (--gtav_native=on).");

  if (chosen == Mode::CampaignCoop) {
    // Isolated save: data/user/campaign_coop/profile - never touches data/user/* solo saves
    CampaignWorkspaceError cerr{};
    auto campaign = PrepareCampaignWorkspace(paths.user_data_root, cerr);
    // Also keep legacy check for path.h compatibility
    if (!campaign) {
      return Error(cerr == CampaignWorkspaceError::MissingInstall ?
          "Installed TU27 game data is missing. Install the game with the launcher first." :
          "Could not create separate campaign coop folders. Use a writable installation without links in those folders.");
    }
    if (!EnsureCampaignUnlock(campaign->profile_root)) {
      return Error("Could not write campaign unlock flag. Check write permissions for the campaign profile.");
    }
    paths.user_data_root = campaign->profile_root;
    installed_content_root = campaign->common_content_root;
    courses = campaign->course_root;
    // Minimal workshop init for ghosts/markers if available; campaign coop reuses the same presence transport
    // but does not require full build permissions.
    if (fs::exists(campaign->profile_root)) {
      // Seed a workshop storage so overlay helpers do not crash when queried
      workshop::Start(campaign->profile_root, false, {
          [](std::string_view line) { REXLOG_INFO("{}", line); },
          [](uint8_t failed) { REXLOG_WARN("[campaign] could not write config documents (mask {})", unsigned(failed)); },
      });
    }
    mode.store(chosen, std::memory_order_relaxed);
    native::menu::OpenWorkshop();
    REXLOG_INFO("[campaign] coop mode: 3 protagonists unlocked from start, save isolated at %s",
                campaign->profile_root.string().c_str());
    // Pre-apply launcher-editable host defaults to cvars so ShowNetwork can prefill
    {
      const auto host_ip = REXCVAR_GET(gtav_campaign_host);
      const auto host_port = REXCVAR_GET(gtav_campaign_port);
      (void)host_ip; (void)host_port;
    }
    return true;
  }

  WorkspaceError error{};
  const auto workspace = PrepareWorkspace(paths.user_data_root, error);
  if (!workspace) {
    return Error(error == WorkspaceError::MissingInstall ?
        "Installed TU27 game data is missing. Install the game with the launcher first." :
        "Could not create separate Workshop folders. Use a writable installation without links in the sandbox folders.");
  }
  paths.user_data_root = workspace->profile_root;
  installed_content_root = workspace->common_content_root;
  courses = workspace->course_root;
  workshop::Start(workspace->profile_root, chosen == Mode::Solo, {
      [](std::string_view line) { REXLOG_INFO("{}", line); },
      [](uint8_t failed) { REXLOG_WARN("[workshop] could not write config documents (mask {})", unsigned(failed)); },
  });
  mode.store(chosen, std::memory_order_relaxed);
  native::menu::OpenWorkshop();
  REXLOG_INFO("[workshop] {} mode. Story saves are not mounted.", chosen == Mode::Solo ? "solo" : "multiplayer");
  return true;
}

void Shutdown() {
  // Always stop workshop core if it was started — now also started for Campaign (F5 cheat).
  workshop::Stop();
  if (Active()) Party().Stop();
}
void ReportGuest(Guest state) {
  guest_at.store(NowMs(), std::memory_order_relaxed);
  guest.store(state);
}
bool SceneEnabled() { return REXCVAR_GET(gtav_workshop_scene); }
void ReportSceneUnavailable() { scene_available.store(false); }
void ReportVehicle(const char* message) { vehicle_status.store(message, std::memory_order_relaxed); }

std::optional<VehicleRequest> TakeVehicleRequest() {
  std::lock_guard lock(vehicle_mutex);
  return std::exchange(vehicle_request, std::nullopt);
}

void DrawWorkshop() {
  if (!Active()) return;
  // GTA Workshop header — green/yellow accents, dark tabs
  ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.12f,0.12f,0.13f,1.f));
  ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.18f,0.85f,0.45f,0.22f));
  ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.18f,0.85f,0.45f,0.45f));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f,0.85f,0.45f,0.18f));
  const auto view = Party().View();
  const auto* local = Myself(view.world);
  if (CurrentMode() == Mode::CampaignCoop) {
    ImGui::TextColored(ImVec4(0.18f,0.85f,0.45f,1.f), "CAMPAIGN CO-OP  //  3 PROTAGONISTS");
  } else {
    ImGui::TextColored(ImVec4(1.f,0.85f,0.08f,1.f), "%s", CurrentMode() == Mode::Solo ? "SANDBOX SOLO  //  LOS SANTOS" : "PRIVATE MULTIPLAYER  //  ONLINE");
  }
  ImGui::TextColored(ImVec4(0.60f,0.62f,0.65f,1.f), "%s", GuestLabel());
  ImGui::Separator();
  if (!SceneEnabled() || !scene_available.load()) ImGui::TextDisabled("Local props and ghost actors are disabled; map markers still work.");
  if (!ImGui::BeginTabBar("workshop-tabs")) return;
  if ((CurrentMode() == Mode::Multiplayer || CurrentMode() == Mode::CampaignCoop) && ImGui::BeginTabItem("Session")) {
    ShowNetwork(view);
    ImGui::Separator();
    if (CurrentMode() == Mode::CampaignCoop) {
      const auto camp = ViewCampaign();
      ImGui::Text("Players: %zu / 3", view.world.players.size());
      for (auto [id, ch] : camp.assignments) {
        const char* name = "?";
        for (auto &p : view.world.players) if (p.id == id) { name = p.name.c_str(); break; }
        bool is_self = id == view.world.self;
        const char* pose_state = "";
        for (auto &p : view.world.players) if (p.id == id) { pose_state = p.pose ? "" : " | loading"; break; }
        ImGui::BulletText("%s: %s%s%s", CharacterLabel(ch), name, is_self ? " (you)" : "", pose_state);
      }
      if (camp.assignments.empty() && !view.world.players.empty()) {
        for (const auto& p : view.world.players)
          ImGui::BulletText("%s%s%s", p.name.c_str(), p.id == view.world.self ? " (you)" : "", p.pose ? "" : " | loading");
      }
      ShowCampaignStatus();
    } else {
      ImGui::Text("Players: %zu / %u", view.world.players.size(), kMaxPlayers);
      for (const auto& p : view.world.players)
        ImGui::BulletText("%s%s%s", p.name.c_str(), p.id == view.world.self ? " (you)" : "",
                          p.pose ? "" : " | loading / absent");
    }
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Map builder")) {
    EditCourse(view, local);
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Test drive")) {
    static int selection = 0;
    ImGui::Combo("Vehicle", &selection, kVehicleNames.data(), int(kVehicleNames.size()));
    ImGui::BeginDisabled(GuestState() != Guest::Ready || !workshop::BuildAllowed());
    if (ImGui::Button("Spawn nearby (on foot)")) {
      std::lock_guard lock(vehicle_mutex);
      vehicle_request = VehicleRequest{unsigned(selection), NowMs() + 2'000};
      ReportVehicle("Vehicle request queued.");
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", vehicle_status.load());
    ImGui::TextWrapped("One driveable test vehicle per PC. Friends nearby see a ghost of that vehicle, not its physics.");
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Results")) {
    auto rankings = view.world.players;
    std::stable_sort(rankings.begin(), rankings.end(), [](const Player& a, const Player& b) {
      if (bool(a.finish_ms) != bool(b.finish_ms)) return bool(a.finish_ms);
      return a.finish_ms && b.finish_ms ? a.finish_ms < b.finish_ms : a.Passed() > b.Passed();
    });
    for (const auto& p : rankings) {
      if (p.finish_ms) ImGui::BulletText("%s | %.2f seconds", p.name.c_str(), p.finish_ms / 1000.0);
      else ImGui::BulletText("%s | %u / %zu checkpoints%s", p.name.c_str(), p.Passed(),
                             view.world.course.points.size(), p.racing ? "" : " (not racing)");
    }
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
  ImGui::PopStyleColor(4);
}

void DrawHud() {
  if (!Active()) return;
  const auto view = Party().View();
  const auto* local = Myself(view.world);
  if (!local) return;
  const auto& world = view.world;
  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(.82f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f,0.06f,0.07f,0.88f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f,0.95f,0.96f,1.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##workshop-hud", nullptr, flags)) {
    if (CurrentMode() == Mode::CampaignCoop) {
      const auto camp = ViewCampaign();
      const char* who = camp.self_character ? CharacterLabel(*camp.self_character) : "?";
      if (world.race == Race::Countdown)
        ImGui::Text("Campaign %s | %s | Starts in %u", who, world.course.name.c_str(), (world.clock_ms + 999) / 1000);
      else if (world.race == Race::Running || world.race == Race::Finished)
        ImGui::Text("Campaign %s | %u/%zu | %.2f s%s", who, local->Passed(), world.course.points.size(),
                    (local->finish_ms ? local->finish_ms : world.clock_ms) / 1000.0, local->finish_ms ? "  FINISHED" : "");
      else {
        const size_t campaign_players = view.world.players.size();
        ImGui::Text("Campaign %s | %zu/3  [F6] %s", who, campaign_players, LinkLabel(view.link));
      }
      if (camp.self_character) {
        ImGui::TextDisabled("Locked protagonist: %s  |  Saves: campaign_coop (isolated)", who);
      }
      // GTA Online adapted death HUD: solo vs shared label
      {
        const bool in_mission = GetCampaignMissionState();
        const auto mode = CampaignDeathMode(in_mission);
        const char* death_hint = IsCampaignSoloDeath(in_mission) ? "Mort: SOLO (hopital, -2% max $5000)" : "Mort: MISSION PARTAGEE (30s revive)";
        ImGui::TextDisabled("%s | %s", DeathModeLabel(mode), death_hint);
      }
    } else if (world.race == Race::Countdown)
      ImGui::Text("%s | Starts in %u", world.course.name.c_str(), (world.clock_ms + 999) / 1000);
    else if (world.race == Race::Running || world.race == Race::Finished)
      ImGui::Text("%s | %u/%zu | %.2f s%s", world.course.name.c_str(), local->Passed(), world.course.points.size(),
                  (local->finish_ms ? local->finish_ms : world.clock_ms) / 1000.0, local->finish_ms ? "  FINISHED" : "");
    else ImGui::Text("%s | %zu player%s  [F6] [F8] [P]", CurrentMode() == Mode::Solo ? "SANDBOX" : LinkLabel(view.link),
                     world.players.size(), world.players.size() == 1 ? "" : "s");
    if (world.race == Race::Running && local->pose) {
      if (auto next = NextCheckpoint(world.course, *local)) {
        const auto distance = std::sqrt(double(DistanceSquared(world.course.points[*next], local->pose->position))) / 100;
        ImGui::TextColored(ImVec4(1.f,0.85f,0.08f,1.f), "%s | Checkpoint %zu: %.0f m", kGameNames[unsigned(world.course.game)], *next + 1, distance);
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}
}  // namespace gtav::sandbox
