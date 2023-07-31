#pragma once
#include <cstdint>
namespace luisa::compute::tensor {
class DenseVectorView {
public:
    uint64_t buffer_handle;
    size_t buffer_offset;
    size_t n;
    size_t inc;
};
}// namespace luisa::compute::tensor