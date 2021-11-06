//
// Created by Mike on 8/1/2021.
//

#include <backends/cuda/cuda_error.h>
#include <backends/cuda/cuda_stream.h>
#include <backends/cuda/cuda_mipmap_array.h>
#include <backends/cuda/cuda_command_encoder.h>

namespace luisa::compute::cuda {

struct RingBufferRecycleContext {
    std::span<std::byte> buffer;
    CUDAStream *stream{nullptr};
    RingBufferRecycleContext(std::span<std::byte> b, CUDAStream *s) noexcept
        : buffer{b}, stream{s} {}
};

[[nodiscard]] decltype(auto) ring_buffer_recycle_context_pool() noexcept {
    static Pool<RingBufferRecycleContext> pool;
    return (pool);
}

void CUDACommandEncoder::visit(const BufferUploadCommand *command) noexcept {
    auto buffer = command->handle() + command->offset();
    auto data = command->data();
    auto size = command->size();
    auto upload_buffer = _stream->upload_pool().allocate(size);
    std::memcpy(upload_buffer.data(), data, size);
    LUISA_CHECK_CUDA(cuMemcpyHtoDAsync(buffer, upload_buffer.data(), size, _stream->handle()));
    LUISA_CHECK_CUDA(cuLaunchHostFunc(
        _stream->handle(), [](void *user_data) noexcept {
            auto context = static_cast<RingBufferRecycleContext *>(user_data);
            context->stream->upload_pool().recycle(context->buffer);
            ring_buffer_recycle_context_pool().recycle(context);
        },
        ring_buffer_recycle_context_pool().create(upload_buffer, _stream)));
}

void CUDACommandEncoder::visit(const BufferDownloadCommand *command) noexcept {
    auto buffer = command->handle() + command->offset();
    auto data = command->data();
    auto size = command->size();
    LUISA_CHECK_CUDA(cuMemcpyDtoHAsync(data, buffer, size, _stream->handle()));
}

void CUDACommandEncoder::visit(const BufferCopyCommand *command) noexcept {
    auto src_buffer = command->src_handle() + command->src_offset();
    auto dst_buffer = command->dst_handle() + command->dst_offset();
    auto size = command->size();
    LUISA_CHECK_CUDA(cuMemcpyDtoDAsync(dst_buffer, src_buffer, size, _stream->handle()));
}

void CUDACommandEncoder::visit(const BufferToTextureCopyCommand *command) noexcept {
    auto mipmap_array = reinterpret_cast<CUDAMipmapArray *>(command->texture());
    CUarray array;
    LUISA_CHECK_CUDA(cuMipmappedArrayGetLevel(&array, mipmap_array->handle(), command->level()));
    CUDA_MEMCPY3D copy{};
    auto pixel_size = pixel_storage_size(command->storage());
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = command->buffer() + command->buffer_offset();
    copy.srcPitch = pixel_size * command->size().x;
    copy.srcHeight = command->size().y;
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.dstArray = array;
    LUISA_CHECK_CUDA(cuMemcpy3DAsync(&copy, _stream->handle()));
}

void CUDACommandEncoder::visit(const ShaderDispatchCommand *command) noexcept {
}

void CUDACommandEncoder::visit(const TextureUploadCommand *command) noexcept {
    auto mipmap_array = reinterpret_cast<CUDAMipmapArray *>(command->handle());
    CUarray array;
    LUISA_CHECK_CUDA(cuMipmappedArrayGetLevel(&array, mipmap_array->handle(), command->level()));
    CUDA_MEMCPY3D copy{};
    auto pixel_size = pixel_storage_size(command->storage());
    auto data = command->data();
    auto size_bytes = command->size().x * command->size().y * command->size().z * pixel_size;
    auto upload_buffer = _stream->upload_pool().allocate(size_bytes);
    std::memcpy(upload_buffer.data(), data, size_bytes);
    copy.srcMemoryType = CU_MEMORYTYPE_HOST;
    copy.srcHost = upload_buffer.data();
    copy.srcPitch = pixel_size * command->size().x;
    copy.srcHeight = command->size().y;
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.dstArray = array;
    LUISA_CHECK_CUDA(cuMemcpy3DAsync(&copy, _stream->handle()));
}

void CUDACommandEncoder::visit(const TextureDownloadCommand *command) noexcept {
    auto mipmap_array = reinterpret_cast<CUDAMipmapArray *>(command->handle());
    CUarray array;
    LUISA_CHECK_CUDA(cuMipmappedArrayGetLevel(&array, mipmap_array->handle(), command->level()));
    CUDA_MEMCPY3D copy{};
    auto pixel_size = pixel_storage_size(command->storage());
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.srcArray = array;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = command->data();
    copy.dstPitch = pixel_size * command->size().x;
    copy.dstHeight = command->size().y;
    LUISA_CHECK_CUDA(cuMemcpy3DAsync(&copy, _stream->handle()));
}

void CUDACommandEncoder::visit(const TextureCopyCommand *command) noexcept {
    auto src_mipmap_array = reinterpret_cast<CUDAMipmapArray *>(command->src_handle());
    auto dst_mipmap_array = reinterpret_cast<CUDAMipmapArray *>(command->dst_handle());
    CUarray src_array;
    CUarray dst_array;
    LUISA_CHECK_CUDA(cuMipmappedArrayGetLevel(&src_array, src_mipmap_array->handle(), command->src_level()));
    LUISA_CHECK_CUDA(cuMipmappedArrayGetLevel(&dst_array, dst_mipmap_array->handle(), command->dst_level()));
    CUDA_MEMCPY3D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.srcArray = src_array;
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.dstArray = dst_array;
    LUISA_CHECK_CUDA(cuMemcpy3DAsync(&copy, _stream->handle()));
}

void CUDACommandEncoder::visit(const TextureToBufferCopyCommand *command) noexcept {
    auto mipmap_array = reinterpret_cast<CUDAMipmapArray *>(command->texture());
    CUarray array;
    LUISA_CHECK_CUDA(cuMipmappedArrayGetLevel(&array, mipmap_array->handle(), command->level()));
    CUDA_MEMCPY3D copy{};
    auto pixel_size = pixel_storage_size(command->storage());
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.srcArray = array;
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = command->buffer() + command->buffer_offset();
    copy.dstPitch = pixel_size * command->size().x;
    copy.dstHeight = command->size().y;
    LUISA_CHECK_CUDA(cuMemcpy3DAsync(&copy, _stream->handle()));
}

void CUDACommandEncoder::visit(const AccelUpdateCommand *command) noexcept {
}
void CUDACommandEncoder::visit(const AccelBuildCommand *command) noexcept {
}
void CUDACommandEncoder::visit(const MeshUpdateCommand *command) noexcept {
}
void CUDACommandEncoder::visit(const MeshBuildCommand *command) noexcept {
}

}// namespace luisa::compute::cuda
