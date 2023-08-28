#include <luisa/runtime/graph/kernel_node.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/graph/graph_builder.h>

namespace luisa::compute::graph {
KernelNode::KernelNode(GraphBuilder *builder, span<uint64_t> arg_ids, const Resource *shader_resource) noexcept
    : GraphNode{builder, GraphNodeType::Kernel},
      _shader_resource{shader_resource},
      _kernel_id{builder->kernel_nodes().size()} {
    auto res = _shader_resource;
    auto device = res->device();
    for (auto &i : arg_ids) {
        auto v = this->builder()->graph_var(i);
        // get the usage of the shader argument from DeviceInterface
        auto usage = device->shader_argument_usage(res->handle(), v->arg_id());
        // add the usage to the kernel node
        this->add_arg_usage(i, usage);
    }
}
}// namespace luisa::compute::graph
