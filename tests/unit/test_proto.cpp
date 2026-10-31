#include "tests/unit/test_framework.hpp"
#include "zeromesh/v1/run.pb.h"

#include <google/protobuf/util/json_util.h>

TEST(ProtoRunSpecRoundTripsThroughJson) {
    // run.json, the format of `zm run --from`, is the JSON mapping of RunSpec.
    const std::string json = R"({
        "name": "sweep",
        "workspace_path": "cases",
        "stages": [{"name": "", "kind": "sim", "env": {"programs": [{"name": "sim", "version": ">=2"}]},
                    "resources": {"cores": 2}}],
        "items": [{"id": "case-1", "argv": ["sim", "case-1"], "cwd": "case-1",
                   "outputs": [{"path": "result.csv", "dest": "/tmp/out/case-1.csv"}]}],
        "policy": {"keep_going": true, "exit_retries": 1}
    })";
    zeromesh::v1::RunSpec spec;
    ASSERT_TRUE(google::protobuf::util::JsonStringToMessage(json, &spec).ok());
    ASSERT_EQ(spec.items(0).outputs(0).dest(), std::string("/tmp/out/case-1.csv"));
    ASSERT_EQ(spec.stages(0).env().programs(0).version(), std::string(">=2"));
    ASSERT_TRUE(spec.policy().has_keep_going() && spec.policy().keep_going());
    ASSERT_FALSE(spec.policy().has_cache()); // unset stays unset: the engine fills defaults

    std::string wire;
    ASSERT_TRUE(spec.SerializeToString(&wire));
    zeromesh::v1::RunSpec back;
    ASSERT_TRUE(back.ParseFromString(wire));
    ASSERT_EQ(back.items(0).argv(1), std::string("case-1"));
    ASSERT_EQ(back.stages(0).resources().cores(), 2);
}
