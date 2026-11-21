#include "workshop_fake_game.h"
#include "utils/ConfigStorage.h"

#include <chrono>
#include <fstream>
#include <limits>

namespace fs = std::filesystem;
using namespace gtav::workshop;
using namespace workshop_test;
using R = BuildResult;
constexpr uint32_t kCone = ModelHash("prop_roadcone02a"), kRamp = ModelHash("prop_mp_ramp_01");
struct Host {
  FakeGame game;
  std::vector<std::string> audit;
  std::vector<std::pair<PlayerId, std::string>> kicked;
  WorkshopCore core;
  uint64_t frame = 0;
  explicit Host(Config c = {}, bool practice = false) : core(std::move(c), practice,
    [&](std::string_view text) { audit.emplace_back(text); },
    [&](PlayerId id, std::string_view reason) { kicked.emplace_back(id, reason); }) {
    CHECK(core.Bind(kLocalOwner, kPed));
    Step();
  }
  void Step(unsigned n = 1, bool auth = true, bool ready = true, std::optional<Preview> preview = {}) {
    while (n--) core.Tick(game, ++frame, auth, ready, true, preview.has_value(), preview, {});
  }
  R Act(Command cmd, PlayerId actor = kLocalOwner) { return core.Execute(actor, cmd, game, frame); }
  CoreView View(PlayerId actor = kLocalOwner) { return core.View(actor); }
  PlayerId Connect(std::string_view id, std::string_view name, Entity e) {
    const auto player = core.Admit(id, name);
    CHECK(player != 0);
    game.entities[e.handle] = {e, {{4, 0, 1}, 0}, false, false};
    CHECK(core.Bind(player, e)); return player;
  }
};

SpawnSpec Cone() { return {kCone, Kind::Prop, {{2, 6, 1}, 0}}; }
Command Place(uint32_t model = kCone) { return {.op = Op::Place, .value = model, .placement = Placement{}}; }

void QueueContracts() {
  FakeGame g;
  EntitySpawnQueue q;
  std::vector<SpawnResult> replies;
  const auto done = [&](SpawnResult r, Entity e) { CHECK((r == SpawnResult::Created) == g.Exists(e)); replies.push_back(r); };
  bool allowed = true;
  const auto permits = [&] { return allowed; };
  g.loaded = false;
  CHECK(q.Push(Cone(), 1, permits, done));
  q.Tick(g, 1, true);
  CHECK(q.HoldingModel() && replies.empty() && g.requests == 1);
  q.Tick(g, 300, true);
  CHECK(replies.empty());
  q.Tick(g, 301, true);
  CHECK(!q.Busy() && replies == std::vector{SpawnResult::Timeout} && g.releases == 1);
  CHECK(q.Push(Cone(), 302, permits, done));
  q.Tick(g, 302, true, false);
  CHECK(!q.HoldingModel());
  q.Tick(g, 302, true);
  allowed = false;
  g.loaded = true;
  q.Tick(g, 302, true);  // cancellation not throttled by same-frame polling
  CHECK(!q.Busy() && replies.back() == SpawnResult::Cancelled && g.creates == 0 && g.releases == 2);
  allowed = true;
  CHECK(q.Push(Cone(), 303, permits, done));
  q.Tick(g, 303, true);
  q.Tick(g, 303, true);
  CHECK(g.creates == 0);
  q.Tick(g, 304, true);
  CHECK(g.creates == 1 && g.releases == 3 && replies.back() == SpawnResult::Created);
  CHECK(q.Push(Cone(), 305, permits, done));
  q.Tick(g, 305, true);
  q.Tick(g, 306, false);
  CHECK(g.releases == 4 && replies.back() == SpawnResult::Cancelled);
  for (size_t i = 0; i < EntitySpawnQueue::kCapacity; ++i) CHECK(q.Push(Cone(), 0, permits, done));
  CHECK(!q.Push(Cone(), 0, permits, done));
  q.Cancel(g);
  CHECK(!q.Busy());
}

void AuthorityAndModes() {
  Host h;
  const auto admin = h.Connect("auth:admin", "Alex", {2, Kind::Ped});
  const auto player = h.Connect("auth:player", "Noa", {3, Kind::Ped});
  CHECK(h.core.Admit("local:workshop", "Fake host") == 0);
  CHECK(h.core.Admit("sim:alex", "Fake sim") == 0);
  CHECK(h.core.Admit("auth:admin", "Alias") == 0);
  CHECK(h.Act({Op::MapMode, 0, 1}, player) == R::Denied);
  CHECK(h.Act({.op = Op::Promote, .target = admin, .on = true}, admin) == R::Denied);
  CHECK(h.Act({.op = Op::Promote, .target = admin, .on = true}) == R::Okay);
  CHECK(h.View(admin).admin && !h.View(player).admin);
  CHECK(h.Act({.op = Op::Promote, .target = player, .on = true}, admin) == R::Denied);
  for (const auto op : {Op::Freeze, Op::Kick, Op::Ban}) {
    CHECK(h.Act({.op = op, .target = kLocalOwner, .on = true, .text = "Reason"}) == R::Denied);
    CHECK(h.Act({.op = op, .target = kLocalOwner, .on = true, .text = "Reason"}, admin) == R::Denied);
  }
  CHECK(h.Act({Op::Heal, kLocalOwner}, admin) == R::Denied);
  CHECK(h.Act({.op = Op::Freeze, .target = player, .on = true}, admin) == R::Okay);
  CHECK(h.game.entities.at(3).anchor);
  CHECK(h.Act({Op::PlayerMode, player, 1}, admin) == R::Okay);
  CHECK(h.Act({Op::Cash, player, 50000}, admin) == R::Okay);
  CHECK(h.View().players.back().wallet == 50000);
  CHECK(h.Act({Op::Cash, player, UINT32_MAX}, admin) == R::Invalid);
  CHECK(h.Act({Op::PlayerMode, 0, 0}) == R::Okay);
  for (const auto fx : {Effect::Sprint, Effect::Noclip, Effect::God, Effect::Jump})
    CHECK(h.Act({.op = Op::Effect, .value = unsigned(fx), .on = true}) == R::Okay);
  h.Step();
  CHECK(h.View().cheats.RunMultiplier() == 1.49f);
  CHECK(h.Act({Op::MapMode, 0, 1}, admin) == R::Okay);
  CHECK(h.View().build && h.View().cheats.Has(Effect::Noclip));  // deliberate Creative override — now also persists in Survival as dev cheat
  CHECK(h.Act({Op::PlayerMode, 0, 2}) == R::Okay);
  CHECK(!h.View().build && h.View().cheats.Has(Effect::Noclip) && h.View().cheats.RunMultiplier() == 1.49f); // F5 dev cheat: host-only but all-modes, not cleared on Survival
  for (const auto effect : {Effect::Noclip, Effect::God, Effect::Sprint})
    CHECK(std::find(h.game.changes.begin(), h.game.changes.end(), std::tuple{effect, kPed, false}) == h.game.changes.end());
  CHECK(h.Act(Place()) == R::Denied);
  CHECK(h.Act({.op = Op::Ban, .target = player, .text = "   "}, admin) == R::Invalid);
  CHECK(h.Act({.op = Op::Ban, .target = player, .text = "Moderation test"}, admin) == R::Okay);
  CHECK(h.kicked.size() == 1 && h.kicked.front().first == player);
  CHECK(!h.game.entities.at(3).anchor);
  CHECK(h.core.Admit("auth:player", "Different name") == 0);
  CHECK(h.View().bans.size() == 1 && !h.View(player).admin && h.View(player).bans.empty());
  CHECK(h.Act({.op = Op::Unban, .text = "auth:player"}, admin) == R::Okay);
  CHECK(h.core.Admit("auth:player", "Can rejoin") != 0);
  CHECK(h.audit.size() >= 6);
  for (const auto& line : h.audit) CHECK(line.size() > 40 && line.substr(19, 2) == "Z " && line.find("[ADMIN AUDIT]") == 21);
  CHECK(h.audit[h.audit.size() - 2].find("Alex -> Ban -> Noa") != std::string::npos);
  h.Step(1, false);
  CHECK(!h.View().admin && !h.View().build && h.Act({Op::PlayerMode}) == R::Denied);
}

void Builder() {
  const auto* cone = FindModel(kCone);
  CHECK(cone && Matches(*cone, "TRAFFIC co") && Matches(*cone, "prop_roadcone") && !Matches(*cone, "galaxy"));
  char hash[11]; std::snprintf(hash, sizeof hash, "0x%08X", unsigned(kCone)); CHECK(Matches(*cone, hash));
  Host h;
  Placement snap; snap.grid_cm = 50; snap.yaw = 58; snap.angle = 45; snap.ground = true; snap.height = 1;
  const auto transform = Project(h.game, {{.22f, .49f, 3}, {0, 1, 0}}, snap);
  CHECK(transform && transform->at == (Vec3{0, 6.5f, 2}) && transform->yaw == 45);
  CHECK(!Project(h.game, {{}, {0, 0, 0}}, snap));
  CHECK(Compose({{10, 10, 0}, 90}, {{2, 0, 3}, 15}).yaw == 105);
  h.Step(2, true, true, Preview{kCone, {}, 1});
  CHECK(h.View().ghost && h.View().count == 0 && h.game.made.front().ghost && h.game.shots_suppressed == 2);
  h.Step(1);
  CHECK(!h.View().ghost && h.game.entities.size() == 1);
  CHECK(h.Act(Place()) == R::Queued);
  h.Step(2);
  CHECK(h.View().count == 1 && h.View().undo && h.View().recent.front() == kCone);
  CHECK(h.Act({Op::Undo}) == R::Okay && h.View().count == 0);
  CHECK(h.Act({Op::Redo}) == R::Queued);
  h.Step(2);
  CHECK(h.View().count == 1 && h.View().undo);
  h.Step(1, true, true, Preview{0, {}, 2});
  const auto id = h.View().aimed;
  CHECK(id);
  CHECK(h.Act({.op = Op::Anchor, .piece = id, .on = true}) == R::Okay);
  CHECK(h.Act({.op = Op::SavePreset, .piece = id, .text = "Aimed group"}) == R::Okay);
  CHECK(h.View().presets.size() == 1 && h.View().presets[0].pieces[0].locked);
  CHECK(h.Act({.op = Op::BuildRule, .value = 1, .on = true}) == R::Okay);
  CHECK(h.Act(Place(kRamp)) == R::Limit);
  const auto player = h.Connect("auth:p", "Player", {2, Kind::Ped});
  CHECK(h.Act({.op = Op::Delete, .piece = id}, player) == R::Denied);
  const Pose proposed{{1, 3, 1}, 37};
  CHECK(h.Act({.op = Op::Place, .value = kRamp, .proposed = Pose{{200, 200, 1}, 37}}, player) == R::Invalid);
  CHECK(h.Act({.op = Op::Place, .value = kRamp, .proposed = proposed}, player) == R::Queued);
  h.game.loaded = false;
  h.Step();
  CHECK(h.core.HoldingModel());
  CHECK(h.Act({Op::PlayerMode, player, 1}) == R::Okay);
  h.Step();
  CHECK(!h.core.HoldingModel() && h.View(player).count == 0 && h.View().count == 1 && h.View().result == R::Cancelled);
  h.game.loaded = true;
  // Check 100 bounded undo entries; older anchor history must not survive after 101 toggles.
  for (unsigned i = 0; i < 105; ++i) CHECK(h.Act({.op = Op::Anchor, .piece = id, .on = (i % 2 == 0)}) == R::Okay);
  for (unsigned i = 0; i < 100; ++i) CHECK(h.Act({Op::Undo}) == R::Okay);
  CHECK(h.Act({Op::Undo}) == R::Missing);
  CHECK(h.Act({Op::Redo}) == R::Okay);
  CHECK(h.Act({Op::Cleanup}) == R::Queued);
  CHECK(h.Act({Op::MapMode, 0, 1}) == R::Okay);
  h.Step(2); CHECK(h.View().count == 1 && h.View().undo && h.View().result == R::Cancelled);
  CHECK(h.Act({Op::MapMode}) == R::Okay);
  CHECK(h.Act({Op::Cleanup}) == R::Queued);
  h.Step(2);
  CHECK(h.View().count == 0 && !h.View().undo && !h.View().redo && !h.View().recent.empty());
  CHECK(h.audit.back().find(" -> Cleanup -> ") != std::string::npos);
}

void SavePresetAnchor() {
  FakeGame g;
  BuildManager build({}, {});
  EntitySpawnQueue queue;
  auto anchor = Cone();
  anchor.model = kRamp;
  anchor.pose.at.x += 4.f;
  anchor.pose.yaw = 180.f;
  CHECK(build.Place(kLocalOwner, {Cone(), anchor}, 16, queue, 1, [] { return true; }) == R::Queued);
  for (unsigned i = 1; i <= 4; ++i) queue.Tick(g, i, true);
  CHECK(build.Total() == 2 && build.SavePreset("Ramp anchor", kLocalOwner, 2, g));
  const auto& preset = build.Presets().front();
  CHECK(preset.pieces.size() == 2 && preset.pieces[0].model == kRamp && preset.pieces[0].relative == Pose{});
  CHECK(std::abs(preset.pieces[1].relative.at.x - 4.f) < .001f);
}

void PresetAndVehicle() {
  Config config;
  config.presets = {{"Ramp & cone", {{kCone, {{0, 0, 0}, 0}, false}, {kRamp, {{2, 0, 0}, 25}, true}}}};
  Host h(config);
  auto cmd = Command{.op = Op::LoadPreset, .text = "Ramp & cone", .placement = Placement{}};
  cmd.placement->yaw = 90;
  h.game.fail = kRamp;
  CHECK(h.Act(cmd) == R::Queued);
  h.Step(6);
  CHECK(h.game.entities.size() == 1 && h.View().count == 0 && h.View().result == R::Failed);
  h.game.fail = 0;
  CHECK(h.Act(cmd) == R::Queued);
  h.Step(5);
  CHECK(h.View().count == 2 && h.View().undo);
  const auto& last = h.game.made.back();
  CHECK(last.locked && last.pose.yaw == 115 && std::abs(last.pose.at.x) < .001f && last.pose.at.y == 8);
  CHECK(h.Act({Op::Undo}) == R::Okay && h.View().count == 0);
  CHECK(h.Act({.op = Op::QuickVehicle, .value = ModelHash("sultan"), .on = true}) == R::Queued);
  h.Step(2);
  CHECK(h.View().count == 1 && std::get<0>(h.game.tools.back()) == Tool::Warp && std::get<2>(h.game.tools.back()) == kPed.handle);
  const auto car = std::get<1>(h.game.tools.back()); h.game.driver = car;
  CHECK(h.Act({Op::Waypoint}) == R::Queued);
  h.Step(2); CHECK(h.game.moved.empty());
  h.Step(); CHECK(h.game.moved == std::vector{car});
  CHECK(h.Act({Op::Landmark, 0, 0}) == R::Queued);
  h.game.ground_ready = false;
  h.Step(300); CHECK(h.game.moved.size() == 1 && h.View().result == R::Timeout);
}

void Simulated() {
  Host h({}, true);
  h.game.no_pointers = true;
  h.Step();
  CHECK(h.Act({Op::Practice}) == R::Queued);
  h.Step(7);
  const auto v = h.View();
  CHECK(v.players.size() == 4 && h.game.entities.size() == 4);
  const auto p = v.players[1];
  CHECK(p.seat == Seat::Simulated && p.ready);
  CHECK(h.Act({Op::PlayerMode, p.id, 1}) == R::Okay);
  CHECK(h.Act({.op = Op::Freeze, .target = p.id, .on = true}) == R::Okay);
  CHECK(h.Act({Op::GoTo, p.id}) == R::Okay);
  CHECK(h.Act({Op::Bring, p.id}) == R::Okay);
  CHECK(h.Act({Op::Kit, p.id}) == R::Okay);
  CHECK(h.Act({.op = Op::Ban, .target = p.id, .text = "Test"}) == R::Okay);
  h.Step();
  CHECK(h.kicked.empty() && h.View().players.size() == 3 && h.game.entities.size() == 3);
  CHECK(h.Act({Op::Practice}) == R::Missing);  // banned seat not respawned
  CHECK(h.Act({.op = Op::Unban, .text = "sim:alex"}) == R::Okay);
  CHECK(h.Act({Op::Practice}) == R::Queued);
  h.Step(3);
  CHECK(h.View().players.size() == 4);
  CHECK(h.Act({Op::Armory}) == R::Queued);
  const auto before = h.game.tools.size();
  h.Step(); CHECK(h.game.tools.size() == before + 1);
  h.Step(unsigned(Weapons().size()));
  CHECK(h.game.tools.size() == before + Weapons().size());
  CHECK(h.audit.back().find("Mass give completed") != std::string::npos);
  CHECK(h.Act({.op = Op::Effect, .value = unsigned(Effect::Sprint), .on = true}) == R::Okay);
  h.Step();
  CHECK(h.game.Destroy(kPed) && h.core.Bind(kLocalOwner, {}));
  h.Step(1, true, false);
  CHECK(!h.View().cheats.active && h.game.changes.back() == (std::tuple{Effect::Sprint, kPed, false}));
}

void WorldTransitions() {
  Host h;
  CHECK(h.Act({.op = Op::MapRule, .value = unsigned(Rule::Police), .on = false}) == R::Okay);
  CHECK(h.Act({.op = Op::PauseClock, .on = true}) == R::Okay);
  CHECK(h.Act({.op = Op::Gravity, .on = true}) == R::Okay);
  CHECK(h.Act({.op = Op::Weather, .value = 3}) == R::Okay);
  CHECK(h.Act({.op = Op::Effect, .value = unsigned(Effect::Fly), .on = true}) == R::Okay);
  h.Step();
  CHECK(h.View().clock_frozen && h.View().low_gravity && h.View().cheats.Has(Effect::Fly));
  CHECK(h.game.rules.end() != std::find(h.game.rules.begin(), h.game.rules.end(), std::pair{Rule::Police, false}));
  h.Step(1, false);
  CHECK(!h.View().admin && !h.View().clock_frozen && !h.View().low_gravity && !h.View().cheats.active);
  CHECK(h.game.changes.back() == (std::tuple{Effect::Fly, kPed, false}));
  CHECK(h.game.rules.back() == (std::pair{Rule::Police, true}));
  CHECK(h.game.tools.back() == (std::tuple{Tool::Weather, Entity{}, UINT32_MAX}));
  const auto last = h.game.rules.size();
  h.Step();
  CHECK(h.game.rules.size() > last && h.game.rules[last] == (std::pair{Rule::Police, false}));
  CHECK(!h.View().cheats.active && !h.View().clock_frozen && !h.View().low_gravity);
}

void Storage() {
  auto root = fs::temp_directory_path() / ("workshop-core-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  CHECK(fs::create_directory(root));
  auto profile = fs::canonical(root) / fs::path(u8"Joueur \u00e9");
  CHECK(fs::create_directory(profile));
  Config saved;
  saved.map = {Mode::Survival, 3};
  saved.permissions = {{"sim:noa"}, true, 100};
  saved.bans = {{"sim:sam", "Sam", "Raison \xc3\xa9 avec \"guillemets\" et \\ barre", UtcNow()}};
  saved.favorites = {kCone, kRamp};
  saved.presets = {{"Groupe \xc3\xa9", {{kCone, {{.25f, -.5f, 1.5f}, 315.f}, true}}}};
  ConfigStorage a(profile);
  CHECK(a.Load().bans.empty() && a.NeedsDefaults() == 15 && !a.Unreadable());
  CHECK(!a.Save(saved, 15));
  ConfigStorage b(profile);
  const auto cfg = b.Load();
  CHECK(!b.NeedsDefaults() && !b.Unreadable() && cfg.permissions.admins == saved.permissions.admins);
  CHECK(cfg.map.mode == Mode::Survival && cfg.map.enabled == 3 && cfg.permissions.prop_limit == 100);
  CHECK(cfg.bans == saved.bans && cfg.favorites == saved.favorites && cfg.presets[0].name == saved.presets[0].name);
  CHECK(cfg.presets[0].pieces[0].relative == saved.presets[0].pieces[0].relative);
  const auto overwrite = [&](const char* file, const std::string& line) {
    std::ofstream stream(profile / "config" / file, std::ios::binary);
    stream << line;
    CHECK(bool(stream));
  };
  overwrite("bans.json", R"({"version":1,"bans":[{"identity":"sim:sam","name":"Sam","reason":"a\u00e9\ud83d\ude80","utc":"2026-11-18T13:30:00Z"}]})");
  CHECK(b.Load().bans[0].reason == "a\xc3\xa9\xf0\x9f\x9a\x80");
  const std::string future = R"({"version":99,"bans":[]})";
  overwrite("bans.json", future);
  const auto c = b.Load();
  CHECK(c.bans.empty() && c.permissions.admins.empty() && !c.permissions.players_build && (b.Unreadable() & 2));
  CHECK((b.Save(saved, 15) & 2) != 0);
  std::ifstream in(profile / "config/bans.json", std::ios::binary);
  CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == future);
  in.close();
  overwrite("build_favorites.json", R"({"version":1,"version":1,"models":[]})");
  overwrite("presets.json", std::string(128 * 1024 + 1, '['));
  b.Load(); CHECK(b.Unreadable() == 15);
#ifndef _WIN32
  const auto link = fs::canonical(root) / "link profile";
  fs::create_directory_symlink(profile, link);
  ConfigStorage linked(link); linked.Load(); CHECK(linked.Unreadable() == 15);
#endif
  fs::remove_all(root);
}
int main() {
  QueueContracts(); AuthorityAndModes(); Builder(); SavePresetAnchor(); PresetAndVehicle(); Simulated(); WorldTransitions(); Storage();
  std::cout << "Workshop queue, authority, builder, simulation and JSON contracts passed\n";
}
