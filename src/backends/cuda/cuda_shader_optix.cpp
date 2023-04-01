//
// Created by Mike on 3/18/2023.
//

#include <backends/cuda/cuda_error.h>
#include <backends/cuda/cuda_device.h>
#include <backends/cuda/cuda_stream.h>
#include <backends/cuda/cuda_buffer.h>
#include <backends/cuda/cuda_accel.h>
#include <backends/cuda/cuda_mipmap_array.h>
#include <backends/cuda/cuda_bindless_array.h>
#include <backends/cuda/cuda_command_encoder.h>
#include <backends/cuda/cuda_shader_optix.h>

namespace luisa::compute::cuda {

struct alignas(optix::SBT_RECORD_ALIGNMENT) OptiXSBTRecord {
    std::byte data[optix::SBT_RECORD_HEADER_SIZE];
};

/// Retrieves direct and continuation stack sizes for each program in the program group and accumulates the upper bounds
/// in the correponding output variables based on the semantic type of the program. Before the first invocation of this
/// function with a given instance of #OptixStackSizes, the members of that instance should be set to 0.
inline void accumulate_stack_sizes(optix::StackSizes &sizes, optix::ProgramGroup group) noexcept {
    optix::StackSizes local{};
    LUISA_CHECK_OPTIX(optix::api().programGroupGetStackSize(group, &local));
    LUISA_VERBOSE("OptiX program group stack sizes: "
                  "CSS_RG = {}, CSS_MS = {}, CSS_CH = {}, CSS_AH = {}, "
                  "CSS_IS = {}, CSS_CC = {}, DSS_DC = {}.",
                  local.cssRG, local.cssMS, local.cssCH, local.cssAH,
                  local.cssIS, local.cssCC, local.dssDC);
    sizes.cssRG = std::max(sizes.cssRG, local.cssRG);
    sizes.cssMS = std::max(sizes.cssMS, local.cssMS);
    sizes.cssCH = std::max(sizes.cssCH, local.cssCH);
    sizes.cssAH = std::max(sizes.cssAH, local.cssAH);
    sizes.cssIS = std::max(sizes.cssIS, local.cssIS);
    sizes.cssCC = std::max(sizes.cssCC, local.cssCC);
    sizes.dssDC = std::max(sizes.dssDC, local.dssDC);
    LUISA_VERBOSE("Accumulated OptiX stack sizes: "
                  "CSS_RG = {}, CSS_MS = {}, CSS_CH = {}, CSS_AH = {}, "
                  "CSS_IS = {}, CSS_CC = {}, DSS_DC = {}.",
                  sizes.cssRG, sizes.cssMS, sizes.cssCH, sizes.cssAH,
                  sizes.cssIS, sizes.cssCC, sizes.dssDC);
}

[[nodiscard]] inline uint compute_continuation_stack_size(optix::StackSizes ss) noexcept {
    auto size = ss.cssRG + std::max(std::max(ss.cssCH, ss.cssMS), ss.cssIS + ss.cssAH);
    LUISA_INFO("Computed OptiX continuation stack size: {}.", size);
    return size;
}

CUDAShaderOptiX::CUDAShaderOptiX(CUDADevice *device,
                                 const char *ptx, size_t ptx_size,
                                 const char *entry, bool enable_debug,
                                 luisa::vector<ShaderDispatchCommand::Argument> bound_arguments) noexcept
    : _device{device},
      _bound_arguments{std::move(bound_arguments)} {

    // create SBT event
    LUISA_CHECK_CUDA(cuEventCreate(&_sbt_event, CU_EVENT_DISABLE_TIMING));

    // create argument buffer
    static constexpr auto pattern = "params[";
    auto ptr = strstr(ptx, pattern);
    if (ptr == nullptr) [[unlikely]] {
        LUISA_ERROR_WITH_LOCATION(
            "Cannot find global symbol 'params' in PTX for {}.",
            entry);
    }
    ptr += std::string_view{pattern}.size();
    char *end = nullptr;
    _argument_buffer_size = strtoull(ptr, &end, 10);
    if (_argument_buffer_size == 0u) [[unlikely]] {
        LUISA_ERROR_WITH_LOCATION(
            "Failed to parse argument buffer size for {}.",
            entry);
    }
    LUISA_VERBOSE_WITH_LOCATION(
        "Argument buffer size for {}: {}.",
        entry, _argument_buffer_size);

    // TODO: enable ray query only when needed

    // create module
    static constexpr std::array trace_closest_payload_semantics{
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE | optix::PAYLOAD_SEMANTICS_MS_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE};

    static constexpr std::array trace_any_payload_semantics{
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ | optix::PAYLOAD_SEMANTICS_MS_WRITE};

    static constexpr std::array ray_query_payload_semantics{
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ_WRITE | optix::PAYLOAD_SEMANTICS_IS_READ | optix::PAYLOAD_SEMANTICS_AH_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE | optix::PAYLOAD_SEMANTICS_MS_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ_WRITE | optix::PAYLOAD_SEMANTICS_IS_READ | optix::PAYLOAD_SEMANTICS_AH_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ_WRITE | optix::PAYLOAD_SEMANTICS_IS_READ | optix::PAYLOAD_SEMANTICS_AH_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ_WRITE | optix::PAYLOAD_SEMANTICS_IS_NONE | optix::PAYLOAD_SEMANTICS_AH_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE,
        optix::PAYLOAD_SEMANTICS_TRACE_CALLER_READ_WRITE | optix::PAYLOAD_SEMANTICS_IS_NONE | optix::PAYLOAD_SEMANTICS_AH_READ | optix::PAYLOAD_SEMANTICS_CH_WRITE};

    std::array<optix::PayloadType, 3u> payload_types{};
    payload_types[0].numPayloadValues = trace_closest_payload_semantics.size();
    payload_types[0].payloadSemantics = trace_closest_payload_semantics.data();
    payload_types[1].numPayloadValues = trace_any_payload_semantics.size();
    payload_types[1].payloadSemantics = trace_any_payload_semantics.data();
    payload_types[2].numPayloadValues = ray_query_payload_semantics.size();
    payload_types[2].payloadSemantics = ray_query_payload_semantics.data();

    optix::ModuleCompileOptions module_compile_options{};
    module_compile_options.maxRegisterCount = optix::COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    module_compile_options.debugLevel = enable_debug ? optix::COMPILE_DEBUG_LEVEL_MINIMAL :
                                                       optix::COMPILE_DEBUG_LEVEL_NONE;
    module_compile_options.optLevel = optix::COMPILE_OPTIMIZATION_LEVEL_3;
    module_compile_options.numPayloadTypes = payload_types.size();
    module_compile_options.payloadTypes = payload_types.data();

    optix::PipelineCompileOptions pipeline_compile_options{};
    pipeline_compile_options.exceptionFlags = optix::EXCEPTION_FLAG_NONE;
    pipeline_compile_options.traversableGraphFlags = optix::TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    pipeline_compile_options.numPayloadValues = 0u;
    pipeline_compile_options.usesPrimitiveTypeFlags = optix::PRIMITIVE_TYPE_FLAGS_TRIANGLE;
    pipeline_compile_options.pipelineLaunchParamsVariableName = "params";

    auto optix_ctx = device->handle().optix_context();
    char log[2048];// For error reporting from OptiX creation functions
    size_t log_size;
    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().moduleCreateFromPTX(
            optix_ctx, &module_compile_options,
            &pipeline_compile_options, ptx, ptx_size,
            log, &log_size, &_module));

    // create program groups
    optix::ProgramGroupOptions program_group_options_rg{};
    optix::ProgramGroupDesc program_group_desc_rg{};
    program_group_desc_rg.kind = optix::PROGRAM_GROUP_KIND_RAYGEN;
    program_group_desc_rg.raygen.module = _module;
    program_group_desc_rg.raygen.entryFunctionName = entry;
    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().programGroupCreate(
            optix_ctx,
            &program_group_desc_rg, 1u,
            &program_group_options_rg,
            log, &log_size, &_program_group_rg));

    optix::ProgramGroupOptions program_group_options_ch_closest{};
    program_group_options_ch_closest.payloadType = &payload_types[0];
    optix::ProgramGroupDesc program_group_desc_ch_closest{};
    program_group_desc_ch_closest.kind = optix::PROGRAM_GROUP_KIND_HITGROUP;
    program_group_desc_ch_closest.hitgroup.moduleCH = _module;
    program_group_desc_ch_closest.hitgroup.entryFunctionNameCH = "__closesthit__trace_closest";
    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().programGroupCreate(
            optix_ctx,
            &program_group_desc_ch_closest, 1u,
            &program_group_options_ch_closest,
            log, &log_size, &_program_group_ch_closest));

    optix::ProgramGroupOptions program_group_options_ch_query{};
    program_group_options_ch_query.payloadType = &payload_types[2];
    optix::ProgramGroupDesc program_group_desc_ch_query{};
    program_group_desc_ch_query.kind = optix::PROGRAM_GROUP_KIND_HITGROUP;
    program_group_desc_ch_query.hitgroup.moduleCH = _module;
    program_group_desc_ch_query.hitgroup.entryFunctionNameCH = "__closesthit__ray_query";
    program_group_desc_ch_query.hitgroup.moduleAH = _module;
    program_group_desc_ch_query.hitgroup.entryFunctionNameAH = "__anyhit__ray_query";
    program_group_desc_ch_query.hitgroup.moduleIS = _module;
    program_group_desc_ch_query.hitgroup.entryFunctionNameIS = "__intersection__ray_query";

    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().programGroupCreate(
            optix_ctx,
            &program_group_desc_ch_query, 1u,
            &program_group_options_ch_query,
            log, &log_size, &_program_group_ch_query));

    optix::ProgramGroupOptions program_group_options_miss_closest{};
    program_group_options_miss_closest.payloadType = &payload_types[0];
    optix::ProgramGroupDesc program_group_desc_miss_closest{};
    program_group_desc_miss_closest.kind = optix::PROGRAM_GROUP_KIND_MISS;
    program_group_desc_miss_closest.miss.module = _module;
    program_group_desc_miss_closest.miss.entryFunctionName = "__miss__trace_closest";
    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().programGroupCreate(
            optix_ctx,
            &program_group_desc_miss_closest, 1u,
            &program_group_options_miss_closest,
            log, &log_size, &_program_group_miss_closest));

    optix::ProgramGroupOptions program_group_options_miss_any{};
    program_group_options_miss_any.payloadType = &payload_types[1];
    optix::ProgramGroupDesc program_group_desc_miss_any{};
    program_group_desc_miss_any.kind = optix::PROGRAM_GROUP_KIND_MISS;
    program_group_desc_miss_any.miss.module = _module;
    program_group_desc_miss_any.miss.entryFunctionName = "__miss__trace_any";
    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().programGroupCreate(
            optix_ctx,
            &program_group_desc_miss_any, 1u,
            &program_group_options_miss_any,
            log, &log_size, &_program_group_miss_any));

    optix::ProgramGroupOptions program_group_options_ray_query{};
    program_group_options_ray_query.payloadType = &payload_types[2];
    optix::ProgramGroupDesc program_group_desc_ray_query{};
    program_group_desc_ray_query.kind = optix::PROGRAM_GROUP_KIND_MISS;
    program_group_desc_ray_query.miss.module = _module;
    program_group_desc_ray_query.miss.entryFunctionName = "__miss__ray_query";
    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().programGroupCreate(
            optix_ctx,
            &program_group_desc_ray_query, 1u,
            &program_group_options_ray_query,
            log, &log_size, &_program_group_miss_query));

    // create pipeline
    std::array program_groups{_program_group_rg,
                              _program_group_ch_closest,
                              _program_group_miss_closest,
                              _program_group_miss_any,
                              _program_group_ch_query,
                              _program_group_miss_query};
    optix::PipelineLinkOptions pipeline_link_options{};
    pipeline_link_options.debugLevel = enable_debug ? optix::COMPILE_DEBUG_LEVEL_MINIMAL :
                                                      optix::COMPILE_DEBUG_LEVEL_NONE;
    pipeline_link_options.maxTraceDepth = 1u;
    LUISA_CHECK_OPTIX_WITH_LOG(
        log, log_size,
        optix::api().pipelineCreate(
            optix_ctx, &pipeline_compile_options, &pipeline_link_options,
            program_groups.data(), program_groups.size(), log, &log_size, &_pipeline));

    // compute stack sizes
    optix::StackSizes stack_sizes{};
    for (auto pg : program_groups) { accumulate_stack_sizes(stack_sizes, pg); }
    auto continuation_stack_size = compute_continuation_stack_size(stack_sizes);
    LUISA_CHECK_OPTIX(optix::api().pipelineSetStackSize(
        _pipeline, 0u, 0u, continuation_stack_size, 2u));
}

CUDAShaderOptiX::~CUDAShaderOptiX() noexcept {
    LUISA_CHECK_CUDA(cuMemFree(_sbt_buffer));
    LUISA_CHECK_CUDA(cuEventDestroy(_sbt_event));
    LUISA_CHECK_OPTIX(optix::api().pipelineDestroy(_pipeline));
    LUISA_CHECK_OPTIX(optix::api().programGroupDestroy(_program_group_rg));
    LUISA_CHECK_OPTIX(optix::api().programGroupDestroy(_program_group_ch_closest));
    LUISA_CHECK_OPTIX(optix::api().programGroupDestroy(_program_group_ch_query));
    LUISA_CHECK_OPTIX(optix::api().programGroupDestroy(_program_group_miss_closest));
    LUISA_CHECK_OPTIX(optix::api().programGroupDestroy(_program_group_miss_any));
    LUISA_CHECK_OPTIX(optix::api().programGroupDestroy(_program_group_miss_query));
    LUISA_CHECK_OPTIX(optix::api().moduleDestroy(_module));
}

void CUDAShaderOptiX::_prepare_sbt(CUDACommandEncoder &encoder) const noexcept {
    auto cuda_stream = encoder.stream()->handle();
    std::scoped_lock lock{_mutex};
    if (_sbt.raygenRecord == 0u) {// create shader binding table if not present
        static constexpr auto sbt_buffer_size = sizeof(OptiXSBTRecord) * 6u;
        LUISA_CHECK_CUDA(cuMemAllocAsync(&_sbt_buffer, sbt_buffer_size, cuda_stream));
        encoder.with_upload_buffer(sbt_buffer_size, [&](CUDAHostBufferPool::View *sbt_record_buffer) noexcept {
            auto sbt_records = reinterpret_cast<OptiXSBTRecord *>(sbt_record_buffer->address());
            LUISA_CHECK_OPTIX(optix::api().sbtRecordPackHeader(_program_group_rg, &sbt_records[0]));
            LUISA_CHECK_OPTIX(optix::api().sbtRecordPackHeader(_program_group_ch_closest, &sbt_records[1]));
            LUISA_CHECK_OPTIX(optix::api().sbtRecordPackHeader(_program_group_ch_query, &sbt_records[2]));
            LUISA_CHECK_OPTIX(optix::api().sbtRecordPackHeader(_program_group_miss_closest, &sbt_records[3]));
            LUISA_CHECK_OPTIX(optix::api().sbtRecordPackHeader(_program_group_miss_any, &sbt_records[4]));
            LUISA_CHECK_OPTIX(optix::api().sbtRecordPackHeader(_program_group_miss_query, &sbt_records[5]));
            LUISA_CHECK_CUDA(cuMemcpyHtoDAsync(_sbt_buffer, sbt_record_buffer->address(),
                                               sbt_buffer_size, cuda_stream));
            LUISA_CHECK_CUDA(cuEventRecord(_sbt_event, cuda_stream));
        });
        _sbt.raygenRecord = _sbt_buffer;
        _sbt.hitgroupRecordBase = _sbt_buffer + sizeof(OptiXSBTRecord);
        _sbt.hitgroupRecordCount = 2u;
        _sbt.hitgroupRecordStrideInBytes = sizeof(OptiXSBTRecord);
        _sbt.missRecordBase = _sbt_buffer + sizeof(OptiXSBTRecord) * 3u;
        _sbt.missRecordCount = 3u;
        _sbt.missRecordStrideInBytes = sizeof(OptiXSBTRecord);
        _sbt_recorded_streams.emplace(encoder.stream()->uid());
    } else {
        if (auto stream_uid = encoder.stream()->uid();
            _sbt_recorded_streams.emplace(stream_uid).second) {
            LUISA_CHECK_CUDA(cuStreamWaitEvent(cuda_stream, _sbt_event, 0u));
        }
    }
}

void CUDAShaderOptiX::launch(CUDACommandEncoder &encoder, ShaderDispatchCommand *command) const noexcept {

    // prepare SBT
    _prepare_sbt(encoder);

    // encode arguments
    encoder.with_upload_buffer(_argument_buffer_size, [&](CUDAHostBufferPool::View *argument_buffer) noexcept {
        auto argument_buffer_offset = static_cast<size_t>(0u);
        auto allocate_argument = [&](size_t bytes) noexcept {
            static constexpr auto alignment = 16u;
            auto offset = (argument_buffer_offset + alignment - 1u) / alignment * alignment;
            LUISA_ASSERT(offset + bytes <= _argument_buffer_size,
                         "Too many arguments in ShaderDispatchCommand");
            argument_buffer_offset = offset + bytes;
            return argument_buffer->address() + offset;
        };

        auto encode_argument = [&allocate_argument, command](const auto &arg) noexcept {
            using Tag = ShaderDispatchCommand::Argument::Tag;
            switch (arg.tag) {
                case Tag::BUFFER: {
                    auto buffer = reinterpret_cast<const CUDABuffer *>(arg.buffer.handle);
                    auto binding = buffer->binding(arg.buffer.offset, arg.buffer.size);
                    auto ptr = allocate_argument(sizeof(binding));
                    std::memcpy(ptr, &binding, sizeof(binding));
                    break;
                }
                case Tag::TEXTURE: {
                    auto texture = reinterpret_cast<const CUDAMipmapArray *>(arg.texture.handle);
                    auto binding = texture->binding(arg.texture.level);
                    auto ptr = allocate_argument(sizeof(binding));
                    std::memcpy(ptr, &binding, sizeof(binding));
                    break;
                }
                case Tag::UNIFORM: {
                    auto uniform = command->uniform(arg.uniform);
                    auto ptr = allocate_argument(uniform.size_bytes());
                    std::memcpy(ptr, uniform.data(), uniform.size_bytes());
                    break;
                }
                case Tag::BINDLESS_ARRAY: {
                    auto array = reinterpret_cast<const CUDABindlessArray *>(arg.bindless_array.handle);
                    auto binding = array->binding();
                    auto ptr = allocate_argument(sizeof(binding));
                    std::memcpy(ptr, &binding, sizeof(binding));
                    break;
                }
                case Tag::ACCEL: {
                    auto accel = reinterpret_cast<const CUDAAccel *>(arg.accel.handle);
                    auto binding = accel->binding();
                    auto ptr = allocate_argument(sizeof(binding));
                    std::memcpy(ptr, &binding, sizeof(binding));
                    break;
                }
            }
        };
        // TODO: optimize this
        for (auto &&arg : _bound_arguments) { encode_argument(arg); }
        for (auto &&arg : command->arguments()) { encode_argument(arg); }
        auto s = command->dispatch_size();
        auto cuda_stream = encoder.stream()->handle();
        if (argument_buffer->is_pooled()) [[likely]] {// if the argument buffer is pooled, we can use the device pointer directly
            auto device_argument_buffer = 0ull;
            LUISA_CHECK_CUDA(cuMemHostGetDevicePointer(
                &device_argument_buffer, argument_buffer->address(), 0u));
            LUISA_CHECK_OPTIX(optix::api().launch(
                _pipeline, cuda_stream, device_argument_buffer,
                _argument_buffer_size, &_sbt, s.x, s.y, s.z));
        } else {// otherwise, we need to copy the argument buffer to the device
            auto device_argument_buffer = 0ull;
            LUISA_CHECK_CUDA(cuMemAllocAsync(
                &device_argument_buffer, _argument_buffer_size, cuda_stream));
            LUISA_CHECK_CUDA(cuMemcpyHtoDAsync(
                device_argument_buffer, argument_buffer->address(),
                _argument_buffer_size, cuda_stream));
            LUISA_CHECK_OPTIX(optix::api().launch(
                _pipeline, cuda_stream, device_argument_buffer,
                _argument_buffer_size, &_sbt, s.x, s.y, s.z));
            LUISA_CHECK_CUDA(cuMemFreeAsync(device_argument_buffer, cuda_stream));
        }
    });
}

}// namespace luisa::compute::cuda
