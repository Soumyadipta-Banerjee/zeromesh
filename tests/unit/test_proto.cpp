#include "tests/unit/test_framework.hpp"
#include "zeromesh/v1/common.pb.h"

TEST(ProtoTaskSpecRoundTrip) {
    zeromesh::v1::TaskSpec spec;
    spec.set_name("t1");
    spec.add_argv("sh");
    spec.add_argv("-c");
    spec.add_argv("echo hi");
    spec.mutable_env_spec()->set_name("host");
    spec.add_outputs("out.txt");

    std::string wire;
    ASSERT_TRUE(spec.SerializeToString(&wire));

    zeromesh::v1::TaskSpec back;
    ASSERT_TRUE(back.ParseFromString(wire));
    ASSERT_EQ(back.name(), std::string("t1"));
    ASSERT_EQ(back.argv_size(), 3);
    ASSERT_EQ(back.argv(2), std::string("echo hi"));
    ASSERT_EQ(back.env_spec().name(), std::string("host"));
    ASSERT_EQ(back.outputs(0), std::string("out.txt"));
}
