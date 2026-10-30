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
using grpc::experimental::IdentityKeyCertPair;
using grpc::experimental::StaticDataCertificateProvider;
using grpc::experimental::TlsCustomVerificationCheckRequest;

std::shared_ptr<StaticDataCertificateProvider> IdentityProvider(const Identity& self) {
    std::vector<IdentityKeyCertPair> pairs(1);
    pairs[0].private_key = self.key_pem;
    pairs[0].certificate_chain = self.cert_pem;
    return std::make_shared<StaticDataCertificateProvider>(pairs);
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
    auto provider = IdentityProvider(self);
    grpc::experimental::TlsServerCredentialsOptions options(provider);
    options.watch_identity_key_cert_pairs();
    options.set_cert_request_type(GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY);
    // Authorisation is the pin check in NodeService, which needs Admit from unpinned peers.
    options.set_certificate_verifier(std::make_shared<grpc::experimental::NoOpCertificateVerifier>());
    return grpc::experimental::TlsServerCredentials(options);
}

std::shared_ptr<grpc::ChannelCredentials> ClientCreds(const Identity& self,
                                                      std::function<bool(std::string_view server_fp)> accept) {
    grpc::experimental::TlsChannelCredentialsOptions options;
    options.set_certificate_provider(IdentityProvider(self));
    options.watch_identity_key_cert_pairs();
    options.set_verify_server_certs(false);
    options.set_check_call_host(false);
    options.set_certificate_verifier(ExternalCertificateVerifier::Create<FingerprintVerifier>(std::move(accept)));
    return grpc::experimental::TlsCredentials(options);
}

std::shared_ptr<grpc::Channel> PeerChannel(const std::string& address, std::shared_ptr<grpc::ChannelCredentials> creds,
                                           bool keepalive) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    if (keepalive) {
        args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, kKeepaliveTimeMs);
        args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, kKeepaliveTimeoutMs);
        args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    }
    return grpc::CreateCustomChannel(address, std::move(creds), args);
}

void AllowKeepalivePings(grpc::ServerBuilder& builder) {
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, kKeepaliveTimeMs / 2);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PING_STRIKES, 0);
}

std::string PeerFingerprint(const grpc::ServerContext& ctx) {
    std::shared_ptr<const grpc::AuthContext> auth = ctx.auth_context();
    if (!auth) return {};
    std::vector<grpc::string_ref> pems = auth->FindPropertyValues(GRPC_X509_PEM_CERT_PROPERTY_NAME);
    if (pems.empty()) return {};
    return FingerprintOfPem(std::string_view(pems.front().data(), pems.front().size()));
}

} // namespace zeromesh::net
