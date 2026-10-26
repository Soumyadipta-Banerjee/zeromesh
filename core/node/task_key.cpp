#include "node/task_key.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

namespace zeromesh::node {

namespace {

// Length-prefixed fields: no delimiter can make two different specs collide.
class KeyWriter {
public:
    void Str(const std::string& s) {
        uint64_t n = s.size();
        buf_.append(reinterpret_cast<const char*>(&n), sizeof(n));
        buf_.append(s);
    }
    void Num(int64_t v) { buf_.append(reinterpret_cast<const char*>(&v), sizeof(v)); }
    void Tag(char t) { buf_.push_back(t); }
    const std::string& bytes() const { return buf_; }

private:
    std::string buf_;
};

std::string Sha256Hex(const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr);
    std::ostringstream ss;
    for (unsigned int i = 0; i < len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(md[i]);
    }
    return ss.str();
}

template <class Map>
void SortedMap(KeyWriter& w, const Map& m) {
    std::map<std::string, std::string> sorted(m.begin(), m.end());
    w.Num(static_cast<int64_t>(sorted.size()));
    for (const auto& [k, v] : sorted) {
        w.Str(k);
        w.Str(v);
    }
}

} // namespace

void Normalize(v1::TaskSpec& spec) {
    auto* p = spec.mutable_policy();
    if (!p->has_may_run_local()) p->set_may_run_local(true);
    if (!p->has_may_run_remote()) p->set_may_run_remote(true);
    if (!p->has_max_retries()) p->set_max_retries(3);
    if (!p->has_timeout_s() || p->timeout_s() <= 0) p->set_timeout_s(3600);
    if (!p->has_allow_network()) p->set_allow_network(false);
    if (!p->has_preemptible()) p->set_preemptible(true);
    if (spec.working_dir().empty()) spec.set_working_dir(".");
    if (spec.env_spec().name().empty()) spec.mutable_env_spec()->set_name("host");
}

std::string TaskKey(const v1::TaskSpec& in) {
    v1::TaskSpec spec = in;
    Normalize(spec);

    KeyWriter w;
    w.Tag('A');
    w.Num(spec.argv_size());
    for (const auto& a : spec.argv()) w.Str(a);
    w.Tag('E');
    SortedMap(w, spec.env());
    w.Tag('I');
    w.Str(spec.input_root().hash());
    w.Num(spec.input_root().size_bytes());
    w.Tag('W');
    w.Str(spec.working_dir());
    w.Tag('O');
    std::vector<std::string> outputs(spec.outputs().begin(), spec.outputs().end());
    std::sort(outputs.begin(), outputs.end());
    w.Num(static_cast<int64_t>(outputs.size()));
    for (const auto& o : outputs) w.Str(o);
    w.Tag('C');
    w.Num(spec.capture_changed() ? 1 : 0);
    w.Tag('V');
    w.Str(spec.env_spec().name());
    SortedMap(w, spec.env_spec().params());
    w.Tag('R');
    w.Str(spec.resources().arch());
    w.Tag('N');
    w.Num(spec.policy().allow_network() ? 1 : 0);
    return Sha256Hex(w.bytes());
}

} // namespace zeromesh::node
