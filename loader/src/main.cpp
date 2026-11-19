// gtav launcher - Windows dashboard, local setup and verified ZIP updates.
//
// First run (or "gtav_launcher.exe --setup"): the player picks their own Disc 1, Disc 2 and Title
// Update 27; the launcher copies and checks the files, recompiles the game executable on this PC
// and starts the game. --play launches directly for shortcuts.
// "Build for iPhone" (or "gtav_launcher.exe --iphone"): from the installed game, iphone\GTAV.ipa for
// Sideloadly / AltStore (install.h RunIphoneBuild); "Build for Android" (--android):
// android\gtav-android-pack.zip for the APK (RunAndroidBuild). The phones install the game files.
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "install.h"
#include "sources.h"
#include "update.h"
#include "windows_update.h"

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
};
constexpr UINT kMsgLog = WM_APP + 1, kMsgStage = WM_APP + 2, kMsgProgress = WM_APP + 3,
               kMsgDone = WM_APP + 4, kMsgAsk = WM_APP + 5, kMsgUpdate = WM_APP + 6;

const wchar_t* kRowLabels[3] = {L"Disc 1 (Install)", L"Disc 2 (Play)", L"Title Update 27"};
const wchar_t* kRowHints[3] = {
    L"ISO image or extracted folder", L"ISO image or extracted folder",
    L"the TU file (e.g. tu00000009_00000000) or a folder with default.xexp + update.rpf"};

fs::path g_root;
HWND g_wnd, g_edit[3], g_install, g_play, g_cancel, g_progress, g_stage, g_log, g_iphone, g_android;
HWND g_rebuild, g_update, g_update_zip, g_release, g_update_text, g_install_label, g_install_text, g_badge;
bool g_close_after_job = false, g_rebuilding = false, g_installing_update = false;
std::wstring g_release_url;
InstallStatus g_status = InstallStatus::NeedsSources;
int g_phone_job = 0;  // the running job: 0 install, 1 iPhone build, 2 Android build
HFONT g_font, g_heading, g_display, g_small, g_mono;
HBRUSH g_field_brush;
UINT g_dpi = 96;
constexpr int kWidth = 928, kHeight = 620;
constexpr COLORREF kCanvas = RGB(16, 22, 33), kPanel = RGB(24, 34, 48), kHero = RGB(28, 41, 56),
                   kField = RGB(15, 20, 30), kBorder = RGB(57, 75, 94), kText = RGB(240, 246, 252),
                   kMuted = RGB(166, 184, 203), kAccent = RGB(119, 236, 187), kDark = RGB(13, 39, 35);
enum class Page { Always, Overview, Setup, Logs };
struct Control { HWND window; Page page; int x, y, width, height; HFONT* font; COLORREF ink; };
std::vector<Control> g_controls;
Page g_page = Page::Overview;
bool g_open_setup = false, g_high_contrast = false;
int Px(int pixel) { return MulDiv(pixel, g_dpi, 96); }
std::atomic<bool> g_busy{false}, g_cancelled{false};
std::thread g_worker;

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
    COMDLG_FILTERSPEC iso[] = {{L"Disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
    COMDLG_FILTERSPEC any[] = {{L"All files", L"*.*"}};
    COMDLG_FILTERSPEC zip[] = {{L"Official Windows update (*.zip)", L"*.zip"}};
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

void ShowPage(Page page) {
  g_page = page;
  for (const auto& control : g_controls)
    ShowWindow(control.window, control.page == Page::Always || control.page == page ? SW_SHOWNA : SW_HIDE);
  InvalidateRect(g_wnd, nullptr, TRUE);
}

void SetBusy(bool busy) {
  g_busy = busy;
  for (int i = 0; i < 3; ++i) {
    EnableWindow(g_edit[i], !busy);
    EnableWindow(GetDlgItem(g_wnd, kIdFile + i), !busy);
    EnableWindow(GetDlgItem(g_wnd, kIdFolder + i), !busy);
  }
  if (!busy) g_status = GetInstallStatus(g_root, g_root);
  const bool ready = g_status == InstallStatus::Ready;
  EnableWindow(g_install, !busy);
  EnableWindow(g_rebuild, !busy && g_status != InstallStatus::NeedsSources);
  EnableWindow(g_iphone, !busy && ready);
  EnableWindow(g_android, !busy && ready);
  EnableWindow(g_cancel, busy);
  EnableWindow(g_play, !busy);
  EnableWindow(g_update, !busy);
  EnableWindow(g_update_zip, !busy);
  EnableWindow(g_release, !busy && !g_release_url.empty());
  if (!busy) {
    SetWindowTextW(g_stage, InstallStatusText(g_status));
    SetWindowTextW(g_badge, ready ? L"READY" : g_status == InstallStatus::NeedsRebuild ? L"REBUILD NEEDED" : L"NOT INSTALLED");
    SetWindowTextW(g_install_label, ready ? L"Ready to play" : g_status == InstallStatus::NeedsRebuild ? L"Build for this release" : L"Set up your game");
    SetWindowTextW(g_install_text, ready ? L"Your local game library matches this runtime.\r\nPlay without re-importing your Xbox 360 discs."
                       : g_status == InstallStatus::NeedsRebuild ? L"Your game files are already here. Recompile for this release using the existing data and cache."
                       : L"Select your Xbox 360 discs and Title Update 27. The game is verified and built on this PC.");
    SetWindowTextW(g_play, ready ? L"Play GTA V" : g_status == InstallStatus::NeedsRebuild ? L"Recompile game" : L"Select discs and TU27");
  }
}

void StartInstall(bool rebuild) {
  if (g_busy) return;
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
        MessageBoxW(g_wnd, (std::wstring(kRowLabels[i]) + L": choose an existing file or folder.").c_str(),
                    L"Setup", MB_ICONINFORMATION);
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
  SetWindowTextW(g_stage, rebuild ? L"Preparing incremental rebuild..." : L"Preparing first-time setup...");
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

void ShowUpdate(HWND window, const UpdateResult& result) {
  std::wstring message = Widen(result.message);
  if (result.url.empty() && result.status != UpdateStatus::Error) {
    MessageBoxW(window, message.c_str(), L"Project updates", MB_ICONINFORMATION);
    return;
  }
  message += L"\n\nOpen GitHub Releases in your browser?";
  if (MessageBoxW(window, message.c_str(), L"Project updates", MB_YESNO | MB_DEFBUTTON2 | MB_ICONINFORMATION) != IDYES)
    return;
  const auto url = result.url.empty() ? Widen(std::string(kReleasePage)) : Widen(result.url);
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
    MessageBoxW(window, url.c_str(), L"Could not open your browser. Visit:", MB_ICONERROR);
}

void StartUpdate() {
  if (g_busy) return;
  g_cancelled = false;
  SetBusy(true);
  SetWindowTextW(g_stage, L"Checking GitHub for project updates...");
  if (g_worker.joinable()) g_worker.join();
  g_worker = std::thread([] {
    auto* result = new UpdateResult(CheckForUpdate());
    if (!PostMessageW(g_wnd, kMsgUpdate, 0, reinterpret_cast<LPARAM>(result))) delete result;
  });
}

void StartUpdateInstall() {
  if (g_busy) return;
  const auto zip = Pick(false, false, true);
  if (zip.empty()) return;
  if (MessageBoxW(g_wnd, L"Close the game first. The launcher will verify the ZIP against the latest stable release, "
                       L"restart to update package files, and keep your data, saves and settings. Continue?",
                  L"Install update", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
  SetWindowTextW(g_log, L"");
  SetWindowTextW(g_stage, L"Checking the official ZIP metadata...");
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  g_cancelled = false;
  g_installing_update = true;
  g_phone_job = 0;
  g_rebuilding = false;
  SetBusy(true);
  if (g_worker.joinable()) g_worker.join();
  g_worker = std::thread([zip] {
    WindowUi ui;
    std::wstring error;
    const auto result = CheckForUpdate();
    bool ok = false;
    if (!ui.Cancelled() && result.asset) {
      ok = PrepareWindowsUpdate(g_root, fs::path(zip), result.asset, ui, &error);
    } else if (!ui.Cancelled()) {
      error = result.status == UpdateStatus::Available
          ? L"GitHub did not provide a usable digest for this ZIP. No files have been replaced."
          : Widen(result.message);
    }
    auto* detail = new std::wstring(std::move(error));
    if (!PostMessageW(g_wnd, kMsgDone, ok, reinterpret_cast<LPARAM>(detail))) delete detail;
  });
}

void StartPhone(bool android) {
  if (g_busy) return;
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
  if (color == kText || color == kMuted) return GetSysColor(COLOR_WINDOWTEXT);
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
  g_display = Font(32, FW_BOLD);
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
  Label(Always, L"V", 40, 34, 46, 42, &g_display, kDark, SS_CENTER);
  Label(Always, L"GTA V Recomp", 106, 22, 470, 43, &g_display);
  Label(Always, L"XBOX 360  /  TITLE UPDATE 27  /  LOCAL RECOMPILATION", 109, 66, 492, 19, &g_small, kMuted);
  const auto version = L"WINDOWS X64   /   v" + Widen(kLauncherVersion);
  Label(Always, version.c_str(), 636, 38, 260, 22, &g_small, kMuted, SS_RIGHT);
  Button(Always, L"Overview", 32, 100, 130, 36, kIdOverview);
  Button(Always, L"Source setup", 171, 100, 140, 36, kIdSetupPage);
  Button(Always, L"Activity log", 320, 100, 132, 36, kIdLogsPage);
  Button(Always, L"Game folder", 764, 100, 132, 36, kIdOpenFolder);

  Label(Overview, L"LOCAL INSTALLATION", 58, 174, 320, 20, &g_small, kMuted);
  g_badge = Label(Overview, L"", 365, 174, 175, 20, &g_small, kAccent, SS_RIGHT);
  g_install_label = Label(Overview, L"", 56, 211, 485, 44, &g_display);
  g_install_text = Label(Overview, L"", 58, 264, 476, 56, &g_font, kMuted);
  g_play = Button(Overview, L"Play GTA V", 58, 332, 200, 42, kIdPlay);
  g_rebuild = Button(Overview, L"Recompile", 270, 332, 132, 42, kIdRebuild);
  Label(Overview, L"PROJECT RELEASES", 605, 174, 256, 20, &g_small, kMuted);
  Label(Overview, L"Updates", 604, 203, 265, 32, &g_heading);
  g_update_text = Label(Overview, L"Check the fork's stable releases. Install the official ZIP without touching your game data.",
                         605, 238, 260, 65, &g_font, kMuted);
  g_update = Button(Overview, L"Check", 605, 308, 118, 31, kIdUpdate);
  g_release = Button(Overview, L"Get ZIP", 733, 308, 139, 31, kIdOpenRelease);
  g_update_zip = Button(Overview, L"Install update ZIP...", 605, 346, 267, 31, kIdInstallUpdate);
  Label(Overview, L"YOUR FILES STAY LOCAL", 56, 428, 365, 21, &g_small, kAccent);
  Label(Overview, L"Game data, saves, settings and the compilation cache are kept in this installation.",
        56, 455, 795, 23, &g_font, kMuted);

  Label(Setup, L"GAME SOURCES", 56, 168, 420, 20, &g_small, kMuted);
  Label(Setup, L"Use your own discs. If the data is already installed, you can recompile from Overview instead.",
        56, 191, 814, 24, &g_font, kMuted);
  for (int i = 0; i < 3; ++i) {
    const int y = 223 + i * 59;
    Label(Setup, kRowLabels[i], 56, y, 180, 23);
    Label(Setup, kRowHints[i], 236, y + 2, 620, 20, &g_small, kMuted);
    g_edit[i] = Make(Setup, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 56, y + 25, 590, 28, kIdEdit + i);
    Button(Setup, L"File...", 657, y + 25, 98, 28, kIdFile + i);
    Button(Setup, L"Folder...", 765, y + 25, 106, 28, kIdFolder + i);
  }
  g_install = Button(Setup, L"Install & build", 56, 435, 181, 38, kIdInstall);
  g_iphone = Button(Setup, L"Build for iPhone", 548, 435, 152, 38, kIdIphone);
  g_android = Button(Setup, L"Build for Android", 711, 435, 160, 38, kIdAndroid);

  Label(Logs, L"ACTIVITY", 56, 167, 335, 26, &g_heading);
  Label(Logs, L"Current launcher activity. Install, update and game logs are kept on disk.",
        56, 198, 805, 22, &g_font, kMuted);
  g_log = Make(Logs, L"EDIT", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
               56, 230, 816, 191, kIdLog, &g_mono);
  Button(Logs, L"Open logs folder", 56, 438, 155, 35, kIdOpenLogs);
  SendMessageW(g_log, EM_SETLIMITTEXT, 128 * 1024, 0);

  Label(Always, L"INSTALLATION STATUS", 56, 527, 325, 19, &g_small, kMuted);
  g_stage = Label(Always, L"", 56, 550, 660, 24, &g_font, kText, SS_ENDELLIPSIS);
  g_progress = Make(Always, PROGRESS_CLASSW, L"", PBS_SMOOTH, 56, 579, 660, 9, kIdProgress);
  SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
  g_cancel = Button(Always, L"Cancel task", 737, 550, 135, 38, kIdCancel);
  Layout();
  LoadInputs();
  SetBusy(false);
  ShowPage(g_open_setup ? Setup : Overview);
  AppendLog(L"Launcher v" + Widen(kLauncherVersion) + L" | " + PathWide(g_root));
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

void PaintDashboard(HWND window) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);
  Box(dc, client, kCanvas, kCanvas);
  Box(dc, {Px(32), Px(29), Px(93), Px(79)}, kAccent, kAccent);
  if (g_page == Page::Overview) {
    Panel(dc, 32, 152, 532, 246, kHero);
    Box(dc, {Px(32), Px(153), Px(36), Px(397)}, kAccent, kAccent);
    Panel(dc, 582, 152, 314, 246);
    Panel(dc, 32, 414, 864, 78);
  } else Panel(dc, 32, 152, 864, 340);
  Panel(dc, 32, 509, 864, 95);
  EndPaint(window, &paint);
}

void PaintButton(const DRAWITEMSTRUCT& item) {
  const auto id = item.CtlID;
  const bool selected = (id == kIdOverview && g_page == Page::Overview) || (id == kIdSetupPage && g_page == Page::Setup) ||
                        (id == kIdLogsPage && g_page == Page::Logs);
  const bool primary = id == kIdPlay || id == kIdInstall;
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool down = (item.itemState & ODS_SELECTED) != 0;
  const auto fill = disabled ? kCanvas : primary ? (down ? RGB(83, 198, 152) : kAccent) : (selected || down) ? kHero : kPanel;
  Box(item.hDC, item.rcItem, fill, selected || primary ? kAccent : kBorder);
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, Theme(disabled ? kMuted : primary ? kDark : kText));
  const auto old_font = SelectObject(item.hDC, g_font);
  wchar_t text[128]{};
  GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
  auto bounds = item.rcItem;
  DrawTextW(item.hDC, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(item.hDC, old_font);
  if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
    InflateRect(&bounds, -Px(3), -Px(3));
    DrawFocusRect(item.hDC, &bounds);
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
      InvalidateRect(hw, nullptr, TRUE);
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (HIWORD(wp) != BN_CLICKED) break;
      if (id == kIdOverview || id == kIdSetupPage || id == kIdLogsPage) {
        ShowPage(id == kIdOverview ? Page::Overview : id == kIdSetupPage ? Page::Setup : Page::Logs);
      } else if (id == kIdOpenLogs || id == kIdOpenFolder) {
        const auto folder = id == kIdOpenLogs ? g_root / L"data/logs" : g_root;
        std::error_code ec;
        if (id == kIdOpenLogs) fs::create_directories(folder, ec);
        if (ec || reinterpret_cast<INT_PTR>(ShellExecuteW(hw, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
          MessageBoxW(hw, folder.c_str(), L"Could not open folder", MB_ICONERROR);
      } else if (id == kIdOpenRelease && !g_release_url.empty()) {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(hw, L"open", g_release_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
          MessageBoxW(hw, g_release_url.c_str(), L"Could not open your browser", MB_ICONERROR);
      } else if (id == kIdInstallUpdate) {
        StartUpdateInstall();
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
        StartUpdate();
      } else if (id == kIdIphone || id == kIdAndroid) {
        StartPhone(id == kIdAndroid);
      } else if (id == kIdCancel) {
        g_cancelled = true;
        SetWindowTextW(g_stage, L"Cancelling...");
      } else if (id == kIdPlay && !g_busy) {
        SetBusy(false);  // installation may have changed in a second launcher window
        if (g_status == InstallStatus::NeedsSources) {
          ShowPage(Page::Setup);
          SetFocus(g_edit[0]);
        } else if (g_status == InstallStatus::NeedsRebuild) {
          StartInstall(true);
        } else {
          std::wstring error;
          if (LaunchGame(g_root, &error)) DestroyWindow(hw);
          else MessageBoxW(hw, error.c_str(), L"Play", MB_ICONERROR);
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
      SetWindowTextW(g_stage, s->c_str());
      InvalidateRect(hw, nullptr, TRUE);
      return 0;
    }
    case kMsgProgress:
      SendMessageW(g_progress, PBM_SETPOS, wp, 0);
      return 0;
    case kMsgAsk: {
      auto* r = reinterpret_cast<AskRequest*>(lp);
      r->answer = !g_cancelled && MessageBoxW(hw, r->question.c_str(), L"Setup", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES;
      SetEvent(r->done);
      return 0;
    }
    case kMsgUpdate: {
      std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lp));
      SetBusy(false);
      if (g_close_after_job) DestroyWindow(hw);
      else if (!g_cancelled) {
        AppendLog(Widen(result->message));
        g_release_url = Widen(result->url);
        EnableWindow(g_release, !g_release_url.empty());
        const std::wstring message = result->asset
            ? L"Version " + Widen(result->asset.version) + L" is available. Get the ZIP, then choose Install update ZIP."
            : Widen(result->message.substr(0, result->message.find('\n')));
        SetWindowTextW(g_update_text, message.c_str());
        InvalidateRect(hw, nullptr, TRUE);
      }
      return 0;
    }
    case kMsgDone: {
      std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(lp));
      if (wp && g_installing_update) {
        DestroyWindow(hw);  // the update worker transferred the lock to the waiting helper
        return 0;
      }
      SetBusy(false);
      if (g_close_after_job) {
        DestroyWindow(hw);
        return 0;
      }
      if (wp && g_phone_job) {
        // Show the result in Explorer: the next step is dragging it into Sideloadly / onto the phone.
        const bool android = g_phone_job == 2;
        SetWindowTextW(g_stage, android ? L"Android build finished: copy android\\gtav-android-pack.zip and your "
                                          L"disc files to the phone."
                                        : L"iPhone build finished: install iphone\\GTAV.ipa with Sideloadly.");
        fs::path out = android ? g_root / L"android" / L"gtav-android-pack.zip" : g_root / L"iphone" / L"GTAV.ipa";
        std::wstring arg = L"/select,\"" + out.wstring() + L"\"";
        ShellExecuteW(hw, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
      } else if (wp) {
        SetWindowTextW(g_stage, L"Game build ready. Press Play.");
        ShowPage(Page::Overview);
        SetFocus(g_play);
      } else {
        SetWindowTextW(g_stage, g_cancelled ? (g_rebuilding || g_phone_job ? L"Cancelled." :
                          L"Cancelled. Retry setup to resume copying, or recompile if all files are present.")
                                            : L"Task stopped. See Activity log for details.");
        if (!g_cancelled) {
          AppendLog(L"error: " + *err);
          MessageBoxW(hw, err->c_str(), L"Setup", MB_ICONERROR);
        }
      }
      return 0;
    }
    case WM_CLOSE:
      if (g_busy) {
        if (g_close_after_job || MessageBoxW(hw, L"Stop the current task and close when it finishes?", L"Setup",
                                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
          return 0;
        if (!g_busy) {
          DestroyWindow(hw);
          return 0;
        }
        g_close_after_job = true;
        g_cancelled = true;
        SetWindowTextW(g_stage, L"Stopping. The window will close when the task ends...");
        return 0;
      }
      DestroyWindow(hw);
      return 0;
    case WM_DESTROY:
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
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto result = CheckForUpdate();
    ShowUpdate(nullptr, result);
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
    AppendLog(L"Update installed. Your local data and settings have been kept.");
    if (g_status == InstallStatus::NeedsRebuild) StartInstall(true);
    else SetWindowTextW(g_stage, g_status == InstallStatus::Ready ? L"Update installed. No recompilation needed."
                                                                : L"Update installed. Select your discs to set up the game.");
  }
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hw, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  if (g_worker.joinable()) g_worker.join();
  CoUninitialize();
  return 0;
}
