// gtav launcher - first-run setup window and game launcher.
//
// First run (or "gtav_launcher.exe --setup"): the player picks their own Disc 1, Disc 2 and Title
// Update 27; the launcher copies and checks the files, recompiles the game executable on this PC
// and starts the game. Later runs start the game directly.
// "Build for iPhone" (or "gtav_launcher.exe --iphone"): from the installed game, iphone\GTAV.ipa for
// Sideloadly / AltStore (install.h RunIphoneBuild); "Build for Android" (--android):
// android\gtav-android-pack.zip for the APK (RunAndroidBuild). The phones install the game files.
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "install.h"
#include "sources.h"
#include "update.h"

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
};
constexpr UINT kMsgLog = WM_APP + 1, kMsgStage = WM_APP + 2, kMsgProgress = WM_APP + 3,
               kMsgDone = WM_APP + 4, kMsgAsk = WM_APP + 5, kMsgUpdate = WM_APP + 6;

const wchar_t* kRowLabels[3] = {L"Disc 1 (Install)", L"Disc 2 (Play)", L"Title Update 27"};
const wchar_t* kRowHints[3] = {
    L"ISO image or extracted folder", L"ISO image or extracted folder",
    L"the TU file (e.g. tu00000009_00000000) or a folder with default.xexp + update.rpf"};

fs::path g_root;
HWND g_wnd, g_edit[3], g_install, g_play, g_cancel, g_progress, g_stage, g_log, g_iphone, g_android;
HWND g_rebuild, g_update;
bool g_close_after_job = false, g_rebuilding = false;
int g_phone_job = 0;  // the running job: 0 install, 1 iPhone build, 2 Android build
HFONT g_font, g_heading;
UINT g_dpi = 96;
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

std::wstring Pick(bool folder, bool iso_filter) {
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
    if (iso_filter)
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

void SetBusy(bool busy) {
  g_busy = busy;
  for (int i = 0; i < 3; ++i) {
    EnableWindow(g_edit[i], !busy);
    EnableWindow(GetDlgItem(g_wnd, kIdFile + i), !busy);
    EnableWindow(GetDlgItem(g_wnd, kIdFolder + i), !busy);
  }
  const auto status = busy ? InstallStatus::NeedsSources : GetInstallStatus(g_root, g_root);
  const bool ready = status == InstallStatus::Ready;
  EnableWindow(g_install, !busy);
  EnableWindow(g_rebuild, !busy && status != InstallStatus::NeedsSources);
  EnableWindow(g_iphone, !busy && ready);
  EnableWindow(g_android, !busy && ready);
  EnableWindow(g_cancel, busy);
  EnableWindow(g_play, !busy && ready);
  EnableWindow(g_update, !busy);
  if (!busy) SetWindowTextW(g_stage, InstallStatusText(status));
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
  SetBusy(true);
  if (g_worker.joinable()) g_worker.join();
  g_worker = std::thread([in, rebuild] {
    WindowUi ui;
    std::wstring error;
    bool ok = rebuild ? RunRebuild(g_root, g_root, ui, &error) : RunInstall(g_root, in, ui, &error);
    PostMessageW(g_wnd, kMsgDone, ok, reinterpret_cast<LPARAM>(new std::wstring(error)));
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

void StartPhone(bool android) {
  if (g_busy) return;
  SetWindowTextW(g_log, L"");
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  g_cancelled = false;
  g_phone_job = android ? 2 : 1;
  SetBusy(true);
  if (g_worker.joinable()) g_worker.join();
  g_worker = std::thread([android] {
    WindowUi ui;
    std::wstring error;
    bool ok = android ? RunAndroidBuild(g_root, g_root, ui, &error) : RunIphoneBuild(g_root, g_root, ui, &error);
    PostMessageW(g_wnd, kMsgDone, ok, reinterpret_cast<LPARAM>(new std::wstring(error)));
  });
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
          DWORD ex = 0) {
  HWND hw = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(w), Px(h), g_wnd,
                            reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return hw;
}

void CreateControls() {
  NONCLIENTMETRICSW ncm = {sizeof(ncm)};
  if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi))
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
  g_font = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW heading = ncm.lfMessageFont;
  heading.lfHeight = -Px(22);
  heading.lfWeight = FW_SEMIBOLD;
  g_heading = CreateFontIndirectW(&heading);
  HWND title = Make(L"STATIC", L"Set up GTA V Recomp", 0, 16, 12, 466, 31, 0);
  SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_heading), TRUE);
  const auto version = L"Launcher v" + Widen(kLauncherVersion);
  Make(L"STATIC", version.c_str(), SS_RIGHT, 494, 18, 230, 20, 0);
  Make(L"STATIC", L"Use your own Xbox 360 discs and Title Update 27. Setup checks, copies and builds locally. "
       L"Allow about 25 GB free for a first install. The update check is separate and optional.",
       0, 16, 47, 708, 40, 0);
  for (int i = 0; i < 3; ++i) {
    int y = 93 + i * 54;
    const auto name = std::to_wstring(i + 1) + L". " + kRowLabels[i];
    Make(L"STATIC", name.c_str(), 0, 16, y, 200, 18, 0);
    Make(L"STATIC", kRowHints[i], SS_ENDELLIPSIS, 216, y, 508, 18, 0);
    g_edit[i] = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 16, y + 20, 520, 24,
                     kIdEdit + i);
    Make(L"BUTTON", L"File...", WS_TABSTOP, 544, y + 19, 84, 26, kIdFile + i);
    Make(L"BUTTON", L"Folder...", WS_TABSTOP, 636, y + 19, 88, 26, kIdFolder + i);
  }
  g_install = Make(L"BUTTON", L"Install && build", WS_TABSTOP, 16, 258, 140, 32, kIdInstall);
  g_rebuild = Make(L"BUTTON", L"Recompile", WS_TABSTOP, 164, 258, 122, 32, kIdRebuild);
  g_cancel = Make(L"BUTTON", L"Cancel", WS_TABSTOP, 294, 258, 100, 32, kIdCancel);
  g_play = Make(L"BUTTON", L"Play", WS_TABSTOP, 604, 258, 120, 32, kIdPlay);
  g_stage = Make(L"STATIC", L"", SS_ENDELLIPSIS, 16, 304, 708, 20, kIdStage);
  g_progress = Make(PROGRESS_CLASSW, L"", 0, 16, 328, 708, 18, kIdProgress);
  SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
  g_log = Make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
               16, 356, 708, 156, kIdLog);
  g_update = Make(L"BUTTON", L"Check for updates", WS_TABSTOP, 16, 524, 160, 30, kIdUpdate);
  g_iphone = Make(L"BUTTON", L"Build for iPhone", WS_TABSTOP, 408, 524, 148, 30, kIdIphone);
  g_android = Make(L"BUTTON", L"Build for Android", WS_TABSTOP, 564, 524, 160, 30, kIdAndroid);
  LoadInputs();
  SetBusy(false);
}

LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_wnd = hw;
      CreateControls();
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (id >= kIdFile && id < kIdFile + 3) {
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
      } else if (id == kIdPlay) {
        std::wstring error;
        if (GetInstallStatus(g_root, g_root) != InstallStatus::Ready) {
          SetBusy(false);
        } else if (LaunchGame(g_root, &error))
          DestroyWindow(hw);
        else
          MessageBoxW(hw, error.c_str(), L"Play", MB_ICONERROR);
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
      else if (!g_cancelled) ShowUpdate(hw, *result);
      return 0;
    }
    case kMsgDone: {
      std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(lp));
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
        SetFocus(g_play);
      } else {
        SetWindowTextW(g_stage, g_cancelled ? (g_rebuilding || g_phone_job ? L"Cancelled." :
                          L"Cancelled. Retry setup to resume copying, or recompile if all files are present.")
                                            : L"Setup stopped. See the log below.");
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
      DeleteObject(g_font);
      DeleteObject(g_heading);
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

fs::path ExeDir() {
  wchar_t buf[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(buf).parent_path();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  g_root = ExeDir();
  int count = 0;
  LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!args) return 1;
  auto has = [&](std::wstring_view option) {
    for (int i = 1; i < count && std::wstring_view(args[i]) != L"--"; ++i)
      if (option == args[i]) return true;
    return false;
  };
  const bool setup = has(L"--setup"), rebuild = has(L"--rebuild"), unattended = has(L"--unattended");
  const bool iphone = has(L"--iphone"), android = has(L"--android"), update = has(L"--check-updates");
  LocalFree(args);
  if (update) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto result = CheckForUpdate();
    ShowUpdate(nullptr, result);
    if (SUCCEEDED(com)) CoUninitialize();
    return result.status == UpdateStatus::Error ? 1 : 0;
  }
  if (rebuild || unattended) return Unattended(rebuild);
  if (iphone || android) return PhoneUnattended(android);
  // The common case: installed and compatible, start the game and get out of the way.
  if (!setup && GetInstallStatus(g_root, g_root) == InstallStatus::Ready && !(GetKeyState(VK_SHIFT) & 0x8000)) {
    std::wstring error;
    if (LaunchGame(g_root, &error)) return 0;
    MessageBoxW(nullptr, error.c_str(), L"GTA V recomp", MB_ICONERROR);
  }

  g_dpi = GetDpiForSystem();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"GtavRecompSetup";
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);
  RECT r = {0, 0, Px(740), Px(570)};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
  HWND hw = CreateWindowW(wc.lpszClassName, L"GTA V recomp setup", style, CW_USEDEFAULT,
                          CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                          inst, nullptr);
  if (!hw) {
    CoUninitialize();
    return 1;
  }
  ShowWindow(hw, show);
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
