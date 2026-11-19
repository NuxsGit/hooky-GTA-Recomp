// Keep the same OS lock for setup, migration, and the launched game. Closing the
// parent's copy does not release it while a child still has the file open.
#pragma once

#include <filesystem>

#ifdef _WIN32
#include "win_process.h"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace loader {

class SetupLock {
 public:
  SetupLock() = default;
  SetupLock(const SetupLock&) = delete;
  SetupLock& operator=(const SetupLock&) = delete;
  ~SetupLock() {
#ifndef _WIN32
    if (fd_ != -1) close(fd_);
#endif
  }

  bool Acquire(const std::filesystem::path& file) {
#ifdef _WIN32
    handle_.Set(win::OpenSetupLock(file));
    return bool(handle_);
#else
    fd_ = open(file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd_ < 0) return false;
    // spawn may replace stdout/stderr, even if either was closed in the parent.
    if (fd_ < 3) {
      const int copy = fcntl(fd_, F_DUPFD_CLOEXEC, 3);
      close(fd_);
      fd_ = copy;
    }
    struct stat attributes{};
    return fd_ >= 0 && fstat(fd_, &attributes) == 0 && S_ISREG(attributes.st_mode) && flock(fd_, LOCK_EX | LOCK_NB) == 0;
#endif
  }

#ifndef _WIN32
  bool KeepInChild() const { return fcntl(fd_, F_SETFD, 0) == 0; }
#endif

 private:
#ifdef _WIN32
  win::Handle handle_;
#else
  int fd_ = -1;
#endif
};

}  // namespace loader
