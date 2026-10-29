#pragma once

#include "exec/batch_runner.hpp"
#include "store/store.hpp"
#include "zeromesh/v1/common.pb.h"
#include "zeromesh/v1/node.pb.h"
#include "zeromesh/v1/run.pb.h"

namespace zeromesh::node {

inline v1::Digest ToProto(const store::Digest& d) {
    v1::Digest out;
    out.set_hash(d.hash);
    out.set_size_bytes(d.size_bytes);
    return out;
}

inline store::Digest FromProto(const v1::Digest& d) {
    return store::Digest{d.hash(), d.size_bytes()};
}

inline v1::FailureKind ToProto(exec::ItemFailure f) {
    switch (f) {
    case exec::ItemFailure::None:
        return v1::FAILURE_NONE;
    case exec::ItemFailure::Exit:
        return v1::FAILURE_EXIT;
    case exec::ItemFailure::MissingOutput:
        return v1::FAILURE_MISSING_OUTPUT;
    case exec::ItemFailure::Oom:
        return v1::FAILURE_OOM;
    case exec::ItemFailure::Timeout:
        return v1::FAILURE_TIMEOUT;
    case exec::ItemFailure::Hung:
        return v1::FAILURE_HUNG;
    case exec::ItemFailure::Sandbox:
        return v1::FAILURE_SANDBOX;
    }
    return v1::FAILURE_SANDBOX;
}

inline v1::OutputFile ToProto(const exec::OutputEntry& o) {
    v1::OutputFile f;
    f.set_path(o.path);
    *f.mutable_digest() = ToProto(o.digest);
    f.set_is_dir(o.is_dir);
    f.set_executable(o.is_executable);
    f.set_symlink(o.is_symlink);
    f.set_target(o.symlink_target);
    return f;
}

inline v1::ItemOutcome ToProto(const exec::ItemRunResult& r) {
    v1::ItemOutcome o;
    o.set_id(r.id);
    o.set_exit_code(r.exit_code);
    o.set_failure(ToProto(r.failure));
    o.set_message(r.message);
    for (const auto& e : r.outputs) *o.add_outputs() = ToProto(e);
    if (!r.stdout_digest.hash.empty()) *o.mutable_stdout() = ToProto(r.stdout_digest);
    if (!r.stderr_digest.hash.empty()) *o.mutable_stderr() = ToProto(r.stderr_digest);
    o.set_stdout_tail(r.stdout_tail);
    o.set_stderr_tail(r.stderr_tail);
    o.set_wall_ms(r.wall_ms);
    o.set_cpu_ms(r.cpu_ms);
    o.set_peak_mem(r.peak_mem);
    o.set_oom_at_limit(r.oom_at_limit);
    o.set_budget(r.budget);
    for (const auto& p : r.footprint) o.add_footprint(p);
    return o;
}

} // namespace zeromesh::node
