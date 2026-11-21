// TU27 story-mode debug tools. Script native wrappers take a three-word Xbox context in r3:
// big-endian {result pointer, argument count, argument pointer}, four bytes per value.
// See docs/DEBUG_AND_UPDATES.md for ABI references, guards and the play-test checklist.
#include "debug_tools.h"

#include "frame_pacing.h"
#include "native/tu27_call.h"
#include "sandbox/campaign_death.h"
#include "sandbox/desktop.h"

#ifdef GTAV_DESKTOP_WORKSHOP
#include "native/capture.h"
#include "sandbox/CreativeRuntime.h"
#include "sandbox/Tu27Workshop.h"
#include <rex/cvar.h>
REXCVAR_DEFINE_BOOL(gtav_workshop_extended_tu27, false, "GTAV",
    "Workshop: enable experimental TU27 output-pointer and string natives (ground/waypoint/weather/max ammo) and Noclip");
#endif

#include <imgui.h>
#include <rex/hook.h>
#include <rex/logging/sink.h>
#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace gtav::debug {
namespace {

using Clock = std::chrono::steady_clock;
Controls controls;
std::shared_ptr<rex::LogCaptureSink> log_sink;
std::atomic<int64_t> last_script_ms{0};
std::atomic<uint32_t> last_game_timer{0};
std::atomic<bool> mode_running{false};
enum class Status { Waiting, Unavailable, Blocked, Ready, Gave, Teleported, Flying, Camera, Rejected, Restocked };
std::atomic<Status> status{Status::Waiting};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

using tu27::Load;
using tu27::NativeCall;
using tu27::Readable;
using tu27::Vec;

// Inspect a real, live call before attempting any synthetic native invocation. No guessed
// host pointers, arbitrary guest reads, or changes to the caller's native context.
std::optional<uint32_t> ScriptSlot(PPCContext& ctx, rex::memory::Memory& mem, unsigned nargs, bool result) {
  const uint32_t call = ctx.r3.u32;
  if (!Readable(mem, call, 12) || Load(mem, call + 4) != nargs) return std::nullopt;
  const uint32_t slot = Load(mem, call + (result ? 0 : 8));
  return Readable(mem, slot, 4) ? std::optional(slot) : std::nullopt;
}

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)

// Addresses and Jenkins hashes are paired in tools/scripttools/handlers.txt. All entries
// are in the existing TU27 partition. No dependency on an external mod loader.
enum Native : uint32_t {
  kRenderCams = 0x82C758E0, kCreateCam = 0x82C75A58, kDestroyCam = 0x82C75AB8,
  kCamExists = 0x82C75B40, kRenderingCam = 0x82C75C88, kCamCoord = 0x82C75E98, kCamRot = 0x82C75EA8,
  kFadedIn = 0x82C766B0, kGameplayCoord = 0x82C767A0, kGameplayRot = 0x82C76808,
  kGameplayFov = 0x82C76860, kGameplayCam = 0x82C76B80, kCutscene = 0x82C78A68,
  kExists = 0x82C7B318, kPaused = 0x82C835F0, kMission = 0x82C86F70, kTimer = 0x82C87678,
  kCoords = 0x82C9ADD8, kHeading = 0x82C9AF18, kDead = 0x82C9B7D0,
  kSetCoords = 0x82C9C000, kVelocity = 0x82C9C2A0, kNetworkGame = 0x82CDBB10,
  kNetworkSession = 0x82CDBB28, kPlayerControl = 0x82CE9818, kPlayer = 0x82CE9A20,
  kPlayerPed = 0x82CE9CA8, kSwitching = 0x82CF1220, kInVehicle = 0x82D0BF30,
  kGive = 0x82D575F0, kSetHealth = 0x82C9C078, kMaxHealth = 0x82D0E0D8,
  kSetArmor = 0x82D0C3C8, kMaxArmor = 0x82CE97B8,
#ifdef GTAV_DESKTOP_WORKSHOP
  kAddBlip = 0x82C81950, kBlipCoords = 0x82C81A10, kBlipColor = 0x82C81CC8,
  kRemoveBlip = 0x82C82248, kBlipExists = 0x82C824D8,
  kRequestModel = 0x82CF0CC0, kModelLoaded = 0x82CF0A90, kReleaseModel = 0x82CF0AE0,
  kIsVehicleModel = 0x82CF0C48, kModelOnDisc = 0x82CF0B50,
  kCreateVehicle = 0x82D5CBC8, kDeleteVehicle = 0x82D4E058, kGroundVehicle = 0x82D4E168,
  kEntityType = 0x82C9B4E8, kDeleteEntity = 0x82C9BC50, kNoCollision = 0x82C9BFA8,
  kDynamic = 0x82C9C048, kSetHeading = 0x82C9C068, kInvincible = 0x82C9C088,
  kGravity = 0x82C9C2B0, kAlpha = 0x82C9C320, kPedVehicle = 0x82D0C510,
  kBlockPed = 0x82D1CD70, kCreatePed = 0x82CE60F0, kCreateObject = 0x82D1E4B0,
  kLockDoors = 0x82D4E510,
#endif
};

constexpr uint32_t HashName(const char* name) {
  uint32_t hash = 0;
  for (; *name; ++name) {
    hash += uint8_t(*name);
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  hash += hash << 3;
  hash ^= hash >> 11;
  return hash + (hash << 15);
}

uint32_t F(float value) { return std::bit_cast<uint32_t>(value); }
float Rad(float degrees) { return degrees * std::numbers::pi_v<float> / 180.f; }
Vec Horizontal(float yaw, float forward, float right, float up) {
  const float s = std::sin(Rad(yaw)), c = std::cos(Rad(yaw));
  return {-s * forward + c * right, c * forward + s * right, up};
}

uint32_t PlayablePed(NativeCall& call, bool mission_assist = false) {
  // Campaign co-op at 3: keep presence and workshop sync during missions (all story missions playable at 3)
  const bool campaign_missions = sandbox::CurrentMode() == sandbox::Mode::CampaignCoop;
  // Track mission state for GTA Online style death routing (solo open world vs shared missions).
  if (campaign_missions) {
    const bool in_mission = call.Flag(kMission);
    if (call.Okay()) sandbox::SetCampaignMissionState(in_mission);
  }
  if (gtav::InFailureEffect() || call.Flag(kNetworkGame) || call.Flag(kNetworkSession) ||
      (!campaign_missions && !mission_assist && call.Flag(kMission)) || call.Flag(kCutscene) || call.Flag(kPaused) ||
      call.Flag(kSwitching) || !call.Flag(kFadedIn) || !call.Okay()) return 0;
  const auto player = call.Read(kPlayer);
  if (!call.Okay() || player >= 32 || !call.Flag(kPlayerControl, {player})) return 0;
  const auto ped = call.Read(kPlayerPed);
  return ped && call.Flag(kExists, {ped}) && !call.Flag(kDead, {ped, 0}) && call.Okay() ? ped : 0;
}

// Only touched by the one guest script thread which passed the runtime ABI check.
struct Cheats {
  uint64_t epoch = 0;
  uint32_t cam = 0, active_ped = 0;
  bool fly = false;
  int64_t last_tick = 0, cam_started = 0;
  Vec origin{}, position{}, rotation{};

  bool Active() const { return cam != 0 || fly; }
  bool Stop(NativeCall& call) {
    fly = false;  // Flight only applies velocity; no invulnerability, collision or gravity edits.
    if (cam) {
      auto cleanup = call.CleanupCall();
      const bool exists = cleanup.Flag(kCamExists, {cam});
      if (!cleanup.Okay()) return false;
      if (exists) {
        const auto rendering = cleanup.Read(kRenderingCam);
        if (!cleanup.Okay() || (rendering == cam && !cleanup.Invoke(kRenderCams, {0, 0, 0, 1, 0, 0})) ||
            !cleanup.Invoke(kDestroyCam, {cam, 0})) return false;
      }
      cam = 0;
    }
    active_ped = 0;
    return true;
  }

  void Tick(NativeCall& call, const Controls::Input& in, int64_t now) {
    const float dt = std::clamp((now - last_tick) / 1000.f, 0.f, .05f);
    last_tick = now;
    if ((epoch != in.epoch || !in.armed) && !Stop(call)) { status = Status::Unavailable; return; }
    epoch = in.epoch;
    if (!in.armed) { status = Status::Ready; return; }

    // Only this one-shot aid can run during a playable Story mission. It does not
    // touch mission flags, checkpoints, wanted level, inventory, or save data directly.
    if (in.action == Action::MissionRestock) {
      if (Active()) { status = Status::Rejected; return; }
      const auto ped = PlayablePed(call, true);
      if (!ped) { status = Status::Blocked; return; }
      const auto player = call.Read(kPlayer);
      const auto health = call.Read(kMaxHealth, {ped});
      const auto armor = call.Read(kMaxArmor, {player});
      if (!call.Okay() || player >= 32 || health == 0 || health > 10000 || armor > 10000 ||
          !controls.StillArmed(epoch)) { status = Status::Rejected; return; }
      status = call.Invoke(kSetHealth, {ped, health}) && (!armor || call.Invoke(kSetArmor, {ped, armor}))
                   ? Status::Restocked : Status::Rejected;
      return;
    }

    // Do not invoke actions from boot, a mission/cutscene, pause/load, or a network session.
    // Do not reapply a toggle when the world becomes playable again.
    const auto ped = PlayablePed(call);
    if (!ped) {
      Stop(call);
      if (in.action || status != Status::Restocked) status = Status::Blocked;
      return;
    }
    const bool on_foot = !call.Flag(kInVehicle, {ped, 0}) && call.Okay();
    if (Active() && (ped != active_ped || (fly && !on_foot))) {
      controls.Reset();
      status = Stop(call) ? Status::Blocked : Status::Unavailable;
      return;
    }
    if (!controls.StillArmed(epoch)) { Stop(call); return; }

    if (in.action) {
      switch (*in.action) {
        case Action::MissionRestock: break;  // handled before the free-roam gate
        case Action::Stop: status = Stop(call) ? Status::Ready : Status::Unavailable; return;
        case Action::Give:
          status = call.Invoke(kGive, {ped, HashName("weapon_pistol"), 60, 0}) ? Status::Gave : Status::Rejected;
          break;
        case Action::Teleport: {
          if (!on_foot || Active() || !call.Flag(kGameplayCam)) { status = Status::Rejected; break; }
          const auto here = call.Vector(kCoords, {ped, 1});
          const float heading = call.Float(kHeading, {ped});
          if (!here.Valid(20'000.f) || !std::isfinite(heading) || !call.Okay()) { status = Status::Rejected; break; }
          const auto step = Horizontal(heading, 10.f, 0.f, 2.f);
          if (!controls.StillArmed(epoch)) return;
          status = call.Invoke(kSetCoords, {ped, F(here.x + step.x), F(here.y + step.y), F(here.z + step.z), 0, 0, 1})
                       ? Status::Teleported : Status::Rejected;
          break;
        }
        case Action::Fly:
          if (fly) { fly = false; status = Status::Ready; }
          else if (on_foot && !cam && call.Flag(kGameplayCam) && call.Okay()) { fly = true; active_ped = ped; }
          else status = Status::Rejected;
          break;
        case Action::Freecam: {
          if (cam) { status = Stop(call) ? Status::Ready : Status::Unavailable; return; }
          if (fly || !call.Flag(kGameplayCam)) { status = Status::Rejected; break; }
          origin = position = call.Vector(kGameplayCoord);
          rotation = call.Vector(kGameplayRot, {2});
          const float fov = call.Float(kGameplayFov);
          if (!origin.Valid(20'000.f) || !rotation.Valid(360.f) || !(fov > 0.f && fov < 180.f) || !call.Okay()) {
            status = Status::Rejected;
            break;
          }
          if (!controls.StillArmed(epoch)) return;
          const auto created = call.Read(kCreateCam, {HashName("default_scripted_camera"), F(position.x),
              F(position.y), F(position.z), F(rotation.x), F(rotation.y), F(rotation.z), F(fov), 1, 2});
          if (created && created != UINT32_MAX && call.Flag(kCamExists, {created}) && call.Okay()) {
            cam = created;
            active_ped = ped;
            cam_started = now;
            call.Invoke(kRenderCams, {1, 0, 0, 1, 0, 0});
          } else status = Status::Rejected;
          break;
        }
      }
    }
    if (!controls.StillArmed(epoch)) { Stop(call); return; }
    if (cam && call.Okay()) {
      const uint32_t rendered = call.Read(kRenderingCam);
      // Allow one tick for camera activation; relinquish if a game script took over later.
      if (rendered != cam && now - cam_started >= 400) { Stop(call); status = Status::Blocked; return; }
      rotation.x = std::clamp(rotation.x + 80.f * dt * in.Axis(Key::LookUp, Key::LookDown), -85.f, 85.f);
      rotation.z = std::remainder(rotation.z + 100.f * dt * in.Axis(Key::LookLeft, Key::LookRight), 360.f);
      const auto step = Horizontal(rotation.z, 12.f * dt * in.Axis(Key::Forward, Key::Back),
                                   12.f * dt * in.Axis(Key::Right, Key::Left), 8.f * dt * in.Axis(Key::Up, Key::Down));
      Vec target{position.x + step.x, position.y + step.y, position.z + step.z};
      if (Vec{target.x - origin.x, target.y - origin.y, target.z - origin.z}.Valid(50.f)) position = target;
      call.Invoke(kCamCoord, {cam, F(position.x), F(position.y), F(position.z)});
      call.Invoke(kCamRot, {cam, F(rotation.x), F(rotation.y), F(rotation.z), 2});
      status = Status::Camera;
    } else if (fly && call.Okay()) {
      const float heading = call.Float(kHeading, {ped});
      if (!std::isfinite(heading) || !call.Okay()) { fly = false; status = Status::Rejected; return; }
      const auto v = Horizontal(heading, 12.f * in.Axis(Key::Forward, Key::Back),
                                12.f * in.Axis(Key::Right, Key::Left), 5.f * in.Axis(Key::Up, Key::Down));
      call.Invoke(kVelocity, {ped, F(v.x), F(v.y), F(v.z)});
      status = Status::Flying;
    } else if (!in.action && (status == Status::Blocked || status == Status::Waiting || status == Status::Flying ||
                             status == Status::Camera)) {
      status = Status::Ready;
    }
    if (!call.Okay()) { controls.Reset(); Stop(call); status = Status::Unavailable; }
  }
};

#ifdef GTAV_DESKTOP_WORKSHOP
// Models come from two small local palettes, not from peer-supplied hashes. A local view is
// reconciled on the already guarded guest script thread. No physics, tasks, or damage packets.
struct WorkshopScene {
  struct Instance { sandbox::Actor actor; uint32_t handle; };
  std::vector<Instance> entities;
  std::array<int64_t, sandbox::kPropModels.size() + sandbox::kVehicleModels.size() + 1> retry{};
  size_t model_slot = 0;
  uint32_t loading = 0;
  int64_t load_by = 0;
  bool checked = false, disabled = false;

  static size_t Slot(const sandbox::Actor& actor) {
    switch (actor.kind) {
      case sandbox::Visual::Prop: return actor.model;
      case sandbox::Visual::Vehicle: return sandbox::kPropModels.size() + actor.model;
      default: return sandbox::kPropModels.size() + sandbox::kVehicleModels.size();
    }
  }
  static uint32_t Model(const sandbox::Actor& a) {
    switch (a.kind) {
      case sandbox::Visual::Prop: return HashName(sandbox::kPropModels[a.model]);
      case sandbox::Visual::Vehicle: return HashName(sandbox::kVehicleModels[a.model]);
      default: return HashName("a_m_y_skater_01");
    }
  }
  static uint32_t Type(sandbox::Visual kind) {
    return kind == sandbox::Visual::Ped ? 1 : kind == sandbox::Visual::Vehicle ? 2 : 3;
  }
  static bool SameModel(const sandbox::Actor& a, const sandbox::Actor& b) {
    return a.kind == b.kind && a.model == b.model;
  }
  static bool Present(NativeCall& call, const Instance& e) {
    return call.Flag(kExists, {e.handle}) && call.Read(kEntityType, {e.handle}) == Type(e.actor.kind) && call.Okay();
  }
  static bool Remove(NativeCall& call, const Instance& e) {
    auto cleanup = call.CleanupCall();
    if (Present(cleanup, e)) {
      cleanup.Invoke(kDeleteEntity, {}, nullptr, e.handle);
      if (Present(cleanup, e)) return false;
    }
    return cleanup.Okay();
  }
  void Release(NativeCall& call) {
    if (!loading) return;
    auto cleanup = call.CleanupCall();
    cleanup.Invoke(kReleaseModel, {loading});
    loading = 0;
  }

  uint32_t Spawn(NativeCall& call, const sandbox::Actor& a, uint32_t hash) {
    const auto x = F(a.position.x / 100.f), y = F(a.position.y / 100.f), z = F(a.position.z / 100.f);
    const auto heading = F(a.heading / 10.f);
    uint32_t entity = 0;
    switch (a.kind) {
      case sandbox::Visual::Prop:
        entity = call.Read(kCreateObject, {hash, x, y, z, 0, 0, 0});
        break;
      case sandbox::Visual::Ped:
        entity = call.Read(kCreatePed, {4, hash, x, y, z, heading, 0, 1});
        break;
      case sandbox::Visual::Vehicle:
        entity = call.Read(kCreateVehicle, {hash, x, y, z, heading, 0, 1});
        break;
    }
    if (!entity || entity == UINT32_MAX) return 0;
    if (!Present(call, {a, entity})) return 0;
    if (a.kind == sandbox::Visual::Ped) call.Invoke(kBlockPed, {entity, 1});
    if (a.kind == sandbox::Visual::Vehicle) call.Invoke(kLockDoors, {entity, 2});
    if (a.kind != sandbox::Visual::Prop) {
      call.Invoke(kNoCollision, {entity, 0, 0});
      call.Invoke(kAlpha, {entity, 150, 0});
    }
    call.Invoke(kInvincible, {entity, 1});
    call.Invoke(kDynamic, {entity, 0});
    call.Invoke(kGravity, {entity, 0});
    call.Invoke(kSetHeading, {entity, heading});
    if (call.Okay()) return entity;
    Remove(call, {a, entity});
    return 0;
  }

  void Update(NativeCall& call, std::vector<sandbox::Actor> wanted, int64_t now, bool streaming_allowed) {
    if (!checked && !wanted.empty()) {
      checked = true;
      // Fail before creating anything if the installed mapping can't configure or delete it.
      if (!call.Supports({kExists, kEntityType, kDeleteEntity, kModelOnDisc, kRequestModel, kModelLoaded,
                          kReleaseModel, kCreateObject, kCreatePed, kCreateVehicle, kDynamic, kGravity,
                          kInvincible, kBlockPed, kNoCollision, kAlpha, kLockDoors, kSetCoords, kSetHeading}))
        Disable();
    }
    if (disabled) wanted.clear();
    for (auto it = entities.begin(); it != entities.end();) {
      const auto target = std::find_if(wanted.begin(), wanted.end(), [&](const sandbox::Actor& a) {
        return a.key == it->actor.key && SameModel(a, it->actor);
      });
      if (target == wanted.end()) {
        if (Remove(call, *it)) it = entities.erase(it);
        else { Disable(); ++it; }
        continue;
      }
      if (!Present(call, *it)) {
        if (!call.Okay()) break;  // keep the tracked handle for cleanup with fresh scratch
        it = entities.erase(it);
        continue;
      }
      const auto& a = *target;
      const auto h = it->handle;
      if (a.position != it->actor.position)
        call.Invoke(kSetCoords, {h, F(a.position.x / 100.f), F(a.position.y / 100.f), F(a.position.z / 100.f), 0, 0, 0});
      if (a.heading != it->actor.heading) call.Invoke(kSetHeading, {h, F(a.heading / 10.f)});
      if (!call.Okay()) break;
      it->actor = a;
      wanted.erase(target);
      ++it;
    }
    if (call.Okay() && !disabled && streaming_allowed) {
      // One outstanding model and at most one spawn per tick. Prefer finishing a pending load
      // over requesting another model when the scene changes underneath it.
      const auto next = std::find_if(wanted.begin(), wanted.end(), [&](const sandbox::Actor& a) {
        const auto slot = Slot(a);
        return loading ? slot == model_slot : now >= retry[slot];
      });
      if (next == wanted.end()) {
        Release(call);
      } else if (!loading) {
        model_slot = Slot(*next);
        const auto hash = Model(*next);
        if (call.Flag(kModelOnDisc, {hash}) && call.Invoke(kRequestModel, {hash})) {
          loading = hash;
          load_by = now + 3'000;
        } else retry[model_slot] = now + 30'000;
      } else if (now >= load_by) {
        retry[model_slot] = now + 30'000;
        Release(call);
      } else if (call.Flag(kModelLoaded, {loading})) {
        const auto handle = Spawn(call, *next, loading);
        if (handle) entities.push_back({*next, handle});
        else retry[model_slot] = now + 30'000;
        Release(call);
      }
    } else Release(call);
    if (!call.Okay()) Disable();
  }
  void Disable() {
    if (disabled) return;
    disabled = true;
    REXLOG_WARN("[workshop] live props/ghosts disabled: TU27 scene native failed or is unavailable");
    sandbox::ReportSceneUnavailable();
  }
};

struct WorkshopWorld {
  struct Marker { uint64_t key; uint32_t handle; sandbox::Point point; };
  std::vector<Marker> markers;
  WorkshopScene scene;
  uint32_t requested_model = 0, car = 0, requesting_ped = 0;
  sandbox::Vehicle test_vehicle = sandbox::Vehicle::Other, requested_vehicle = sandbox::Vehicle::Other;
  int64_t model_by = 0;

  static float Meters(int32_t cm) { return cm / 100.f; }

  void Markers(NativeCall& call, const sandbox::World& world, bool visible) {
    // Protocol IDs are only keys for *our* blip handles. They never become guest pointers,
    // entity IDs, executable names, file paths, or native hashes.
    struct Target { uint64_t key; sandbox::Point point; uint32_t color; };
    std::vector<Target> want;
    if (visible) {
      for (const auto& p : world.players) {
        if (p.id != world.self && p.pose) want.push_back({p.id, p.pose->position, 3});
        if (p.id == world.self) {
          if (const auto next = sandbox::NextCheckpoint(world.course, p))
            want.push_back({0x100000000ull, world.course.points[*next], 5});
        }
      }
      if (world.race == sandbox::Race::Editing) {
        for (size_t i = 0; i < world.course.points.size(); ++i)
          want.push_back({0x200000000ull + i, world.course.points[i], 5});
      }
    }
    for (auto it = markers.begin(); it != markers.end();) {
      if (std::any_of(want.begin(), want.end(), [&](const Target& t) { return t.key == it->key; })) {
        ++it;
        continue;
      }
      if (call.Flag(kBlipExists, {it->handle})) call.Invoke(kRemoveBlip, {}, nullptr, it->handle);
      if (!call.Okay()) return;
      it = markers.erase(it);
    }
    for (const auto& t : want) {
      const float x = Meters(t.point.x), y = Meters(t.point.y), z = Meters(t.point.z);
      auto it = std::find_if(markers.begin(), markers.end(), [&](const Marker& m) { return m.key == t.key; });
      if (it != markers.end() && !call.Flag(kBlipExists, {it->handle})) {
        if (!call.Okay()) return;
        markers.erase(it);
        it = markers.end();
      }
      if (it == markers.end()) {
        const auto handle = call.Read(kAddBlip, {F(x), F(y), F(z)});
        if (!call.Okay()) return;
        if (!handle || handle == UINT32_MAX) continue;
        markers.push_back({t.key, handle, t.point});
        call.Invoke(kBlipColor, {handle, t.color});
      } else if (t.point != it->point && call.Invoke(kBlipCoords, {it->handle, F(x), F(y), F(z)})) {
        it->point = t.point;
      }
      if (!call.Okay()) return;
    }
  }

  void Release(NativeCall& call) {
    if (requested_model) {
      auto cleanup = call.CleanupCall();
      cleanup.Invoke(kReleaseModel, {requested_model});
      requested_model = 0;
    }
  }

  void Vehicles(NativeCall& call, uint32_t ped, const Vec& position, int64_t now) {
    const auto request = sandbox::TakeVehicleRequest();
    if (!requested_model && !request) return;
    if (!ped || call.Flag(kInVehicle, {ped, 0}) || !call.Okay() || (requested_model && ped != requesting_ped)) {
      Release(call);
      sandbox::ReportVehicle("Load free roam and stand on foot before spawning a car.");
      return;
    }
    if (request && request->deadline >= now) {
      if (request->preset < sandbox::kVehicleModels.size()) {
        Release(call);
        scene.Release(call);
        const auto hash = HashName(sandbox::kVehicleModels[request->preset]);
        if (call.Flag(kModelOnDisc, {hash}) && call.Flag(kIsVehicleModel, {hash}) &&
            call.Invoke(kRequestModel, {hash})) {
          requested_model = hash;
          requested_vehicle = sandbox::Vehicle(request->preset + 1);
          requesting_ped = ped;
          model_by = now + 5'000;
          sandbox::ReportVehicle("Streaming test vehicle...");
        } else sandbox::ReportVehicle("That vehicle model is unavailable in the loaded game.");
      }
    }
    if (!requested_model) return;
    if (now >= model_by || !call.Okay()) {
      Release(call);
      sandbox::ReportVehicle("Vehicle streaming timed out.");
      return;
    }
    if (!call.Flag(kModelLoaded, {requested_model})) return;
    const float heading = call.Float(kHeading, {ped});
    if (!std::isfinite(heading) || !call.Okay()) { Release(call); return; }
    const auto offset = Horizontal(heading, 6.f, 0.f, 1.f);
    const Vec target{position.x + offset.x, position.y + offset.y, position.z + offset.z};
    if (!target.Valid(20'000.f)) { Release(call); return; }
    if (car && call.Flag(kExists, {car})) {
      call.Invoke(kDeleteVehicle, {}, nullptr, car);
      if (!call.Okay() || call.Flag(kExists, {car})) {
        Release(call);
        sandbox::ReportVehicle("Couldn't remove the previous test vehicle.");
        return;
      }
    }
    car = call.Read(kCreateVehicle, {requested_model, F(target.x), F(target.y), F(target.z), F(heading), 0, 0});
    Release(call);
    if (car && car != UINT32_MAX && call.Flag(kExists, {car}) && call.Okay()) {
      test_vehicle = requested_vehicle;
      call.Invoke(kGroundVehicle, {car});
      sandbox::ReportVehicle("Local test vehicle spawned nearby.");
    } else {
      car = 0;
      sandbox::ReportVehicle("The game did not create the requested vehicle.");
    }
  }

  void Tick(NativeCall& call, uint32_t ped, int64_t now, bool creative_holds_model) {
    auto pose_call = call.CleanupCall();
    std::optional<sandbox::Presence> pose;
    Vec coords{};
    if (ped && call.Okay()) {
      const bool driving = pose_call.Flag(kInVehicle, {ped, 0});
      uint32_t entity = ped;
      if (driving) {
        // If this optional getter isn't available, keep checkpoint/radar poses on the ped.
        auto vehicle_call = call.CleanupCall();
        const auto in_car = vehicle_call.Read(kPedVehicle, {ped, 0});  // not the last vehicle
        if (in_car && vehicle_call.Flag(kExists, {in_car}) && vehicle_call.Okay()) entity = in_car;
      }
      coords = pose_call.Vector(kCoords, {entity, 1});
      const float heading = pose_call.Float(kHeading, {entity});
      auto point = sandbox::Point::FromMeters(coords.x, coords.y, coords.z);
      if (point && pose_call.Okay() && std::isfinite(heading)) {
        const auto vehicle = !driving ? sandbox::Vehicle::Foot : entity == car ? test_vehicle : sandbox::Vehicle::Other;
        pose = sandbox::Presence{*point, sandbox::HeadingFromDegrees(heading), vehicle};
      }
    }
    sandbox::Party().Publish(pose);
    sandbox::ReportGuest((!call.Okay() || (ped && !pose)) ? sandbox::Guest::Unavailable :
                          pose ? sandbox::Guest::Ready : sandbox::Guest::Busy);
    const auto world = sandbox::Party().View().world;
    // Cleanup uses a fresh call state, so a rejected getter doesn't leave stale actors/blips.
    auto map_call = call.CleanupCall();
    Markers(map_call, world, pose.has_value());
    auto vehicle_call = call.CleanupCall();
    if (!creative_holds_model) Vehicles(vehicle_call, pose && workshop::BuildAllowed() ? ped : 0, coords, now);
    auto scene_call = call.CleanupCall();
    scene.Update(scene_call, sandbox::SceneTargets(world, sandbox::SceneEnabled() ? pose : std::nullopt),
                 now, !requested_model && !creative_holds_model);
  }
};
#endif

void GuestTick(PPCContext& ctx, uint8_t* base, rex::runtime::ThreadState& thread,
               uint32_t timer, PPCFunc* original, int64_t now) {
  // Do not run synthetic natives just to view logs. Serialize the tick (including recursive
  // timer calls) and bind to a VM thread only for an armed tool or a selected Workshop mode.
  static std::atomic_flag busy;
  if (busy.test_and_set(std::memory_order_acquire)) return;
  struct ReleaseTick {
    std::atomic_flag& busy;
    ~ReleaseTick() { busy.clear(std::memory_order_release); }
  } release{busy};
  static Cheats cheats;
  static uint32_t script_thread = 0;
  static bool verified = false, failed = false;
  last_script_ms = now;
  last_game_timer = timer;
#ifdef GTAV_DESKTOP_WORKSHOP
  static WorkshopWorld legacy;
  static workshop::FlightState flight;
  static uint64_t last_frame = UINT64_MAX;
  static int64_t next_workshop = 0, last_creative = 0;
  const bool workshop_active = sandbox::Active();
  const bool workshop_tick = workshop_active && now >= next_workshop;
  const uint64_t frame = native::GuestFrame();
  const bool creative_tick = workshop_active && (workshop_tick || (frame != last_frame && workshop::WantsFrame()));
  const bool allow_cheats = !workshop_active;
#else
  constexpr bool workshop_active = false, creative_tick = false, allow_cheats = true;
#endif
  const bool cheat_tick = ((allow_cheats && controls.Armed()) || cheats.Active()) && now - cheats.last_tick >= 16;
  if (!workshop_active && !controls.Armed() && !cheats.Active()) {
    script_thread = 0;
    if (!failed) status = Status::Ready;
    return;
  }
  if (failed || (!cheat_tick && !creative_tick)) return;
  const auto id = thread.thread_id();
  if (!id || (script_thread && script_thread != id)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->function_dispatcher()) return;
  NativeCall call(ctx, base, *thread.memory(), *kernel->function_dispatcher());
  if (!call.SafeStack()) {
    controls.Reset();
    status = Status::Unavailable;
#ifdef GTAV_DESKTOP_WORKSHOP
    if (workshop_active) sandbox::ReportGuest(sandbox::Guest::Unavailable);
#endif
    return;
  }
  if (!verified) {
    // Same native, actual guest timer versus a synthetic call. Do not arm actions if the
    // installed code doesn't accept the expected context/result slots on this thread.
    if (!call.Invoke(kTimer, {}, original) || call.Result() - timer > 1000) {
      controls.Reset();
      status = Status::Unavailable;
      failed = true;
#ifdef GTAV_DESKTOP_WORKSHOP
      if (workshop_active) sandbox::ReportGuest(sandbox::Guest::Unavailable);
#endif
      REXLOG_WARN("[debug] TU27 script native ABI check failed; cheats disabled");
      return;
    }
    REXLOG_INFO("[debug] validated script timer context on guest thread {}", id);
    verified = true;
  }
  script_thread = id;
  if (cheat_tick) {
    const auto in = controls.Consume(now);
    if (allow_cheats) {
      if (in.action) REXLOG_INFO("[debug] story action {} requested", static_cast<unsigned>(*in.action));
      cheats.Tick(call, in, now);
    } else cheats.Stop(call);
    mode_running = cheats.Active();
  }
#ifdef GTAV_DESKTOP_WORKSHOP
  if (creative_tick) {
    auto workshop_call = call.CleanupCall();
    const auto ped = PlayablePed(workshop_call);
    workshop::Tu27Workshop game(workshop_call, {ped, workshop::Kind::Ped}, REXCVAR_GET(gtav_workshop_extended_tu27),
                               flight, (now - last_creative) / 1000.f);
    last_creative = now;
    last_frame = frame;
    // Never share a streaming reference with the original F6 test car/scene path. Whichever
    // consumer already requested a model finishes/releases it before the other can acquire one.
    const bool holding = workshop::GuestFrame(game, {ped, workshop::Kind::Ped}, frame, !legacy.requested_model && !legacy.scene.loading);
    if (workshop_tick) {
      next_workshop = now + 100;
      legacy.Tick(workshop_call, ped, now, holding);
    }
  }
#endif
}
#endif

const char* StatusText(Status value) {
  switch (value) {
    case Status::Waiting: return "Waiting for a live TU27 script thread.";
    case Status::Unavailable: return "Native ABI unavailable. No cheat actions enabled.";
    case Status::Blocked: return "Free-roam tools blocked. Mission assist requires a controllable player.";
    case Status::Ready: return "Story tools ready. Only enable them in free roam.";
    case Status::Gave: return "Requested a pistol with 60 rounds.";
    case Status::Teleported: return "Teleported a short distance in the loaded world.";
    case Status::Flying: return "Fly active. Closing the panel releases control (you can fall).";
    case Status::Camera: return "Freecam active. Close F2 to return to the game camera.";
    case Status::Restocked: return "Health and available armor restored.";
    case Status::Rejected: return "Action refused in the current world state.";
  }
  return "";
}

}  // namespace

Controls& PanelControls() { return controls; }

void InstallLogCapture() {
#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
  if (log_sink) return;
  log_sink = std::make_shared<rex::LogCaptureSink>();
  log_sink->set_pattern("%H:%M:%S.%e [%l] [%n] %v");
  rex::AddSink(log_sink);
#endif
}

void DrawPanel(unsigned fps, double frame_ms, float scale) {
  const auto last = last_script_ms.load();
  const auto now = NowMs();
  const bool alive = last != 0 && now - last < 2'000;
  ImGui::Text("%u fps | %.1f ms | %s", fps, frame_ms, InFailureEffect() ? "Death/failure effect" : "Story debug");
  if (last) ImGui::TextDisabled("Guest script timer: %u ms (last sample %.1f s ago)",
                               last_game_timer.load(), double(now - last) / 1000.0);
  if (!ImGui::BeginTabBar("debug-pages")) return;
  if (ImGui::BeginTabItem("Live logs")) {
    static bool warnings = false, follow = true;
    ImGui::Checkbox("Warnings and errors only", &warnings);
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow);
    static std::vector<rex::LogEntry> entries;
    static uint64_t generation = 0;
    static int64_t next_poll = 0;
    if (log_sink && now >= next_poll) {
      next_poll = now + 250;
      const auto next = log_sink->generation();
      if (next != generation) {
        generation = next;
        log_sink->CopyEntries(entries);
        for (auto& entry : entries) {
          if (entry.text.size() > 2048) entry.text.resize(2048);
          std::replace(entry.text.begin(), entry.text.end(), '\n', ' ');
          std::replace(entry.text.begin(), entry.text.end(), '\r', ' ');
        }
      }
    }
    std::array<const rex::LogEntry*, rex::LogCaptureSink::kCapacity> visible{};
    int count = 0;
    for (const auto& entry : entries)
      if (!warnings || entry.level >= spdlog::level::warn) visible[count++] = &entry;
    if (ImGui::BeginChild("log-lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
      ImGuiListClipper list;
      list.Begin(count);
      while (list.Step()) {
        for (int i = list.DisplayStart; i < list.DisplayEnd; ++i) {
          const auto& log = *visible[i];
          if (log.level >= spdlog::level::warn)
            ImGui::PushStyleColor(ImGuiCol_Text, log.level == spdlog::level::warn ? ImVec4(1, .78f, .35f, 1) : ImVec4(1, .5f, .45f, 1));
          ImGui::TextUnformatted(log.text.c_str());
          if (log.level >= spdlog::level::warn) ImGui::PopStyleColor();
        }
      }
      if (follow && count > 0) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Story tools")) {
    ImGui::TextWrapped("Experimental, offline only. Use a spare save. The overlay captures gameplay input; "
                       "closing it stops flight and releases the camera on the guest script thread.");
    const auto state = status.load();
    ImGui::TextDisabled("%s", alive || state == Status::Unavailable ? StatusText(state) : StatusText(Status::Waiting));
    bool armed = controls.Armed();
#ifdef GTAV_DESKTOP_WORKSHOP
    const bool workshop_mode = sandbox::Active();
    if (workshop_mode) ImGui::TextDisabled("Story tools are Campaign-only. Use F6 Workshop for AdminOS.");
#else
    constexpr bool workshop_mode = false;
#endif
    ImGui::BeginDisabled(workshop_mode || !alive || state == Status::Unavailable);
    if (ImGui::Checkbox("Enable story-mode actions for this panel", &armed)) controls.Arm(armed);
    ImGui::BeginDisabled(!armed || state == Status::Blocked);
    if (ImGui::Button("Give pistol + ammo")) controls.Queue(Action::Give, now);
    ImGui::SameLine();
    if (ImGui::Button("Teleport 10 m forward")) controls.Queue(Action::Teleport, now);
    if (ImGui::Button("Toggle fly")) controls.Queue(Action::Fly, now);
    ImGui::SameLine();
    if (ImGui::Button("Toggle freecam")) controls.Queue(Action::Freecam, now);
    ImGui::SameLine();
    if (ImGui::Button("Stop")) controls.Queue(Action::Stop, now);
    ImGui::EndDisabled();
    ImGui::SeparatorText("Mission assist / Prologue");
    ImGui::TextWrapped("Restore health and armor during playable Story sections. This does not skip or complete objectives.");
    ImGui::BeginDisabled(!armed || mode_running.load());
    if (ImGui::Button("Restore health and armor")) controls.Queue(Action::MissionRestock, now);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextWrapped("Move: WASD / ZQSD (physical keys). Rise / descend: E / Q (E / A on AZERTY). "
                       "Freecam look: I / J / K / L. Fly and teleport require being on foot.");
    ImGui::TextDisabled("F2 (Fn + F2 on some keyboards), Esc or B to close.");
    ImGui::Dummy(ImVec2(0, 2 * scale));
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

}  // namespace gtav::debug

REX_EXTERN(__imp__sub_82C77D00);
REX_HOOK_RAW(sub_82C77D00) {  // SET_CAM_DEATH_FAIL_EFFECT_STATE(int)
  if (auto* thread = rex::runtime::ThreadState::Get()) {
    if (auto slot = gtav::debug::ScriptSlot(ctx, *thread->memory(), 1, false)) {
      const uint32_t state = gtav::debug::Load(*thread->memory(), *slot);
      if (state <= 16) gtav::NoteFailureEffect(static_cast<int>(state));
    }
  }
  __imp__sub_82C77D00(ctx, base);
}

REX_EXTERN(__imp__sub_82C86EE8);
REX_HOOK_RAW(sub_82C86EE8) {  // SET_TIME_SCALE(float); diagnostics only
  if (gtav::InFailureEffect()) {
    if (auto* thread = rex::runtime::ThreadState::Get()) {
      if (auto slot = gtav::debug::ScriptSlot(ctx, *thread->memory(), 1, false)) {
        static std::atomic<uint32_t> previous{0};
        const auto bits = gtav::debug::Load(*thread->memory(), *slot);
        const float scale = std::bit_cast<float>(bits);
        if (std::isfinite(scale) && previous.exchange(bits) != bits)
          REXLOG_INFO("[mission] guest requested time scale {}", scale);
      }
    }
  }
  __imp__sub_82C86EE8(ctx, base);
}

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
REX_EXTERN(__imp__sub_82C87678);
REX_HOOK_RAW(sub_82C87678) {  // GET_GAME_TIMER(), in the real game's VM
  using namespace gtav::debug;
  // The original wrapper may clobber r3. Read its actual result slot before dispatch.
  const bool panel_or_mode = controls.Open() || mode_running.load(std::memory_order_relaxed)
#ifdef GTAV_DESKTOP_WORKSHOP
      || gtav::sandbox::Active()
#endif
      ;
  const bool interested = panel_or_mode || gtav::InFailureEffect();
  auto* thread = interested ? rex::runtime::ThreadState::Get() : nullptr;
  auto result = thread ? ScriptSlot(ctx, *thread->memory(), 0, true) : std::nullopt;
  __imp__sub_82C87678(ctx, base);
  if (!result) return;
  const auto now = NowMs();
  const uint32_t timer = Load(*thread->memory(), *result);
  if (gtav::InFailureEffect()) {
    static std::atomic<int64_t> next_log{0};
    auto next = next_log.load();
    if (now >= next && next_log.compare_exchange_strong(next, now + 3'000))
      REXLOG_INFO("[mission] guest GET_GAME_TIMER={} ms", timer);
  }
  if (panel_or_mode) GuestTick(ctx, base, *thread, timer, &__imp__sub_82C87678, now);
}
#endif
