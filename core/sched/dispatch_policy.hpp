#pragma once

#include "plugin/loader.hpp"

#include <map>
#include <string>
#include <string_view>

namespace zeromesh::sched {

void RegisterBuiltinDispatchPolicies(plugin::Registry& registry);

// "" means "pull". Null when no built-in or plugin policy has that name.
[[nodiscard]] plugin::IDispatchPolicy* LookupDispatchPolicy(plugin::Registry& registry, std::string_view kind);

// Whether `n` satisfies the task's constraints (local/remote, arch, memory, exclusions),
// ignoring capacity.
[[nodiscard]] bool Fits(const plugin::DispatchTaskView& t, const plugin::DispatchNodeView& n);

// Tasks `n` can still take: slots + `queue` - in_flight, or cores_total / task cores -
// in_flight for a node that announces no slots.
[[nodiscard]] int Capacity(const plugin::DispatchNodeView& n, int task_cores, int queue);

[[nodiscard]] int ParamInt(const std::map<std::string, std::string>& params, const char* key, int def);

} // namespace zeromesh::sched
