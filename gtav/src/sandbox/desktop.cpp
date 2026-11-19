#include "desktop.h"

#include "courses.h"
#include "../frontend/play_hub.h"
#include "../native/menu.h"

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

REXCVAR_DEFINE_STRING(gtav_mode, "campaign", "GTAV", "Desktop startup mode: menu | solo | multiplayer | campaign");
REXCVAR_DEFINE_BOOL(gtav_workshop_scene, true, "GTAV", "Workshop: show local props and remote ghosts in free roam");

namespace gtav::sandbox {
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
  switch (GuestState()) {
    case Guest::Loading: return "Waiting for the TU27 world. Load a sandbox save and enter free roam.";
    case Guest::Ready: return "Free roam ready. F6 toggles the Workshop.";
    case Guest::Busy: return "Waiting for free roam (not during a cutscene, mission or pause).";
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
  ImGui::BeginDisabled(racing);
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
  static char name[21] = "Player", address[16] = "127.0.0.1", code[33]{};
  static int port = kDefaultPort;
  static bool local_only = false;
  static std::string notice;
  ImGui::TextUnformatted(LinkLabel(view.link));
  if (!view.error.empty()) ImGui::TextWrapped("%s", view.error.c_str());
  const bool idle = view.link == Link::Idle || view.link == Link::Failed;
  if (idle) {
    ImGui::InputText("Nickname", name, sizeof(name));
    ImGui::InputInt("TCP port", &port, 0);
    const bool port_ok = port > 0 && port <= 65535;
    ImGui::Checkbox("This PC only (testing)", &local_only);
    if (ImGui::Button("Host here"))
      notice = port_ok && Party().Host(uint16_t(port), name, local_only) ? "" :
          "Enter a port from 1 to 65535 and a nickname (letters, digits, spaces, - or _).";
    ImGui::Spacing();
    ImGui::InputText("Host IPv4", address, sizeof(address));
    ImGui::InputText("Invite code", code, sizeof(code), ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::Button("Join friend")) {
      if (port_ok && Party().Join(address, uint16_t(port), name, code)) {
        std::fill(std::begin(code), std::end(code), '\0');
        notice.clear();
      } else notice = "Enter the host IPv4 address, TCP port and 32-character invite code.";
    }
  } else {
    if (view.link == Link::Hosting) {
      ImGui::Text("Listening on TCP %u. Share your reachable IPv4 and this code privately.", unsigned(view.port));
      ImGui::TextUnformatted(view.invite.c_str());
      ImGui::SameLine();
      if (ImGui::Button("Copy code")) ImGui::SetClipboardText(view.invite.c_str());
    }
    if (ImGui::Button(view.link == Link::Hosting ? "Stop server" : "Disconnect")) {
      Party().Stop();
      notice.clear();
    }
  }
  if (!notice.empty()) ImGui::TextWrapped("%s", notice.c_str());
  ImGui::TextWrapped("For friends outside your LAN, forward this TCP port yourself or use a trusted private VPN. "
                     "IPv4 only. Traffic is not encrypted. No Rockstar services or public matchmaking.");
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
    const auto selected = frontend::ChooseMode(workshop_enabled);
    if (!selected) return false;
    chosen = *selected;
  } else if (requested == "solo") chosen = Mode::Solo;
  else if (requested == "multiplayer") chosen = Mode::Multiplayer;
  else if (requested == "campaign-coop") return Error("Campaign co-op is unavailable: story missions are not synchronized.");
  else if (requested != "campaign") return Error("Unknown mode. Use menu, solo, multiplayer or campaign.");

  if (chosen == Mode::Campaign) return true;
  if (!workshop_enabled) return Error("Workshop modes need the native renderer (--gtav_native=on).");
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
  mode.store(chosen, std::memory_order_relaxed);
  native::menu::OpenWorkshop();
  REXLOG_INFO("[workshop] {} mode. Story saves are not mounted.", chosen == Mode::Solo ? "solo" : "multiplayer");
  return true;
}

void Shutdown() { if (Active()) Party().Stop(); }
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
  const auto view = Party().View();
  const auto* local = Myself(view.world);
  ImGui::TextUnformatted(CurrentMode() == Mode::Solo ? "SANDBOX SOLO" : "PRIVATE MULTIPLAYER");
  ImGui::TextWrapped("%s", GuestLabel());
  if (!SceneEnabled() || !scene_available.load()) ImGui::TextDisabled("Local props and ghost actors are disabled; map markers still work.");
  if (!ImGui::BeginTabBar("workshop-tabs")) return;
  if (CurrentMode() == Mode::Multiplayer && ImGui::BeginTabItem("Session")) {
    ShowNetwork(view);
    ImGui::Separator();
    ImGui::Text("Players: %zu / %u", view.world.players.size(), kMaxPlayers);
    for (const auto& p : view.world.players)
      ImGui::BulletText("%s%s%s", p.name.c_str(), p.id == view.world.self ? " (you)" : "",
                        p.pose ? "" : " | loading / absent");
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Map builder")) {
    EditCourse(view, local);
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Test drive")) {
    static int selection = 0;
    ImGui::Combo("Vehicle", &selection, kVehicleNames.data(), int(kVehicleNames.size()));
    ImGui::BeginDisabled(GuestState() != Guest::Ready);
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
}

void DrawHud() {
  if (!Active()) return;
  const auto view = Party().View();
  const auto* local = Myself(view.world);
  if (!local) return;
  const auto& world = view.world;
  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(.7f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##workshop-hud", nullptr, flags)) {
    if (world.race == Race::Countdown)
      ImGui::Text("%s | Starts in %u", world.course.name.c_str(), (world.clock_ms + 999) / 1000);
    else if (world.race == Race::Running || world.race == Race::Finished)
      ImGui::Text("%s | %u/%zu | %.2f s%s", world.course.name.c_str(), local->Passed(), world.course.points.size(),
                  (local->finish_ms ? local->finish_ms : world.clock_ms) / 1000.0, local->finish_ms ? "  FINISHED" : "");
    else ImGui::Text("%s | %zu player%s  [F6]", CurrentMode() == Mode::Solo ? "SANDBOX" : LinkLabel(view.link),
                     world.players.size(), world.players.size() == 1 ? "" : "s");
    if (world.race == Race::Running && local->pose) {
      if (auto next = NextCheckpoint(world.course, *local)) {
        const auto distance = std::sqrt(double(DistanceSquared(world.course.points[*next], local->pose->position))) / 100;
        ImGui::Text("%s | Checkpoint %zu: %.0f m (yellow marker)", kGameNames[unsigned(world.course.game)], *next + 1, distance);
      }
    }
  }
  ImGui::End();
}
}  // namespace gtav::sandbox
