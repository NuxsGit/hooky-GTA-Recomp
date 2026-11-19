#pragma once

#include "world.h"

#include <filesystem>

namespace gtav::sandbox {

// Only local user operations use these paths. The protocol carries a bounded value, never a path.
bool SaveCourse(const std::filesystem::path& folder, const Course& course);
std::optional<Course> LoadCourse(const std::filesystem::path& folder, std::string_view name);
}  // namespace gtav::sandbox
