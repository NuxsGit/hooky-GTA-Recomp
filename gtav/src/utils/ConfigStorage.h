#pragma once

#include "../core/WorkshopCore.h"

#include <filesystem>

namespace gtav::workshop {
// One instance is owned by the storage thread after startup. Calls here perform disk I/O.
class ConfigStorage {
 public:
  explicit ConfigStorage(std::filesystem::path workshop_profile);
  Config Load();
  uint8_t NeedsDefaults() const { return missing_; }
  uint8_t Unreadable() const { return unreadable_; }
  // Returns the bitmask of documents that were not written. Never overwrites an invalid or
  // newer document discovered at load. Config data cannot influence the filesystem path.
  uint8_t Save(const Config&, uint8_t documents) const;
 private:
  std::filesystem::path root_;
  uint8_t missing_ = 0, unreadable_ = 0;
};
}  // namespace gtav::workshop
