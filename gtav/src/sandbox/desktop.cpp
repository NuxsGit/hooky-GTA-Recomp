#include "desktop.h"

#include "courses.h"
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

namespace gtav::sandbox {
namespace {
std::atomic<Mode> mode{Mode::Campaign};
std::filesystem::path courses;
std::atomic<Guest> guest{Guest::Loading};
std::atomic<int64_t> guest_at{0};
std::mutex vehicle_mutex;
std::optional<VehicleRequest> vehicle_request;
std::atomic<const char*> vehicle_status{"Select a local test vehicle."};
constexpr std::array<const char*, 5> kVehicleModels = {"blista", "sultan", "adder", "bati", "bmx"};
constexpr std::array<const char*, 5> kVehicleNames = {"Blista", "Sultan", "Adder", "Bati 801", "BMX"};

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
  const bool racing = world.race == Race::Countdown || world.race == Race::Running;
  ImGui::Text("Active course: %s | %zu / %u checkpoints", world.course.name.c_str(), world.course.points.size(), kMaxCheckpoints);
  if (!view.Owner()) {
    ImGui::TextDisabled("The host owns the layout and race. You can play it, not edit it.");
    return;
  }
  ImGui::BeginDisabled(racing);
  ImGui::InputText("Course name", name, sizeof(name));
  ImGui::BeginDisabled(!local || !local->pose || GuestState() != Guest::Ready);
  if (ImGui::Button("Add checkpoint here") && local && local->pose) {
    auto course = world.course;
    course.name = name;
    course.points.push_back(*local->pose);
    notice = Party().SetCourse(std::move(course)) ? "Checkpoint added." :
        "Use a valid name, up to 24 checkpoints and at least 15 m between consecutive points.";
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Undo last") && !world.course.points.empty()) {
    auto course = world.course;
    course.points.pop_back();
    Party().SetCourse(std::move(course));
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear layout"))
    notice = Party().SetCourse(Course{name, {}}) ? "Layout cleared (saved files are unchanged)." : "Use a valid course name.";
  if (ImGui::Button("Save course")) {
    auto course = world.course;
    course.name = name;
    notice = SaveCourse(courses, course) ? "Course saved on this PC." : "Could not save course. Check name and write access.";
  }
  ImGui::SameLine();
  if (ImGui::Button("Load course")) {
    auto course = LoadCourse(courses, name);
    notice = course && Party().SetCourse(std::move(*course)) ? "Course loaded." : "No valid local course with that name.";
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  if (!racing) {
    if (ImGui::Button("Start checkpoint race")) {
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
  ImGui::TextDisabled("Course files store checkpoint layouts on the existing GTA map, not terrain or scripts.");
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
  Mode chosen = Mode::Campaign;
  if (requested == "menu") {
    const SDL_MessageBoxButtonData choices[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Sandbox solo"},
        {0, 2, "Private multiplayer"}, {0, 3, "Campaign"}, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
    const SDL_MessageBoxData menu{SDL_MESSAGEBOX_INFORMATION, SDL_GetKeyboardFocus(), "GTA V Recomp | Play",
        "Choose how to play.\n\nSandbox modes use a separate local profile. Multiplayer lets you host or join a friend's PC.\n"
        "Campaign opens your existing story saves.", int(std::size(choices)), choices, nullptr};
    int id = -1;
    if (!SDL_ShowMessageBox(&menu, &id)) return Error("Could not open the game mode selector. Use --gtav_mode explicitly.");
    if (id == 1) chosen = Mode::Solo;
    else if (id == 2) chosen = Mode::Multiplayer;
    else if (id != 3) return false;
  } else if (requested == "solo") chosen = Mode::Solo;
  else if (requested == "multiplayer") chosen = Mode::Multiplayer;
  else if (requested != "campaign") return Error("Unknown mode. Use menu, solo, multiplayer or campaign.");

  if (chosen == Mode::Campaign) return true;
  if (rex::cvar::GetFlagByName("gtav_native") != "on") return Error("Workshop modes need the native renderer (--gtav_native=on).");
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
const char* VehicleModel(unsigned preset) { return preset < kVehicleModels.size() ? kVehicleModels[preset] : nullptr; }
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
  if (ImGui::BeginTabItem("Course builder")) {
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
    ImGui::TextDisabled("One local test vehicle at a time. Vehicle physics are not replicated.");
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Race results")) {
    auto rankings = view.world.players;
    std::stable_sort(rankings.begin(), rankings.end(), [](const Player& a, const Player& b) {
      if (bool(a.finish_ms) != bool(b.finish_ms)) return bool(a.finish_ms);
      return a.finish_ms && b.finish_ms ? a.finish_ms < b.finish_ms : a.passed > b.passed;
    });
    for (const auto& p : rankings) {
      if (p.finish_ms) ImGui::BulletText("%s | %.2f seconds", p.name.c_str(), p.finish_ms / 1000.0);
      else ImGui::BulletText("%s | %u / %zu checkpoints%s", p.name.c_str(), unsigned(p.passed),
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
      ImGui::Text("%s | %u/%zu | %.2f s%s", world.course.name.c_str(), unsigned(local->passed), world.course.points.size(),
                  (local->finish_ms ? local->finish_ms : world.clock_ms) / 1000.0, local->finish_ms ? "  FINISHED" : "");
    else ImGui::Text("%s | %zu player%s  [F6]", CurrentMode() == Mode::Solo ? "SANDBOX" : LinkLabel(view.link),
                     world.players.size(), world.players.size() == 1 ? "" : "s");
    if (world.race == Race::Running && local->pose && local->racing && local->passed < world.course.points.size()) {
      const auto p = world.course.points[local->passed];
      const auto distance = std::sqrt(double(DistanceSquared(p, *local->pose))) / 100;
      ImGui::Text("Checkpoint %u: %.0f m away (yellow map marker)", unsigned(local->passed + 1), distance);
    }
  }
  ImGui::End();
}
}  // namespace gtav::sandbox
