#pragma once
namespace luisa::compute::graph{
struct GraphDependency {
    union {
        struct {
            uint64_t src;
            uint64_t dst;
        };
        uint64_t id[2];
    };
};
}// namespace luisa::compute::graph