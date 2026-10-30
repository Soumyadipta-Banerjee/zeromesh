#pragma once

#include "zeromesh/v1/common.pb.h"

#include <functional>
#include <string>
#include <string_view>

namespace zeromesh::node {

struct TaskCallbacks {
    std::function<void(v1::TaskState, std::string detail)> on_state; // non-terminal states only
    std::function<void(int fd, std::string_view chunk)> on_log;      // fd 1 = stdout, 2 = stderr
    // The task committed a unit of its work (exec/units.hpp); its blob is in the CAS of the node
    // that reports it. Optional. May be called from any thread, before on_done.
    std::function<void(const std::string& unit_id, const v1::Digest& digest)> on_unit;
    std::function<void(v1::TaskResult)> on_done;                     // exactly once per Execute
};

// Runs tasks somewhere. Execute is asynchronous (an implementation may also finish synchronously)
// and calls cb.on_done exactly once. Cancel of an unknown or finished task is a no-op.
class Executor {
public:
    virtual ~Executor() = default;
    virtual void Execute(const std::string& task_id, const v1::TaskSpec& spec, TaskCallbacks cb) = 0;
    virtual void Cancel(const std::string& task_id) = 0;
};

} // namespace zeromesh::node
