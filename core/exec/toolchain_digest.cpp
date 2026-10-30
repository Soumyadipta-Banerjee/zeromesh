#include "exec/toolchain_digest.hpp"

#include <elf.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace zeromesh::exec {

namespace {

namespace fs = std::filesystem;

constexpr int kMaxInterpreterDepth = 4;
constexpr size_t kMaxLibraries = 512;
constexpr size_t kMaxShebangBytes = 256;
constexpr size_t kReadChunkBytes = size_t{64} * 1024;

constexpr const char* kDefaultLibDirs[] = {
    "/lib64", "/usr/lib64", "/lib", "/usr/lib", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/usr/local/lib",
};

std::string Hex(const unsigned char* md, unsigned int len) {
    std::ostringstream ss;
    for (unsigned int i = 0; i < len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(md[i]);
    return ss.str();
}

// Streaming hash with a per-process cache keyed on (path, size, mtime): toolchain files are large
// and hashed once per task otherwise.
std::string HashFile(const fs::path& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return "";

    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    std::ostringstream key;
    key << path.string() << '\0' << st.st_size << '\0' << st.st_mtim.tv_sec << '.' << st.st_mtim.tv_nsec;
    {
        std::lock_guard lock(mu);
        if (auto it = cache.find(key.str()); it != cache.end()) return it->second;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    char buf[kReadChunkBytes];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) EVP_DigestUpdate(ctx, buf, static_cast<size_t>(in.gcount()));
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);

    std::string hex = Hex(md, len);
    std::lock_guard lock(mu);
    cache[key.str()] = hex;
    return hex;
}

bool ReadAt(std::ifstream& in, uint64_t offset, void* dst, size_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    return static_cast<bool>(in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size)));
}

struct ElfInfo {
    std::string interp;
    std::vector<std::string> needed;
    std::vector<std::string> runpath;
};

std::string CStringAt(const std::string& table, uint64_t offset) {
    if (offset >= table.size()) return "";
    return std::string(table.c_str() + offset);
}

// Little-endian ELF64 only; anything else is hashed as an opaque file.
bool ReadElf(const fs::path& path, ElfInfo& info) {
    std::ifstream in(path, std::ios::binary);
    Elf64_Ehdr eh{};
    if (!in || !ReadAt(in, 0, &eh, sizeof(eh))) return false;
    if (std::string_view(reinterpret_cast<const char*>(eh.e_ident), SELFMAG) != ELFMAG) return false;
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) return false;

    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph{};
        if (!ReadAt(in, eh.e_phoff + static_cast<uint64_t>(i) * eh.e_phentsize, &ph, sizeof(ph))) return false;
        if (ph.p_type != PT_INTERP || ph.p_filesz == 0 || ph.p_filesz > PATH_MAX) continue;
        std::string name(ph.p_filesz, '\0');
        if (ReadAt(in, ph.p_offset, name.data(), name.size())) info.interp = name.c_str();
    }

    for (uint16_t i = 0; i < eh.e_shnum; ++i) {
        Elf64_Shdr sh{};
        if (!ReadAt(in, eh.e_shoff + static_cast<uint64_t>(i) * eh.e_shentsize, &sh, sizeof(sh))) return false;
        if (sh.sh_type != SHT_DYNAMIC || sh.sh_link >= eh.e_shnum) continue;

        Elf64_Shdr str{};
        if (!ReadAt(in, eh.e_shoff + static_cast<uint64_t>(sh.sh_link) * eh.e_shentsize, &str, sizeof(str)))
            return false;
        std::string strtab(str.sh_size, '\0');
        if (!ReadAt(in, str.sh_offset, strtab.data(), strtab.size())) return false;

        std::vector<Elf64_Dyn> dyn(sh.sh_size / sizeof(Elf64_Dyn));
        if (!ReadAt(in, sh.sh_offset, dyn.data(), dyn.size() * sizeof(Elf64_Dyn))) return false;
        for (const auto& d : dyn) {
            if (d.d_tag == DT_NULL) break;
            if (d.d_tag == DT_NEEDED) info.needed.push_back(CStringAt(strtab, d.d_un.d_val));
            if (d.d_tag == DT_RUNPATH || d.d_tag == DT_RPATH) {
                std::stringstream ss(CStringAt(strtab, d.d_un.d_val));
                for (std::string dir; std::getline(ss, dir, ':');) {
                    if (!dir.empty()) info.runpath.push_back(dir);
                }
            }
        }
    }
    return true;
}

fs::path FindLibrary(const std::string& soname, const fs::path& origin_dir, const std::vector<std::string>& runpath) {
    std::vector<fs::path> dirs;
    for (std::string dir : runpath) {
        if (const auto pos = dir.find("$ORIGIN"); pos != std::string::npos) dir.replace(pos, 7, origin_dir.string());
        dirs.emplace_back(dir);
    }
    for (const char* dir : kDefaultLibDirs) dirs.emplace_back(dir);

    for (const auto& dir : dirs) {
        std::error_code ec;
        fs::path candidate = fs::canonical(dir / soname, ec);
        if (!ec && fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

fs::path ResolveExecutable(const std::string& exe, const std::string& path_env) {
    std::vector<fs::path> candidates;
    if (exe.find('/') != std::string::npos) {
        candidates.emplace_back(exe);
    } else {
        std::stringstream ss(path_env);
        for (std::string dir; std::getline(ss, dir, ':');) {
            if (!dir.empty()) candidates.emplace_back(fs::path(dir) / exe);
        }
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        fs::path real = fs::canonical(c, ec);
        if (!ec && fs::is_regular_file(real, ec) && access(real.c_str(), X_OK) == 0) return real;
    }
    return {};
}

// Interpreter named on a `#!` line, or "" when `path` is not a script.
std::string ShebangInterpreter(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string head(kMaxShebangBytes, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    if (head.rfind("#!", 0) != 0) return "";
    std::istringstream line(
        head.substr(2, head.find('\n') == std::string::npos ? std::string::npos : head.find('\n') - 2));
    std::string interp;
    line >> interp;
    return interp;
}

// Adds "<label>" -> content hash for the program and everything it loads.
void Collect(const fs::path& real, const std::string& prefix, int depth, std::map<std::string, std::string>& out) {
    out[prefix + "exe"] = HashFile(real);

    if (depth < kMaxInterpreterDepth) {
        if (const std::string interp = ShebangInterpreter(real); !interp.empty()) {
            if (fs::path resolved = ResolveExecutable(interp, ""); !resolved.empty()) {
                Collect(resolved, prefix + "interp/", depth + 1, out);
            } else {
                out[prefix + "interp/unresolved:" + interp] = "";
            }
        }
    }

    ElfInfo root;
    if (!ReadElf(real, root)) return;
    if (!root.interp.empty()) out[prefix + "loader"] = HashFile(root.interp);

    std::deque<std::pair<fs::path, ElfInfo>> queue;
    queue.emplace_back(real, std::move(root));
    std::set<std::string> seen;
    while (!queue.empty() && seen.size() < kMaxLibraries) {
        auto [object, info] = std::move(queue.front());
        queue.pop_front();
        for (const auto& soname : info.needed) {
            if (!seen.insert(soname).second) continue;
            const fs::path lib = FindLibrary(soname, object.parent_path(), info.runpath);
            std::string label = prefix;
            label.append("lib/").append(soname);
            out[label] = lib.empty() ? "" : HashFile(lib);
            ElfInfo child;
            if (!lib.empty() && ReadElf(lib, child)) queue.emplace_back(lib, std::move(child));
        }
    }
}

} // namespace

std::string ToolchainDigest(const std::string& exe, const std::string& path_env) {
    if (exe.empty()) return "";
    const fs::path real = ResolveExecutable(exe, path_env);
    if (real.empty()) return "";

    std::map<std::string, std::string> entries;
    Collect(real, "", 0, entries);

    std::string material;
    for (const auto& [label, hash] : entries)
        material.append(label).push_back('\0'), material.append(hash).push_back('\n');

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(material.data(), material.size(), md, &len, EVP_sha256(), nullptr);
    return Hex(md, len);
}

} // namespace zeromesh::exec
