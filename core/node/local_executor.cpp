#include "node/local_executor.hpp"

#include "node/convert.hpp"

#include <algorithm>
#include <exception>

namespace zeromesh::node {

namespace {

v1::TaskResult Failed(const std::string& node_id, std::string error) {
    v1::TaskResult r;
    r.set_exit_code(-1);
    r.set_executed_by(node_id);
    r.set_error(std::move(error));
    return r;
}

v1::TaskResult Cancelled(const std::string& node_id) {
    v1::TaskResult r = Failed(node_id, "cancelled");
    r.set_cancelled(true);
    return r;
}

v1::TaskResult ToProto(const exec::RunResult& rr, const std::string& node_id) {
    v1::TaskResult r;
    r.set_exit_code(rr.exit_code);
    r.set_timed_out(rr.timed_out);
    r.set_cancelled(rr.cancelled);
    *r.mutable_stdout() = node::ToProto(rr.stdout_digest);
    *r.mutable_stderr() = node::ToProto(rr.stderr_digest);
    r.set_stdout_head(rr.stdout_head);
    r.set_stderr_head(rr.stderr_head);
    for (const auto& o : rr.outputs) {
        auto* e = r.add_outputs();
        e->set_path(o.path);
        *e->mutable_digest() = node::ToProto(o.digest);
        e->set_is_dir(o.is_dir);
        e->set_is_executable(o.is_executable);
        e->set_is_symlink(o.is_symlink);
        e->set_symlink_target(o.symlink_target);
    }
    r.set_executed_by(node_id);
    r.set_duration_ms(rr.duration.count());
    r.set_error(rr.error);
    return r;
}

} // namespace

LocalExecutor::LocalExecutor(std::shared_ptr<exec::SandboxRunner> runner, std::shared_ptr<plugin::Registry> registry,
                             std::string node_id, int slots)
    : runner_(std::move(runner)), registry_(std::move(registry)), node_id_(std::move(node_id)) {
    int n = std::max(1, slots);
    workers_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this](std::stop_token st) { Worker(st); });
    }
}

LocalExecutor::~LocalExecutor() {
    {
        std::lock_guard lock(mu_);
        for (auto& [id, item] : items_) item->stop.request_stop();
    }
    for (auto& w : workers_) w.request_stop();
    cv_.notify_all();
    workers_.clear();  // joins

    // Tasks that never started still get their single on_done.
    std::deque<std::shared_ptr<Item>> left;
    {
        std::lock_guard lock(mu_);
        left.swap(queue_);
        items_.clear();
    }
    for (auto& item : left) {
        if (item->cb.on_done) item->cb.on_done(Cancelled(node_id_));
    }
}

void LocalExecutor::Execute(const std::string& task_id, const v1::TaskSpec& spec, TaskCallbacks cb) {
    auto item = std::make_shared<Item>();
    item->task_id = task_id;
    item->spec = spec;
    item->cb = std::move(cb);
    {
        std::lock_guard lock(mu_);
        items_[task_id] = item;
        queue_.push_back(item);
    }
    cv_.notify_one();
}

void LocalExecutor::Cancel(const std::string& task_id) {
    std::shared_ptr<Item> dequeued;
    {
        std::lock_guard lock(mu_);
        auto it = items_.find(task_id);
        if (it == items_.end()) return;
        auto item = it->second;
        if (item->started) {
            item->stop.request_stop();
            return;
        }
        std::erase(queue_, item);
        items_.erase(it);
        dequeued = std::move(item);
    }
    if (dequeued->cb.on_done) dequeued->cb.on_done(Cancelled(node_id_));
}

void LocalExecutor::Worker(std::stop_token st) {
    for (;;) {
        std::shared_ptr<Item> item;
        {
            std::unique_lock lock(mu_);
            if (!cv_.wait(lock, st, [this] { return !queue_.empty(); })) return;
            item = queue_.front();
            queue_.pop_front();
            item->started = true;
        }
        v1::TaskResult result = Run(*item);
        {
            std::lock_guard lock(mu_);
            auto it = items_.find(item->task_id);
            if (it != items_.end() && it->second == item) items_.erase(it);
        }
        if (item->cb.on_done) item->cb.on_done(std::move(result));
    }
}

v1::TaskResult LocalExecutor::Run(Item& item) {
    const v1::TaskSpec& spec = item.spec;
    if (item.stop.stop_requested()) return Cancelled(node_id_);
    if (item.cb.on_state) item.cb.on_state(v1::RUNNING, "");

    const std::string& env_name = spec.env_spec().name();
    plugin::IEnvProvider* env = registry_ ? registry_->Env(env_name) : nullptr;
    if (env == nullptr) return Failed(node_id_, "no env provider '" + env_name + "'");

    plugin::TaskContext ctx;
    ctx.task_id = item.task_id;
    ctx.env.params.insert(spec.env_spec().params().begin(), spec.env_spec().params().end());
    ctx.env.argv.assign(spec.argv().begin(), spec.argv().end());

    try {
        plugin::Availability avail = env->Check(ctx.env);
        if (!avail.ok) {
            return Failed(node_id_, avail.reason.empty() ? "env provider '" + env_name + "' cannot run this task"
                                                         : avail.reason);
        }
        plugin::Result<plugin::SandboxEnv> prepared = env->Prepare(ctx);
        if (!prepared.value) {
            return Failed(node_id_, prepared.error.empty() ? "env provider '" + env_name + "' failed to prepare"
                                                           : prepared.error);
        }

        exec::RunSpec rs;
        rs.argv.assign(spec.argv().begin(), spec.argv().end());
        rs.env.assign(spec.env().begin(), spec.env().end());
        std::sort(rs.env.begin(), rs.env.end());
        rs.input_root = FromProto(spec.input_root());
        rs.working_dir = spec.working_dir().empty() ? "." : spec.working_dir();
        rs.outputs.assign(spec.outputs().begin(), spec.outputs().end());
        rs.capture_changed = spec.capture_changed();
        int timeout_s = spec.policy().timeout_s() > 0 ? spec.policy().timeout_s() : 3600;
        rs.timeout = std::chrono::seconds(timeout_s);
        rs.allow_network = spec.policy().allow_network();
        rs.sandbox_env = std::move(*prepared.value);
        rs.node_id = node_id_;

        exec::LogSink sink;
        if (item.cb.on_log) sink = [&item](int fd, std::string_view chunk) { item.cb.on_log(fd, chunk); };

        exec::RunResult rr;
        try {
            rr = runner_->Run(rs, sink, item.stop.get_token());
        } catch (const std::exception& e) {
            env->Release(ctx);
            return Failed(node_id_, std::string("sandbox: ") + e.what());
        }
        env->Release(ctx);
        return ToProto(rr, node_id_);
    } catch (const std::exception& e) {
        return Failed(node_id_, std::string("env provider '") + env_name + "': " + e.what());
    }
}

} // namespace zeromesh::node
