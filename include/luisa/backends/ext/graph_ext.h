#pragma once
#include <luisa/runtime/graph/graph.h>
#include <luisa/runtime/rhi/device_interface.h>
#include <luisa/runtime/graph/graph_def.h>
#include <luisa/runtime/graph/graph_builder.h>

namespace luisa::compute::graph {
class GraphBuilder;

class LC_RUNTIME_API GraphExt : public DeviceExtension {
    friend class GraphBase;
public:
    static constexpr luisa::string_view name = "GraphExt";
    GraphExt(DeviceInterface *device) noexcept : _device_interface{device} {}

    template<typename... Args>
    auto create_graph(const GraphDefBase<Args...> &gd) noexcept { return Graph<Args...>{this, gd._builder.get()}; };

    auto device_interface() noexcept { return _device_interface; }
protected:
    virtual GraphInterface *create_graph_interface() noexcept { return nullptr; }
    virtual void destroy_graph_interface(GraphInterface *graph_interface) noexcept {}
    DeviceInterface *_device_interface = nullptr;
};
}// namespace luisa::compute::graph
