#include "net/identity.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace zeromesh::net {

namespace {

namespace fs = std::filesystem;

constexpr int kValidityDays = 3650;
constexpr int kSerialBytes = 16;

struct BioFree {
    void operator()(BIO* b) const { BIO_free(b); }
};
struct X509Free {
    void operator()(X509* x) const { X509_free(x); }
};
struct PkeyFree {
    void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
};
struct BnFree {
    void operator()(BIGNUM* b) const { BN_free(b); }
};
struct ExtFree {
    void operator()(X509_EXTENSION* e) const { X509_EXTENSION_free(e); }
};
using BioPtr = std::unique_ptr<BIO, BioFree>;
using X509Ptr = std::unique_ptr<X509, X509Free>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyFree>;
using BnPtr = std::unique_ptr<BIGNUM, BnFree>;
using ExtPtr = std::unique_ptr<X509_EXTENSION, ExtFree>;

[[noreturn]] void SslFail(const std::string& what) {
    throw std::runtime_error("identity: " + what);
}

void Check(int ok, const char* what) {
    if (ok != 1) SslFail(what);
}

BioPtr MemBio(std::string_view data) {
    BioPtr bio(BIO_new_mem_buf(data.data(), static_cast<int>(data.size())));
    if (!bio) SslFail("BIO_new_mem_buf");
    return bio;
}

std::string BioString(BIO* bio) {
    char* p = nullptr;
    long n = BIO_get_mem_data(bio, &p);
    return n > 0 ? std::string(p, static_cast<size_t>(n)) : std::string();
}

X509Ptr ParseCert(std::string_view pem) {
    if (pem.empty()) return nullptr;
    BioPtr bio = MemBio(pem);
    return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

std::string Fingerprint(X509* cert) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (X509_digest(cert, EVP_sha256(), md, &len) != 1 || len != 32) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned i = 0; i < len; ++i) {
        out.push_back(kHex[md[i] >> 4]);
        out.push_back(kHex[md[i] & 0x0f]);
    }
    return out;
}

std::string CommonName(X509* cert) {
    char buf[256];
    int n = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buf, sizeof(buf));
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

void AddExtension(X509* cert, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    ExtPtr ext(X509V3_EXT_conf_nid(nullptr, &ctx, nid, value));
    if (!ext) SslFail("X509V3_EXT_conf_nid");
    Check(X509_add_ext(cert, ext.get(), -1), "X509_add_ext");
}

Identity Generate(std::string_view node_id) {
    PkeyPtr key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (!key) SslFail("EC P-256 key generation failed");

    X509Ptr cert(X509_new());
    if (!cert) SslFail("X509_new");
    Check(X509_set_version(cert.get(), X509_VERSION_3), "X509_set_version");

    // Random positive, non-zero serial (RFC 5280 §4.1.2.2: at most 20 octets).
    unsigned char serial[kSerialBytes];
    Check(RAND_bytes(serial, sizeof(serial)), "RAND_bytes");
    serial[0] = static_cast<unsigned char>((serial[0] & 0x7f) | 0x01);
    BnPtr bn(BN_bin2bn(serial, sizeof(serial), nullptr));
    if (!bn || !BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(cert.get()))) SslFail("serial");

    if (!X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) ||
        !X509_time_adj_ex(X509_getm_notAfter(cert.get()), kValidityDays, 0, nullptr)) {
        SslFail("validity");
    }

    X509_NAME* name = X509_get_subject_name(cert.get());
    Check(X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8,
                                     reinterpret_cast<const unsigned char*>(node_id.data()),
                                     static_cast<int>(node_id.size()), -1, 0),
          "subject CN");
    Check(X509_set_issuer_name(cert.get(), name), "issuer");
    Check(X509_set_pubkey(cert.get(), key.get()), "X509_set_pubkey");
    AddExtension(cert.get(), NID_basic_constraints, "critical,CA:FALSE");
    AddExtension(cert.get(), NID_key_usage, "critical,digitalSignature");
    AddExtension(cert.get(), NID_ext_key_usage, "serverAuth,clientAuth");
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) SslFail("X509_sign");

    BioPtr cert_bio(BIO_new(BIO_s_mem()));
    BioPtr key_bio(BIO_new(BIO_s_mem()));
    if (!cert_bio || !key_bio) SslFail("BIO_new");
    Check(PEM_write_bio_X509(cert_bio.get(), cert.get()), "PEM_write_bio_X509");
    Check(PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr),
          "PEM_write_bio_PrivateKey");

    Identity id;
    id.node_id = std::string(node_id);
    id.cert_pem = BioString(cert_bio.get());
    id.key_pem = BioString(key_bio.get());
    id.fingerprint = Fingerprint(cert.get());
    if (id.fingerprint.empty()) SslFail("fingerprint");
    return id;
}

std::optional<std::string> ReadFileIfExists(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

bool IsFingerprint(std::string_view fp) {
    if (fp.size() != 64) return false;
    for (char c : fp) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool IsValidNodeId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
                  c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string FingerprintOfPem(std::string_view pem) {
    X509Ptr cert = ParseCert(pem);
    return cert ? Fingerprint(cert.get()) : std::string();
}

void WritePrivateFileAtomic(const fs::path& path, std::string_view content) {
    const fs::path tmp = path.string() + ".tmp";
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "create " + tmp.string());
    bool ok = ::fchmod(fd, 0600) == 0;
    size_t off = 0;
    while (ok && off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0)
            ok = false;
        else
            off += static_cast<size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    int saved = errno;
    ::close(fd);
    if (ok && ::rename(tmp.c_str(), path.c_str()) != 0) {
        ok = false;
        saved = errno;
    }
    if (!ok) {
        ::unlink(tmp.c_str());
        throw std::system_error(saved, std::generic_category(), "write " + path.string());
    }
}

Identity Identity::LoadOrCreate(const fs::path& state_dir, std::string_view node_id) {
    if (!IsValidNodeId(node_id)) throw std::invalid_argument("identity: invalid node id");
    fs::create_directories(state_dir);
    const fs::path cert_path = state_dir / "node.crt";
    const fs::path key_path = state_dir / "node.key";

    auto cert_pem = ReadFileIfExists(cert_path);
    auto key_pem = ReadFileIfExists(key_path);
    if (cert_pem && key_pem) {
        X509Ptr cert = ParseCert(*cert_pem);
        if (!cert) SslFail("malformed " + cert_path.string());
        BioPtr bio = MemBio(*key_pem);
        PkeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
        if (!key) SslFail("malformed " + key_path.string());
        if (X509_check_private_key(cert.get(), key.get()) != 1) {
            SslFail(key_path.string() + " does not match " + cert_path.string());
        }
        if (CommonName(cert.get()) == node_id) {
            ::chmod(key_path.c_str(), 0600); // tighten a key file someone loosened
            Identity id;
            id.node_id = std::string(node_id);
            id.cert_pem = std::move(*cert_pem);
            id.key_pem = std::move(*key_pem);
            id.fingerprint = Fingerprint(cert.get());
            if (id.fingerprint.empty()) SslFail("fingerprint");
            return id;
        }
    }

    Identity id = Generate(node_id);
    WritePrivateFileAtomic(key_path, id.key_pem);
    WritePrivateFileAtomic(cert_path, id.cert_pem);
    return id;
}

} // namespace zeromesh::net
