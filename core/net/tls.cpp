#include "net/tls.hpp"

#include <grpc/grpc.h>
#include <grpc/grpc_security_constants.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/auth_context.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_certificate_verifier.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <grpcpp/support/channel_arguments.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace zeromesh::net {

namespace {

using grpc::experimental::ExternalCertificateVerifier;
using grpc::experimental::InMemoryCertificateProvider;
using grpc::experimental::TlsCustomVerificationCheckRequest;

std::shared_ptr<InMemoryCertificateProvider> IdentityProvider(const Identity& self) {
    auto provider = std::make_shared<InMemoryCertificateProvider>();
    std::vector<grpc::experimental::IdentityKeyOrSignerCertPair> pairs(1);
    pairs[0].private_key = self.key_pem;
    pairs[0].certificate_chain = self.cert_pem;
    absl::Status st = provider->UpdateIdentityKeyCertPair(std::move(pairs));
    if (!st.ok()) throw std::runtime_error("tls: bad node identity: " + std::string(st.message()));
    return provider;
}

// Accepts the server iff accept(fingerprint of its leaf certificate).
class FingerprintVerifier final : public ExternalCertificateVerifier {
public:
    explicit FingerprintVerifier(std::function<bool(std::string_view)> accept) : accept_(std::move(accept)) {}

    bool Verify(TlsCustomVerificationCheckRequest* request, std::function<void(grpc::Status)> /*callback*/,
                grpc::Status* sync_status) override {
        grpc::string_ref pem = request->peer_cert();
        std::string fp = FingerprintOfPem(std::string_view(pem.data(), pem.size()));
        if (fp.empty()) {
            *sync_status = grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "peer sent no usable certificate");
        } else if (!accept_(fp)) {
            *sync_status = grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "peer certificate " + fp + " is not pinned");
        } else {
            *sync_status = grpc::Status::OK;
        }
        return true; // synchronous
    }

    void Cancel(TlsCustomVerificationCheckRequest* /*request*/) override {}

private:
    std::function<bool(std::string_view)> accept_;
};

} // namespace

std::shared_ptr<grpc::ServerCredentials> ServerCreds(const Identity& self) {
    auto options = grpc::experimental::TlsServerCredentialsOptions::Create(IdentityProvider(self));
    if (!options.ok()) throw std::runtime_error("tls: " + std::string(options.status().message()));
    options->set_cert_request_type(GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY);
    // Authorisation is the pin check in NodeService, which needs Admit from unpinned peers.
    options->set_certificate_verifier(std::make_shared<grpc::experimental::NoOpCertificateVerifier>());
    options->set_min_tls_version(grpc_tls_version::TLS1_3);
    return grpc::experimental::TlsServerCredentials(*options);
}

std::shared_ptr<grpc::ChannelCredentials> ClientCreds(const Identity& self,
                                                      std::function<bool(std::string_view server_fp)> accept) {
    grpc::experimental::TlsChannelCredentialsOptions options;
    options.set_identity_certificate_provider(IdentityProvider(self));
    options.set_verify_server_certs(false);
    options.set_check_call_host(false);
    options.set_certificate_verifier(ExternalCertificateVerifier::Create<FingerprintVerifier>(std::move(accept)));
    options.set_min_tls_version(grpc_tls_version::TLS1_3);
    return grpc::experimental::TlsCredentials(options);
}

std::shared_ptr<grpc::Channel> PeerChannel(const std::string& address,
                                           std::shared_ptr<grpc::ChannelCredentials> creds) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    return grpc::CreateCustomChannel(address, std::move(creds), args);
}

std::string PeerFingerprint(const grpc::ServerContext& ctx) {
    std::shared_ptr<const grpc::AuthContext> auth = ctx.auth_context();
    if (!auth) return {};
    std::vector<grpc::string_ref> pems = auth->FindPropertyValues(GRPC_X509_PEM_CERT_PROPERTY_NAME);
    if (pems.empty()) return {};
    return FingerprintOfPem(std::string_view(pems.front().data(), pems.front().size()));
}

} // namespace zeromesh::net
