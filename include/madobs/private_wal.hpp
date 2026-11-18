#ifndef MADOBS_PRIVATE_WAL_HPP
#define MADOBS_PRIVATE_WAL_HPP
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

#include "madobs/private.h"
namespace madobs {
namespace detail {
// Exact, versioned policy identity. Rule ordering is significant deliberately;
// no semantic migrations are guessed. No raw struct bytes or uninitialized
// data.
inline std::vector<unsigned char> PolicyIdentity(
    const madobs_privacy_policy_v1& p) {
  std::vector<unsigned char> bytes{'M', 'A', 'D', 'O', 'B', 'S', 'P', '1'};
  const auto put = [&bytes](uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
      bytes.push_back(static_cast<unsigned char>(value >> (8 * i)));
  };
  put(p.abi_version);
  put(p.event_count);
  for (uint32_t i = 0; i < p.event_count; ++i) {
    const auto& e = p.events[i];
    put(e.sdk_id);
    put(e.sdk_version);
    put(e.event_id);
    put(e.max_severity);
    put(e.attribute_count);
    for (uint32_t j = 0; j < e.attribute_count; ++j) {
      const auto& a = e.attributes[j];
      put(a.key);
      put(a.value_count);
      for (uint32_t k = 0; k < a.value_count; ++k) put(a.values[k]);
    }
  }
  return bytes;
}
class PrivateWalLock final {
 public:
  PrivateWalLock() = default;
  PrivateWalLock(const PrivateWalLock&) = delete;
  PrivateWalLock& operator=(const PrivateWalLock&) = delete;
  ~PrivateWalLock() { Release(); }
  void Release() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  bool Acquire(const char* path, const madobs_privacy_policy_v1& policy,
               madobs_private_open_result* result) {
    const auto fail = [result](madobs_private_open_result reason) {
      *result = reason;
      return false;
    };
    const auto identity = PolicyIdentity(policy);
    const bool created = ::mkdir(path, 0700) == 0;
    if (!created && errno != EEXIST)
      return fail(MADOBS_PRIVATE_OPEN_WAL_UNAVAILABLE);
    int dir = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0)
      return fail(errno == ELOOP || errno == ENOTDIR
                      ? MADOBS_PRIVATE_OPEN_WAL_UNSAFE
                      : MADOBS_PRIVATE_OPEN_WAL_UNAVAILABLE);
    struct stat ds{};
    if (::fstat(dir, &ds) != 0) {
      ::close(dir);
      return fail(MADOBS_PRIVATE_OPEN_WAL_IO_ERROR);
    }
    if (ds.st_uid != ::geteuid() || (ds.st_mode & 0077) != 0) {
      ::close(dir);
      return fail(MADOBS_PRIVATE_OPEN_WAL_UNSAFE);
    }
    int fd = ::openat(
        dir, ".madobs-policy-v1",
        O_RDWR | O_NOFOLLOW | O_CLOEXEC | (created ? O_CREAT | O_EXCL : 0),
        0600);
    if (fd < 0) {
      const int error = errno;
      ::close(dir);
      return fail(error == ELOOP ? MADOBS_PRIVATE_OPEN_WAL_UNSAFE
                                 : (!created && error == ENOENT
                                        ? MADOBS_PRIVATE_OPEN_WAL_UNBOUND
                                        : MADOBS_PRIVATE_OPEN_WAL_IO_ERROR));
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      ::close(dir);
      return fail(MADOBS_PRIVATE_OPEN_WAL_IO_ERROR);
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != ::geteuid() ||
        (st.st_mode & 0077) != 0) {
      ::close(fd);
      ::close(dir);
      return fail(MADOBS_PRIVATE_OPEN_WAL_UNSAFE);
    }
    if (::flock(fd, created ? LOCK_EX : (LOCK_EX | LOCK_NB)) != 0) {
      const int error = errno;
      ::close(fd);
      ::close(dir);
      return fail(error == EWOULDBLOCK || error == EAGAIN
                      ? MADOBS_PRIVATE_OPEN_WAL_BUSY
                      : MADOBS_PRIVATE_OPEN_WAL_IO_ERROR);
    }
    // Refresh size only after obtaining the lock: a concurrent initializer may
    // have finished between the first stat and our acquisition.
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      ::close(dir);
      return fail(MADOBS_PRIVATE_OPEN_WAL_IO_ERROR);
    }
    bool ok = true;
    *result = created ? MADOBS_PRIVATE_OPEN_WAL_IO_ERROR
                      : MADOBS_PRIVATE_OPEN_POLICY_OR_MANIFEST_MISMATCH;
    if (ok && created) {
      size_t offset = 0;
      while (offset < identity.size()) {
        const auto n =
            ::write(fd, identity.data() + offset, identity.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
          ok = false;
          break;
        }
        offset += static_cast<size_t>(n);
      }
      if (ok) ok = ::fsync(fd) == 0 && ::fsync(dir) == 0;
    } else if (ok) {
      if (st.st_size != static_cast<off_t>(identity.size())) ok = false;
      size_t offset = 0;
      unsigned char buf[256];
      while (ok && offset < identity.size()) {
        size_t want = identity.size() - offset;
        if (want > sizeof(buf)) want = sizeof(buf);
        const auto n = ::read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
          if (n < 0) *result = MADOBS_PRIVATE_OPEN_WAL_IO_ERROR;
          ok = false;
          break;
        }
        for (ssize_t i = 0; i < n; ++i)
          if (buf[i] != identity[offset + static_cast<size_t>(i)]) ok = false;
        offset += static_cast<size_t>(n);
      }
    }
    // Reopening may follow a prior fsync failure with valid cached bytes.
    // Re-establish manifest durability before allowing any new WAL activity.
    if (ok && !created && (::fsync(fd) != 0 || ::fsync(dir) != 0)) {
      *result = MADOBS_PRIVATE_OPEN_WAL_IO_ERROR;
      ok = false;
    }
    ::close(dir);
    if (!ok) {
      if (fd >= 0) ::close(fd);
      return false;
    }
    fd_ = fd;
    *result = MADOBS_PRIVATE_OPEN_OK;
    return true;
  }

 private:
  int fd_ = -1;
};
}  // namespace detail
}  // namespace madobs
#endif
