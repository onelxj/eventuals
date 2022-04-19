#pragma once

#include "eventuals/event-loop.h"

////////////////////////////////////////////////////////////////////////

namespace eventuals {

////////////////////////////////////////////////////////////////////////

inline auto Poll(EventLoop& loop, int fd, PollEvents events) {
  return loop.Poll(fd, events);
}

////////////////////////////////////////////////////////////////////////

inline auto Poll(int fd, PollEvents events) {
  return EventLoop::Default().Poll(fd, events);
}

////////////////////////////////////////////////////////////////////////

} // namespace eventuals

////////////////////////////////////////////////////////////////////////
