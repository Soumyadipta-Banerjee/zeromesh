#pragma once

// mTLS for NodeApi (spec §9). Certificates are self-signed, so no chain is ever
// verified: the server requires a client certificate and the NodeApi service
// authorises by its pinned fingerprint; the client decides whether to accept
// the server's leaf fingerprint through `accept` during the handshake.

#include "net/identity.hpp"

#include <grpcpp/channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace zeromesh::net {

// TLS 1.3 server credentials presenting `self`; a client certificate is requested
// and required but not chain-verified (GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY).
std::shared_ptr<grpc::ServerCredentials> ServerCreds(const Identity& self);

// TLS 1.3 client credentials presenting `self`. Server chain and host-name checks are
// off; instead the handshake fails unless accept(<server leaf fingerprint>) is true.
// `accept` may run on any gRPC thread and must be thread-safe.
std::shared_ptr<grpc::ChannelCredentials> ClientCreds(const Identity& self,
                                                      std::function<bool(std::string_view server_fp)> accept);

// Transport keepalive for NodeApi: the client pings every 5 s and declares the
// connection dead when a ping goes unanswered for 10 s, so a vanished peer breaks
// long-running streams (RunTask) instead of hanging them.
inline constexpr int kKeepaliveTimeMs = 5000;
inline constexpr int kKeepaliveTimeoutMs = 10000;

// A channel to "ip:port" with its own subchannel pool, so connections made under
// different credentials (admission vs pinned) are never shared. With `keepalive`,
// the channel sends the pings above, also while no call is active.
std::shared_ptr<grpc::Channel> PeerChannel(const std::string& address,
                                           std::shared_ptr<grpc::ChannelCredentials> creds,
                                           bool keepalive = false);

// Server arguments that accept the client keepalive pings above (by default a gRPC
// server sends GOAWAY to clients pinging this often). Every NodeApi server uses this.
void AllowKeepalivePings(grpc::ServerBuilder& builder);

// Fingerprint of the caller's TLS client certificate, or "" if there is none.
std::string PeerFingerprint(const grpc::ServerContext& ctx);

} // namespace zeromesh::net
