#pragma once

#include "store/store.hpp"
#include "zeromesh/v1/common.pb.h"

namespace zeromesh::node {

inline v1::Digest ToProto(const store::Digest& d) {
    v1::Digest out;
    out.set_hash(d.hash);
    out.set_size_bytes(d.size_bytes);
    return out;
}

inline store::Digest FromProto(const v1::Digest& d) { return store::Digest{d.hash(), d.size_bytes()}; }

} // namespace zeromesh::node
