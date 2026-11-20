// Exercise the production mailbox and disk worker. Only the guest game, clock and
// session state are synthetic; the authority and queue are the same as in the host.
#include "workshop_fake_game.h"
#include "sandbox/CreativeRuntime.h"
#include "sandbox/desktop.h"
#include "utils/ConfigStorage.h"

#include <atomic>
#include <chrono>

namespace fs = std::filesystem;
namespace sb = gtav::sandbox;
using namespace gtav::workshop;
using namespace workshop_test;

namespace {
std::atomic test_link{sb::Link::Idle};
std::atomic test_mode{sb::Mode::Solo};
std::atomic<int64_t> test_now = 100;
constexpr auto kCone = ModelHash("prop_roadcone02a");
constexpr Entity kNewPed{2, Kind::Ped};
}

namespace gtav::sandbox {
Mode CurrentMode() { return test_mode; }
int64_t NowMs() { return test_now; }
Room::Room(std::string) {}  // no network or room state used by this fixture
Session& Party() { static Session session; return session; }
Session::~Session() = default;
Link Session::LinkState() const { return test_link; }
}  // namespace gtav::sandbox

namespace {
struct RuntimeFixture {
  fs::path root;
  FakeGame game;
  uint64_t frame = 0;
  std::vector<std::string> audit;
  std::atomic<uint8_t> failed{};

  explicit RuntimeFixture(sb::Mode chosen = sb::Mode::Solo)
      : root(fs::canonical(fs::temp_directory_path()) /
          ("workshop-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) /
          fs::path(u8"Joueur \u00e9")) {
    CHECK(fs::create_directories(root));
    test_mode = chosen;
    test_link = chosen == sb::Mode::Solo ? sb::Link::Idle : sb::Link::Hosting;
    Start(root, chosen == sb::Mode::Solo, {
        [this](std::string_view line) { audit.emplace_back(line); },
        [this](uint8_t mask) { failed |= mask; },
    });
    CHECK(!Fresh(*Snapshot()) && !BuildAllowed());
    CHECK(!Step() && BuildAllowed());
  }
  ~RuntimeFixture() { Stop(); fs::remove_all(root.parent_path()); }
  bool Step(Entity ped = kPed, bool can_stream = true) {
    test_now += 40;
    return GuestFrame(game, ped, ++frame, can_stream);
  }
};

void MailboxAndWorldEpoch() {
  RuntimeFixture r;
  CHECK(!Submit({Op::Heal}));  // no panel, no command
  SetCapture(false, true);
  CHECK(!Submit({.op = Op::Favorite, .value = kCone}, true));
  for (unsigned i = 0; i < 32; ++i) CHECK(Submit({Op::Heal}));
  CHECK(!Submit({Op::Heal}));
  for (unsigned i = 1; i <= 8; ++i) {
    CHECK(!r.Step());
    CHECK(r.game.tools.size() == 4 * i);  // bounded guest-thread draining
  }
  CHECK(Submit({Op::Heal}));
  // A window focus change after the batch was drained invalidates that batch too.
  r.game.on_features = [] { SetCapture(false, false); };
  CHECK(!r.Step() && !ActiveCapture() && !WantsFrame());
  CHECK(r.game.tools.size() == 32);
  SetCapture(false, true);
  CHECK(Submit({Op::Heal}));
  test_now += 2'001;
  CHECK(!Fresh(*Snapshot()) && !BuildAllowed());
  CHECK(!r.Step() && r.game.tools.size() == 32);
  CHECK(Submit({Op::Heal}));
  r.game.entities.emplace(kNewPed.handle, FakeGame::Instance{kNewPed, {{0, 0, 1}, 0}, false, false});
  CHECK(!r.Step(kNewPed) && r.game.tools.size() == 32);
  CHECK(Submit({Op::Heal}) && !r.Step(kNewPed));
  CHECK(r.game.tools.back() == (std::tuple{Tool::Heal, kNewPed, 0u}));
}

void ModeChangeAndProfileFlush() {
  RuntimeFixture r;
  const auto before = Snapshot();
  SetCapture(true, false);
  CHECK(Submit({.op = Op::Favorite, .value = kCone}, true));
  CHECK(Submit({.op = Op::Place, .value = kCone, .placement = Placement{}}, true));
  CHECK(!r.Step(kPed, false) && r.game.requests == 0);  // the legacy F6 path owns the model hold
  CHECK(r.Step() && r.game.requests == 1);
  SetCapture(false, true);
  CHECK(Submit({Op::MapMode, 0, unsigned(Mode::Survival)}));
  // A loaded model is ready to commit NOW. Revocation must run before the spawn poll.
  CHECK(!r.Step() && r.game.releases == 1 && r.game.creates == 0);
  CHECK(!BuildAllowed() && Snapshot()->result == BuildResult::Cancelled);
  CHECK(before->build && before->favorites.empty());  // old publication did not mutate
  CHECK(r.audit.size() == 1 && r.audit.front().find("[ADMIN AUDIT] You -> Map mode -> Workshop map") != std::string::npos);
  Stop();  // no sleep or polling of the filesystem before shutdown
  CHECK(!WantsFrame() && !Fresh(*Snapshot()) && !r.failed);
  ConfigStorage disk(r.root);
  const auto saved = disk.Load();
  CHECK(!disk.Unreadable() && !disk.NeedsDefaults());
  CHECK(saved.map.mode == Mode::Survival && saved.favorites == std::vector{kCone});
  Start(r.root, true);
  CHECK(!r.Step() && !BuildAllowed() && Snapshot()->map.mode == Mode::Survival);
  CHECK(Snapshot()->favorites == std::vector{kCone});
}

void HostToGuest() {
  RuntimeFixture r(sb::Mode::Multiplayer);
  SetCapture(true, false);
  CHECK(Submit({.op = Op::Place, .value = kCone, .placement = Placement{}}, true));
  CHECK(r.Step());
  SetCapture(false, true);
  CHECK(Submit({Op::Heal}));
  r.game.on_features = [] { test_link = sb::Link::Joined; };
  CHECK(!r.Step() && r.game.releases == 1 && r.game.creates == 0 && r.game.tools.empty());
  CHECK(!Snapshot()->admin && !BuildAllowed() && !Submit({Op::Heal}));
  test_link = sb::Link::Connecting;
  CHECK(!LocalAuthority());
  test_link = sb::Link::Opening;
  CHECK(!LocalAuthority());
  test_link = sb::Link::Failed;
  CHECK(LocalAuthority() && !r.Step() && BuildAllowed());
  CHECK(!Snapshot()->practice);  // only Solo simulates the practice roster
}

void FocusAndFlight() {
  RuntimeFixture r;
  SetCapture(false, true);
  CHECK(Submit({.op = Op::Effect, .value = unsigned(Effect::Fly), .on = true}));
  CHECK(!r.Step() && !FlightKey(0, true));
  SetCapture(false, false);
  CHECK(FlightKey(0, true) && !r.Step() && r.game.flight.back().forward == 1.f);
  // The game has started the frame, but focus was lost before the effects are applied.
  r.game.on_features = [] { ResetInput(); };
  CHECK(!r.Step() && r.game.flight.back().forward == 0.f);
  CHECK(FlightKey(0, true));
  r.game.on_features = [] { SetCapture(false, false, true); };  // F1/F2/F6 capture work too
  CHECK(!r.Step() && r.game.flight.back().forward == 0.f);
}
}  // namespace

int main() {
  MailboxAndWorldEpoch();
  ModeChangeAndProfileFlush();
  HostToGuest();
  FocusAndFlight();
  std::cout << "Workshop runtime mailbox, change of authority, focus and profile flush passed\n";
}
