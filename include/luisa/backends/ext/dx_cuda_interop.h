#pragma once
#include <luisa/runtime/rhi/device_interface.h>
#include <luisa/runtime/buffer.h>
#include <luisa/runtime/image.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/volume.h>

namespace luisa::compute {
class DxCudaInterop : public DeviceExtension {
protected:
    ~DxCudaInterop() noexcept = default;

public:
    [[nodiscard]] virtual BufferCreationInfo create_interop_buffer(const Type *element, size_t elem_count) noexcept = 0;
    [[nodiscard]] virtual ResourceCreationInfo create_interop_texture(
        PixelFormat format, uint dimension,
        uint width, uint height, uint depth,
        uint mipmap_levels, bool simultaneous_access) noexcept = 0;
    virtual uint64_t cuda_buffer(uint64_t dx_buffer_handle) noexcept = 0;
    virtual uint64_t cuda_texture(uint64_t dx_texture_handle) noexcept = 0;
    virtual uint64_t cuda_event(uint64_t dx_event_handle) noexcept = 0;
    template<typename T>
    Buffer<T> create_buffer(Device const &device, size_t elem_count) noexcept {
        return Buffer<T>{device.impl(), create_interop_buffer(Type::of<T>(), elem_count)};
    }
    template<typename T>
    Image<T> create_image(Device const &device, PixelStorage pixel, uint width, uint height, uint mip_levels = 1u, bool simultaneous_access = false) noexcept {
        return Image<T>{
            device.impl(),
            create_interop_texture(pixel_storage_to_format<T>(pixel), 2, width, height, 1, mip_levels, simultaneous_access),
            pixel,
            uint2(width, height),
            mip_levels};
    }
    template<typename T>
    Image<T> create_image(Device const &device, PixelStorage pixel, uint2 size, uint mip_levels = 1u, bool simultaneous_access = false) noexcept {
        return Image<T>{
            device.impl(),
            create_interop_texture(pixel_storage_to_format<T>(pixel), 2, size.x, size.y, 1, mip_levels, simultaneous_access),
            pixel,
            size,
            mip_levels};
    }
    template<typename T>
    Volume<T> create_volume(Device const &device, PixelStorage pixel, uint width, uint height, uint volume, uint mip_levels = 1u, bool simultaneous_access = false) noexcept {
        return Volume<T>{
            device.impl(),
            create_interop_texture(pixel_storage_to_format<T>(pixel), 3, width, height, volume, mip_levels, simultaneous_access),
            pixel,
            uint3(width, height, volume),
            mip_levels};
    }
    template<typename T>
    Volume<T> create_image(Device const &device, PixelStorage pixel, uint3 size, uint mip_levels = 1u, bool simultaneous_access = false) noexcept {
        return Volume<T>{
            device.impl(),
            create_interop_texture(pixel_storage_to_format<T>(pixel), 3, size.x, size.y, size.z, mip_levels, simultaneous_access),
            pixel,
            size,
            mip_levels};
    }
};
}// namespace luisa::compute
