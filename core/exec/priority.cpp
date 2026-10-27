#include "exec/priority.hpp"

#include <sched.h>
#include <sys/resource.h>

namespace zeromesh::exec {

bool EnterIdlePriority() {
    sched_param param{};
    param.sched_priority = 0;
    if (sched_setscheduler(0, SCHED_IDLE, &param) == 0) return true;
    setpriority(PRIO_PROCESS, 0, 19);
    return false;
}

} // namespace zeromesh::exec
