// gtav launcher - Windows library, local setup and verified Stable/Nightly updates.
//
// First run (or "gtav_launcher.exe --setup"): the player picks their own Disc 1, Disc 2 and Title
// Update 27; the launcher copies and checks the files, recompiles the game executable on this PC
// and starts the game. --play launches directly for shortcuts.
// "Build for iPhone" (or "gtav_launcher.exe --iphone"): from the installed game, iphone\GTAV.ipa for
// Sideloadly / AltStore (install.h RunIphoneBuild); "Build for Android" (--android):
// android\gtav-android-pack.zip for the APK (RunAndroidBuild). The phones install the game files.
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "install.h"
#include "sources.h"
#include "update.h"
#include "windows_update.h"
#include "win_process.h"

namespace fs = std::filesystem;
using namespace loader;

namespace {

enum : int {
  kIdEdit = 100,     // + row
  kIdFile = 110,     // + row
  kIdFolder = 120,   // + row
  kIdInstall = 200,
  kIdPlay = 201,
  kIdCancel = 202,
  kIdProgress = 203,
  kIdStage = 204,
  kIdLog = 205,
  kIdIphone = 206,
  kIdAndroid = 207,
  kIdRebuild = 208,
  kIdUpdate = 209,
  kIdOverview = 210,
  kIdSetupPage = 211,
  kIdLogsPage = 212,
  kIdOpenLogs = 213,
  kIdInstallUpdate = 214,
  kIdOpenRelease = 215,
  kIdOpenFolder = 216,
  kIdUpdatesPage = 217,
  kIdStable = 218,
  kIdNightly = 219,
  kIdAuto = 220,
  kIdDownload = 221,
  kIdGoToUpdates = 222,
};
constexpr UINT kMsgLog = WM_APP + 1, kMsgStage = WM_APP + 2, kMsgProgress = WM_APP + 3,
               kMsgDone = WM_APP + 4, kMsgAsk = WM_APP + 5, kMsgUpdate = WM_APP + 6;
constexpr UINT_PTR kDashboardTimer = 1;
constexpr ULONGLONG kCheckInterval = 20 * 60 * 1000;
constexpr wchar_t kDeferredUpdate[] = L"Mise à jour prête. Elle attend la fermeture du jeu ou de l’installation en cours.";

const wchar_t* kRowLabels[3] = {L"Disque 1", L"Disque 2", L"Mise à jour TU27"};
const wchar_t* kRowHints[3] = {L"ISO ou dossier extrait · installation", L"ISO ou dossier extrait · jeu",
    L"Fichier TU ou dossier contenant default.xexp et update.rpf"};

fs::path g_root;
HWND g_wnd, g_edit[3], g_install, g_play, g_cancel, g_progress, g_stage, g_log, g_iphone, g_android;
HWND g_rebuild, g_update, g_update_zip, g_release, g_download, g_stable, g_nightly, g_auto;
HWND g_update_text, g_feed_text, g_latest_text, g_auto_card, g_channel_hint;
HWND g_install_label, g_install_text, g_badge, g_kicker, g_title;
bool g_close_after_job = false, g_rebuilding = false, g_installing_update = false, g_update_automatic = false;
bool g_checking = false, g_no_background = false, g_pause_auto = false;
unsigned g_check_generation = 0;
struct CheckReply { unsigned generation; UpdateResult result; };
UpdatePreferences g_preferences;
UpdateResult g_update_result;
std::wstring g_update_notice = L"Aucune recherche effectuée.";
ULONGLONG g_next_check = 0, g_update_attempt_after = 0;
InstallStatus g_status = InstallStatus::NeedsSources;
int g_phone_job = 0;  // the running job: 0 install, 1 iPhone build, 2 Android build
HFONT g_font, g_heading, g_display, g_small, g_mono;
HBRUSH g_field_brush;
UINT g_dpi = 96;
constexpr int kWidth = 1024, kHeight = 648;
constexpr COLORREF kCanvas = RGB(15, 18, 25), kRail = RGB(19, 24, 34), kPanel = RGB(24, 31, 44),
                   kHero = RGB(30, 42, 60), kField = RGB(12, 17, 27),
                   kBorder = RGB(54, 66, 85), kText = RGB(246, 244, 242), kMuted = RGB(177, 189, 208),
                   kAccent = RGB(255, 181, 112), kMint = RGB(122, 229, 194), kDark = RGB(30, 25, 24);
enum class Page { Always, Overview, Setup, Updates, Logs };
struct Control { HWND window; Page page; int x, y, width, height; HFONT* font; COLORREF ink; };
std::vector<Control> g_controls;
Page g_page = Page::Overview;
bool g_open_setup = false, g_high_contrast = false;
int Px(int pixel) { return MulDiv(pixel, g_dpi, 96); }
bool g_busy = false;
std::atomic<bool> g_cancelled{false}, g_window_closed{false};
win::Handle g_game;
std::thread g_worker, g_check_worker;

fs::path SettingsPath() { return g_root / L"data" / L"launcher.ini"; }

void SaveInputs(const InstallInputs& in) {
  std::error_code ec;
  fs::create_directories(g_root / L"data", ec);
  std::ofstream f(SettingsPath());
  for (const auto* field : {&in.disc1, &in.disc2, &in.tu}) f << Narrow(*field) << "\n";
}

void LoadInputs() {
  std::ifstream f(SettingsPath());
  std::string line;
  for (HWND e : g_edit) {
    if (!std::getline(f, line)) break;
    SetWindowTextW(e, Widen(line).c_str());
  }
}

std::wstring Pick(bool folder, bool iso_filter, bool zip_filter = false) {
  std::wstring result;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dlg))))
    return result;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
  if (!folder) {
    COMDLG_FILTERSPEC iso[] = {{L"Images disque (*.iso)", L"*.iso"}, {L"Tous les fichiers", L"*.*"}};
    COMDLG_FILTERSPEC any[] = {{L"Tous les fichiers", L"*.*"}};
    COMDLG_FILTERSPEC zip[] = {{L"Mise à jour Windows (*.zip)", L"*.zip"}};
    if (zip_filter)
      dlg->SetFileTypes(1, zip);
    else if (iso_filter)
      dlg->SetFileTypes(2, iso);
    else
      dlg->SetFileTypes(1, any);
  }
  if (SUCCEEDED(dlg->Show(g_wnd))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  return result;
}

// Posts copies of strings to the window thread.
void PostText(UINT msg, const std::wstring& s) {
  auto* text = new std::wstring(s);
  if (!PostMessageW(g_wnd, msg, 0, reinterpret_cast<LPARAM>(text))) delete text;
}

struct AskRequest {
  std::wstring question;
  bool answer = false;
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~AskRequest() { CloseHandle(done); }
};

class WindowUi : public InstallUi {
 public:
  void Stage(const std::wstring& t) override { PostText(kMsgStage, t); }
  void Log(const std::wstring& t) override { PostText(kMsgLog, t); }
  void Progress(double f) override {
    int v = int(f * 1000);
    if (v != last_) {
      last_ = v;
      PostMessageW(g_wnd, kMsgProgress, v, 0);
    }
  }
  bool Cancelled() override { return g_cancelled; }
  bool Ask(const std::wstring& q) override {
    AskRequest r;
    r.question = q;
    if (!r.done || !PostMessageW(g_wnd, kMsgAsk, 0, reinterpret_cast<LPARAM>(&r))) return false;
    WaitForSingleObject(r.done, INFINITE);
    return r.answer;
  }

 private:
  int last_ = -1;
};

void AppendLog(const std::wstring& s) {
  int len = GetWindowTextLengthW(g_log);
  SendMessageW(g_log, EM_SETSEL, len, len);
  std::wstring line = s + L"\r\n";
  SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

const wchar_t* ChannelName(UpdateChannel channel) {
  return channel == UpdateChannel::Nightly ? L"Nightly" : L"Stable";
}

void RedrawDashboard() {
  RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void RefreshUpdateControls() {
  const bool nightly = g_preferences.channel == UpdateChannel::Nightly;
  const bool auto_install = g_preferences.automatic && !g_no_background && !g_pause_auto;
  const bool installable = !g_busy && !g_checking && !g_game;
  EnableWindow(g_update, !g_busy && !g_checking);
  EnableWindow(g_stable, !g_busy);
  EnableWindow(g_nightly, !g_busy);
  EnableWindow(g_update_zip, installable);
  EnableWindow(g_download, installable && bool(g_update_result.asset));
  EnableWindow(g_auto, !g_busy);
  EnableWindow(g_release, !g_busy);
  SetWindowTextW(g_auto, g_preferences.automatic ? L"Automatique : activé" : L"Automatique : désactivé");
  SetWindowTextW(g_channel_hint, nightly ? L"Canal actif : Nightly. Builds expérimentaux, régressions possibles."
                                         : L"Canal actif : Stable. Versions validées, recommandées pour jouer.");
  SetWindowTextW(g_feed_text, (L"GitHub  /  " + Widen(std::string(ReleaseRepository(g_preferences.channel)))).c_str());
  SetWindowTextW(g_update_text, g_update_notice.c_str());
  const auto latest = g_update_result.latest.empty() ? std::wstring(L"—") : L"v" + Widen(g_update_result.latest);
  SetWindowTextW(g_latest_text, (L"DERNIÈRE VERSION  /  " + latest).c_str());
  const auto status = g_installing_update ? L"installation en cours" : auto_install && g_game && g_update_result.asset ? L"mise à jour après le jeu" :
      g_update_result.asset ? L"mise à jour disponible" : g_checking ? L"recherche en cours" :
      auto_install ? L"mises à jour automatiques" : g_preferences.automatic ? L"automatique en pause" : L"mises à jour sur demande";
  SetWindowTextW(g_auto_card, (std::wstring(ChannelName(g_preferences.channel)) + L"  ·  " + status).c_str());
  RedrawDashboard();
}

void ShowPage(Page page) {
  g_page = page;
  SetWindowTextW(g_kicker, page == Page::Overview ? L"BIBLIOTHÈQUE   /   XBOX 360" : L"HOOKY   /   CENTRE DE CONTRÔLE");
  SetWindowTextW(g_title, page == Page::Overview ? L"GTA V Recomp" : page == Page::Setup ? L"Installation" :
                         page == Page::Updates ? L"Mises à jour" : L"Journal d’activité");
  for (const auto& control : g_controls) {
    ShowWindow(control.window, control.page == Page::Always || control.page == page ? SW_SHOWNA : SW_HIDE);
  }
  RedrawDashboard();
}

void SetBusy(bool busy) {
  g_busy = busy;
  const bool playing = bool(g_game), editable = !busy && !playing;
  for (int i = 0; i < 3; ++i) {
    EnableWindow(g_edit[i], editable);
    EnableWindow(GetDlgItem(g_wnd, kIdFile + i), editable);
    EnableWindow(GetDlgItem(g_wnd, kIdFolder + i), editable);
  }
  if (editable) g_status = GetInstallStatus(g_root, g_root);
  const bool ready = g_status == InstallStatus::Ready, needs_build = g_status == InstallStatus::NeedsRebuild;
  EnableWindow(g_install, editable);
  EnableWindow(g_rebuild, editable && g_status != InstallStatus::NeedsSources);
  EnableWindow(g_iphone, editable && ready);
  EnableWindow(g_android, editable && ready);
  EnableWindow(g_cancel, busy);
  EnableWindow(g_play, editable);
  if (!busy) {
    SetWindowTextW(g_stage, playing ? L"Jeu en cours — aucune mise à jour ne sera appliquée" : ready ? L"Prêt à jouer" :
                           needs_build ? L"Données présentes — recompilation nécessaire" : L"Choisis tes disques et TU27 pour commencer");
    SetWindowTextW(g_badge, playing ? L"EN JEU" : ready ? L"PRÊT" : needs_build ? L"À RECOMPILER" : L"À CONFIGURER");
    SetWindowTextW(g_install_label, ready ? L"Direction Los Santos." : needs_build ? L"Recompile le jeu." : L"Prépare ton jeu.");
    SetWindowTextW(g_install_text, ready ? L"Campagne, bac à sable ou multijoueur privé. Tes données restent sur ce PC."
                       : needs_build ? L"Tes fichiers sont prêts. Recompile cette version sans réimporter tes disques."
                       : L"Importe tes deux disques Xbox 360 et la mise à jour TU27. Le jeu sera compilé sur ce PC.");
    SetWindowTextW(g_play, playing ? L"Jeu en cours" : ready ? L"Jouer" : needs_build ? L"Compiler le jeu" : L"Configurer le jeu");
  }
  RefreshUpdateControls();
}

void StartInstall(bool rebuild) {
  if (g_busy || g_game) return;
  InstallInputs in;
  if (!rebuild) {
    std::wstring* fields[3] = {&in.disc1, &in.disc2, &in.tu};
    for (int i = 0; i < 3; ++i) {
      const int length = GetWindowTextLengthW(g_edit[i]);
      std::wstring path(size_t(length) + 1, L'\0');
      path.resize(GetWindowTextW(g_edit[i], path.data(), length + 1));
      std::error_code ec;
      const auto input = fs::absolute(path, ec);
      if (path.empty() || ec || (!fs::is_regular_file(input, ec) && !fs::is_directory(input, ec))) {
        MessageBoxW(g_wnd, (std::wstring(kRowLabels[i]) + L" : choisis un fichier ou un dossier existant.").c_str(),
                    L"Installation", MB_ICONINFORMATION);
        SetFocus(g_edit[i]);
        return;
      }
      *fields[i] = input.wstring();
    }
    SaveInputs(in);
  }
  SetWindowTextW(g_log, L"");
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  g_cancelled = false;
  g_phone_job = 0;
  g_rebuilding = rebuild;
  g_installing_update = false;
  SetWindowTextW(g_stage, rebuild ? L"Préparation de la recompilation…" : L"Préparation de l’installation…");
  SetBusy(true);
  if (g_worker.joinable()) g_worker.join();
  g_worker = std::thread([in, rebuild] {
    WindowUi ui;
    std::wstring error;
    bool ok = rebuild ? RunRebuild(g_root, g_root, ui, &error) : RunInstall(g_root, in, ui, &error);
    auto* detail = new std::wstring(std::move(error));
    if (!PostMessageW(g_wnd, kMsgDone, ok, reinterpret_cast<LPARAM>(detail))) delete detail;
  });
}

void ShowUpdate(HWND window, const UpdateResult& result, UpdateChannel channel) {
  std::wstring message = Widen(result.message);
  if (result.url.empty() && result.status != UpdateStatus::Error) {
    MessageBoxW(window, message.c_str(), L"Mises à jour", MB_ICONINFORMATION);
    return;
  }
  message += L"\n\nOuvrir les versions GitHub ?";
  if (MessageBoxW(window, message.c_str(), L"Mises à jour", MB_YESNO | MB_DEFBUTTON2 | MB_ICONINFORMATION) != IDYES) return;
  const auto url = Widen(result.url.empty() ? std::string(ReleasePage(channel)) : result.url);
  ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void StartUpdateCheck() {
  if (g_busy || g_checking) return;
  if (g_check_worker.joinable()) g_check_worker.join();
  g_checking = true;
  g_update_result = {};
  g_update_notice = L"Recherche des versions publiées sur GitHub…";
  g_next_check = GetTickCount64() + kCheckInterval;
  RefreshUpdateControls();
  const auto channel = g_preferences.channel;
  const auto window = g_wnd;
  const auto generation = g_check_generation;
  g_check_worker = std::thread([channel, window, generation] {
    auto* reply = new CheckReply{generation, CheckForUpdate(channel)};
    if (g_window_closed || !PostMessageW(window, kMsgUpdate, 0, reinterpret_cast<LPARAM>(reply))) delete reply;
  });
}

std::wstring UpdateSummary(const UpdateResult& result) {
  switch (result.status) {
    case UpdateStatus::Available:
      return L"Version v" + Widen(result.latest) + (result.asset ? L" disponible et vérifiable."
          : L" publiée, mais sans empreinte SHA-256 utilisable. Installation désactivée.");
    case UpdateStatus::Current: return L"À jour · dernière version publiée : v" + Widen(result.latest);
    case UpdateStatus::NoRelease: return L"Aucune version publiée sur ce canal pour le moment.";
    case UpdateStatus::NoPackage: return L"Version v" + Widen(result.latest) + L" publiée sans ZIP Windows x64.";
    case UpdateStatus::OlderChannel: return L"Ce canal est plus ancien que ta version installée. Utilise un dossier séparé pour y revenir.";
    default: return L"GitHub n’a pas répondu correctement. Tu peux continuer à jouer hors ligne.";
  }
}

void StartUpdateInstall(fs::path zip, bool automatic) {
  if (g_busy || g_checking || g_game || (zip.empty() && !g_update_result.asset)) return;
  const auto channel = g_preferences.channel;
  const auto asset = g_update_result.asset;
  SetWindowTextW(g_log, L"");
  SetWindowTextW(g_stage, zip.empty() ? L"Préparation du téléchargement…" : L"Vérification du ZIP sélectionné…");
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  g_cancelled = false;
  g_installing_update = true;
  g_update_automatic = automatic;
  g_pause_auto = true;  // avoid repeated downloads in this session on cancellation or failure
  g_phone_job = 0;
  g_rebuilding = false;
  SetBusy(true);
  if (g_worker.joinable()) g_worker.join();
  g_worker = std::thread([zip = std::move(zip), channel, asset] {
    WindowUi ui;
    std::wstring error;
    auto prepared = UpdatePreparation::Failed;
    if (zip.empty()) {
      prepared = PrepareWindowsUpdate(g_root, {}, asset, ui, &error);
    } else {
      const auto official = CheckForUpdate(channel);
      if (!ui.Cancelled() && official.asset) {
        prepared = PrepareWindowsUpdate(g_root, zip, official.asset, ui, &error);
      } else if (!ui.Cancelled()) {
        error = official.status == UpdateStatus::Available ? L"Empreinte SHA-256 officielle indisponible. Aucun fichier remplacé."
                                                          : Widen(official.message);
      }
    }
    auto* detail = new std::wstring(std::move(error));
    if (!PostMessageW(g_wnd, kMsgDone, static_cast<WPARAM>(prepared), reinterpret_cast<LPARAM>(detail))) delete detail;
  });
}

void InstallSelectedZip() {
  if (g_busy || g_checking || g_game) return;
  const auto zip = Pick(false, false, true);
  if (zip.empty()) return;
  if (MessageBoxW(g_wnd, L"Le ZIP doit correspondre à la dernière version du canal choisi. Ferme le jeu avant de continuer.\n\n"
                       L"Le launcher vérifiera le ZIP et redémarrera sans modifier tes sauvegardes.",
                  L"Installer un ZIP", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES)
    StartUpdateInstall(zip, false);
}

void SelectChannel(UpdateChannel channel) {
  if (g_busy || channel == g_preferences.channel) return;
  if (channel == UpdateChannel::Nightly &&
      MessageBoxW(g_wnd, L"Nightly contient des builds expérimentaux : des régressions sont possibles. "
                        L"Si les mises à jour automatiques sont activées, ces builds seront installés automatiquement à l’arrêt du jeu.\n\n"
                        L"Passer sur Nightly ?", L"Canal Nightly", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
  auto prefs = g_preferences;
  prefs.channel = channel;
  if (!SaveWindowsUpdatePreferences(g_root, prefs)) {
    MessageBoxW(g_wnd, L"Impossible d’enregistrer la préférence du canal.", L"Mises à jour", MB_ICONERROR);
    return;
  }
  g_preferences = prefs;
  g_pause_auto = false;
  ++g_check_generation;  // a reply from the old selection must not reinstall that channel
  g_update_result = {};
  g_update_notice = L"Canal sélectionné. Lance une recherche pour voir les versions disponibles.";
  g_next_check = 0;
  RefreshUpdateControls();
}

void ToggleAutomatic() {
  if (g_busy) return;
  auto prefs = g_preferences;
  prefs.automatic = !prefs.automatic;
  if (!SaveWindowsUpdatePreferences(g_root, prefs)) {
    MessageBoxW(g_wnd, L"Impossible d’enregistrer la préférence de mise à jour.", L"Mises à jour", MB_ICONERROR);
    return;
  }
  g_preferences = prefs;
  if (prefs.automatic) { g_pause_auto = false; g_next_check = 0; }
  RefreshUpdateControls();
}

void DashboardTick() {
  const auto now = GetTickCount64();
  if (g_game && WaitForSingleObject(g_game.Get(), 0) == WAIT_OBJECT_0) {
    g_game.Close();
    g_update_attempt_after = now;
    AppendLog(L"Jeu fermé.");
    SetBusy(false);
  }
  // Don't take over the window during source selection, log review or a modal dialog.
  if (g_no_background || !g_preferences.automatic || g_busy || g_checking || !IsWindowEnabled(g_wnd) ||
      (g_page != Page::Overview && g_page != Page::Updates)) return;
  if (now >= g_next_check) { StartUpdateCheck(); return; }
  if (!g_update_result.asset || g_pause_auto || now < g_update_attempt_after) return;
  // The worker still takes the same lock before staging; the timer's probe is only for scheduling.
  if (g_game || !WindowsUpdateIdle(g_root)) {
    g_update_attempt_after = now + 5'000;
    if (g_update_notice != kDeferredUpdate) {
      g_update_notice = kDeferredUpdate;
      RefreshUpdateControls();
    }
    return;
  }
  StartUpdateInstall({}, true);
}

void StartPhone(bool android) {
  if (g_busy || g_game) return;
  SetWindowTextW(g_log, L"");
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  g_cancelled = false;
  g_phone_job = android ? 2 : 1;
  g_installing_update = false;
  SetBusy(true);
  if (g_worker.joinable()) g_worker.join();
  g_worker = std::thread([android] {
    WindowUi ui;
    std::wstring error;
    bool ok = android ? RunAndroidBuild(g_root, g_root, ui, &error) : RunIphoneBuild(g_root, g_root, ui, &error);
    auto* detail = new std::wstring(std::move(error));
    if (!PostMessageW(g_wnd, kMsgDone, ok, reinterpret_cast<LPARAM>(detail))) delete detail;
  });
}

COLORREF Theme(COLORREF color) {
  if (!g_high_contrast) return color;
  if (color == kAccent) return GetSysColor(COLOR_HIGHLIGHT);
  if (color == kDark) return GetSysColor(COLOR_HIGHLIGHTTEXT);
  if (color == kText || color == kMuted || color == kMint) return GetSysColor(COLOR_WINDOWTEXT);
  if (color == kBorder) return GetSysColor(COLOR_WINDOWTEXT);
  return GetSysColor(COLOR_WINDOW);
}

HFONT Font(int size, int weight, const wchar_t* family = L"Segoe UI") {
  return CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
}

void Layout() {
  for (auto* font : {&g_font, &g_heading, &g_display, &g_small, &g_mono}) if (*font) DeleteObject(*font);
  g_font = Font(15, FW_NORMAL);
  g_heading = Font(19, FW_SEMIBOLD);
  g_display = Font(38, FW_BOLD);
  g_small = Font(12, FW_SEMIBOLD);
  g_mono = Font(13, FW_NORMAL, L"Consolas");
  for (const auto& control : g_controls) {
    SetWindowPos(control.window, nullptr, Px(control.x), Px(control.y), Px(control.width), Px(control.height),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(control.window, WM_SETFONT, reinterpret_cast<WPARAM>(*control.font), TRUE);
  }
}

HWND Make(Page page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
          int id = 0, HFONT* font = &g_font, COLORREF ink = kText) {
  HWND hw = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, g_wnd,
                            reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  g_controls.push_back({hw, page, x, y, w, h, font, ink});
  return hw;
}

HWND Label(Page page, const wchar_t* text, int x, int y, int w, int h, HFONT* font = &g_font,
           COLORREF color = kText, DWORD style = 0) {
  return Make(page, L"STATIC", text, SS_NOPREFIX | style, x, y, w, h, 0, font, color);
}

HWND Button(Page page, const wchar_t* text, int x, int y, int w, int h, int id) {
  return Make(page, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id);
}

void CreateControls() {
  using enum Page;
  HIGHCONTRASTW contrast{sizeof(contrast)};
  g_high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                    (contrast.dwFlags & HCF_HIGHCONTRASTON);
  g_field_brush = CreateSolidBrush(Theme(kField));
  if (!g_high_contrast) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));  // immersive dark title bar on supported Windows versions
  }
  Label(Always, L"H", 22, 34, 44, 41, &g_heading, kDark, SS_CENTERIMAGE | SS_CENTER);
  Label(Always, L"HOOKY", 80, 34, 107, 25, &g_heading);
  Label(Always, L"RECOMP  /  DESKTOP", 81, 61, 115, 17, &g_small, kMuted);
  Label(Always, L"BIBLIOTHÈQUE", 26, 117, 150, 20, &g_small, kMuted);
  Button(Always, L"Accueil", 18, 145, 170, 43, kIdOverview);
  Button(Always, L"Installation", 18, 194, 170, 43, kIdSetupPage);
  Button(Always, L"Mises à jour", 18, 243, 170, 43, kIdUpdatesPage);
  Button(Always, L"Journaux", 18, 292, 170, 43, kIdLogsPage);
  Label(Always, L"SUR CE PC", 26, 479, 148, 18, &g_small, kMuted);
  Button(Always, L"Dossier du jeu", 18, 506, 170, 38, kIdOpenFolder);
  Label(Always, L"WINDOWS X64", 26, 579, 149, 18, &g_small, kMuted);
  const auto version = L"v" + Widen(kLauncherVersion);
  Label(Always, version.c_str(), 26, 603, 157, 20, &g_mono, kText, SS_ENDELLIPSIS);

  g_kicker = Label(Always, L"", 233, 25, 585, 21, &g_small, kAccent);
  g_title = Label(Always, L"", 231, 48, 645, 51, &g_display);
  Label(Always, L"X360  /  TU27", 859, 57, 137, 33, &g_small, kMint, SS_CENTER | SS_CENTERIMAGE);

  Label(Overview, L"GRAND THEFT AUTO V", 257, 148, 430, 20, &g_small, kAccent);
  g_install_label = Label(Overview, L"", 253, 181, 458, 55, &g_display);
  g_install_text = Label(Overview, L"", 257, 244, 391, 53, &g_font, kMuted);
  g_play = Button(Overview, L"Jouer", 256, 313, 202, 43, kIdPlay);
  g_rebuild = Button(Overview, L"Recompiler", 469, 313, 133, 43, kIdRebuild);
  Label(Overview, L"01   /   SAN ANDREAS", 780, 337, 184, 20, &g_small, kMuted, SS_RIGHT);

  Label(Overview, L"01  /  INSTALLATION LOCALE", 256, 414, 317, 20, &g_small, kMuted);
  g_badge = Label(Overview, L"", 254, 445, 317, 27, &g_heading, kMint);
  Label(Overview, L"Disques et sauvegardes sur ton PC", 256, 482, 317, 20, &g_small, kMuted);
  Label(Overview, L"02  /  DISTRIBUTION", 631, 414, 330, 20, &g_small, kMuted);
  g_auto_card = Label(Overview, L"", 629, 441, 341, 23, &g_font, kText, SS_ENDELLIPSIS);
  Button(Overview, L"Gérer les mises à jour", 629, 475, 201, 31, kIdGoToUpdates);

  for (int i = 0; i < 3; ++i) {
    const int y = 145 + i * 84;
    Label(Setup, kRowLabels[i], 256, y, 156, 24, &g_heading);
    Label(Setup, kRowHints[i], 424, y + 4, 553, 23, &g_small, kMuted, SS_ENDELLIPSIS);
    g_edit[i] = Make(Setup, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 254, y + 31, 501, 32, kIdEdit + i);
    SendMessageW(g_edit[i], EM_SETLIMITTEXT, 32767, 0);
    Button(Setup, L"Fichier…", 769, y + 29, 93, 36, kIdFile + i);
    Button(Setup, L"Dossier…", 873, y + 29, 103, 36, kIdFolder + i);
  }
  g_install = Button(Setup, L"Installer et compiler", 254, 410, 218, 43, kIdInstall);
  Label(Setup, L"EXPORTS OPTIONNELS", 255, 481, 215, 21, &g_small, kMuted);
  g_iphone = Button(Setup, L"Construire pour iPhone", 565, 473, 201, 37, kIdIphone);
  g_android = Button(Setup, L"Construire pour Android", 777, 473, 199, 37, kIdAndroid);

  Label(Updates, L"CANAL DE DISTRIBUTION", 255, 138, 367, 22, &g_small, kMuted);
  g_stable = Button(Updates, L"Stable   /   Recommandé", 254, 167, 345, 48, kIdStable);
  g_nightly = Button(Updates, L"Nightly   /   Expérimental", 616, 167, 360, 48, kIdNightly);
  g_channel_hint = Label(Updates, L"", 255, 231, 714, 21, &g_font, kMuted, SS_ENDELLIPSIS);
  g_auto = Button(Updates, L"", 254, 269, 237, 39, kIdAuto);
  g_feed_text = Label(Updates, L"", 509, 279, 467, 21, &g_mono, kMuted, SS_RIGHT | SS_ENDELLIPSIS);
  Label(Updates, (L"INSTALLÉE  /  " + version).c_str(), 255, 336, 357, 23, &g_small, kMint, SS_ENDELLIPSIS);
  g_latest_text = Label(Updates, L"", 619, 336, 357, 23, &g_small, kAccent, SS_RIGHT | SS_ENDELLIPSIS);
  g_update_text = Label(Updates, L"", 254, 367, 721, 53, &g_font, kText);
  g_download = Button(Updates, L"Installer maintenant", 254, 424, 206, 42, kIdDownload);
  g_update = Button(Updates, L"Rechercher", 469, 424, 147, 42, kIdUpdate);
  g_update_zip = Button(Updates, L"ZIP local…", 625, 424, 135, 42, kIdInstallUpdate);
  g_release = Button(Updates, L"Versions GitHub", 769, 424, 207, 42, kIdOpenRelease);
  Label(Updates, L"SHA-256 vérifié · Aucun changement aux sauvegardes · Le launcher redémarre après installation",
        255, 485, 719, 23, &g_small, kMuted);

  Label(Logs, L"ACTIVITÉ DE CETTE SESSION", 255, 140, 455, 23, &g_small, kMuted);
  g_log = Make(Logs, L"EDIT", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
               254, 175, 722, 272, kIdLog, &g_mono);
  Button(Logs, L"Ouvrir le dossier des journaux", 254, 471, 257, 38, kIdOpenLogs);
  SendMessageW(g_log, EM_SETLIMITTEXT, 128 * 1024, 0);

  Label(Always, L"ÉTAT DE L’INSTALLATION", 255, 556, 518, 18, &g_small, kMuted);
  g_stage = Label(Always, L"", 254, 578, 572, 25, &g_font, kText, SS_ENDELLIPSIS);
  g_progress = Make(Always, PROGRESS_CLASSW, L"", PBS_SMOOTH, 254, 611, 575, 8, kIdProgress);
  SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
  g_cancel = Button(Always, L"Annuler", 845, 569, 131, 42, kIdCancel);
  Layout();
  LoadInputs();
  SetBusy(false);
  ShowPage(g_open_setup ? Setup : Overview);
  AppendLog(L"Launcher " + version + L" | " + PathWide(g_root));
}

void Box(HDC dc, RECT r, COLORREF background, COLORREF border) {
  auto brush = CreateSolidBrush(Theme(background));
  FillRect(dc, &r, brush);
  DeleteObject(brush);
  if (border == background) return;
  brush = CreateSolidBrush(Theme(border));
  FrameRect(dc, &r, brush);
  DeleteObject(brush);
}

void Panel(HDC dc, int x, int y, int w, int h, COLORREF background = kPanel) {
  Box(dc, {Px(x), Px(y), Px(x + w), Px(y + h)}, background, kBorder);
}

// Original, asset-free night skyline. Geometry stays behind the right side of the hero card.
void PaintCity(HDC dc) {
  if (g_high_contrast) return;
  const auto saved = SaveDC(dc);
  if (!saved) return;
  IntersectClipRect(dc, Px(645), Px(120), Px(997), Px(376));
  for (int i = 0; i < 32; ++i) {
    auto mix = [i](int a, int b) { return (a * (31 - i) + b * i) / 31; };
    const auto color = RGB(mix(30, 80), mix(42, 53), mix(60, 67));
    Box(dc, {Px(645 + i * 11), Px(120), Px(645 + (i + 1) * 11), Px(376)}, color, color);
  }
  const auto sun = CreateSolidBrush(RGB(187, 101, 105));
  auto old_brush = SelectObject(dc, sun);
  auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
  Ellipse(dc, Px(831), Px(151), Px(941), Px(261));
  SelectObject(dc, old_brush);
  DeleteObject(sun);
  for (const int i : {2, 3, 4, 5, 6}) {
    const auto line = RGB(91, 60, 75);
    Box(dc, {Px(805), Px(203 + i * 6), Px(966), Px(204 + i * 6)}, line, line);
  }
  const int elevations[] = {18, 31, 24, 49, 41, 63, 27, 52, 72, 38, 52, 33, 44, 21};
  for (int i = 0; i < int(std::size(elevations)); ++i) {
    const int x = 645 + i * 26, y = 289 - elevations[i];
    const auto silhouette = RGB(33, 37, 57);
    Box(dc, {Px(x), Px(y), Px(x + 25), Px(310)}, silhouette, silhouette);
    const auto lit = RGB(130, 97, 94);
    for (int yy = y + 7; yy < 282; yy += 10) {
      if ((yy + i) % 3) Box(dc, {Px(x + 7), Px(yy), Px(x + 11), Px(yy + 2)}, lit, lit);
      if (i % 3 == 1) Box(dc, {Px(x + 16), Px(yy), Px(x + 20), Px(yy + 2)}, lit, lit);
    }
  }
  const auto inset = CreatePen(PS_SOLID, std::max(1, Px(1)), RGB(95, 87, 113));
  SelectObject(dc, inset);
  for (int i = 0; i < 6; ++i) {
    MoveToEx(dc, Px(794 + i * 10), Px(303), nullptr);
    LineTo(dc, Px(634 + i * 91), Px(376));
  }
  for (const int y : {305, 318, 340, 369}) {
    MoveToEx(dc, Px(645), Px(y), nullptr);
    LineTo(dc, Px(997), Px(y));
  }
  SelectObject(dc, old_pen);
  DeleteObject(inset);
  RestoreDC(dc, saved);
}

void PaintDashboard(HWND window) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);
  Box(dc, client, kCanvas, kCanvas);
  Box(dc, {0, 0, Px(206), client.bottom}, kRail, kRail);
  Box(dc, {Px(205), 0, Px(206), client.bottom}, kBorder, kBorder);
  Box(dc, {Px(22), Px(34), Px(66), Px(75)}, kAccent, kAccent);
  Panel(dc, 859, 57, 137, 33, kRail);
  Box(dc, {Px(26), Px(559), Px(181), Px(560)}, kBorder, kBorder);
  if (g_page == Page::Overview) {
    Panel(dc, 232, 119, 766, 258, kHero);
    PaintCity(dc);
    Box(dc, {Px(233), Px(120), Px(236), Px(376)}, kAccent, kAccent);
    Panel(dc, 232, 396, 361, 128);
    Panel(dc, 608, 396, 390, 128);
  } else {
    Panel(dc, 232, 116, 766, 416);
    if (g_page == Page::Setup || g_page == Page::Updates) {
      const int y = g_page == Page::Setup ? 397 : 321;
      Box(dc, {Px(254), Px(y), Px(976), Px(y + 1)}, kBorder, kBorder);
    }
  }
  Panel(dc, 232, 542, 766, 87, kRail);
  EndPaint(window, &paint);
}

void PaintButton(const DRAWITEMSTRUCT& item) {
  const auto id = item.CtlID;
  const bool rail = id == kIdOverview || id == kIdSetupPage || id == kIdUpdatesPage || id == kIdLogsPage || id == kIdOpenFolder;
  const bool active_page = (id == kIdOverview && g_page == Page::Overview) || (id == kIdSetupPage && g_page == Page::Setup) ||
                           (id == kIdUpdatesPage && g_page == Page::Updates) || (id == kIdLogsPage && g_page == Page::Logs);
  const bool selected = active_page || (id == kIdNightly && g_preferences.channel == UpdateChannel::Nightly) ||
      (id == kIdStable && g_preferences.channel == UpdateChannel::Stable) || (id == kIdAuto && g_preferences.automatic);
  const bool primary = id == kIdPlay || id == kIdInstall || id == kIdDownload;
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool down = (item.itemState & ODS_SELECTED) != 0;
  const auto fill = disabled ? kRail : primary ? (down ? RGB(229, 146, 85) : kAccent) : (selected || down) ? kHero : rail ? kRail : kPanel;
  Box(item.hDC, item.rcItem, fill, disabled ? kBorder : selected || primary ? kAccent : rail ? kRail : kBorder);
  if (active_page) {
    auto bar = item.rcItem;
    bar.right = bar.left + Px(3);
    Box(item.hDC, bar, kAccent, kAccent);
  }
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, Theme(disabled ? kMuted : primary ? kDark : kText));
  const auto old_font = SelectObject(item.hDC, primary ? g_heading : g_font);
  wchar_t text[128]{};
  GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
  auto bounds = item.rcItem;
  bounds.left += Px(rail ? 42 : 8);
  bounds.right -= Px(8);
  DrawTextW(item.hDC, text, -1, &bounds, (rail ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  if (rail) {
    auto glyph = item.rcItem;
    glyph.left += Px(10);
    glyph.right = glyph.left + Px(23);
    SelectObject(item.hDC, g_small);
    SetTextColor(item.hDC, Theme(active_page ? kAccent : kMuted));
    const auto prefix = id == kIdOverview ? L"01" : id == kIdSetupPage ? L"02" : id == kIdUpdatesPage ? L"03" :
                        id == kIdLogsPage ? L"04" : L"↗";
    DrawTextW(item.hDC, prefix, -1, &glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  SelectObject(item.hDC, old_font);
  if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
    auto focus = item.rcItem;
    InflateRect(&focus, -Px(3), -Px(3));
    DrawFocusRect(item.hDC, &focus);
  }
}

UINT FitDpi(HWND window, UINT dpi) {
  MONITORINFO monitor{sizeof(monitor)};
  auto hmon = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (GetMonitorInfoW(hmon, &monitor)) {
    const auto& area = monitor.rcWork;
    dpi = std::min(dpi, UINT(std::max(1, MulDiv(area.right - area.left - 24, 96, kWidth + 20))));
    dpi = std::min(dpi, UINT(std::max(1, MulDiv(area.bottom - area.top - 24, 96, kHeight + 48))));
  }
  return dpi;
}

LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_wnd = hw;
      CreateControls();
      return 0;
    case WM_TIMER:
      if (wp == kDashboardTimer) DashboardTick();
      return 0;
    case WM_ERASEBKGND:
      return 1;  // PaintDashboard covers the client; avoid a blank flash during repaint
    case WM_PAINT:
      PaintDashboard(hw);
      return 0;
    case WM_DRAWITEM:
      if (auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp); item->CtlType == ODT_BUTTON) {
        PaintButton(*item);
        return TRUE;
      }
      break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      auto dc = reinterpret_cast<HDC>(wp);
      auto child = reinterpret_cast<HWND>(lp);
      const int id = GetDlgCtrlID(child);
      if (id == kIdLog || (id >= kIdEdit && id < kIdEdit + 3)) {
        SetTextColor(dc, Theme(kText));
        SetBkColor(dc, Theme(kField));
        return reinterpret_cast<LRESULT>(g_field_brush);
      }
      auto control = std::find_if(g_controls.begin(), g_controls.end(), [child](const Control& c) { return c.window == child; });
      SetTextColor(dc, Theme(control == g_controls.end() ? kText : control->ink));
      SetBkMode(dc, TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }
    case WM_DPICHANGED: {
      g_dpi = FitDpi(hw, HIWORD(wp));
      const RECT suggested = *reinterpret_cast<const RECT*>(lp);
      RECT size{0, 0, Px(kWidth), Px(kHeight)};
      AdjustWindowRectExForDpi(&size, DWORD(GetWindowLongPtrW(hw, GWL_STYLE)), FALSE, 0, HIWORD(wp));
      SetWindowPos(hw, nullptr, suggested.left, suggested.top, size.right - size.left, size.bottom - size.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      Layout();
      RedrawDashboard();
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (HIWORD(wp) != BN_CLICKED) break;
      if (id == kIdOverview || id == kIdSetupPage || id == kIdUpdatesPage || id == kIdLogsPage || id == kIdGoToUpdates) {
        ShowPage(id == kIdOverview ? Page::Overview : id == kIdSetupPage ? Page::Setup :
                 id == kIdLogsPage ? Page::Logs : Page::Updates);
        if (id == kIdGoToUpdates) SetFocus(g_stable);
      } else if (id == kIdOpenLogs || id == kIdOpenFolder) {
        const auto folder = id == kIdOpenLogs ? g_root / L"data/logs" : g_root;
        std::error_code ec;
        if (id == kIdOpenLogs) fs::create_directories(folder, ec);
        if (ec || reinterpret_cast<INT_PTR>(ShellExecuteW(hw, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
          MessageBoxW(hw, folder.c_str(), L"Impossible d’ouvrir le dossier", MB_ICONERROR);
      } else if (id == kIdOpenRelease && !g_busy) {
        const auto url = Widen(g_update_result.url.empty() ? std::string(ReleasePage(g_preferences.channel)) : g_update_result.url);
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(hw, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
          MessageBoxW(hw, url.c_str(), L"Impossible d’ouvrir le navigateur", MB_ICONERROR);
      } else if (id == kIdStable || id == kIdNightly) {
        SelectChannel(id == kIdStable ? UpdateChannel::Stable : UpdateChannel::Nightly);
      } else if (id == kIdAuto) {
        ToggleAutomatic();
      } else if (id == kIdDownload) {
        StartUpdateInstall({}, false);
      } else if (id == kIdInstallUpdate) {
        InstallSelectedZip();
      } else if (id >= kIdFile && id < kIdFile + 3) {
        int row = id - kIdFile;
        std::wstring p = Pick(false, row < 2);
        if (!p.empty()) SetWindowTextW(g_edit[row], p.c_str());
      } else if (id >= kIdFolder && id < kIdFolder + 3) {
        std::wstring p = Pick(true, false);
        if (!p.empty()) SetWindowTextW(g_edit[id - kIdFolder], p.c_str());
      } else if (id == kIdInstall || id == kIdRebuild) {
        StartInstall(id == kIdRebuild);
      } else if (id == kIdUpdate) {
        StartUpdateCheck();
      } else if (id == kIdIphone || id == kIdAndroid) {
        StartPhone(id == kIdAndroid);
      } else if (id == kIdCancel) {
        g_cancelled = true;
        EnableWindow(g_cancel, FALSE);
        SetWindowTextW(g_stage, L"Annulation…");
        RedrawDashboard();
      } else if (id == kIdPlay && !g_busy && !g_game) {
        SetBusy(false);  // an unattended setup may have changed the installation
        if (g_status == InstallStatus::NeedsSources) {
          ShowPage(Page::Setup);
          SetFocus(g_edit[0]);
        } else if (g_status == InstallStatus::NeedsRebuild) {
          StartInstall(true);
        } else {
          std::wstring error;
          if (LaunchWindowsGame(g_root, &error, {}, &g_game)) {
            AppendLog(L"Jeu démarré.");
            SetBusy(false);
          } else MessageBoxW(hw, error.c_str(), L"Jouer", MB_ICONERROR);
        }
      }
      return 0;
    }
    case kMsgLog: {
      std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));
      AppendLog(*s);
      return 0;
    }
    case kMsgStage: {
      std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));
      if (!g_cancelled) SetWindowTextW(g_stage, s->c_str());
      RedrawDashboard();
      return 0;
    }
    case kMsgProgress:
      SendMessageW(g_progress, PBM_SETPOS, wp, 0);
      return 0;
    case kMsgAsk: {
      auto* r = reinterpret_cast<AskRequest*>(lp);
      r->answer = !g_cancelled && MessageBoxW(hw, r->question.c_str(), L"Installation", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES;
      SetEvent(r->done);
      return 0;
    }
    case kMsgUpdate: {
      std::unique_ptr<CheckReply> reply(reinterpret_cast<CheckReply*>(lp));
      g_checking = false;
      if (reply->generation == g_check_generation) {
        g_update_result = std::move(reply->result);
        g_update_attempt_after = GetTickCount64() + 3'000;  // let Play or a channel change take precedence
        g_update_notice = UpdateSummary(g_update_result);
        AppendLog(std::wstring(ChannelName(g_preferences.channel)) + L" | " + Widen(g_update_result.message));
      }
      RefreshUpdateControls();
      return 0;
    }
    case kMsgDone: {
      std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(lp));
      const auto prepared = static_cast<UpdatePreparation>(wp);
      if (g_installing_update && prepared == UpdatePreparation::Ready) {
        DestroyWindow(hw);  // the update worker transferred the lock to the waiting helper
        return 0;
      }
      const bool update_job = std::exchange(g_installing_update, false);
      const bool automatic = std::exchange(g_update_automatic, false);
      SetBusy(false);
      if (g_close_after_job) {
        DestroyWindow(hw);
        return 0;
      }
      if (update_job && prepared == UpdatePreparation::Deferred && !g_cancelled) {
        g_pause_auto = !automatic;
        g_update_attempt_after = GetTickCount64() + 5'000;
        g_update_notice = kDeferredUpdate;
        RefreshUpdateControls();
        if (!automatic) MessageBoxW(hw, L"Ferme le jeu ou l’installation en cours, puis réessaie.", L"Mises à jour", MB_ICONINFORMATION);
      } else if (update_job) {
        g_update_notice = g_cancelled ? L"Mise à jour annulée. L’installation n’a pas été modifiée."
            : L"Mise à jour non installée. Consulte les journaux ou réessaie manuellement.";
        SetWindowTextW(g_stage, g_update_notice.c_str());
        if (!g_cancelled) {
          AppendLog(L"Mise à jour | " + *err);
          if (!automatic) MessageBoxW(hw, err->c_str(), L"Mises à jour", MB_ICONERROR);
        }
        RefreshUpdateControls();
      } else if (wp && g_phone_job) {
        // Show the result in Explorer: the next step is dragging it into Sideloadly / onto the phone.
        const bool android = g_phone_job == 2;
        SetWindowTextW(g_stage, android ? L"Export Android prêt : copie le ZIP et tes disques sur le téléphone."
                                        : L"Export iPhone prêt : installe GTAV.ipa avec Sideloadly.");
        fs::path out = android ? g_root / L"android" / L"gtav-android-pack.zip" : g_root / L"iphone" / L"GTAV.ipa";
        std::wstring arg = L"/select,\"" + out.wstring() + L"\"";
        ShellExecuteW(hw, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
      } else if (wp) {
        SetWindowTextW(g_stage, L"Le jeu est prêt. Tu peux jouer.");
        ShowPage(Page::Overview);
        SetFocus(g_play);
      } else {
        SetWindowTextW(g_stage, g_cancelled ? (g_rebuilding || g_phone_job ? L"Tâche annulée." :
                          L"Installation annulée. Relance-la pour reprendre la copie.")
                                            : L"Tâche interrompue. Consulte les journaux.");
        if (!g_cancelled) {
          AppendLog(L"error: " + *err);
          MessageBoxW(hw, err->c_str(), L"Installation", MB_ICONERROR);
        }
      }
      return 0;
    }
    case WM_CLOSE:
      if (g_busy) {
        if (g_close_after_job || (!g_installing_update &&
            MessageBoxW(hw, L"Annuler la tâche en cours et fermer le launcher ?", L"GTA V Recomp",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)) return 0;
        if (!g_busy) {
          DestroyWindow(hw);
          return 0;
        }
        g_close_after_job = true;
        g_cancelled = true;
        SetWindowTextW(g_stage, L"Annulation de la tâche avant fermeture…");
        return 0;
      }
      DestroyWindow(hw);
      return 0;
    case WM_DESTROY:
      g_window_closed = true;
      KillTimer(hw, kDashboardTimer);
      for (auto font : {g_font, g_heading, g_display, g_small, g_mono}) if (font) DeleteObject(font);
      if (g_field_brush) DeleteObject(g_field_brush);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// "--unattended": no window; installs from the three paths in data\launcher.ini (one per line),
// writes the stage and progress lines to data\logs\unattended.txt, says no to every question and
// returns 0 on success. For testing a release and for scripted installs.
class FileUi : public InstallUi {
 public:
  explicit FileUi(const fs::path& path) : f_(path, std::ios::app) {}
  void Stage(const std::wstring& t) override { Write(L"stage: " + t); }
  void Log(const std::wstring& t) override { Write(t); }
  void Progress(double f) override {
    int v = int(f * 100);
    if (v != last_) Write(L"progress: " + std::to_wstring(last_ = v) + L"%");
  }
  bool Cancelled() override { return false; }
  bool Ask(const std::wstring& q) override {
    Write(L"question (answered no): " + q);
    return false;
  }

 private:
  void Write(const std::wstring& s) {
    std::lock_guard lock(m_);
    std::u8string u = fs::path(s).u8string();
    f_ << std::string(u.begin(), u.end()) << "\n";
    f_.flush();
  }
  std::mutex m_;
  std::ofstream f_;
  int last_ = -1;
};

int Unattended(bool rebuild = false) {
  std::error_code ec;
  fs::create_directories(g_root / L"data" / L"logs", ec);
  FileUi ui(g_root / L"data" / L"logs" / (rebuild ? L"rebuild_unattended.txt" : L"unattended.txt"));
  std::wstring error;
  if (rebuild) {
    bool ok = RunRebuild(g_root, g_root, ui, &error);
    ui.Log(ok ? L"done" : L"error: " + error);
    return ok ? 0 : 1;
  }
  InstallInputs in;
  std::ifstream f(SettingsPath());
  for (std::wstring* field : {&in.disc1, &in.disc2, &in.tu}) {
    std::string line;
    std::getline(f, line);
    *field = Widen(line);
  }
  if (in.disc1.empty() || in.disc2.empty() || in.tu.empty()) {
    ui.Log(L"error: data\\launcher.ini needs three lines: Disc 1, Disc 2, Title Update");
    return 1;
  }
  bool ok = RunInstall(g_root, in, ui, &error);
  ui.Log(ok ? L"done" : L"error: " + error);
  return ok ? 0 : 1;
}

// "--iphone" / "--android": the phone builds without a window (data\logs\<phone>_unattended.txt).
int PhoneUnattended(bool android) {
  std::error_code ec;
  fs::create_directories(g_root / L"data" / L"logs", ec);
  FileUi ui(g_root / L"data" / L"logs" / (android ? L"android_unattended.txt" : L"iphone_unattended.txt"));
  std::wstring error;
  bool ok = Preflight(g_root, &error) && (android ? RunAndroidBuild(g_root, g_root, ui, &error)
                                                  : RunIphoneBuild(g_root, g_root, ui, &error));
  ui.Log(ok ? L"done" : L"error: " + error);
  return ok ? 0 : 1;
}

fs::path ExePath() {
  std::wstring buf(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  return length && length < buf.size() ? fs::path(buf.substr(0, length)) : fs::path();
}

uintptr_t HandleArg(const wchar_t* text) {
  if (!*text || std::wstring_view(text).find_first_not_of(L"0123456789") != std::wstring_view::npos) return 0;
  return static_cast<uintptr_t>(std::wcstoull(text, nullptr, 10));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  const auto exe = ExePath();
  if (exe.empty()) return 1;
  g_root = exe.parent_path();
  int count = 0;
  LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!args) return 1;
  auto has = [&](std::wstring_view option) {
    for (int i = 1; i < count && std::wstring_view(args[i]) != L"--"; ++i)
      if (option == args[i]) return true;
    return false;
  };
  if (count == 4 && (std::wstring_view(args[1]) == L"--commit-update" || std::wstring_view(args[1]) == L"--recover-update")) {
    const bool recover = std::wstring_view(args[1]) == L"--recover-update";
    const auto parent = HandleArg(args[2]), lock = HandleArg(args[3]);
    LocalFree(args);
    return FinishWindowsUpdate(exe, parent, lock, recover);
  }
  const bool setup = has(L"--setup"), rebuild = has(L"--rebuild"), unattended = has(L"--unattended");
  const bool iphone = has(L"--iphone"), android = has(L"--android"), update = has(L"--check-updates");
  const bool play = has(L"--play"), updated = has(L"--updated");
  g_no_background = has(L"--no-auto-update");
  std::vector<std::wstring> game_args;
  for (int i = 1; i < count; ++i) {
    if (std::wstring_view(args[i]) == L"--") {
      for (++i; i < count; ++i) game_args.emplace_back(args[i]);
      break;
    }
  }
  LocalFree(args);
  if (HasPendingWindowsUpdate(g_root)) {
    std::wstring error;
    if (StartWindowsUpdateRecovery(g_root, &error)) return 0;
    if (!unattended && !rebuild) MessageBoxW(nullptr, error.c_str(), L"GTA V Recomp | Update recovery", MB_ICONERROR);
    return 1;
  }
  if (update) {
    g_preferences = LoadWindowsUpdatePreferences(g_root);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto result = CheckForUpdate(g_preferences.channel);
    ShowUpdate(nullptr, result, g_preferences.channel);
    if (SUCCEEDED(com)) CoUninitialize();
    return result.status == UpdateStatus::Error ? 1 : 0;
  }
  if (rebuild || unattended) return Unattended(rebuild);
  if (iphone || android) return PhoneUnattended(android);
  if (play && !setup) {
    std::wstring error;
    if (LaunchGame(g_root, &error, game_args)) return 0;
    MessageBoxW(nullptr, error.c_str(), L"GTA V Recomp", MB_ICONINFORMATION);
  }
  // Another window mapped from this same EXE would prevent the helper replacing it.
  // Headless setup/play keep using the separate installation lock; only the dashboard
  // holds this one, for its lifetime, including any final thread cleanup.
  std::error_code lock_error;
  fs::create_directories(g_root / L"data", lock_error);
  win::Handle dashboard_lock(lock_error ? nullptr : win::OpenSetupLock(g_root / L"data/launcher-ui.lock"));
  if (!dashboard_lock) {
    MessageBoxW(nullptr, L"Le launcher est déjà ouvert ou ce dossier n’est pas accessible en écriture.",
                L"GTA V Recomp", MB_ICONINFORMATION);
    return 1;
  }
  g_preferences = LoadWindowsUpdatePreferences(g_root);
  g_open_setup = setup || (GetKeyState(VK_SHIFT) & 0x8000);
  const UINT system_dpi = GetDpiForSystem();
  g_dpi = FitDpi(nullptr, system_dpi);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"GtavRecompDashboard";
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);
  RECT r = {0, 0, Px(kWidth), Px(kHeight)};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, system_dpi);
  HWND hw = CreateWindowW(wc.lpszClassName, L"GTA V Recomp", style, CW_USEDEFAULT,
                          CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                          inst, nullptr);
  if (!hw) {
    CoUninitialize();
    return 1;
  }
  ShowWindow(hw, show);
  if (updated) {
    g_next_check = GetTickCount64() + kCheckInterval;  // do not update again immediately after a clean restart
    AppendLog(L"Mise à jour installée. Tes données et réglages ont été conservés.");
    if (g_status == InstallStatus::NeedsRebuild) StartInstall(true);
    else SetWindowTextW(g_stage, g_status == InstallStatus::Ready ? L"Mise à jour installée. Prêt à jouer."
                                                                : L"Mise à jour installée. Choisis tes disques pour commencer.");
  }
  SetTimer(hw, kDashboardTimer, 1'000, nullptr);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hw, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  if (g_worker.joinable()) g_worker.join();
  if (g_check_worker.joinable()) g_check_worker.join();
  while (PeekMessageW(&m, nullptr, kMsgUpdate, kMsgUpdate, PM_REMOVE)) {
    delete reinterpret_cast<CheckReply*>(m.lParam);
  }
  CoUninitialize();
  return 0;
}
