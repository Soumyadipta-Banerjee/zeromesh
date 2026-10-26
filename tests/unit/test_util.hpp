#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace zeromesh::testing {

// A fresh directory under $TMPDIR, removed when the object goes out of scope.
class TempDir {
public:
    TempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "zmtest-XXXXXX").string();
        char* made = mkdtemp(tmpl.data());
        if (made == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = made;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& rel) const { return path_ / rel; }

private:
    std::filesystem::path path_;
};

inline void WriteFile(const std::filesystem::path& p, const std::string& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
}

inline std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace zeromesh::testing
