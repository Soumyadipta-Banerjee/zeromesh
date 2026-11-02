#include "node/version_probe.hpp"

#include "exec/toolchain_digest.hpp"

namespace zeromesh::node {

VersionProber::VersionProber(std::shared_ptr<exec::BatchRunner> runner, std::shared_ptr<plugin::Registry> registry,
                             std::string node_id)
    : runner_(std::move(runner)), registry_(std::move(registry)), node_id_(std::move(node_id)) {}

std::optional<std::string> VersionProber::operator()(const v1::Program& program, const std::string& resolved) {
    std::vector<std::string> argv(program.probe().begin(), program.probe().end());
    if (argv.empty()) argv = {resolved, "--version"};
    std::string key = exec::ToolchainDigest(resolved, "");
    for (const auto& a : argv) key += "\x1f" + a;
    {
        std::lock_guard lock(mu_);
        if (auto it = seen_.find(key); it != seen_.end()) return it->second;
    }
    std::optional<std::string> out;
    plugin::IEnvProvider* host = registry_ ? registry_->Env("host") : nullptr;
    if (host && runner_) {
        auto prepared = host->Prepare(plugin::BatchContext{});
        if (prepared.value) {
            exec::BatchRunSpec spec;
            spec.sandbox_env = std::move(*prepared.value);
            spec.timeout = std::chrono::seconds(20);
            spec.term_grace = std::chrono::seconds(1);
            spec.node_id = node_id_;
            spec.cores = 1;
            exec::ItemSpec item;
            item.id = "version-probe";
            item.argv = argv;
            spec.items.push_back(std::move(item));
            exec::BatchCallbacks cb;
            cb.on_item = [&out](exec::ItemRunResult r) {
                if (r.exit_code == 0) out = r.stdout_tail.empty() ? r.stderr_tail : r.stdout_tail;
            };
            runner_->Run(spec, cb, {});
        }
    }
    std::lock_guard lock(mu_);
    seen_[key] = out;
    return out;
}

} // namespace zeromesh::node
