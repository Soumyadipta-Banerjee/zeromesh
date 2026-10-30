#include "node/env_capture.hpp"

#include "exec/subprocess.hpp"
#include "node/convert.hpp"
#include "store/deny_list.hpp"
#include "store/stat_cache.hpp"
#include "store/store.hpp"

#include <elf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

namespace zeromesh::node {

namespace fs = std::filesystem;

namespace {

constexpr auto kProbeTimeout = std::chrono::minutes(5);
constexpr auto kListTimeout = std::chrono::seconds(30);

grpc::Status Invalid(const std::string& why) {
    return {grpc::StatusCode::INVALID_ARGUMENT, why};
}

// Decodes a string as strace prints it (C escapes: \" \\ \n \ooo \xHH ...) from `pos` up to the
// unescaped `end`. Returns the position after `end`, or npos when the string is cut short.
size_t Unescape(std::string_view s, size_t pos, char end, std::string* out) {
    out->clear();
    while (pos < s.size()) {
        const char c = s[pos++];
        if (c == end) return pos;
        if (c != '\\') {
            out->push_back(c);
            continue;
        }
        if (pos >= s.size()) break;
        const char e = s[pos++];
        if (e >= '0' && e <= '7') {
            int v = e - '0';
            for (int k = 0; k < 2 && pos < s.size() && s[pos] >= '0' && s[pos] <= '7'; ++k) v = v * 8 + (s[pos++] - '0');
            out->push_back(static_cast<char>(v));
        } else if (e == 'x' && pos + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[pos])) &&
                   std::isxdigit(static_cast<unsigned char>(s[pos + 1]))) {
            out->push_back(static_cast<char>(std::stoi(std::string(s.substr(pos, 2)), nullptr, 16)));
            pos += 2;
        } else {
            switch (e) {
            case 'n': out->push_back('\n'); break;
            case 't': out->push_back('\t'); break;
            case 'r': out->push_back('\r'); break;
            case 'v': out->push_back('\v'); break;
            case 'f': out->push_back('\f'); break;
            default: out->push_back(e); // \" \\ and the rest stand for themselves
            }
        }
    }
    return std::string_view::npos;
}

bool Under(const fs::path& p, const fs::path& dir) {
    if (dir.empty()) return false;
    auto [d, q] = std::mismatch(dir.begin(), dir.end(), p.begin(), p.end());
    return d == dir.end();
}

template <class Ehdr, class Phdr>
std::optional<std::string> ElfInterp(std::ifstream& in) {
    Ehdr eh{};
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(&eh), sizeof(eh))) return std::nullopt;
    if (eh.e_phentsize != sizeof(Phdr) || eh.e_phnum == 0 || eh.e_phnum > 4096) return std::nullopt;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Phdr ph{};
        in.seekg(static_cast<std::streamoff>(eh.e_phoff + i * sizeof(Phdr)));
        if (!in.read(reinterpret_cast<char*>(&ph), sizeof(ph))) return std::nullopt;
        if (ph.p_type != PT_INTERP) continue;
        if (ph.p_filesz == 0 || ph.p_filesz > 4096) return std::nullopt;
        std::string s(ph.p_filesz, '\0');
        in.seekg(static_cast<std::streamoff>(ph.p_offset));
        if (!in.read(s.data(), static_cast<std::streamsize>(s.size()))) return std::nullopt;
        s.resize(std::strlen(s.c_str()));
        if (s.empty() || s[0] != '/') return std::nullopt;
        return s;
    }
    return std::nullopt;
}

// The PT_INTERP of an ELF file in this machine's byte order, if it has one.
std::optional<std::string> ElfInterpreter(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    unsigned char ident[EI_NIDENT]{};
    if (!in.read(reinterpret_cast<char*>(ident), EI_NIDENT) || std::memcmp(ident, ELFMAG, SELFMAG) != 0) return std::nullopt;
    const unsigned char host = std::endian::native == std::endian::little ? ELFDATA2LSB : ELFDATA2MSB;
    if (ident[EI_DATA] != host) return std::nullopt;
    if (ident[EI_CLASS] == ELFCLASS64) return ElfInterp<Elf64_Ehdr, Elf64_Phdr>(in);
    if (ident[EI_CLASS] == ELFCLASS32) return ElfInterp<Elf32_Ehdr, Elf32_Phdr>(in);
    return std::nullopt;
}

// The shared libraries `program`'s loader resolves for it; names it cannot find go to `missing`.
std::vector<std::string> SharedLibraries(const std::string& loader, const fs::path& program,
                                         std::set<std::string>* missing) {
    std::vector<std::string> out;
    exec::CommandResult r = exec::RunCommand({loader, "--list", program.string()},
                                             {.cwd = {}, .timeout = kListTimeout, .max_output = 1 << 20});
    if (!r.ok()) return out;
    std::istringstream ss(r.output);
    for (std::string line; std::getline(ss, line);) {
        // "\tlibz.so.1 => /lib/x86_64-linux-gnu/libz.so.1 (0x...)", "\t/lib64/ld-linux-x86-64.so.2 (0x...)",
        // "\tlibfoo.so => not found"
        const auto arrow = line.find(" => ");
        std::string rest = arrow == std::string::npos ? line : line.substr(arrow + 4);
        rest.erase(0, rest.find_first_not_of(" \t"));
        if (rest.rfind("not found", 0) == 0) {
            std::string name = line.substr(0, arrow);
            name.erase(0, name.find_first_not_of(" \t"));
            missing->insert(name);
            continue;
        }
        const auto addr = rest.find(" (0x");
        if (rest.empty() || rest[0] != '/' || addr == std::string::npos) continue;
        out.push_back(rest.substr(0, addr));
    }
    return out;
}

// Regular files with an execute bit that are dynamically linked ELF programs, under `root`.
std::vector<std::pair<fs::path, std::string>> ProgramsUnder(const fs::path& root) {
    std::vector<std::pair<fs::path, std::string>> out;
    auto consider = [&](const fs::path& p) {
        struct stat st{};
        if (::lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0111) == 0) return;
        if (auto interp = ElfInterpreter(p)) out.emplace_back(p, *interp);
    };
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(root, ec))) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            consider(it->path());
        }
    } else {
        consider(root);
    }
    return out;
}

std::string Quoted(const google::protobuf::RepeatedPtrField<std::string>& argv) {
    std::string s;
    for (const auto& a : argv) s += (s.empty() ? "" : " ") + a;
    return "'" + s + "'";
}

std::string LastLine(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    const auto nl = text.rfind('\n');
    return nl == std::string::npos ? text : text.substr(nl + 1);
}

} // namespace

bool EnvTreeMounts(std::string_view name) {
    static constexpr std::string_view kDonorOwn[] = {"etc", "proc", "sys", "dev",  "tmp",
                                                     "run", "var",  "boot", "root", "lost+found"};
    if (name.empty() || name == "." || name == "..") return false;
    return std::find(std::begin(kDonorOwn), std::end(kDonorOwn), name) == std::end(kDonorOwn);
}

bool EnvTreeCarries(const fs::path& abs) {
    if (!abs.is_absolute()) return false;
    auto it = abs.begin();
    if (++it == abs.end()) return false; // "/" itself
    const std::string top = it->string();
    if (top != "etc") return EnvTreeMounts(top);
    for (const char* e : kEnvTreeEtcPaths) {
        if (Under(abs, e)) return true;
    }
    return false;
}

TracedFiles ParseStraceLog(std::string_view log) {
    TracedFiles out;
    std::set<std::string> seen_open, seen_exec;
    size_t start = 0;
    while (start < log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string_view::npos) end = log.size();
        const std::string_view line = log.substr(start, end - start);
        start = end + 1;

        size_t p = 0;
        while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p]))) ++p; // -f: "PID "
        while (p < line.size() && line[p] == ' ') ++p;
        const size_t paren = line.find('(', p);
        if (paren == std::string_view::npos) continue;
        const std::string_view name = line.substr(p, paren - p);
        const bool exec = name == "execve" || name == "execveat";
        const bool open = name == "open" || name == "openat" || name == "openat2";
        if (!exec && !open) continue;

        // The directory a relative path is resolved against: what -y printed for the dirfd.
        std::string base;
        size_t q = paren + 1;
        if (name == "openat" || name == "openat2" || name == "execveat") {
            const size_t comma = line.find(", \"", q);
            if (comma == std::string_view::npos) continue;
            const size_t lt = line.find('<', q);
            if (lt != std::string_view::npos && lt < comma && Unescape(line, lt + 1, '>', &base) == std::string_view::npos) {
                continue;
            }
            q = comma + 2;
        }
        if (q >= line.size() || line[q] != '"') continue;
        std::string path;
        const size_t after = Unescape(line, q + 1, '"', &path);
        if (after == std::string_view::npos || path.empty()) continue;

        if (open) {
            const size_t ret = line.rfind(") = ");
            const std::string_view flags = line.substr(after, ret == std::string_view::npos ? std::string_view::npos : ret - after);
            bool skip = false;
            for (const char* f : {"O_WRONLY", "O_RDWR", "O_CREAT", "O_TRUNC", "O_DIRECTORY"}) {
                skip = skip || flags.find(f) != std::string_view::npos;
            }
            if (skip) continue;
        }
        fs::path full = path;
        if (!full.is_absolute()) {
            if (base.empty() || base[0] != '/') continue;
            full = fs::path(base) / full;
        }
        std::string norm = full.lexically_normal().string();
        if (norm.size() > 1 && norm.back() == '/') norm.pop_back();
        if (exec) {
            if (seen_exec.insert(norm).second) out.executed.push_back(norm);
        } else if (seen_open.insert(norm).second) {
            out.opened.push_back(norm);
        }
    }
    return out;
}

std::vector<std::string> ExecLoaders(const fs::path& exe) {
    std::vector<std::string> out;
    fs::path cur = exe;
    for (int depth = 0; depth < 4; ++depth) {
        std::ifstream in(cur, std::ios::binary);
        char head[256]{};
        in.read(head, sizeof(head));
        const auto n = static_cast<size_t>(in.gcount());
        if (n >= 2 && head[0] == '#' && head[1] == '!') {
            std::string_view line(head + 2, n - 2);
            line = line.substr(0, line.find('\n'));
            const size_t b = line.find_first_not_of(" \t");
            if (b == std::string_view::npos) break;
            line = line.substr(b);
            const std::string interp(line.substr(0, line.find_first_of(" \t\r")));
            if (interp.empty() || interp[0] != '/') break;
            out.push_back(interp);
            cur = interp;
            continue;
        }
        if (auto interp = ElfInterpreter(cur)) out.push_back(*interp);
        break;
    }
    return out;
}

EnvCapturer::EnvCapturer(std::shared_ptr<store::MerkleBuilder> merkle, fs::path state_dir)
    : merkle_(std::move(merkle)), dir_(std::move(state_dir) / "captures") {}

grpc::Status EnvCapturer::Capture(const v1::CaptureRequest& req, v1::CaptureReply* reply) {
    if (req.probes_size() == 0 && req.paths_size() == 0) return Invalid("give a command to trace, paths, or both");
    for (const auto& probe : req.probes()) {
        if (probe.argv_size() == 0) return Invalid("a probe has an empty command");
    }
    const fs::path cwd = fs::path(req.cwd()).lexically_normal();
    std::error_code ec;
    if (!req.cwd().empty() && (!cwd.is_absolute() || !fs::is_directory(cwd, ec))) {
        return Invalid("the probe directory must be an absolute directory: '" + req.cwd() + "'");
    }

    std::lock_guard lock(mu_);
    std::vector<std::string> notes;
    std::set<std::string> wanted;

    // Paths given whole, and the libraries of the programs inside them.
    std::set<std::string> missing_libs;
    for (const auto& given : req.paths()) {
        const fs::path p = fs::path(given).lexically_normal();
        if (!p.is_absolute()) return Invalid("capture paths must be absolute: '" + given + "'");
        if (fs::symlink_status(p, ec).type() == fs::file_type::not_found) return Invalid("no such path: " + given);
        if (!EnvTreeCarries(p)) {
            return Invalid(given + " is never mounted from an environment; donors use their own /etc (except "
                                   "ld.so.cache, alternatives, ssl), /proc, /dev, /tmp, /run, /var, /boot and /root");
        }
        wanted.insert(p.string());
        for (const auto& [program, loader] : ProgramsUnder(p)) {
            wanted.insert(loader);
            for (auto& lib : SharedLibraries(loader, program, &missing_libs)) wanted.insert(std::move(lib));
        }
    }
    if (!missing_libs.empty()) {
        std::string list;
        for (const auto& m : missing_libs) list += (list.empty() ? "" : ", ") + m;
        notes.push_back("libraries no loader could find (not captured): " + list);
    }

    // Probes, traced.
    if (req.probes_size() > 0) {
        const std::string strace = exec::FindOnPath("strace");
        if (strace.empty()) {
            return {grpc::StatusCode::FAILED_PRECONDITION,
                    "tracing a command needs strace on this machine: install it (apt install strace), or capture "
                    "with paths only"};
        }
        fs::create_directories(dir_, ec);
        char tmpl[] = "/tmp/zm-capture-XXXXXX";
        if (::mkdtemp(tmpl) == nullptr) return {grpc::StatusCode::INTERNAL, "cannot create a scratch directory"};
        const fs::path scratch = tmpl;
        struct Cleanup {
            fs::path dir;
            ~Cleanup() {
                std::error_code e;
                fs::remove_all(dir, e);
            }
        } cleanup{scratch};
        const fs::path run_in = req.cwd().empty() ? scratch : cwd;
        // Files under the probe directory are the user's work, not the environment (unless that is "/").
        const fs::path workspace = run_in == "/" ? fs::path() : run_in;

        for (int i = 0; i < req.probes_size(); ++i) {
            const auto& probe = req.probes(i);
            std::string program = probe.argv(0);
            if (program.find('/') != std::string::npos && !fs::path(program).is_absolute()) program = (run_in / program).string();
            const std::string resolved = exec::FindOnPath(program);
            if (resolved.empty()) {
                return Invalid("probe program '" + probe.argv(0) +
                               "' is not executable here (zmd's PATH); give its absolute path");
            }
            const fs::path log = scratch / ("trace-" + std::to_string(i) + ".log");
            std::vector<std::string> argv = {strace,     "-f", "-qq", "-y", "-e", "signal=none", "-e",
                                             "status=successful", "-e", "trace=open,openat,openat2,execve,execveat",
                                             "-o",       log.string(), "--", resolved};
            for (int a = 1; a < probe.argv_size(); ++a) argv.push_back(probe.argv(a));
            exec::CommandResult r = exec::RunCommand(argv, {.cwd = run_in, .timeout = kProbeTimeout, .max_output = 64 << 10});
            if (!r.error.empty()) return {grpc::StatusCode::FAILED_PRECONDITION, "cannot run strace: " + r.error};
            if (r.timed_out) {
                return {grpc::StatusCode::DEADLINE_EXCEEDED, "probe " + Quoted(probe.argv()) + " did not finish in " +
                                                                 std::to_string(kProbeTimeout.count()) + " minutes"};
            }
            std::ifstream in(log, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (text.empty()) {
                return {grpc::StatusCode::FAILED_PRECONDITION,
                        "strace recorded nothing for " + Quoted(probe.argv()) + ": " + LastLine(r.output)};
            }
            if (r.exit_code != 0) {
                notes.push_back("probe " + Quoted(probe.argv()) + " exited with " + std::to_string(r.exit_code) +
                                " (" + LastLine(r.output) + "); what it used is captured");
            }
            const TracedFiles traced = ParseStraceLog(text);
            auto keep = [&](const std::string& p) {
                const fs::path f = p;
                if (!EnvTreeCarries(f) || Under(f, workspace) || Under(f, scratch)) return false;
                return fs::is_regular_file(f, ec);
            };
            for (const auto& f : traced.opened) {
                if (keep(f)) wanted.insert(f);
            }
            for (const auto& f : traced.executed) {
                if (!keep(f)) continue;
                wanted.insert(f);
                for (auto& l : ExecLoaders(f)) wanted.insert(std::move(l));
            }
        }
    }

    // The tree. Its stat cache belongs to this request, so an unchanged capture rereads nothing.
    fs::create_directories(dir_, ec);
    const std::string key = store::CasStore::ComputeDigest(req.SerializeAsString()).hash;
    store::StatCache stat_cache(dir_ / (key + ".stat"));
    const store::IgnoreRules deny = store::DenyRules();
    std::vector<std::string> denied;
    uint64_t bytes = 0, files = 0;
    store::TreeOptions opts;
    opts.deny = &deny;
    opts.denied = &denied;
    opts.stat_cache = &stat_cache;
    opts.total_bytes = &bytes;
    opts.file_count = &files;
    store::Digest tree;
    try {
        tree = merkle_->BuildFromPaths(std::vector<fs::path>(wanted.begin(), wanted.end()), opts);
    } catch (const std::exception& e) {
        return {grpc::StatusCode::INTERNAL, std::string("cannot store the environment: ") + e.what()};
    }
    stat_cache.Save();

    const fs::path last_file = dir_ / (key + ".tree");
    std::string last;
    std::ifstream(last_file) >> last;
    std::ofstream(last_file, std::ios::trunc) << tree.hash << "\n";

    *reply->mutable_tree() = ToProto(tree);
    reply->set_files(static_cast<int64_t>(files));
    reply->set_bytes(static_cast<int64_t>(bytes));
    std::sort(denied.begin(), denied.end());
    for (const auto& d : denied) reply->add_denied(d);
    reply->set_reused(last == tree.hash);
    std::string note;
    for (const auto& n : notes) note += (note.empty() ? "" : "; ") + n;
    reply->set_note(note);
    return grpc::Status::OK;
}

} // namespace zeromesh::node
