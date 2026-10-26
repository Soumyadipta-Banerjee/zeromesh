#pragma once

namespace zeromesh::exec {

// Puts the calling process in SCHED_IDLE, falling back to nice 19. Returns true on SCHED_IDLE.
bool EnterIdlePriority();

} // namespace zeromesh::exec
