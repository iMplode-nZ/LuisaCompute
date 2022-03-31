
#include <Api/LCDevice.h>
#include <DXRuntime/Device.h>
#include <Resource/DefaultBuffer.h>
#include <Codegen/DxCodegen.h>
#include <Shader/ShaderCompiler.h>
#include <Codegen/ShaderHeader.h>
#include <Resource/RenderTexture.h>
#include <Resource/BindlessArray.h>
#include <Shader/ComputeShader.h>
#include <Api/LCCmdBuffer.h>
#include <Api/LCEvent.h>
#include <vstl/MD5.h>
#include <Shader/ShaderSerializer.h>
#include <Resource/BottomAccel.h>
#include <Shader/PipelineLibrary.h>
#include <Resource/TopAccel.h>
#include <vstl/BinaryReader.h>
#include <Api/LCSwapChain.h>
using namespace toolhub::directx;
namespace toolhub::directx {
LCDevice::LCDevice(const Context &ctx)
    : LCDeviceInterface(ctx) {
}
void *LCDevice::native_handle() const noexcept {
    return nativeDevice.device.Get();
}
uint64_t LCDevice::create_buffer(size_t size_bytes) noexcept {
    return reinterpret_cast<uint64>(
        static_cast<Buffer *>(
            new DefaultBuffer(
                &nativeDevice,
                size_bytes,
                nativeDevice.defaultAllocator)));
}
void LCDevice::destroy_buffer(uint64_t handle) noexcept {
    delete reinterpret_cast<Buffer *>(handle);
}
void *LCDevice::buffer_native_handle(uint64_t handle) const noexcept {
    return reinterpret_cast<Buffer *>(handle)->GetResource();
}
uint64_t LCDevice::create_texture(
    PixelFormat format,
    uint dimension,
    uint width,
    uint height,
    uint depth,
    uint mipmap_levels) noexcept {
    return reinterpret_cast<uint64>(
        new RenderTexture(
            &nativeDevice,
            width,
            height,
            TextureBase::ToGFXFormat(format),
            (TextureDimension)dimension,
            depth,
            mipmap_levels,
            true,
            nativeDevice.defaultAllocator));
}
void LCDevice::destroy_texture(uint64_t handle) noexcept {
    delete reinterpret_cast<RenderTexture *>(handle);
}
void *LCDevice::texture_native_handle(uint64_t handle) const noexcept {
    return reinterpret_cast<RenderTexture *>(handle)->GetResource();
}
uint64_t LCDevice::create_bindless_array(size_t size) noexcept {
    return reinterpret_cast<uint64>(
        new BindlessArray(&nativeDevice, size));
}
void LCDevice::destroy_bindless_array(uint64_t handle) noexcept {
    delete reinterpret_cast<BindlessArray *>(handle);
}
void LCDevice::emplace_buffer_in_bindless_array(uint64_t array, size_t index, uint64_t handle, size_t offset_bytes) noexcept {
    auto buffer = reinterpret_cast<Buffer *>(handle);
    reinterpret_cast<BindlessArray *>(array)
        ->Bind(BufferView(buffer, offset_bytes), index);
}
void LCDevice::emplace_tex2d_in_bindless_array(uint64_t array, size_t index, uint64_t handle, Sampler sampler) noexcept {
    auto tex = reinterpret_cast<RenderTexture *>(handle);
    reinterpret_cast<BindlessArray *>(array)
        ->Bind(std::pair<TextureBase const *, Sampler>(tex, sampler), index);
}
void LCDevice::emplace_tex3d_in_bindless_array(uint64_t array, size_t index, uint64_t handle, Sampler sampler) noexcept {
    emplace_tex2d_in_bindless_array(array, index, handle, sampler);
}
bool LCDevice::is_buffer_in_bindless_array(uint64_t array, uint64_t handle) const noexcept {
    return reinterpret_cast<BindlessArray *>(array)
        ->IsPtrInBindless(handle);
}
bool LCDevice::is_texture_in_bindless_array(uint64_t array, uint64_t handle) const noexcept {
    return reinterpret_cast<BindlessArray *>(array)
        ->IsPtrInBindless(handle);
}
void LCDevice::remove_buffer_in_bindless_array(uint64_t array, size_t index) noexcept {
    reinterpret_cast<BindlessArray *>(array)
        ->UnBind(BindlessArray::BindTag::Buffer, index);
}
void LCDevice::remove_tex2d_in_bindless_array(uint64_t array, size_t index) noexcept {
    reinterpret_cast<BindlessArray *>(array)
        ->UnBind(BindlessArray::BindTag::Tex2D, index);
}
void LCDevice::remove_tex3d_in_bindless_array(uint64_t array, size_t index) noexcept {
    reinterpret_cast<BindlessArray *>(array)
        ->UnBind(BindlessArray::BindTag::Tex3D, index);
}
uint64_t LCDevice::create_stream() noexcept {
    return reinterpret_cast<uint64>(
        new LCCmdBuffer(
            &nativeDevice,
            nativeDevice.defaultAllocator,
            D3D12_COMMAND_LIST_TYPE_COMPUTE));
}

void LCDevice::destroy_stream(uint64_t handle) noexcept {
    delete reinterpret_cast<LCCmdBuffer *>(handle);
}
void LCDevice::synchronize_stream(uint64_t stream_handle) noexcept {
    reinterpret_cast<LCCmdBuffer *>(stream_handle)->Sync();
}
void LCDevice::dispatch(uint64_t stream_handle, CommandList const &v) noexcept {
    reinterpret_cast<LCCmdBuffer *>(stream_handle)
        ->Execute({&v, 1}, maxAllocatorCount);
}
void LCDevice::dispatch(uint64_t stream_handle, CommandList const &v, luisa::move_only_function<void()> &&callback) noexcept {
    reinterpret_cast<LCCmdBuffer *>(stream_handle)
        ->Execute({&v, 1}, maxAllocatorCount, &callback);
}
void LCDevice::dispatch(uint64_t stream_handle, vstd::span<const CommandList> lists) noexcept {
    reinterpret_cast<LCCmdBuffer *>(stream_handle)
        ->Execute(lists, maxAllocatorCount);
}
void LCDevice::dispatch(uint64_t stream_handle, vstd::span<const CommandList> lists, luisa::move_only_function<void()> &&callback) noexcept {
    reinterpret_cast<LCCmdBuffer *>(stream_handle)
        ->Execute(lists, maxAllocatorCount, &callback);
}

void *LCDevice::stream_native_handle(uint64_t handle) const noexcept {
    return reinterpret_cast<LCCmdBuffer *>(handle)
        ->queue.Queue();
}
uint64_t LCDevice::create_shader(Function kernel, std::string_view meta_options) noexcept {

    auto str = CodegenUtility::Codegen(kernel);
    if (str) {
        return reinterpret_cast<uint64_t>(
            ComputeShader::CompileCompute(
                &nativeDevice,
                *str,
                kernel.block_size(),
                kernel.raytracing() ? 65u : 60u,
                {}));
    }
    return 0;
}

void LCDevice::destroy_shader(uint64_t handle) noexcept {
    auto shader = reinterpret_cast<Shader *>(handle);
    delete shader;
}
uint64_t LCDevice::create_event() noexcept {
    return reinterpret_cast<uint64>(
        new LCEvent(&nativeDevice));
}
void LCDevice::destroy_event(uint64_t handle) noexcept {
    delete reinterpret_cast<LCEvent *>(handle);
}
void LCDevice::signal_event(uint64_t handle, uint64_t stream_handle) noexcept {
    reinterpret_cast<LCEvent *>(handle)->Signal(
        &reinterpret_cast<LCCmdBuffer *>(stream_handle)->queue);
}

void LCDevice::wait_event(uint64_t handle, uint64_t stream_handle) noexcept {
    reinterpret_cast<LCEvent *>(handle)->Wait(
        &reinterpret_cast<LCCmdBuffer *>(stream_handle)->queue);
}
void LCDevice::synchronize_event(uint64_t handle) noexcept {
    reinterpret_cast<LCEvent *>(handle)->Sync();
}

uint64_t LCDevice::create_mesh(
    uint64_t v_buffer,
    size_t v_offset,
    size_t v_stride,
    size_t v_count,
    uint64_t t_buffer,
    size_t t_offset,
    size_t t_count,
    AccelBuildHint hint) noexcept {
    return reinterpret_cast<uint64>(
        (
            new BottomAccel(
                &nativeDevice,
                reinterpret_cast<Buffer *>(v_buffer),
                v_offset * v_stride,
                v_stride,
                v_count,
                reinterpret_cast<Buffer *>(t_buffer),
                t_offset * 3 * sizeof(uint),
                t_count * 3,
                hint)));
}
void LCDevice::destroy_mesh(uint64_t handle) noexcept {
    delete reinterpret_cast<BottomAccel *>(handle);
}
uint64_t LCDevice::create_accel(AccelBuildHint hint) noexcept {
    return reinterpret_cast<uint64>(new TopAccel(
        &nativeDevice,
        hint));
}
void LCDevice::emplace_back_instance_in_accel(uint64_t accel, uint64_t mesh, luisa::float4x4 transform, bool visible) noexcept {
    auto topAccel = reinterpret_cast<TopAccel *>(accel);
    topAccel->Emplace(
        reinterpret_cast<BottomAccel *>(mesh),
        transform,
        visible);
}
void LCDevice::pop_back_instance_from_accel(uint64_t accel) noexcept {
    auto topAccel = reinterpret_cast<TopAccel *>(accel);
    topAccel->PopBack();
}
void LCDevice::set_instance_mesh_in_accel(uint64_t accel, uint64_t index, uint64_t mesh) noexcept {
    auto topAccel = reinterpret_cast<TopAccel *>(accel);
    topAccel->Update(
        index,
        reinterpret_cast<BottomAccel *>(mesh));
}
bool LCDevice::is_buffer_in_accel(uint64_t accel, uint64_t buffer) const noexcept {
    auto topAccel = reinterpret_cast<TopAccel *>(accel);
    return topAccel->IsBufferInAccel(reinterpret_cast<Buffer *>(buffer));
}
bool LCDevice::is_mesh_in_accel(uint64_t accel, uint64_t mesh) const noexcept {
    auto topAccel = reinterpret_cast<TopAccel *>(accel);
    auto meshAccel = reinterpret_cast<BottomAccel *>(mesh);
    return topAccel->IsMeshInAccel((meshAccel)->GetMesh());
}
uint64_t LCDevice::get_vertex_buffer_from_mesh(uint64_t mesh_handle) const noexcept {
    auto meshAccel = reinterpret_cast<BottomAccel *>(mesh_handle);
    return reinterpret_cast<uint64>((meshAccel)->GetMesh()->vHandle);
}
uint64_t LCDevice::get_triangle_buffer_from_mesh(uint64_t mesh_handle) const noexcept {
    auto meshAccel = reinterpret_cast<BottomAccel *>(mesh_handle);
    return reinterpret_cast<uint64>((meshAccel)->GetMesh()->iHandle);
}
void LCDevice::destroy_accel(uint64_t handle) noexcept {
    delete reinterpret_cast<TopAccel *>(handle);
}
uint64_t LCDevice::create_swap_chain(
    uint64 window_handle,
    uint64 stream_handle,
    uint width,
    uint height,
    bool allow_hdr,
    uint back_buffer_size) noexcept {
    return reinterpret_cast<uint64>(
        new LCSwapChain(
            &nativeDevice,
            &reinterpret_cast<LCCmdBuffer *>(stream_handle)->queue,
            nativeDevice.defaultAllocator,
            reinterpret_cast<HWND>(window_handle),
            width,
            height,
            allow_hdr,
            back_buffer_size));
}
void LCDevice::destroy_swap_chain(uint64_t handle) noexcept {
    delete reinterpret_cast<LCSwapChain *>(handle);
}
PixelStorage LCDevice::swap_chain_pixel_storage(uint64_t handle) noexcept {
    return PixelStorage::BYTE4;
}
void LCDevice::present_display_stream(uint64_t stream_handle, uint64_t swapchain_handle, uint64_t image_handle) noexcept {
    reinterpret_cast<LCCmdBuffer *>(stream_handle)
        ->Present(
            reinterpret_cast<LCSwapChain *>(swapchain_handle),
            reinterpret_cast<RenderTexture *>(image_handle));
}
VSTL_EXPORT_C LCDeviceInterface *create(Context const &c, std::string_view) {
    return new LCDevice(c);
}
VSTL_EXPORT_C void destroy(LCDeviceInterface *device) {
    delete static_cast<LCDevice *>(device);
}
}// namespace toolhub::directx