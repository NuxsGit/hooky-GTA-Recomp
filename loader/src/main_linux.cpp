// gtav launcher (Linux) - first-run setup window (or terminal) and game launcher.
//
//   ./gtav_launcher                         installed: start the game; otherwise the setup window
//   ./gtav_launcher --setup                 the setup window again (installed or not)
//   ./gtav_launcher --setup DISC1 DISC2 TU  setup in the terminal with these paths
//   ./gtav_launcher --cli [--setup]         terminal setup even with a desktop (paths asked for)
//   ./gtav_launcher --unattended            setup from data/launcher.ini, no questions (testing)
//   ./gtav_launcher --rebuild               validate the installed data and recompile without discs
//   ./gtav_launcher --check-updates         check the fork's stable GitHub release
//   ./gtav_launcher --iphone                after setup: iphone/GTAV.ipa for Sideloadly / AltStore
//   ./gtav_launcher -- ARGS...              pass ARGS to the game (for example --vulkan_device=1)
//
// Setup is the same as on Windows and macOS (install.cpp): the player's own Disc 1, Disc 2 and
// Title Update 27 are copied and checked, the game executable is recompiled on this machine into
// gtav_game.so, then the game starts. The window is GTK 3, loaded at run time (dlopen): the
// launcher needs only glibc, and without GTK or a display it falls back to the terminal.
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef GTAV_GTK_UI
// Headers for the types only: every GTK/GLib function is called through dlsym pointers, and the
// cast macros must not call into GObject.
#define G_DISABLE_CAST_CHECKS
#include <gtk/gtk.h>
#endif

#include "campaign_config.h"
#include "desktop_home.h"
#include "install.h"
#include "sources.h"
#include "update.h"

namespace fs = std::filesystem;
using namespace loader;

namespace {

const char* kRowLabels[3] = {"Disc 1 (Install)", "Disc 2 (Play)", "Title Update 27"};
const char* kRowHints[3] = {
    "ISO image or extracted folder", "ISO image or extracted folder",
    "the TU file (e.g. tu00000009_00000000) or a folder with default.xexp + update.rpf"};

fs::path g_root;
volatile std::sig_atomic_t g_cancelled = 0;

fs::path SettingsPath() { return g_root / "data" / "launcher.ini"; }

void AppendCampaignArgs(std::vector<std::wstring>& args) {
  auto cfg = LoadCampaignHostConfig(g_root);
  if (!cfg.address.empty()) args.push_back(L"--gtav_campaign_host=" + Widen(cfg.address));
  if (cfg.port != 30127) args.push_back(L"--gtav_campaign_port=" + std::to_wstring(cfg.port));
}

std::vector<std::string> LoadInputs() {
  std::vector<std::string> v(3);
  std::ifstream f(SettingsPath());
  for (std::string& s : v) std::getline(f, s);
  return v;
}

void SaveInputs(const std::vector<std::string>& v) {
  std::error_code ec;
  fs::create_directories(g_root / "data", ec);
  std::ofstream f(SettingsPath());
  for (const std::string& s : v) f << s << "\n";
}

std::string Trim(std::string s) {
  while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
  s.erase(0, i);
  // A path dropped into the terminal from a file manager arrives quoted.
  if (s.size() >= 2 && (s[0] == '\'' || s[0] == '"') && s.back() == s[0])
    s = s.substr(1, s.size() - 2);
  if (s.rfind("~/", 0) == 0) {
    if (const char* home = getenv("HOME")) s = std::string(home) + s.substr(1);
  }
  return s;
}

// Terminal UI for the install: stage lines, a progress line when stdout is a terminal, y/n
// questions on stdin, Ctrl+C cancels. Everything also goes to data/logs/install.log (install.cpp).
class TermUi : public InstallUi {
 public:
  TermUi() : tty_(isatty(STDOUT_FILENO)) {}
  void Stage(const std::wstring& t) override {
    std::string s = Narrow(t);
    if (s == last_stage_) return;
    last_stage_ = s;
    Line(s);
  }
  void Log(const std::wstring& t) override { Line("  " + Narrow(t)); }
  void Progress(double f) override {
    int v = int(f * 1000);
    if (v == last_ || !tty_) {
      if (!tty_ && v / 100 != last_ / 100) printf("progress: %d%%\n", v / 10);
      last_ = v;
      return;
    }
    last_ = v;
    int bar = v * 40 / 1000;
    printf("\r  [%.*s%*s] %5.1f%%", bar, "########################################", 40 - bar, "",
           v / 10.0);
    fflush(stdout);
    progress_shown_ = true;
  }
  bool Cancelled() override { return g_cancelled != 0; }
  bool Ask(const std::wstring& q) override {
    Line(Narrow(q));
    printf("[y/N] ");
    fflush(stdout);
    std::string answer;
    if (!std::getline(std::cin, answer)) return false;
    answer = Trim(answer);
    return answer == "y" || answer == "Y" || answer == "yes";
  }

 private:
  void Line(const std::string& s) {
    if (progress_shown_) {
      printf("\r%60s\r", "");
      progress_shown_ = false;
    }
    printf("%s\n", s.c_str());
    fflush(stdout);
  }
  bool tty_;
  bool progress_shown_ = false;
  int last_ = -1;
  std::string last_stage_;
};

// "--unattended": installs from the three paths in data/launcher.ini (one per line), writes the
// stage and progress lines to data/logs/unattended.txt, says no to every question and returns 0 on
// success. For testing a release and for scripted installs. Same as the Windows launcher.
class FileUi : public InstallUi {
 public:
  explicit FileUi(const fs::path& path) : f_(path, std::ios::app) {}
  void Stage(const std::wstring& t) override { Write(L"stage: " + t); }
  void Log(const std::wstring& t) override { Write(t); }
  void Progress(double f) override {
    int v = int(f * 100);
    if (v != last_) Write(L"progress: " + std::to_wstring(last_ = v) + L"%");
  }
  bool Cancelled() override { return g_cancelled != 0; }
  bool Ask(const std::wstring& q) override {
    Write(L"question (answered no): " + q);
    return false;
  }

 private:
  void Write(const std::wstring& s) {
    std::lock_guard lock(m_);
    f_ << Narrow(s) << "\n";
    f_.flush();
  }
  std::mutex m_;
  std::ofstream f_;
  int last_ = -1;
};

int Unattended() {
  std::error_code ec;
  fs::create_directories(g_root / "data" / "logs", ec);
  FileUi ui(g_root / "data" / "logs" / "unattended.txt");
  std::wstring error;
  if (!Preflight(g_root, &error)) {
    ui.Log(L"error: " + error);
    return 1;
  }
  std::vector<std::string> v = LoadInputs();
  if (v[0].empty() || v[1].empty() || v[2].empty()) {
    ui.Log(L"error: data/launcher.ini needs three lines: Disc 1, Disc 2, Title Update");
    return 1;
  }
  InstallInputs in{Widen(v[0]), Widen(v[1]), Widen(v[2])};
  bool ok = RunInstall(g_root, in, ui, &error);
  ui.Log(ok ? L"done" : L"error: " + error);
  return ok ? 0 : 1;
}

void AddDesktopEntry(InstallUi* ui);

int Rebuild() {
  TermUi ui;
  std::wstring error;
  if (!RunRebuild(g_root, g_root, ui, &error)) {
    fprintf(stderr, "\nRecompile stopped: %s\n(log: %s)\n", Narrow(error).c_str(),
            (g_root / "data/logs/install.log").c_str());
    return 1;
  }
  AddDesktopEntry(&ui);
  return 0;
}

int CheckUpdates() {
  const auto result = CheckForUpdate();
  puts(result.message.c_str());
  if (!result.url.empty()) puts(result.url.c_str());
  else if (result.status == UpdateStatus::Error) printf("Releases: %s\n", kReleasePage.data());
  return result.status == UpdateStatus::Error ? 1 : 0;
}

int Setup(std::vector<std::string> given, const std::vector<std::wstring>& game_args) {
  printf("GTA V Recomp | Launcher v%s\n\n"
         "Use your own Xbox 360 discs and Title Update 27. Setup checks, copies and builds locally.\n"
         "Allow about 25 GB free for a first install. Ctrl+C stops the task.\n"
         "Check project updates separately with --check-updates.\n\n", kLauncherVersion);
  if (given.empty() && GetInstallStatus(g_root, g_root) != InstallStatus::NeedsSources) {
    TermUi ui;
    if (ui.Ask(L"Existing game files found. Recompile without the discs?")) return Rebuild();
  }
  std::vector<std::string> saved = LoadInputs();
  std::vector<std::string> paths(3);
  for (int i = 0; i < 3; ++i) {
    if (i < int(given.size())) {
      paths[i] = Trim(given[i]);
      continue;
    }
    printf("%s - %s\n", kRowLabels[i], kRowHints[i]);
    if (!saved[i].empty()) printf("  [Enter: %s]\n", saved[i].c_str());
    printf("> ");
    fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return 1;
    paths[i] = Trim(line).empty() ? saved[i] : Trim(line);
    if (paths[i].empty()) {
      fprintf(stderr, "No path given for %s.\n", kRowLabels[i]);
      return 1;
    }
  }
  for (int i = 0; i < 3; ++i) {
    std::error_code ec;
    if (!fs::is_regular_file(paths[i], ec) && !fs::is_directory(paths[i], ec)) {
      fprintf(stderr, "%s: %s does not exist.\n", kRowLabels[i], paths[i].c_str());
      return 1;
    }
    paths[i] = fs::absolute(paths[i], ec).string();
  }
  SaveInputs(paths);

  printf("\n");
  TermUi ui;
  std::wstring error;
  InstallInputs in{Widen(paths[0]), Widen(paths[1]), Widen(paths[2])};
  if (!RunInstall(g_root, in, ui, &error)) {
    fprintf(stderr, "\nSetup stopped: %s\n(log: %s)\n", Narrow(error).c_str(),
            (g_root / "data" / "logs" / "install.log").c_str());
    return 1;
  }
  AddDesktopEntry(&ui);
  printf("\nSetup finished. Start the game now? [Y/n] ");
  fflush(stdout);
  std::string answer;
  if (std::getline(std::cin, answer) && (Trim(answer) == "n" || Trim(answer) == "N")) return 0;
  {
    auto args = game_args;
    AppendCampaignArgs(args);
    if (!LaunchGame(g_root, &error, args)) {
      fprintf(stderr, "%s\n", Narrow(error).c_str());
      return 1;
    }
  }
  return 0;
}

// "GTA V recomp" in the desktop's applications menu (~/.local/share/applications), pointing at
// this launcher, with a "Setup and updates" action. Rewritten after every setup (the folder may move).
void AddDesktopEntry(InstallUi* ui) {
  const char* home = getenv("HOME");
  if (!home || !*home) return;
  const char* data = getenv("XDG_DATA_HOME");
  fs::path dir = data && *data ? fs::path(data) / "applications" : fs::path(home) / ".local/share/applications";
  std::error_code ec;
  fs::create_directories(dir, ec);
  // Exec quoting (Desktop Entry spec): the path in double quotes, \ " ` $ escaped.
  std::string exe;
  for (char c : (g_root / "gtav_launcher").string()) {
    if (c == '"' || c == '`' || c == '$' || c == '\\') exe += '\\';
    exe += c;
  }
  std::ofstream f(dir / "gtav-recomp.desktop");
  f << "[Desktop Entry]\nType=Application\nName=GTA V recomp\n"
       "Comment=GTA V for Xbox 360, recompiled from your own copy\n"
    << "Exec=\"" << exe << "\"\nPath=" << g_root.string() << "\n"
    << "Icon=applications-games\nTerminal=false\nCategories=Game;\nActions=Setup;\n\n"
       "[Desktop Action Setup]\nName=Setup and updates\n"
    << "Exec=\"" << exe << "\" --setup\n";
  f.close();
  if (f && ui) ui->Log(L"Added \"GTA V recomp\" to the applications menu.");
}

bool HaveDisplay() {
  const char* x = getenv("DISPLAY");
  const char* w = getenv("WAYLAND_DISPLAY");
  return (x && *x) || (w && *w);
}

#ifdef GTAV_GTK_UI
//-----------------------------------------------------------------------------
// Setup window (GTK 3 through dlopen)
//-----------------------------------------------------------------------------

struct Gtk {
#define GTAV_GTK_FUNCTIONS(X)                                                                        \
  X(gtk_init_check) X(gtk_main) X(gtk_main_quit) X(gtk_events_pending) X(gtk_main_iteration)        \
  X(gtk_window_new) X(gtk_window_set_title) X(gtk_window_set_default_size) X(gtk_window_set_position) \
  X(gtk_container_add) X(gtk_container_set_border_width) X(gtk_box_new) X(gtk_box_pack_start)       \
  X(gtk_box_set_spacing) X(gtk_label_new) X(gtk_label_set_line_wrap) X(gtk_label_set_xalign)        \
  X(gtk_label_set_text) X(gtk_label_set_markup) X(gtk_entry_new) X(gtk_entry_set_text)              \
  X(gtk_entry_get_text) X(gtk_editable_set_editable) X(gtk_button_new_with_label)                   \
  X(gtk_button_set_label) X(gtk_widget_set_sensitive) X(gtk_widget_show_all) X(gtk_widget_hide)     \
  X(gtk_widget_destroy) X(gtk_widget_set_hexpand) X(gtk_widget_set_vexpand)                          \
  X(gtk_progress_bar_new) X(gtk_progress_bar_set_fraction) X(gtk_scrolled_window_new)               \
  X(gtk_scrolled_window_set_policy) X(gtk_text_view_new) X(gtk_text_view_set_editable)              \
  X(gtk_text_view_set_monospace) X(gtk_text_view_set_cursor_visible) X(gtk_text_view_get_buffer)    \
  X(gtk_text_view_scroll_mark_onscreen) X(gtk_text_buffer_get_end_iter) X(gtk_text_buffer_insert)   \
  X(gtk_text_buffer_place_cursor) X(gtk_text_buffer_get_insert) X(gtk_file_chooser_dialog_new)      \
  X(gtk_file_chooser_get_filename) X(gtk_file_chooser_set_filename) X(gtk_dialog_run)               \
  X(gtk_message_dialog_new) X(gtk_button_box_new) X(gtk_button_box_set_layout) X(g_signal_connect_data) \
  X(g_idle_add) X(g_free) X(gtk_show_uri_on_window) X(g_error_free)
#define GTAV_GTK_MEMBER(name) decltype(&::name) name = nullptr;
  GTAV_GTK_FUNCTIONS(GTAV_GTK_MEMBER)
#undef GTAV_GTK_MEMBER

  bool Load() {
    void* h = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) return false;
    bool ok = true;
#define GTAV_GTK_LOAD(name) ok = ok && (name = reinterpret_cast<decltype(name)>(dlsym(h, #name)));
    GTAV_GTK_FUNCTIONS(GTAV_GTK_LOAD)
#undef GTAV_GTK_LOAD
    return ok;
  }
};
Gtk g;

// Runs f on the GTK main loop (from the install thread).
void OnMain(std::function<void()> f) {
  g.g_idle_add(
      [](gpointer p) -> gboolean {
        auto* fn = static_cast<std::function<void()>*>(p);
        (*fn)();
        delete fn;
        return FALSE;
      },
      new std::function<void()>(std::move(f)));
}

GtkWidget* g_window = nullptr;

bool Question(const std::string& text) {
  GtkWidget* d = g.gtk_message_dialog_new(GTK_WINDOW(g_window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_YES_NO, "%s", text.c_str());
  g.gtk_window_set_title(GTK_WINDOW(d), "GTA V recomp setup");
  bool yes = g.gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES;
  g.gtk_widget_destroy(d);
  return yes;
}

void ErrorBox(const std::string& text) {
  GtkWidget* d = g.gtk_message_dialog_new(g_window ? GTK_WINDOW(g_window) : nullptr, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", text.c_str());
  g.gtk_window_set_title(GTK_WINDOW(d), "GTA V recomp");
  g.gtk_dialog_run(GTK_DIALOG(d));
  g.gtk_widget_destroy(d);
}

template <class F>
void Connect(gpointer widget, const char* signal, F handler, gpointer data = nullptr) {
  g.g_signal_connect_data(widget, signal, G_CALLBACK(handler), data, nullptr, GConnectFlags(0));
}

class SetupWindow : public InstallUi {
 public:
  explicit SetupWindow(std::vector<std::wstring> game_args) : game_args_(std::move(game_args)) {}

  void Build() {
    g_window = window_ = g.gtk_window_new(GTK_WINDOW_TOPLEVEL);
    const auto version = std::string("GTA V Recomp  |  Launcher v") + kLauncherVersion;
    g.gtk_window_set_title(GTK_WINDOW(window_), version.c_str());
    g.gtk_window_set_default_size(GTK_WINDOW(window_), 760, 630);
    g.gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
    g.gtk_container_set_border_width(GTK_CONTAINER(window_), 16);
    Connect(window_, "delete-event", +[](GtkWidget*, GdkEvent*, gpointer self) -> gboolean {
      return static_cast<SetupWindow*>(self)->CloseRequested() ? FALSE : TRUE;
    }, this);
    Connect(window_, "destroy", +[](GtkWidget*, gpointer) { g.gtk_main_quit(); });

    GtkWidget* box = g.gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    g.gtk_container_add(GTK_CONTAINER(window_), box);
    GtkWidget* heading = g.gtk_label_new(nullptr);
    g.gtk_label_set_markup(GTK_LABEL(heading), R"(<span size="x-large" weight="bold">Set up GTA V Recomp</span>)");
    g.gtk_label_set_xalign(GTK_LABEL(heading), 0);
    g.gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 4);
    GtkWidget* intro = g.gtk_label_new(
        "Use your own Xbox 360 discs and Title Update 27. Setup checks, copies and builds locally. "
        "Allow about 25 GB free for a first install. The update check is separate and optional.");
    g.gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    g.gtk_label_set_xalign(GTK_LABEL(intro), 0);
    g.gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 4);

    std::vector<std::string> saved = LoadInputs();
    for (int i = 0; i < 3; ++i) {
      GtkWidget* label = g.gtk_label_new(nullptr);
      std::string markup = "<b>" + std::to_string(i + 1) + ". " + kRowLabels[i] + "</b>  <span alpha=\"60%\">" + kRowHints[i] + "</span>";
      g.gtk_label_set_markup(GTK_LABEL(label), Escape(markup).c_str());
      g.gtk_label_set_xalign(GTK_LABEL(label), 0);
      g.gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, i ? 6 : 10);
      GtkWidget* row = g.gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      paths_[i] = g.gtk_entry_new();
      g.gtk_entry_set_text(GTK_ENTRY(paths_[i]), saved[i].c_str());
      g.gtk_widget_set_hexpand(paths_[i], TRUE);
      g.gtk_box_pack_start(GTK_BOX(row), paths_[i], TRUE, TRUE, 0);
      file_[i] = g.gtk_button_new_with_label("Choose file...");
      folder_[i] = g.gtk_button_new_with_label("Folder...");
      Connect(file_[i], "clicked", +[](GtkButton* b, gpointer self) { static_cast<SetupWindow*>(self)->Choose(b, false); }, this);
      Connect(folder_[i], "clicked", +[](GtkButton* b, gpointer self) { static_cast<SetupWindow*>(self)->Choose(b, true); }, this);
      g.gtk_box_pack_start(GTK_BOX(row), file_[i], FALSE, FALSE, 0);
      g.gtk_box_pack_start(GTK_BOX(row), folder_[i], FALSE, FALSE, 0);
      g.gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    }

    stage_ = g.gtk_label_new("");
    g.gtk_label_set_xalign(GTK_LABEL(stage_), 0);
    g.gtk_label_set_line_wrap(GTK_LABEL(stage_), TRUE);
    g.gtk_box_pack_start(GTK_BOX(box), stage_, FALSE, FALSE, 10);
    progress_ = g.gtk_progress_bar_new();
    g.gtk_box_pack_start(GTK_BOX(box), progress_, FALSE, FALSE, 0);

    GtkWidget* scroll = g.gtk_scrolled_window_new(nullptr, nullptr);
    g.gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    g.gtk_widget_set_vexpand(scroll, TRUE);
    log_ = g.gtk_text_view_new();
    g.gtk_text_view_set_editable(GTK_TEXT_VIEW(log_), FALSE);
    g.gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_), FALSE);
    g.gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_), TRUE);
    g.gtk_container_add(GTK_CONTAINER(scroll), log_);
    g.gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 6);

    GtkWidget* buttons = g.gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    g.gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    g.gtk_box_set_spacing(GTK_BOX(buttons), 8);
    install_ = g.gtk_button_new_with_label("Install & build");
    rebuild_ = g.gtk_button_new_with_label("Recompile");
    cancel_ = g.gtk_button_new_with_label("Cancel");
    play_ = g.gtk_button_new_with_label("Play");
    Connect(install_, "clicked", +[](GtkButton*, gpointer self) { static_cast<SetupWindow*>(self)->InstallClicked(false); }, this);
    Connect(rebuild_, "clicked", +[](GtkButton*, gpointer self) { static_cast<SetupWindow*>(self)->InstallClicked(true); }, this);
    Connect(cancel_, "clicked", +[](GtkButton*, gpointer self) { static_cast<SetupWindow*>(self)->Cancel(); }, this);
    Connect(play_, "clicked", +[](GtkButton*, gpointer self) { static_cast<SetupWindow*>(self)->Play(); }, this);
    for (auto* button : {install_, rebuild_, cancel_, play_}) g.gtk_container_add(GTK_CONTAINER(buttons), button);
    g.gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    GtkWidget* tools = g.gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    g.gtk_button_box_set_layout(GTK_BUTTON_BOX(tools), GTK_BUTTONBOX_END);
    g.gtk_box_set_spacing(GTK_BOX(tools), 8);
    update_ = g.gtk_button_new_with_label("Check for updates");
    iphone_ = g.gtk_button_new_with_label("Build for iPhone");
    android_ = g.gtk_button_new_with_label("Build for Android");
    Connect(update_, "clicked", +[](GtkButton*, gpointer self) { static_cast<SetupWindow*>(self)->UpdateClicked(); }, this);
    Connect(iphone_, "clicked", +[](GtkButton*, gpointer self) { static_cast<SetupWindow*>(self)->PhoneClicked(false); }, this);
    Connect(android_, "clicked", +[](GtkButton*, gpointer self) { static_cast<SetupWindow*>(self)->PhoneClicked(true); }, this);
    for (auto* button : {update_, iphone_, android_}) g.gtk_container_add(GTK_CONTAINER(tools), button);
    g.gtk_box_pack_start(GTK_BOX(box), tools, FALSE, FALSE, 0);

    SetBusy(false);
    g.gtk_widget_show_all(window_);
  }

  // InstallUi: called on the install thread.
  void Stage(const std::wstring& t) override {
    std::string s = Narrow(t);
    OnMain([this, s] { g.gtk_label_set_text(GTK_LABEL(stage_), s.c_str()); });
  }
  void Log(const std::wstring& t) override {
    std::string s = Narrow(t);
    OnMain([this, s] { Append(s); });
  }
  void Progress(double f) override {
    int v = int(f * 1000);
    if (v == last_) return;
    last_ = v;
    OnMain([this, f] { g.gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), f); });
  }
  bool Cancelled() override { return cancelled_ || g_cancelled; }
  bool Ask(const std::wstring& q) override {
    std::string s = Narrow(q);
    std::mutex m;
    std::condition_variable cv;
    int answer = -1;
    OnMain([&] {
      bool yes = !Cancelled() && Question(s);
      std::lock_guard lock(m);
      answer = yes;
      cv.notify_one();
    });
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return answer >= 0; });
    return answer == 1;
  }

 private:
  // Markup: the labels and hints are ours except for '&', which must be an entity.
  static std::string Escape(const std::string& s) {
    std::string out;
    for (char c : s) out += c == '&' ? std::string("&amp;") : std::string(1, c);
    return out;
  }

  void Append(const std::string& s) {
    GtkTextBuffer* buf = g.gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_));
    GtkTextIter end;
    g.gtk_text_buffer_get_end_iter(buf, &end);
    std::string line = s + "\n";
    g.gtk_text_buffer_insert(buf, &end, line.c_str(), -1);
    g.gtk_text_buffer_get_end_iter(buf, &end);
    g.gtk_text_buffer_place_cursor(buf, &end);
    g.gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_), g.gtk_text_buffer_get_insert(buf));
  }

  void SetBusy(bool busy) {
    busy_ = busy;
    for (int i = 0; i < 3; ++i) {
      g.gtk_widget_set_sensitive(file_[i], !busy);
      g.gtk_widget_set_sensitive(folder_[i], !busy);
      g.gtk_editable_set_editable(GTK_EDITABLE(paths_[i]), !busy);
    }
    const auto status = busy ? InstallStatus::NeedsSources : GetInstallStatus(g_root, g_root);
    const bool ready = status == InstallStatus::Ready;
    g.gtk_widget_set_sensitive(install_, !busy);
    g.gtk_widget_set_sensitive(rebuild_, !busy && status != InstallStatus::NeedsSources);
    g.gtk_widget_set_sensitive(play_, !busy && ready);
    g.gtk_widget_set_sensitive(iphone_, !busy && ready);
    g.gtk_widget_set_sensitive(android_, !busy && ready);
    g.gtk_widget_set_sensitive(cancel_, busy);
    g.gtk_widget_set_sensitive(update_, !busy);
    if (!busy) g.gtk_label_set_text(GTK_LABEL(stage_), Narrow(InstallStatusText(status)).c_str());
  }

  void Cancel() {
    cancelled_ = true;
    g.gtk_label_set_text(GTK_LABEL(stage_), "Stopping the current task...");
  }

  bool EndTask() {
    SetBusy(false);
    if (!quit_when_stopped_) return false;
    g.gtk_widget_destroy(window_);
    return true;
  }

  void UpdateClicked() {
    if (busy_) return;
    cancelled_ = false;
    SetBusy(true);
    g.gtk_label_set_text(GTK_LABEL(stage_), "Checking GitHub for project updates...");
    std::thread([this] {
      auto result = CheckForUpdate();
      OnMain([this, result = std::move(result)] {
        if (EndTask() || Cancelled()) return;
        Append(result.message);
        if (result.url.empty() && result.status != UpdateStatus::Error) {
          g.gtk_label_set_text(GTK_LABEL(stage_), result.message.c_str());
          return;
        }
        if (!Question(result.message + "\n\nOpen GitHub Releases in your browser?")) return;
        const auto url = result.url.empty() ? std::string(kReleasePage) : result.url;
        GError* error = nullptr;
        if (!g.gtk_show_uri_on_window(GTK_WINDOW(window_), url.c_str(), GDK_CURRENT_TIME, &error)) {
          ErrorBox("Could not open your browser. Visit: " + url);
          if (error) g.g_error_free(error);
        }
      });
    }).detach();
  }

  void PhoneClicked(bool android) {
    if (busy_) return;
    cancelled_ = false;
    last_ = -1;
    g.gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), 0);
    SetBusy(true);
    std::thread([this, android] {
      std::wstring err;
      bool ok = android ? RunAndroidBuild(g_root, g_root, *this, &err) : RunIphoneBuild(g_root, g_root, *this, &err);
      std::string e = Narrow(err);
      OnMain([this, ok, e, android] { PhoneFinished(ok, e, android); });
    }).detach();
  }

  void PhoneFinished(bool ok, const std::string& error, bool android) {
    if (EndTask()) return;
    if (ok) {
      g.gtk_label_set_text(GTK_LABEL(stage_), android ? "Android pack ready in android/." : "iPhone build ready in iphone/.");
      g.gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), 1.0);
      return;
    }
    if (cancelled_) {
      g.gtk_label_set_text(GTK_LABEL(stage_), "Cancelled.");
      return;
    }
    g.gtk_label_set_text(GTK_LABEL(stage_), android ? "Android build stopped." : "iPhone build stopped.");
    ErrorBox(error + "\n\n(log: " + (g_root / "data" / "logs" / (android ? "android.log" : "iphone.log")).string() + ")");
  }

  void Choose(GtkButton* button, bool folder) {
    int i = 0;
    while (i < 3 && GTK_WIDGET(button) != (folder ? folder_[i] : file_[i])) ++i;
    if (i == 3) return;
    std::string title = std::string(kRowLabels[i]) + ": " + kRowHints[i];
    GtkWidget* d = g.gtk_file_chooser_dialog_new(
        title.c_str(), GTK_WINDOW(window_),
        folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
        GTK_RESPONSE_CANCEL, folder ? "_Select" : "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    std::string current = g.gtk_entry_get_text(GTK_ENTRY(paths_[i]));
    std::error_code ec;
    if (!current.empty() && fs::exists(current, ec)) g.gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d), current.c_str());
    if (g.gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
      if (gchar* name = g.gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d))) {
        g.gtk_entry_set_text(GTK_ENTRY(paths_[i]), name);
        (g.g_free)(name);  // parenthesised: g_free is also a function-like macro in newer GLib
      }
    }
    g.gtk_widget_destroy(d);
  }

  void InstallClicked(bool rebuild) {
    if (busy_) return;
    InstallInputs in;
    if (!rebuild) {
      std::vector<std::string> paths(3);
      for (int i = 0; i < 3; ++i) {
        paths[i] = Trim(g.gtk_entry_get_text(GTK_ENTRY(paths_[i])));
        std::error_code ec;
        auto path = fs::absolute(paths[i], ec);
        if (paths[i].empty() || ec || (!fs::is_regular_file(path, ec) && !fs::is_directory(path, ec))) {
          ErrorBox(std::string(kRowLabels[i]) + ": choose an existing file or folder.");
          return;
        }
        paths[i] = path.string();
      }
      SaveInputs(paths);
      in = {Widen(paths[0]), Widen(paths[1]), Widen(paths[2])};
    }
    cancelled_ = false;
    last_ = -1;
    SetBusy(true);
    std::thread([this, in, rebuild] {
      std::wstring err;
      bool ok = rebuild ? RunRebuild(g_root, g_root, *this, &err) : RunInstall(g_root, in, *this, &err);
      if (ok) AddDesktopEntry(this);
      std::string e = Narrow(err);
      OnMain([this, ok, e, rebuild] { Finished(ok, e, rebuild); });
    }).detach();
  }

  void Finished(bool ok, const std::string& error, bool rebuild) {
    if (EndTask()) return;
    if (ok) {
      g.gtk_label_set_text(GTK_LABEL(stage_), "Game build ready. Press Play.");
      g.gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), 1.0);
      return;
    }
    if (Cancelled()) {
      g.gtk_label_set_text(GTK_LABEL(stage_), rebuild ? "Recompile cancelled." :
          "Cancelled. Retry setup to resume copying, or recompile if all files are present.");
      return;
    }
    g.gtk_label_set_text(GTK_LABEL(stage_), "Setup stopped. See the log below.");
    ErrorBox(error + "\n\n(log: " + (g_root / "data/logs/install.log").string() + ")");
  }

  bool CloseRequested() {
    if (!busy_) return true;
    if (!quit_when_stopped_ && Question("Stop the current task and close when it finishes?")) {
      if (!busy_) return true;
      Cancel();
      quit_when_stopped_ = true;
    }
    return false;
  }

  void Play() {
    if (busy_ || GetInstallStatus(g_root, g_root) != InstallStatus::Ready) {
      if (!busy_) SetBusy(false);
      return;
    }
    // The launcher process becomes the game (exec): take the window down first.
    g.gtk_widget_hide(window_);
    while (g.gtk_events_pending()) g.gtk_main_iteration();
    std::wstring error;
    auto args = game_args_;
    AppendCampaignArgs(args);
    if (LaunchGame(g_root, &error, args)) return;
    g.gtk_widget_show_all(window_);
    ErrorBox(Narrow(error));
  }

  std::vector<std::wstring> game_args_;
  GtkWidget* window_ = nullptr;
  GtkWidget* paths_[3] = {};
  GtkWidget* file_[3] = {};
  GtkWidget* folder_[3] = {};
  GtkWidget* stage_ = nullptr;
  GtkWidget* progress_ = nullptr;
  GtkWidget* log_ = nullptr;
  GtkWidget* install_ = nullptr;
  GtkWidget* play_ = nullptr;
  GtkWidget* iphone_ = nullptr;
  GtkWidget* android_ = nullptr;
  GtkWidget* rebuild_ = nullptr;
  GtkWidget* cancel_ = nullptr;
  GtkWidget* update_ = nullptr;
  bool busy_ = false;
  bool quit_when_stopped_ = false;
  std::atomic<bool> cancelled_{false};
  int last_ = -1;
};

bool LoadGui() {
  static int loaded = -1;
  if (loaded < 0) loaded = HaveDisplay() && g.Load() && g.gtk_init_check(nullptr, nullptr);
  return loaded == 1;
}

// The setup window until it is closed (or the game starts). false: no GTK / display.
bool RunSetupWindow(const std::vector<std::wstring>& game_args, int* code) {
  if (!LoadGui()) return false;
  SetupWindow w(game_args);
  w.Build();
  g.gtk_main();
  *code = 0;
  return true;
}

// Errors when there may be no terminal to see them (started from the desktop).
void ShowError(const std::string& text) {
  fprintf(stderr, "%s\n", text.c_str());
  if (LoadGui()) ErrorBox(text);
}
#else
bool RunSetupWindow(const std::vector<std::wstring>&, int*) { return false; }
void ShowError(const std::string& text) { fprintf(stderr, "%s\n", text.c_str()); }
#endif

}  // namespace

int main(int argc, char** argv) {
  std::error_code ec;
  g_root = fs::read_symlink("/proc/self/exe", ec).parent_path();
  if (ec) g_root = fs::absolute(argv[0]).parent_path();
  std::signal(SIGINT, [](int) { g_cancelled = 1; });

  bool setup = false, unattended = false, cli = false, iphone = false, android = false;
  bool rebuild = false, update = false;
  DesktopPaths folders;
  std::vector<std::string> forwarded{argv[0]}, paths;
  std::vector<std::wstring> game_args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--portable") { folders.portable = true; continue; }
    if (a == "--home" || a == "--migrate-from") {
      if (++i == argc || !fs::path(argv[i]).is_absolute()) {
        fprintf(stderr, "%s requires an absolute path.\n", a.c_str());
        return 2;
      }
      auto& path = a == "--home" ? folders.home : folders.migrate_from;
      path = fs::path(argv[i]).lexically_normal();
      if (path.filename().empty() && path.has_relative_path()) path = path.parent_path();
      continue;
    }
    forwarded.push_back(a);
    if (a == "--") {
      for (++i; i < argc; ++i) {
        game_args.push_back(Widen(argv[i]));
        forwarded.emplace_back(argv[i]);
      }
      break;
    }
    if (a == "--play") {
      // Normal desktop start already launches directly when installed.
    } else if (a == "--setup") {
      setup = true;
    } else if (a == "--unattended") {
      unattended = true;
    } else if (a == "--cli") {
      cli = true;
    } else if (a == "--iphone") {
      iphone = true;
    } else if (a == "--android") {
      android = true;
    } else if (a == "--rebuild") {
      rebuild = true;
    } else if (a == "--check-updates") {
      update = true;
    } else if (a == "--version") {
      puts(kLauncherVersion);
      return 0;
    } else if (a == "--help" || a == "-h") {
      printf("usage: %s [--setup [DISC1 DISC2 TU]] [--cli] [--unattended] [--rebuild] [--check-updates] [--version] [--iphone] [--android]\n       [--portable | --home DIR [--migrate-from OLD_DIR]] [-- GAME_ARGS...]\n", argv[0]);
      return 0;
    } else if (a.rfind("--", 0) == 0) {
      fprintf(stderr, "unknown option %s (game options go after --)\n", a.c_str());
      return 2;
    } else {
      paths.push_back(a);
    }
  }
  if (update) return CheckUpdates();
  fs::path home;
  std::wstring folder_error;
  if (!OpenDesktopHome(g_root, folders, &home, &folder_error, [] { return g_cancelled != 0; })) {
    if (cli || unattended || rebuild || iphone || android) fprintf(stderr, "%s\n", Narrow(folder_error).c_str());
    else ShowError(Narrow(folder_error));
    return 1;
  }
  if (!SameFolder(g_root, home)) {
    if (cli || !HaveDisplay()) printf("Game folder: %s\n", home.c_str());
    fflush(stdout);
    forwarded.front() = (home / kDesktopLauncher).string();
    forwarded.insert(forwarded.begin() + 1, "--portable");
    std::vector<char*> child_args;
    for (auto& arg : forwarded) child_args.push_back(arg.data());
    child_args.push_back(nullptr);
    // Keep the original cwd so --setup relative disc paths continue to work.
    execv(child_args.front(), child_args.data());
    ShowError("Could not open the installed launcher at " + forwarded.front());
    return 1;
  }
  if (rebuild) return Rebuild();
  if (unattended) return Unattended();
  if (iphone) {
    // The iPhone build (install.h RunIphoneBuild) from the installed game.
    std::wstring error;
    TermUi ui;
    if (!Preflight(g_root, &error) || !RunIphoneBuild(g_root, g_root, ui, &error)) {
      fprintf(stderr, "\niPhone build stopped: %s\n", Narrow(error).c_str());
      return 1;
    }
    printf("\nDone: %s\nInstall it on the phone with Sideloadly or AltStore, then open GTA V there: it\n"
           "installs the game files from your discs itself.\n",
           (g_root / "iphone" / "GTAV.ipa").c_str());
    return 0;
  }
  if (android) {
    // The Android build (install.h RunAndroidBuild) from the installed game.
    std::wstring error;
    TermUi ui;
    if (!Preflight(g_root, &error) || !RunAndroidBuild(g_root, g_root, ui, &error)) {
      fprintf(stderr, "\nAndroid build stopped: %s\n", Narrow(error).c_str());
      return 1;
    }
    printf("\nDone: %s\nCopy it and your disc files to the phone (Download folder), install the GTA V\n"
           "APK, open it and press Install: it installs the game files from your discs itself.\n",
           (g_root / "android" / "gtav-android-pack.zip").c_str());
    return 0;
  }
  if (!paths.empty()) setup = true;
  const bool window = !cli && paths.empty();

  std::wstring error;
  if (!setup && GetInstallStatus(g_root, g_root) == InstallStatus::Ready) {
    if (SameFolder(g_root, DefaultGameHome())) AddDesktopEntry(nullptr);
    {
      auto args = game_args;
      AppendCampaignArgs(args);
      if (LaunchGame(g_root, &error, args)) return 0;
    }
    if (!window) {
      fprintf(stderr, "%s\n", Narrow(error).c_str());
      return 1;
    }
    ShowError(Narrow(error));
  }
  int code = 0;
  if (window && RunSetupWindow(game_args, &code)) return code;
  if (!Preflight(g_root, &error)) {
    fprintf(stderr, "%s\n", Narrow(error).c_str());
    return 1;
  }
  return Setup(paths, game_args);
}
