#pragma once
#include <luisa/runtime/graph/graph_var_id.h>
namespace luisa::compute::graph{
struct GraphDependency {
    union {
        struct {
            GraphInputVarId src;
            GraphInputVarId dst;
        };
        GraphInputVarId id[2];
    };
};
}// namespace luisa::compute::graph