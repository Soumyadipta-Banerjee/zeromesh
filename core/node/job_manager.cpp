#include "node/job_manager.hpp"

#include "node/convert.hpp"
#include "node/mirror_manifest.hpp"
#include "node/task_key.hpp"

#include <sodium.h>

#include <exception>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zeromesh::node {

// ---------------------------------------------------------------------------------------------
// EventStream

std::optional<v1::JobEvent> EventStream::Next(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    cv_.wait_for(lock, timeout, [this] { return closed_ || ended_ || !queue_.empty(); });
    if (closed_ || queue_.empty()) return std::nullopt;
    v1::JobEvent ev = std::move(queue_.front());
    queue_.pop_front();
    return ev;
}

void EventStream::Close() {
    {
        std::lock_guard lock(mu_);
        closed_ = true;
        queue_.clear();
    }
    cv_.notify_all();
}

void EventStream::Push(const v1::JobEvent& ev) {
    {
        std::lock_guard lock(mu_);
        if (closed_ || ended_) return;
        queue_.push_back(ev);
    }
    cv_.notify_all();
}

void EventStream::End() {
    {
        std::lock_guard lock(mu_);
        ended_ = true;
    }
    cv_.notify_all();
}

bool EventStream::closed() const {
    std::lock_guard lock(mu_);
    return closed_;
}

// ---------------------------------------------------------------------------------------------
// JobManager::Impl

namespace {

bool IsTerminal(v1::TaskState s) { return s == v1::SUCCEEDED || s == v1::FAILED || s == v1::CANCELLED; }

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    randombytes_buf(buf.data(), buf.size());
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
}

// Joins an overlay destination and an output path, dropping empty and "." segments.
std::string JoinPath(const std::string& dest, const std::string& rel) {
    std::string out;
    for (const std::string* part : {&dest, &rel}) {
        size_t start = 0;
        while (start <= part->size()) {
            size_t slash = part->find('/', start);
            if (slash == std::string::npos) slash = part->size();
            std::string seg = part->substr(start, slash - start);
            if (!seg.empty() && seg != ".") {
                if (!out.empty()) out.push_back('/');
                out += seg;
            }
            start = slash + 1;
        }
    }
    return out;
}

struct TaskRun {
    v1::TaskSpec spec;
    std::vector<size_t> deps;  // depends_on and inputs_from tasks, deduplicated
    std::vector<size_t> dependents;
    v1::TaskState state{v1::QUEUED};
    bool started{false};    // claimed for cache lookup / execution
    bool executing{false};  // handed to the executor
    std::string task_id;
    std::string task_key;
    std::string worker_id;  // set at PLACED, before the task finishes
    v1::TaskResult result;
    bool has_result{false};
    bool hidden{false};  // cleared from the monitor; result is still served by GetJob
    size_t retained_log_bytes{0};
};

struct Job {
    std::string id;
    std::string name;
    std::vector<TaskRun> tasks;
    std::unordered_map<std::string, size_t> index;
    std::vector<v1::JobEvent> history;
    std::vector<std::shared_ptr<EventStream>> watchers;
    size_t terminal{0};
    size_t retained_log_bytes{0};
    bool done{false};
    bool cancelled{false};
    std::optional<std::chrono::steady_clock::time_point> finished_at;
};

std::chrono::seconds g_finished_job_blob_grace{std::chrono::minutes(10)};
std::function<std::chrono::steady_clock::time_point()> g_job_clock;

std::chrono::steady_clock::time_point JobClockNow() {
    if (g_job_clock) return g_job_clock();
    return std::chrono::steady_clock::now();
}

void ProtectTreeHashes(std::unordered_set<std::string>& out, store::MerkleBuilder& merkle, const store::Digest& root) {
    if (root.hash.empty()) return;
    try {
        for (const auto& d : merkle.CollectTree(root)) out.insert(d.hash);
    } catch (...) {
        out.insert(root.hash);
    }
}

void ProtectMirrorRoot(std::unordered_set<std::string>& out, store::MerkleBuilder& merkle, const v1::TaskSpec& spec) {
    const auto& params = spec.env_spec().params();
    auto it = params.find(kMirrorRootDigestParam);
    if (it == params.end() || !store::CasStore::IsValidHash(it->second)) return;
    ProtectTreeHashes(out, merkle, store::Digest{it->second, 0});
}

void ProtectOutputEntry(std::unordered_set<std::string>& out, store::MerkleBuilder& merkle, const v1::OutputEntry& o) {
    if (o.digest().hash().empty()) return;
    if (o.is_dir()) {
        ProtectTreeHashes(out, merkle, FromProto(o.digest()));
    } else {
        out.insert(o.digest().hash());
    }
}

void ProtectTaskResultBlobs(std::unordered_set<std::string>& out, store::MerkleBuilder& merkle, const v1::TaskResult& r) {
    if (!r.stdout().hash().empty()) out.insert(r.stdout().hash());
    if (!r.stderr().hash().empty()) out.insert(r.stderr().hash());
    for (const auto& o : r.outputs()) ProtectOutputEntry(out, merkle, o);
}

// Inputs copied out under the lock for a task that is about to start.
struct Ready {
    size_t index;
    v1::TaskSpec spec;
    std::vector<std::pair<std::string, v1::OutputEntry>> overlays;  // (destination, output)
};

} // namespace

class JobManager::Impl : public std::enable_shared_from_this<JobManager::Impl> {
public:
    Impl(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
         std::shared_ptr<ActionCache> cache, std::shared_ptr<Executor> executor)
        : cas_(std::move(cas)), merkle_(std::move(merkle)), cache_(std::move(cache)), executor_(std::move(executor)) {}

    std::string Submit(v1::JobSpec spec);
    void Cancel(const std::string& job_id);
    std::optional<v1::JobStatus> Get(const std::string& job_id) const;
    std::vector<v1::ActiveTask> ListActive() const;
    void ClearFinished();
    std::unordered_set<std::string> ProtectedBlobHashes() const;
    std::shared_ptr<EventStream> Watch(const std::string& job_id);
    void CancelAll();

private:
    using JobPtr = std::shared_ptr<Job>;

    void Advance(const JobPtr& job);
    void Start(const JobPtr& job, Ready r);
    void Complete(const JobPtr& job, size_t i, v1::TaskResult result);
    void OnState(const JobPtr& job, size_t i, v1::TaskState state, std::string detail);
    void OnLog(const JobPtr& job, size_t i, int fd, std::string_view chunk);

    // The following require mu_.
    void Emit(Job& job, v1::JobEvent ev);
    void Finish(Job& job, size_t i, v1::TaskState state, const std::string& detail, v1::TaskResult result);

    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::shared_ptr<ActionCache> cache_;
    std::shared_ptr<Executor> executor_;

    static constexpr size_t kFinishedJobsListed = 256;
    static constexpr size_t kMaxRetainedFinishedJobs = 256;
    static constexpr size_t kMaxRetainedLogBytesPerTask = 1024 * 1024;
    // A 240-chunk job at 1 MiB/task would otherwise retain ~240 MiB of log history.
    static constexpr size_t kMaxRetainedLogBytesPerJob = 8 * 1024 * 1024;

    void EvictFinishedJobs();

    mutable std::mutex mu_;
    std::unordered_map<std::string, JobPtr> jobs_;
    std::deque<std::string> job_order_;  // submit order (for monitor history)
};

std::string JobManager::Impl::Submit(v1::JobSpec spec) {
    auto job = std::make_shared<Job>();
    job->name = spec.name();
    job->tasks.resize(static_cast<size_t>(spec.tasks_size()));
    for (size_t i = 0; i < job->tasks.size(); ++i) {
        v1::TaskSpec& t = *spec.mutable_tasks(static_cast<int>(i));
        if (t.name().empty()) throw std::invalid_argument("task " + std::to_string(i) + " has no name");
        if (!job->index.emplace(t.name(), i).second) throw std::invalid_argument("duplicate task name '" + t.name() + "'");
        Normalize(t);
        job->tasks[i].spec = std::move(t);
    }
    for (size_t i = 0; i < job->tasks.size(); ++i) {
        TaskRun& t = job->tasks[i];
        std::unordered_set<size_t> seen;
        auto add_dep = [&](const std::string& name) {
            auto it = job->index.find(name);
            if (it == job->index.end()) {
                throw std::invalid_argument("task '" + t.spec.name() + "' depends on unknown task '" + name + "'");
            }
            if (seen.insert(it->second).second) {
                t.deps.push_back(it->second);
                job->tasks[it->second].dependents.push_back(i);
            }
        };
        for (const auto& d : t.spec.depends_on()) add_dep(d);
        for (const auto& f : t.spec.inputs_from()) add_dep(f.task());
    }
    // Kahn's algorithm: anything left unvisited is on a cycle.
    std::vector<size_t> indegree(job->tasks.size());
    std::vector<size_t> frontier;
    for (size_t i = 0; i < job->tasks.size(); ++i) {
        indegree[i] = job->tasks[i].deps.size();
        if (indegree[i] == 0) frontier.push_back(i);
    }
    size_t visited = 0;
    while (!frontier.empty()) {
        size_t n = frontier.back();
        frontier.pop_back();
        ++visited;
        for (size_t d : job->tasks[n].dependents) {
            if (--indegree[d] == 0) frontier.push_back(d);
        }
    }
    if (visited != job->tasks.size()) throw std::invalid_argument("job '" + job->name + "' has a dependency cycle");

    {
        std::lock_guard lock(mu_);
        do {
            job->id = RandomHex(8);
        } while (jobs_.contains(job->id));
        jobs_[job->id] = job;
        job_order_.push_back(job->id);
        for (size_t i = 0; i < job->tasks.size(); ++i) {
            job->tasks[i].task_id = job->id + "/" + job->tasks[i].spec.name();
            v1::JobEvent ev;
            ev.set_task_name(job->tasks[i].spec.name());
            ev.set_state(v1::QUEUED);
            Emit(*job, std::move(ev));
        }
        if (job->tasks.empty()) job->done = true;
    }
    std::string id = job->id;
    Advance(job);
    return id;
}

void JobManager::Impl::Advance(const JobPtr& job) {
    std::vector<Ready> ready;
    {
        std::lock_guard lock(mu_);
        if (job->cancelled) return;
        for (size_t i = 0; i < job->tasks.size(); ++i) {
            TaskRun& t = job->tasks[i];
            if (t.started || IsTerminal(t.state)) continue;
            bool deps_ok = true;
            for (size_t d : t.deps) deps_ok = deps_ok && job->tasks[d].state == v1::SUCCEEDED;
            if (!deps_ok) continue;
            t.started = true;
            Ready r{i, t.spec, {}};
            for (const auto& from : t.spec.inputs_from()) {
                const TaskRun& src = job->tasks[job->index.at(from.task())];
                for (const auto& out : src.result.outputs()) r.overlays.emplace_back(from.path(), out);
            }
            ready.push_back(std::move(r));
        }
    }
    for (auto& r : ready) Start(job, std::move(r));
}

void JobManager::Impl::Start(const JobPtr& job, Ready r) {
    v1::TaskSpec& spec = r.spec;
    try {
        store::Digest root = FromProto(spec.input_root());
        for (const auto& [dest, out] : r.overlays) {
            if (out.is_symlink()) continue;
            root = merkle_->Overlay(root, JoinPath(dest, out.path()), FromProto(out.digest()), out.is_dir(),
                                    out.is_executable());
        }
        *spec.mutable_input_root() = ToProto(root);
    } catch (const std::exception& e) {
        v1::TaskResult res;
        res.set_exit_code(-1);
        res.set_error(std::string("inputs_from: ") + e.what());
        Complete(job, r.index, std::move(res));
        return;
    }

    std::string key = TaskKey(spec);
    std::string task_id;
    {
        std::lock_guard lock(mu_);
        job->tasks[r.index].task_key = key;
        task_id = job->tasks[r.index].task_id;
    }
    if (auto hit = cache_->Get(key)) {
        hit->set_cache_hit(true);
        Complete(job, r.index, std::move(*hit));
        return;
    }

    bool cancelled = false;
    {
        std::lock_guard lock(mu_);
        if (job->cancelled) {
            v1::TaskResult res;
            res.set_cancelled(true);
            res.set_exit_code(-1);
            Finish(*job, r.index, v1::CANCELLED, "job cancelled", std::move(res));
            cancelled = true;
        } else {
            job->tasks[r.index].executing = true;
        }
    }
    if (cancelled) return;

    std::weak_ptr<Impl> weak = weak_from_this();
    size_t i = r.index;
    TaskCallbacks cb;
    cb.on_state = [weak, job, i](v1::TaskState s, std::string detail) {
        if (auto self = weak.lock()) self->OnState(job, i, s, std::move(detail));
    };
    cb.on_log = [weak, job, i](int fd, std::string_view chunk) {
        if (auto self = weak.lock()) self->OnLog(job, i, fd, chunk);
    };
    cb.on_done = [weak, job, i](v1::TaskResult res) {
        if (auto self = weak.lock()) self->Complete(job, i, std::move(res));
    };
    try {
        executor_->Execute(task_id, spec, std::move(cb));
    } catch (const std::exception& e) {
        v1::TaskResult res;
        res.set_exit_code(-1);
        res.set_error(std::string("executor: ") + e.what());
        Complete(job, i, std::move(res));
        return;
    }
    // Cancel may have raced with Execute registering the task.
    bool need_cancel = false;
    {
        std::lock_guard lock(mu_);
        need_cancel = job->cancelled && !IsTerminal(job->tasks[i].state);
    }
    if (need_cancel) executor_->Cancel(task_id);
}

void JobManager::Impl::Complete(const JobPtr& job, size_t i, v1::TaskResult result) {
    v1::TaskState state;
    std::string detail;
    if (result.cancelled()) {
        state = v1::CANCELLED;
        detail = "cancelled";
    } else if (!result.error().empty()) {
        state = v1::FAILED;
        detail = result.error();
    } else if (result.timed_out()) {
        state = v1::FAILED;
        detail = "timed out";
    } else if (result.exit_code() != 0) {
        state = v1::FAILED;
        detail = "exit code " + std::to_string(result.exit_code());
    } else {
        state = v1::SUCCEEDED;
    }

    std::string key;
    {
        std::lock_guard lock(mu_);
        key = job->tasks[i].task_key;
    }
    if (!key.empty()) result.set_task_key(key);
    if (state == v1::SUCCEEDED && !result.cache_hit() && !key.empty()) cache_->Put(key, result);
    {
        std::lock_guard lock(mu_);
        Finish(*job, i, state, detail, std::move(result));
    }
    Advance(job);
}

void JobManager::Impl::OnState(const JobPtr& job, size_t i, v1::TaskState state, std::string detail) {
    if (IsTerminal(state)) return;  // terminal states arrive through on_done
    std::lock_guard lock(mu_);
    TaskRun& t = job->tasks[i];
    if (IsTerminal(t.state)) return;
    // Placer reports placement as "node <id>" (see sched::Placer).
    constexpr std::string_view kNode = "node ";
    if (detail.rfind(kNode, 0) == 0 && detail.size() > kNode.size()) {
        t.worker_id = detail.substr(kNode.size());
    }
    t.state = state;
    v1::JobEvent ev;
    ev.set_task_name(t.spec.name());
    ev.set_state(state);
    ev.set_detail(std::move(detail));
    Emit(*job, std::move(ev));
}

void JobManager::Impl::OnLog(const JobPtr& job, size_t i, int fd, std::string_view chunk) {
    std::lock_guard lock(mu_);
    TaskRun& t = job->tasks[i];
    if (IsTerminal(t.state)) return;
    v1::JobEvent ev;
    ev.set_task_name(t.spec.name());
    ev.set_state(t.state);
    ev.set_log_fd(fd);
    ev.set_log_chunk(std::string(chunk));
    Emit(*job, std::move(ev));
}

void JobManager::Impl::Emit(Job& job, v1::JobEvent ev) {
    ev.set_job_id(job.id);
    bool retain = true;
    if (!ev.log_chunk().empty()) {
        const size_t chunk = ev.log_chunk().size();
        if (job.retained_log_bytes + chunk > kMaxRetainedLogBytesPerJob) {
            retain = false;
        } else {
            auto it = job.index.find(ev.task_name());
            if (it != job.index.end()) {
                TaskRun& t = job.tasks[it->second];
                if (t.retained_log_bytes + chunk > kMaxRetainedLogBytesPerTask) {
                    retain = false;
                } else {
                    t.retained_log_bytes += chunk;
                    job.retained_log_bytes += chunk;
                }
            } else {
                job.retained_log_bytes += chunk;
            }
        }
    }
    std::erase_if(job.watchers, [](const auto& w) { return w->closed(); });
    for (auto& w : job.watchers) w->Push(ev);
    if (retain) job.history.push_back(std::move(ev));
}

void JobManager::Impl::EvictFinishedJobs() {
    size_t finished = 0;
    for (const auto& id : job_order_) {
        auto it = jobs_.find(id);
        if (it != jobs_.end() && it->second->done) ++finished;
    }
    while (finished > kMaxRetainedFinishedJobs) {
        bool removed = false;
        for (auto it = job_order_.begin(); it != job_order_.end(); ++it) {
            auto jt = jobs_.find(*it);
            if (jt == jobs_.end() || !jt->second->done) continue;
            jobs_.erase(jt);
            job_order_.erase(it);
            --finished;
            removed = true;
            break;
        }
        if (!removed) break;
    }
}

void JobManager::Impl::Finish(Job& job, size_t i, v1::TaskState state, const std::string& detail,
                              v1::TaskResult result) {
    TaskRun& t = job.tasks[i];
    if (IsTerminal(t.state)) return;
    t.state = state;
    if (t.worker_id.empty()) t.worker_id = result.executed_by();
    t.result = std::move(result);
    t.has_result = true;
    v1::JobEvent ev;
    ev.set_task_name(t.spec.name());
    ev.set_state(state);
    ev.set_detail(detail);
    *ev.mutable_result() = t.result;
    Emit(job, std::move(ev));
    ++job.terminal;

    if (state != v1::SUCCEEDED) {
        for (size_t d : t.dependents) {
            if (job.tasks[d].started || IsTerminal(job.tasks[d].state)) continue;
            v1::TaskResult r;
            r.set_exit_code(-1);
            if (job.cancelled) {
                r.set_cancelled(true);
                Finish(job, d, v1::CANCELLED, "job cancelled", std::move(r));
            } else {
                std::string why = "dependency " + t.spec.name() + " failed";
                r.set_error(why);
                Finish(job, d, v1::FAILED, why, std::move(r));
            }
        }
    }

    if (job.terminal == job.tasks.size() && !job.done) {
        job.done = true;
        job.finished_at = JobClockNow();
        for (auto& w : job.watchers) w->End();
        job.watchers.clear();
        EvictFinishedJobs();
    }
}

void JobManager::Impl::Cancel(const std::string& job_id) {
    std::vector<std::string> running;
    {
        std::lock_guard lock(mu_);
        auto it = jobs_.find(job_id);
        if (it == jobs_.end() || it->second->done) return;
        Job& job = *it->second;
        job.cancelled = true;
        for (size_t i = 0; i < job.tasks.size(); ++i) {
            TaskRun& t = job.tasks[i];
            if (IsTerminal(t.state)) continue;
            if (t.executing) {
                running.push_back(t.task_id);
            } else if (!t.started) {
                v1::TaskResult r;
                r.set_exit_code(-1);
                r.set_cancelled(true);
                Finish(job, i, v1::CANCELLED, "job cancelled", std::move(r));
            }
        }
    }
    for (const auto& id : running) executor_->Cancel(id);
}

void JobManager::Impl::CancelAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mu_);
        for (const auto& [id, job] : jobs_) {
            if (!job->done) ids.push_back(id);
        }
    }
    for (const auto& id : ids) Cancel(id);
}

std::optional<v1::JobStatus> JobManager::Impl::Get(const std::string& job_id) const {
    std::lock_guard lock(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return std::nullopt;
    const Job& job = *it->second;
    v1::JobStatus st;
    st.set_job_id(job.id);
    st.set_name(job.name);
    for (const auto& t : job.tasks) {
        (*st.mutable_tasks())[t.spec.name()] = t.state;
        if (t.has_result) (*st.mutable_results())[t.spec.name()] = t.result;
    }
    st.set_done(job.done);
    return st;
}

std::vector<v1::ActiveTask> JobManager::Impl::ListActive() const {
    std::lock_guard lock(mu_);
    std::vector<v1::ActiveTask> out;

    // Newest-first among finished jobs, but always include every in-flight job.
    std::vector<JobPtr> open;
    std::vector<JobPtr> finished;
    for (auto it = job_order_.rbegin(); it != job_order_.rend(); ++it) {
        auto jt = jobs_.find(*it);
        if (jt == jobs_.end()) continue;
        if (jt->second->done) finished.push_back(jt->second);
        else open.push_back(jt->second);
    }
    if (finished.size() > kFinishedJobsListed) {
        finished.resize(kFinishedJobsListed);
    }

    auto append_job = [&](const JobPtr& job, bool want_terminal) {
        for (const auto& t : job->tasks) {
            if (t.hidden || IsTerminal(t.state) != want_terminal) continue;
            v1::ActiveTask a;
            a.set_job_id(job->id);
            a.set_task_name(t.spec.name());
            a.set_state(t.state);
            a.set_role("submitted");
            for (const auto& arg : t.spec.argv()) a.add_argv(arg);
            a.set_cores(t.spec.resources().cores() > 0 ? t.spec.resources().cores() : 1);
            a.set_worker_id(t.worker_id);
            if (t.has_result && !t.result.error().empty()) a.set_detail(t.result.error());
            out.push_back(std::move(a));
        }
    };

    // Working rows first (open jobs, then any still-running tasks on a finishing job),
    // then DONE/FAILED/CANCEL so the monitor never blanks mid-job.
    for (const auto& job : open) append_job(job, false);
    for (const auto& job : finished) append_job(job, false);
    for (const auto& job : open) append_job(job, true);
    for (const auto& job : finished) append_job(job, true);
    return out;
}

std::shared_ptr<EventStream> JobManager::Impl::Watch(const std::string& job_id) {
    std::lock_guard lock(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return nullptr;
    Job& job = *it->second;
    auto stream = std::make_shared<EventStream>();
    for (const auto& ev : job.history) stream->Push(ev);
    if (job.done) stream->End();
    else job.watchers.push_back(stream);
    return stream;
}

// ---------------------------------------------------------------------------------------------
// JobManager

JobManager::JobManager(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
                       std::shared_ptr<ActionCache> cache, std::shared_ptr<Executor> executor) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialisation failed");
    impl_ = std::make_shared<Impl>(std::move(cas), std::move(merkle), std::move(cache), std::move(executor));
}

JobManager::~JobManager() { impl_->CancelAll(); }

std::string JobManager::Submit(v1::JobSpec spec) { return impl_->Submit(std::move(spec)); }
void JobManager::Cancel(const std::string& job_id) { impl_->Cancel(job_id); }
std::optional<v1::JobStatus> JobManager::Get(const std::string& job_id) const { return impl_->Get(job_id); }
std::vector<v1::ActiveTask> JobManager::ListActive() const { return impl_->ListActive(); }
void JobManager::ClearFinished() { impl_->ClearFinished(); }

void JobManager::Impl::ClearFinished() {
    std::lock_guard lock(mu_);
    for (auto& [id, job] : jobs_) {
        for (auto& t : job->tasks) {
            if (IsTerminal(t.state)) t.hidden = true;
        }
    }
}

std::unordered_set<std::string> JobManager::Impl::ProtectedBlobHashes() const {
    struct JobSnap {
        bool done{false};
        std::optional<std::chrono::steady_clock::time_point> finished_at;
        std::vector<v1::TaskSpec> specs;
        std::vector<v1::TaskResult> results;
        std::vector<bool> has_result;
    };
    std::vector<JobSnap> snaps;
    const auto now = JobClockNow();
    {
        std::lock_guard lock(mu_);
        for (const auto& [_, job] : jobs_) {
            if (!job) continue;
            const bool in_grace =
                job->done && job->finished_at &&
                (now - *job->finished_at < g_finished_job_blob_grace);
            if (job->done && !in_grace) continue;
            JobSnap snap;
            snap.done = job->done;
            snap.finished_at = job->finished_at;
            snap.specs.reserve(job->tasks.size());
            snap.results.reserve(job->tasks.size());
            snap.has_result.reserve(job->tasks.size());
            for (const auto& t : job->tasks) {
                snap.specs.push_back(t.spec);
                snap.has_result.push_back(t.has_result);
                if (t.has_result) snap.results.push_back(t.result);
                else snap.results.emplace_back();
            }
            snaps.push_back(std::move(snap));
        }
    }

    std::unordered_set<std::string> out;
    store::MerkleBuilder merkle(cas_);
    for (const auto& job : snaps) {
        const bool in_grace =
            job.done && job.finished_at && (now - *job.finished_at < g_finished_job_blob_grace);
        for (size_t i = 0; i < job.specs.size(); ++i) {
            if (!job.done) {
                ProtectTreeHashes(out, merkle, FromProto(job.specs[i].input_root()));
                ProtectMirrorRoot(out, merkle, job.specs[i]);
            }
            if (!job.has_result[i]) continue;
            if (!job.done || in_grace) ProtectTaskResultBlobs(out, merkle, job.results[i]);
        }
    }
    return out;
}

std::unordered_set<std::string> JobManager::ProtectedBlobHashes() const { return impl_->ProtectedBlobHashes(); }

void JobManager::SetFinishedJobBlobGracePeriodForTesting(std::chrono::seconds period) {
    g_finished_job_blob_grace = period;
}

void JobManager::SetClockForTesting(std::function<std::chrono::steady_clock::time_point()> now) {
    g_job_clock = std::move(now);
}

void JobManager::ResetProtectedBlobTestingHooks() {
    g_finished_job_blob_grace = std::chrono::minutes(10);
    g_job_clock = {};
}
std::shared_ptr<EventStream> JobManager::Watch(const std::string& job_id) { return impl_->Watch(job_id); }

} // namespace zeromesh::node
