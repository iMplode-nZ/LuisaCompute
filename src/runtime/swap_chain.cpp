#include <runtime/swap_chain.h>
#include <runtime/device.h>
#include <runtime/stream.h>
#include <core/logging.h>

namespace luisa::compute {

SwapChain::SwapChain(DeviceInterface *device, const SwapChainCreationInfo &create_info) noexcept
    : Resource{device, Tag::SWAP_CHAIN, create_info},
      _storage{create_info.storage} {}

SwapChain Device::create_swapchain(uint64_t window_handle,
                                   Stream const &stream,
                                   uint2 resolution,
                                   bool allow_hdr,
                                   bool vsync,
                                   uint back_buffer_size) noexcept {
    if (stream.stream_tag() != StreamTag::GRAPHICS) [[unlikely]] {
        LUISA_ERROR("Only graphics queue can create swap chain!");
    }
    return {impl(), window_handle, stream.handle(),
            resolution.x, resolution.y, allow_hdr, vsync, back_buffer_size};
}

SwapChain::SwapChain(DeviceInterface *device, uint64_t window_handle, uint64_t stream_handle,
                     uint width, uint height, bool allow_hdr, bool vsync, uint back_buffer_size) noexcept
    : SwapChain{device,
                device->create_swap_chain(
                    window_handle, stream_handle, width, height,
                    allow_hdr, vsync, back_buffer_size)} {}

SwapChain::Present SwapChain::present(ImageView<float> frame) const noexcept {
    LUISA_ASSERT(frame.level() == 0u,
                 "Only the base-level image is presentable in a swapchain.");
    return {this, frame};
}

SwapChain::~SwapChain() noexcept {
    if (*this) { device()->destroy_swap_chain(handle()); }
}
void StreamEvent<SwapChain::Present>::execute(
    DeviceInterface *device,
    uint64_t stream_handle,
    SwapChain::Present const &present) noexcept {
    device->present_display_in_stream(stream_handle, present.chain->handle(), present.frame.handle());
}
}// namespace luisa::compute
