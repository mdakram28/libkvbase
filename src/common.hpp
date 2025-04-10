#ifndef COMMON_HPP
#define COMMON_HPP

#include <sys/time.h>
#include "spdlog/spdlog.h"

#define STATIC_PTR_CAST(ptr, type) static_cast<type>(static_cast<void*>(ptr))
#define STATIC_CONST_PTR_CAST(ptr, type) \
  static_cast<type>(static_cast<const void*>(ptr))

typedef uint64_t ServerId;
typedef uint64_t ServerTerm;

constexpr ServerId INVALID_SERVER = 0;

constexpr uint64_t CONFIG_HEARTBEAT_SEND_DELAY_MS = 500;
constexpr uint64_t CONFIG_HEARTBEAT_TIMEOUT_MS = 2000;
constexpr uint64_t CONFIG_VOTE_REQUEST_TIMEOUT = 3000;

uint64_t currentTimeMs() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  long int ms = tp.tv_sec * 1000 + tp.tv_usec / 1000;
  return ms;
}

template <class ThisClass, class... FuncArgs>
class ThrottledFunction {
 public:
  ThrottledFunction(std::function<void(ThisClass*, uint64_t, FuncArgs...)> callback,
                    uint64_t delay, ThisClass* thisRef)
      : m_callback(callback), m_delay(delay), m_thisRef(thisRef) {}

  void operator()(uint64_t currentTime, FuncArgs&&... args) {
    if (currentTime > m_lastCallTime + m_delay) {
      m_callback(m_thisRef, currentTime, std::forward(args)...);
      m_lastCallTime = currentTime;
    }
  }

  void resetTimer() { m_lastCallTime = 0; }

 private:
  uint64_t m_delay{0};
  uint64_t m_lastCallTime{0};
  std::function<void(ThisClass*, uint64_t, FuncArgs...)> m_callback;
  ThisClass* m_thisRef;
};

#endif /* COMMON_HPP */
