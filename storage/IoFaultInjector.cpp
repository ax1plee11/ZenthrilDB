#include "storage/IoFaultInjector.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace zenthrildb {
namespace {
constexpr std::size_t kFaultPointCount = static_cast<std::size_t>(IoFaultPoint::SnapshotSyncParent) + 1;
std::mutex gMutex;
std::array<std::size_t, kFaultPointCount> gCountdowns{};
std::array<bool, kFaultPointCount> gCrash{};

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
std::size_t indexOf(IoFaultPoint point) noexcept {
  return static_cast<std::size_t>(point);
}
#endif
} // namespace

void IoFaultInjector::arm(IoFaultPoint point, std::size_t occurrence) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  if (occurrence == 0) throw std::invalid_argument("Fault occurrence must be positive");
  std::lock_guard lock(gMutex);
  gCountdowns[indexOf(point)] = occurrence;
  gCrash[indexOf(point)] = false;
#else
  (void)point;
  (void)occurrence;
  throw std::logic_error("Fault injection is disabled in this build");
#endif
}

void IoFaultInjector::armCrash(IoFaultPoint point) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  std::lock_guard lock(gMutex);
  gCountdowns[indexOf(point)] = 1;
  gCrash[indexOf(point)] = true;
#else
  (void)point;
  throw std::logic_error("Fault injection is disabled in this build");
#endif
}

void IoFaultInjector::clear() noexcept {
  std::lock_guard lock(gMutex);
  gCountdowns.fill(0);
  gCrash.fill(false);
}

bool IoFaultInjector::consume(IoFaultPoint point) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  std::lock_guard lock(gMutex);
  auto& count = gCountdowns[indexOf(point)];
  if (count == 0) return false;
  if (--count != 0) return false;
  if (gCrash[indexOf(point)]) std::_Exit(91);
  return true;
#else
  (void)point;
  return false;
#endif
}

bool IoFaultInjector::isArmed(IoFaultPoint point) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  std::lock_guard lock(gMutex);
  return gCountdowns[indexOf(point)] != 0;
#else
  (void)point;
  return false;
#endif
}

void IoFaultInjector::failIfArmed(IoFaultPoint point) {
  if (!consume(point)) return;
  if (point == IoFaultPoint::NoSpace) {
#ifdef ENOSPC
    throw std::system_error(ENOSPC, std::generic_category(), "Injected storage full error");
#else
    throw std::runtime_error("Injected storage full error");
#endif
  }
  throw std::system_error(std::make_error_code(std::errc::io_error), "Injected I/O failure");
}

} // namespace zenthrildb
