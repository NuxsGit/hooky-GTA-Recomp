#include "ConfigStorage.h"

#include <charconv>
#include <fstream>
#include <map>
#include <variant>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gtav::workshop {
namespace fs = std::filesystem;
namespace {
constexpr size_t kMaxBytes = 128 * 1024;
constexpr uint8_t kAll = 15;
constexpr std::pair<Document, const char*> kDocs[] = {
    {Document::Permissions, "permissions.json"}, {Document::Bans, "bans.json"},
    {Document::Favorites, "build_favorites.json"}, {Document::Presets, "presets.json"},
};
constexpr const char* kRuleKeys[] = {"pvp", "fall_damage", "traffic", "peds", "police"};

// Closed schema: objects, arrays, UTF-8 strings, booleans and exact signed integers. There
// are no floats, exponent notation, nulls or arbitrary values in these documents. Reject
// duplicate keys and bound recursion, object sizes and file bytes before decoding settings.
struct Value {
  using Object = std::map<std::string, Value, std::less<>>;
  using Array = std::vector<Value>;
  std::variant<int64_t, bool, std::string, Object, Array> v;
  const Value* Get(std::string_view key) const {
    const auto* obj = As<Object>();
    if (!obj) return nullptr;
    const auto it = obj->find(key);
    return it != obj->end() ? &it->second : nullptr;
  }
  template<class T> const T* As() const { return std::get_if<T>(&v); }
};
class Parser {
 public:
  explicit Parser(std::string_view input) : input_(input) {}
  std::optional<Value> Read() {
    auto value = At(0);
    Space();
    return value && input_.empty() ? value : std::nullopt;
  }
 private:
  void Space() { while (!input_.empty() && std::string_view(" \t\r\n").find(input_.front()) != input_.npos) input_.remove_prefix(1); }
  char Peek() { Space(); return input_.empty() ? '\0' : input_.front(); }
  bool Take(char c) { if (Peek() != c || input_.empty()) return false; input_.remove_prefix(1); return true; }
  bool Word(std::string_view s) { if (!input_.starts_with(s)) return false; input_.remove_prefix(s.size()); return true; }
  std::optional<uint32_t> HexWord() {
    if (input_.size() < 4) return {};
    uint32_t val = 0;
    for (unsigned i = 0; i < 4; ++i) {
      char c = input_[i];
      if (c >= '0' && c <= '9') val = val * 16 + (c - '0');
      else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
      else return {};
    }
    input_.remove_prefix(4);
    return val;
  }
  std::optional<std::string> String() {
    if (!Take('"')) return {};
    std::string s;
    while (!input_.empty() && s.size() <= 160) {
      const unsigned char c = input_.front(); input_.remove_prefix(1);
      if (c == '"') return ValidText(s, 160) ? std::optional(std::move(s)) : std::nullopt;
      if (c < 32) return {};
      if (c != '\\') { s += char(c); continue; }
      if (input_.empty()) return {};
      const char esc = input_.front(); input_.remove_prefix(1);
      if (esc == '"' || esc == '\\' || esc == '/') { s += esc; continue; }
      if (esc != 'u') return {};  // control code escapes cannot occur in this single-line schema
      auto val = HexWord();
      if (!val) return {};
      if (*val >= 0xd800 && *val <= 0xdbff) {
        if (!Word("\\u")) return {};
        auto low = HexWord();
        if (!low || *low < 0xdc00 || *low > 0xdfff) return {};
        *val = 0x10000 + ((*val - 0xd800) << 10) + (*low - 0xdc00);
      }
      if (*val < 0x80) s += char(*val);
      else if (*val < 0x800) {
        s += char(0xc0 | (*val >> 6)); s += char(0x80 | (*val & 63));
      } else {
        if (*val >= 0x10000) { s += char(0xf0 | (*val >> 18)); s += char(0x80 | ((*val >> 12) & 63)); }
        else s += char(0xe0 | (*val >> 12));
        s += char(0x80 | ((*val >> 6) & 63)); s += char(0x80 | (*val & 63));
      }
    }
    return {};
  }
  std::optional<Value> At(unsigned depth) {
    if (depth > 8 || ++nodes_ > 5500) return {};
    const char ch = Peek();
    if (ch == '{') {
      Take('{'); Value::Object o;
      if (!Take('}')) {
        do {
          auto key = String();
          if (!key || o.size() >= 16 || !Take(':')) return {};
          auto value = At(depth + 1);
          if (!value || !o.emplace(std::move(*key), std::move(*value)).second) return {};
        } while (Take(','));
        if (!Take('}')) return {};
      }
      return Value{std::move(o)};
    }
    if (ch == '[') {
      Take('['); Value::Array a;
      if (!Take(']')) {
        do {
          auto value = At(depth + 1);
          if (!value || a.size() >= 128) return {};
          a.push_back(std::move(*value));
        } while (Take(','));
        if (!Take(']')) return {};
      }
      return Value{std::move(a)};
    }
    if (ch == '"') { if (auto s = String()) return Value{std::move(*s)}; return {}; }
    if (Word("true")) return Value{true};
    if (Word("false")) return Value{false};
    if (ch != '-' && (ch < '0' || ch > '9')) return {};
    int64_t value;
    const char* first = input_.data();
    const auto [end, error] = std::from_chars(first, first + input_.size(), value);
    const char* digits = first + (ch == '-');
    if (error != std::errc{} || (*digits == '0' && end - digits > 1)) return {};
    input_.remove_prefix(end - first);
    return Value{value};
  }
  std::string_view input_;
  size_t nodes_ = 0;
};

template<class T> const T* Field(const Value& v, std::string_view name) {
  const auto* field = v.Get(name);
  return field ? field->As<T>() : nullptr;
}
bool ExactFields(const Value& value, size_t fields) {
  const auto* obj = value.As<Value::Object>();
  return obj && obj->size() == fields;
}
bool UInt(const Value& v, uint32_t& out) {
  const auto* n = v.As<int64_t>();
  if (!n || *n < 0 || *n > UINT32_MAX) return false;
  out = uint32_t(*n);
  return true;
}
bool Decode(Document doc, const Value& value, Config& out) {
  const auto* version = Field<int64_t>(value, "version");
  if (!version || *version != 1) return false;
  switch (doc) {
    case Document::Permissions: {
      const auto* admins = Field<Value::Array>(value, "admins");
      const auto* allowed = Field<bool>(value, "players_build");
      const auto* limit = Field<int64_t>(value, "prop_limit");
      const auto* mode = Field<std::string>(value, "map_mode");
      const auto* rules = value.Get("rules");
      if (!ExactFields(value, 6) || !admins || !allowed || !limit || *limit < 1 || *limit > 128 || !mode ||
          (*mode != "Creative" && *mode != "Survival") || !rules || !ExactFields(*rules, std::size(kRuleKeys))) return false;
      Permissions settings{{}, *allowed, unsigned(*limit)};
      for (const auto& a : *admins) {
        const auto* id = a.As<std::string>();
        if (!id) return false;
        settings.admins.push_back(*id);
      }
      MapRules map{*mode == "Survival" ? Mode::Survival : Mode::Creative, 0};
      for (unsigned i = 0; i < std::size(kRuleKeys); ++i) {
        const auto* on = Field<bool>(*rules, kRuleKeys[i]);
        if (!on) return false;
        if (*on) map.enabled |= 1u << i;
      }
      if (!settings.Valid()) return false;
      out.permissions = std::move(settings);
      out.map = map;
      return true;
    }
    case Document::Bans: {
      const auto* items = Field<Value::Array>(value, "bans");
      if (!items || !ExactFields(value, 2)) return false;
      std::vector<Ban> bans;
      for (const auto& row : *items) {
        const auto* identity = Field<std::string>(row, "identity");
        const auto* name = Field<std::string>(row, "name");
        const auto* reason = Field<std::string>(row, "reason");
        const auto* utc = Field<std::string>(row, "utc");
        if (!ExactFields(row, 4) || !identity || !name || !reason || !utc) return false;
        bans.push_back({*identity, *name, *reason, *utc});
      }
      if (!PermissionManager::ValidBans(bans)) return false;
      out.bans = std::move(bans);
      return true;
    }
    case Document::Favorites: {
      const auto* items = Field<Value::Array>(value, "models");
      if (!items || !ExactFields(value, 2)) return false;
      std::vector<uint32_t> hashes;
      for (const auto& row : *items) { uint32_t h; if (!UInt(row, h)) return false; hashes.push_back(h); }
      if (!ValidFavorites(hashes)) return false;
      out.favorites = std::move(hashes);
      return true;
    }
    case Document::Presets: {
      const auto* items = Field<Value::Array>(value, "presets");
      if (!items || items->size() > BuildManager::kPresets || !ExactFields(value, 2)) return false;
      std::vector<Preset> presets;
      for (const auto& row : *items) {
        const auto* name = Field<std::string>(row, "name");
        const auto* pieces = Field<Value::Array>(row, "pieces");
        if (!ExactFields(row, 2) || !name || !pieces || pieces->size() > BuildManager::kPresetPieces) return false;
        Preset preset{*name, {}};
        for (const auto& piece : *pieces) {
          uint32_t model;
          const auto* val = piece.Get("model");
          const auto* x = Field<int64_t>(piece, "x_cm"), *y = Field<int64_t>(piece, "y_cm"), *z = Field<int64_t>(piece, "z_cm");
          const auto* yaw = Field<int64_t>(piece, "yaw_mdeg");
          const auto* locked = Field<bool>(piece, "locked");
          const auto coord = [](const int64_t* n) { return n && *n >= -10000 && *n <= 10000; };
          if (!ExactFields(piece, 6) || !val || !UInt(*val, model) || !coord(x) || !coord(y) || !coord(z) ||
              !yaw || *yaw < 0 || *yaw >= 360000 || !locked) return false;
          preset.pieces.push_back({model, {{float(*x) / 100, float(*y) / 100, float(*z) / 100}, float(*yaw) / 1000}, *locked});
        }
        if (!ValidPreset(preset) || std::any_of(presets.begin(), presets.end(), [&](const Preset& p) { return p.name == preset.name; })) return false;
        presets.push_back(std::move(preset));
      }
      out.presets = std::move(presets);
      return true;
    }
  }
  return false;
}

std::string Quote(std::string_view text) {
  std::string out(1, '"');
  for (char c : text) { if (c == '"' || c == '\\') out += '\\'; out += c; }
  return out + '"';
}
const char* Boolean(bool on) { return on ? "true" : "false"; }
std::string Encode(Document doc, const Config& config) {
  std::string out = "{\n  \"version\": 1,\n  ";
  bool comma = false;
  const auto next = [&] { if (comma) out += ", "; comma = true; };
  switch (doc) {
    case Document::Permissions: {
      if (!config.map.Valid() || !config.permissions.Valid()) return {};
      out += "\"admins\": [";
      for (const auto& admin : config.permissions.admins) { next(); out += Quote(admin); }
      out += "],\n  \"players_build\": "; out += Boolean(config.permissions.players_build);
      out += ",\n  \"prop_limit\": " + std::to_string(config.permissions.prop_limit);
      out += ",\n  \"map_mode\": " + Quote(Label(config.map.mode)) + ",\n  \"rules\": {";
      comma = false;
      for (unsigned i = 0; i < std::size(kRuleKeys); ++i) { next(); out += Quote(kRuleKeys[i]) + ": " + Boolean(config.map.Get(Rule(i))); }
      out += '}'; break;
    }
    case Document::Bans:
      if (!PermissionManager::ValidBans(config.bans)) return {};
      out += "\"bans\": [";
      for (const auto& b : config.bans) {
        next(); out += "\n    {\"identity\": " + Quote(b.identity) + ", \"name\": " + Quote(b.name) +
          ", \"reason\": " + Quote(b.reason) + ", \"utc\": " + Quote(b.utc) + "}";
      }
      out += comma ? "\n  ]" : "]"; break;
    case Document::Favorites:
      if (!ValidFavorites(config.favorites)) return {};
      out += "\"models\": [";
      for (auto h : config.favorites) { next(); out += std::to_string(h); }
      out += ']'; break;
    case Document::Presets:
      if (config.presets.size() > BuildManager::kPresets) return {};
      out += "\"presets\": [";
      for (auto i = config.presets.begin(); i != config.presets.end(); ++i) {
        if (!ValidPreset(*i) || std::any_of(config.presets.begin(), i, [&](const Preset& p) { return i->name == p.name; })) return {};
        next(); out += "\n    {\"name\": " + Quote(i->name) + ", \"pieces\": [";
        bool first = true;
        for (const auto& p : i->pieces) {
          if (!first) out += ',';
          first = false;
          auto cm = [](float n) { return std::to_string(std::lround(n * 100.f)); };
          out += "\n      {\"model\": " + std::to_string(p.model) + ", \"x_cm\": " + cm(p.relative.at.x) +
            ", \"y_cm\": " + cm(p.relative.at.y) + ", \"z_cm\": " + cm(p.relative.at.z) +
            ", \"yaw_mdeg\": " + std::to_string(std::lround(p.relative.yaw * 1000.f) % 360000) +
            ", \"locked\": " + Boolean(p.locked) + "}";
        }
        out += "\n    ]}";
      }
      out += comma ? "\n  ]" : "]"; break;
  }
  return out + "\n}\n";
}

bool PlainDirectory(const fs::path& path) {
  std::error_code ec;
  return fs::is_directory(fs::symlink_status(path, ec)) && fs::canonical(path, ec) == path && !ec;
}
bool PlainFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(fs::symlink_status(path, ec)) && fs::canonical(path, ec) == path && !ec;
}
bool Absent(const fs::path& path) {
  std::error_code ec;
  return fs::symlink_status(path, ec).type() == fs::file_type::not_found;
}
bool Write(const fs::path& path, std::string_view data) {
  if (data.empty() || data.size() > kMaxBytes || (!Absent(path) && !PlainFile(path))) return false;
  auto staging = path;
  staging += ".new";
  std::error_code ec;
  if (!Absent(staging)) {
    // Reject directories/junctions. A stale regular stage from our own interrupted write is disposable.
    if (!PlainFile(staging) || !fs::remove(staging, ec) || ec) return false;
  }
  std::ofstream stream(staging, std::ios::binary);
  stream.write(data.data(), std::streamsize(data.size()));
  stream.close();
  if (stream) {
#ifdef _WIN32
    if (MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
    fs::rename(staging, path, ec);
    if (!ec) return true;
#endif
  }
  fs::remove(staging, ec);
  return false;
}
}  // namespace

ConfigStorage::ConfigStorage(fs::path profile) {
  std::error_code ec;
  const auto parent = fs::canonical(profile, ec);
  if (!ec && parent == profile && PlainDirectory(parent)) root_ = parent / "config";
}
Config ConfigStorage::Load() {
  Config out;
  unreadable_ = missing_ = 0;
  std::error_code ec;
  if (!root_.empty() && Absent(root_)) fs::create_directory(root_, ec);
  if (root_.empty() || ec || !PlainDirectory(root_)) { unreadable_ = kAll; return out; }
  for (const auto& [doc, name] : kDocs) {
    const auto file = root_ / name;
    if (Absent(file)) { missing_ |= uint8_t(doc); continue; }
    if (!PlainFile(file)) { unreadable_ |= uint8_t(doc); continue; }
    std::ifstream stream(file, std::ios::binary);
    std::string raw(kMaxBytes + 1, '\0');
    stream.read(raw.data(), std::streamsize(raw.size()));
    if (!stream.eof() || stream.bad()) { unreadable_ |= uint8_t(doc); continue; }
    raw.resize(size_t(stream.gcount()));
    if (raw.starts_with("\xef\xbb\xbf")) raw.erase(0, 3);
    const auto value = Parser(raw).Read();
    if (!value || !Decode(doc, *value, out)) unreadable_ |= uint8_t(doc);
  }
  // If ban data is unreadable, do not install persistent Admin grants from a separate file.
  if (unreadable_ & uint8_t(Document::Bans)) {
    out.permissions.admins.clear();
    out.permissions.players_build = false;
    unreadable_ |= uint8_t(Document::Permissions);
  }
  return out;
}
uint8_t ConfigStorage::Save(const Config& cfg, uint8_t documents) const {
  if (!PlainDirectory(root_)) return documents;
  uint8_t failed = 0;
  for (const auto& [doc, name] : kDocs) {
    if ((documents & uint8_t(doc)) && ((unreadable_ & uint8_t(doc)) || !Write(root_ / name, Encode(doc, cfg))))
      failed |= uint8_t(doc);
  }
  return failed;
}
}  // namespace gtav::workshop
