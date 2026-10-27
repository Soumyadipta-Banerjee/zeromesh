#pragma once

#include "zeromesh/v1/common.pb.h"

#include <functional>
#include <string>
#include <string_view>

namespace zeromesh::node {

struct TaskCallbacks {
    std::function<void(v1::TaskState, std::string detail)> on_state;  // non-terminal states only
    std::function<void(int fd, std::string_view chunk)> on_log;       // fd 1 = stdout, 2 = stderr
    std::function<void(v1::TaskResult)> on_done;                      // exactly once per Execute
};

struct ExecuteContext {
    std::string job_id;
    v1::DispatchPolicy dispatch;
};

// Runs tasks somewhere. Execute is asynchronous (an implementation may also finish synchronously)
// and calls cb.on_done exactly once. Cancel of an unknown or finished task is a no-op.
class Executor {
public:
    virtual ~Executor() = default;
    virtual void Execute(const std::string& task_id, const v1::TaskSpec& spec, TaskCallbacks cb,
                         ExecuteContext ctx = {}) = 0;
    virtual void Cancel(const std::string& task_id) = 0;
};

} // namespace zeromesh::node
