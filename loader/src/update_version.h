#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace loader {

enum class UpdateChannel { Stable, Nightly };

struct BuildVersion {
  std::array<unsigned, 3> base{};
  uint64_t nightly = 0;
  UpdateChannel Channel() const { return nightly ? UpdateChannel::Nightly : UpdateChannel::Stable; }
  bool operator==(const BuildVersion&) const = default;
};

inline std::optional<BuildVersion> ParseBuildVersion(std::string_view text) {
  BuildVersion parsed;
  if (text.size() > 64) return std::nullopt;
  auto number = [](std::string_view part, auto& out) {
    if (part.empty() || (part.size() > 1 && part.front() == '0')) return false;
    const auto end = part.data() + part.size();
    const auto [ptr, ec] = std::from_chars(part.data(), end, out);
    return ec == std::errc{} && ptr == end;
  };
  constexpr std::string_view suffix = "-nightly.";
  if (const auto offset = text.find(suffix); offset != text.npos) {
    const auto serial = text.substr(offset + suffix.size());
    if (serial.size() > 18 || !number(serial, parsed.nightly) || !parsed.nightly) return std::nullopt;
    text = text.substr(0, offset);
  }
  for (size_t i = 0; i < parsed.base.size(); ++i) {
    const auto dot = text.find('.');
    if ((i < 2) != (dot != text.npos) || !number(text.substr(0, dot), parsed.base[i])) return std::nullopt;
    if (i < 2) text.remove_prefix(dot + 1);
  }
  return parsed;
}

inline bool CanUpdate(BuildVersion installed, BuildVersion target) {
  if (installed.base != target.base) return target.base > installed.base;
  // The user explicitly picked the target feed. Permit a channel change at the same base
  // (including moving a nightly to its stable release), but never roll back a base version.
  return target.Channel() != installed.Channel() || target.nightly > installed.nightly;
}

}  // namespace loader
