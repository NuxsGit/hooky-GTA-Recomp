#include "courses.h"

#include <fstream>
#include <locale>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace gtav::sandbox {
namespace fs = std::filesystem;
namespace {
constexpr auto kMagic = "GTAV-COURSE-2", kLegacyMagic = "GTAV-COURSE-1";
constexpr size_t kMaxFileBytes = 4096;
fs::path CoursePath(const fs::path& folder, std::string_view name) { return folder / ("course-" + std::string(name) + ".gtcourse"); }
}

bool SaveCourse(const fs::path& folder, const Course& course) {
  if (!course.Valid()) return false;
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec) return false;
  const auto path = CoursePath(folder, course.name);
  auto tmp = path;
  tmp += ".new";
  // Remove a scratch file left by an interrupted save without following it.
  if (fs::is_directory(fs::symlink_status(tmp, ec))) return false;
  fs::remove(tmp, ec);
  if (ec) return false;
  std::ofstream out(tmp, std::ios::binary);
  out.imbue(std::locale::classic());
  out << kMagic << '\n' << course.name << '\n' << course.points.size() << '\n';
  for (auto p : course.points) out << p.x << ' ' << p.y << ' ' << p.z << '\n';
  out << unsigned(course.game) << ' ' << course.props.size() << '\n';
  for (const auto& p : course.props)
    out << unsigned(p.kind) << ' ' << p.heading << ' ' << p.position.x << ' ' << p.position.y << ' ' << p.position.z << '\n';
  out.close();
  if (out) {
#ifdef _WIN32
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
    fs::rename(tmp, path, ec);
    if (!ec) return true;
#endif
  }
  fs::remove(tmp, ec);
  return false;
}

std::optional<Course> LoadCourse(const fs::path& folder, std::string_view name) {
  if (!ValidName(name, 32)) return std::nullopt;
  const auto path = CoursePath(folder, name);
  std::error_code ec;
  if (!fs::is_regular_file(fs::symlink_status(path, ec)) || fs::file_size(path, ec) > kMaxFileBytes || ec)
    return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  std::string text(kMaxFileBytes + 1, '\0');
  in.read(text.data(), std::streamsize(text.size()));
  if (in.bad() || size_t(in.gcount()) > kMaxFileBytes) return std::nullopt;
  text.resize(size_t(in.gcount()));
  std::istringstream lines(text);
  lines.imbue(std::locale::classic());
  std::string magic;
  Course course;
  unsigned count;
  if (!std::getline(lines, magic) || (magic != kMagic && magic != kLegacyMagic) || !std::getline(lines, course.name) ||
      course.name != name || !(lines >> count) || count > kMaxCheckpoints) return std::nullopt;
  for (unsigned i = 0; i < count; ++i) {
    Point p;
    if (!(lines >> p.x >> p.y >> p.z)) return std::nullopt;
    course.points.push_back(p);
  }
  if (magic == kMagic) {
    unsigned game, props;
    if (!(lines >> game >> props) || game >= unsigned(Minigame::Count) || props > kMaxProps) return std::nullopt;
    course.game = Minigame(game);
    for (unsigned i = 0; i < props; ++i) {
      unsigned kind, heading;
      Prop p;
      if (!(lines >> kind >> heading >> p.position.x >> p.position.y >> p.position.z) ||
          kind >= unsigned(PropKind::Count) || heading >= kFullTurn) return std::nullopt;
      p.kind = PropKind(kind);
      p.heading = uint16_t(heading);
      course.props.push_back(p);
    }
  }
  lines >> std::ws;
  return lines.eof() && course.Valid() ? std::optional(std::move(course)) : std::nullopt;
}
}  // namespace gtav::sandbox
