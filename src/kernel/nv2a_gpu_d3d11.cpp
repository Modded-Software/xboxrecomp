#include "nv2a_gpu.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <d3dcompiler.h>
#include "nv2a_gpu_shader.h"
#include "../d3d/d3d8_swizzle.h"
#include <vector>
#include <array>
#include <cstring>
#include <string>
#include <cstddef>
#include <chrono>
#include <algorithm>
#include <set>
#include <unordered_map>
#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

using Microsoft::WRL::ComPtr;

static struct {
    double draw, texture, hash, upload, sync, state;
    double sync_copy, sync_wait, sync_publish, sync_unmap;
    double color_publish, depth_publish;
    double sync_event_wait;
    double constants;
    double draw_setup, draw_textures, draw_streams, draw_shaders, draw_submit, draw_bookkeeping;
    double vertex_compile, pixel_compile;
    uint64_t vertex_shader_hits, vertex_shader_misses, pixel_shader_hits, pixel_shader_misses, shader_evictions;
    uint64_t vertex_shader_failures, pixel_shader_failures;
    uint64_t hash_bytes;
    uint64_t blend_created, depth_created, sampler_created;
    uint64_t color_created, depth_surface_created, color_refreshed, depth_refreshed;
    uint64_t color_refresh_reused, depth_refresh_reused;
    uint64_t color_readbacks, depth_readbacks, color_masked_draws, completion_waits;
    uint64_t constant_uploads, constant_reuses;
    uint64_t color_copy_bytes, color_publish_bytes, depth_publish_bytes, depth_convert_bytes;
    uint64_t coverage_mask_updates, coverage_mask_rows;
    uint64_t color_refresh_bytes, color_refresh_full_bytes;
    uint64_t color_bulk_readbacks;
    uint64_t color_clear_publications, depth_clear_publications, clear_readback_bytes_avoided;
    uint64_t texture_created, texture_updated, texture_update_bytes;
    uint64_t texture_evicted, texture_cache_entries, texture_cache_bytes;
    uint64_t texture_decode_failures, texture_create_failures;
    uint64_t fixed_draws, lit_draws, skinned_draws, texgen_draws, texture_matrix_draws;
    uint64_t signed_texture_stages, packed_texture_stages;
    uint64_t window_clip_draws, window_scissor_draws, window_shader_draws, window_exclusive_draws;
    uint64_t line_primitives, point_primitives;
    uint64_t sync_calls, target_evictions;
    uint64_t native_clears, clear_fallbacks;
    uint64_t stream_discards, stream_appends;
    uint64_t alias_copies, alias_copy_bytes, alias_copy_reuses, texture_alias_syncs;
    uint64_t compatible_target_views, partial_target_clears, target_layout_syncs;
} gpu_timing;

struct GpuTimer {
    double *total;
    std::chrono::steady_clock::time_point start;
    explicit GpuTimer(double &target) : total(&target), start(std::chrono::steady_clock::now()) {}
    void next_phase(double &target) {
        auto now = std::chrono::steady_clock::now();
        *total += std::chrono::duration<double>(now - start).count();
        total = &target;
        start = now;
    }
    ~GpuTimer() { *total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
};

static void report_gpu_samples();

extern "C" void nv2a_gpu_report(void)
{
    report_gpu_samples();
    std::fprintf(stderr, "[GPU-D3D11] time: draw %.3fs texture %.3fs hash %.3fs upload %.3fs sync %.3fs state %.3fs; hashed %.3f GiB\n",
                 gpu_timing.draw, gpu_timing.texture, gpu_timing.hash, gpu_timing.upload, gpu_timing.sync, gpu_timing.state,
                 (double)gpu_timing.hash_bytes / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] draw phases: setup %.3fs textures %.3fs streams %.3fs shaders %.3fs submit %.3fs bookkeeping %.3fs\n",
                 gpu_timing.draw_setup, gpu_timing.draw_textures, gpu_timing.draw_streams,
                 gpu_timing.draw_shaders, gpu_timing.draw_submit, gpu_timing.draw_bookkeeping);
    std::fprintf(stderr, "[GPU-D3D11] states created: %llu blend, %llu depth, %llu sampler\n",
                 (unsigned long long)gpu_timing.blend_created, (unsigned long long)gpu_timing.depth_created,
                 (unsigned long long)gpu_timing.sampler_created);
    std::fprintf(stderr, "[GPU-D3D11] texture uploads: %llu created, %llu updated; %.3f GiB updated; %llu decode failures, %llu create failures\n",
                 (unsigned long long)gpu_timing.texture_created, (unsigned long long)gpu_timing.texture_updated,
                 (double)gpu_timing.texture_update_bytes / 1073741824.0,
                 (unsigned long long)gpu_timing.texture_decode_failures,
                 (unsigned long long)gpu_timing.texture_create_failures);
    std::fprintf(stderr, "[GPU-D3D11] texture cache: %llu entries, %.1f MiB, %llu evictions\n",
                 (unsigned long long)gpu_timing.texture_cache_entries,
                 (double)gpu_timing.texture_cache_bytes / 1048576.0,
                 (unsigned long long)gpu_timing.texture_evicted);
    std::fprintf(stderr, "[GPU-D3D11] surfaces: %llu color created, %llu depth created; %llu color refreshed, %llu depth refreshed\n",
                 (unsigned long long)gpu_timing.color_created, (unsigned long long)gpu_timing.depth_surface_created,
                 (unsigned long long)gpu_timing.color_refreshed, (unsigned long long)gpu_timing.depth_refreshed);
    std::fprintf(stderr, "[GPU-D3D11] target cache: %llu evictions; %llu synchronization calls\n",
                 (unsigned long long)gpu_timing.target_evictions, (unsigned long long)gpu_timing.sync_calls);
    std::fprintf(stderr, "[GPU-D3D11] clears: %llu native, %llu CPU-path requests\n",
                 (unsigned long long)gpu_timing.native_clears, (unsigned long long)gpu_timing.clear_fallbacks);
    std::fprintf(stderr, "[GPU-D3D11] streams: %llu discard maps, %llu append maps\n",
                 (unsigned long long)gpu_timing.stream_discards, (unsigned long long)gpu_timing.stream_appends);
    std::fprintf(stderr, "[GPU-D3D11] source aliases: %llu GPU copies, %.3f GiB, %llu reused; %llu CPU synchronization fallbacks\n",
                 (unsigned long long)gpu_timing.alias_copies, (double)gpu_timing.alias_copy_bytes / 1073741824.0,
                 (unsigned long long)gpu_timing.alias_copy_reuses, (unsigned long long)gpu_timing.texture_alias_syncs);
    std::fprintf(stderr, "[GPU-D3D11] compatible targets: %llu reused views, %llu rectangle clears, %llu layout synchronizations\n",
                 (unsigned long long)gpu_timing.compatible_target_views,
                 (unsigned long long)gpu_timing.partial_target_clears,
                 (unsigned long long)gpu_timing.target_layout_syncs);
    std::fprintf(stderr, "[GPU-D3D11] unchanged refreshes: %llu color, %llu depth\n",
                 (unsigned long long)gpu_timing.color_refresh_reused, (unsigned long long)gpu_timing.depth_refresh_reused);
    std::fprintf(stderr, "[GPU-D3D11] readback: copy %.3fs wait %.3fs publish %.3fs unmap %.3fs\n",
                 gpu_timing.sync_copy, gpu_timing.sync_wait, gpu_timing.sync_publish, gpu_timing.sync_unmap);
    std::fprintf(stderr, "[GPU-D3D11] publication: color %.3fs depth %.3fs\n",
                 gpu_timing.color_publish, gpu_timing.depth_publish);
    std::fprintf(stderr, "[GPU-D3D11] bulk color readbacks: %llu\n",
                 (unsigned long long)gpu_timing.color_bulk_readbacks);
    std::fprintf(stderr, "[GPU-D3D11] known clears: %llu color, %llu depth; %.3f GiB readback avoided\n",
                 (unsigned long long)gpu_timing.color_clear_publications,
                 (unsigned long long)gpu_timing.depth_clear_publications,
                 (double)gpu_timing.clear_readback_bytes_avoided / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] completion: %.3fs in %llu event waits; %llu color-masked draws; readbacks %llu color, %llu depth\n",
                 gpu_timing.sync_event_wait, (unsigned long long)gpu_timing.completion_waits,
                 (unsigned long long)gpu_timing.color_masked_draws, (unsigned long long)gpu_timing.color_readbacks,
                 (unsigned long long)gpu_timing.depth_readbacks);
    std::fprintf(stderr, "[GPU-D3D11] constants: %llu uploads, %llu unchanged reuses, %.3fs\n",
                 (unsigned long long)gpu_timing.constant_uploads,
                 (unsigned long long)gpu_timing.constant_reuses, gpu_timing.constants);
    std::fprintf(stderr, "[GPU-D3D11] shaders: vertex %llu hits/%llu misses/%llu failures, %.3fs compile; pixel %llu hits/%llu misses/%llu failures, %.3fs compile; %llu evictions\n",
                 (unsigned long long)gpu_timing.vertex_shader_hits, (unsigned long long)gpu_timing.vertex_shader_misses,
                 (unsigned long long)gpu_timing.vertex_shader_failures,
                 gpu_timing.vertex_compile, (unsigned long long)gpu_timing.pixel_shader_hits,
                 (unsigned long long)gpu_timing.pixel_shader_misses,
                 (unsigned long long)gpu_timing.pixel_shader_failures, gpu_timing.pixel_compile,
                 (unsigned long long)gpu_timing.shader_evictions);
    std::fprintf(stderr, "[GPU-D3D11] fixed function: %llu draws, %llu lit, %llu skinned, %llu texgen, %llu texture-matrix\n",
                 (unsigned long long)gpu_timing.fixed_draws, (unsigned long long)gpu_timing.lit_draws,
                 (unsigned long long)gpu_timing.skinned_draws, (unsigned long long)gpu_timing.texgen_draws,
                 (unsigned long long)gpu_timing.texture_matrix_draws);
    std::fprintf(stderr, "[GPU-D3D11] texture state: %llu signed-channel stages, %llu expanded-format stages\n",
                 (unsigned long long)gpu_timing.signed_texture_stages, (unsigned long long)gpu_timing.packed_texture_stages);
    std::fprintf(stderr, "[GPU-D3D11] native primitives: %llu lines, %llu points\n",
                 (unsigned long long)gpu_timing.line_primitives, (unsigned long long)gpu_timing.point_primitives);
    std::fprintf(stderr, "[GPU-D3D11] window clip: %llu configured draws, %llu scissor-only, %llu pixel-union, %llu exclusive\n",
                 (unsigned long long)gpu_timing.window_clip_draws, (unsigned long long)gpu_timing.window_scissor_draws,
                 (unsigned long long)gpu_timing.window_shader_draws, (unsigned long long)gpu_timing.window_exclusive_draws);
    std::fprintf(stderr, "[GPU-D3D11] surface bytes: color copied %.3f GiB published %.3f GiB; depth published %.3f GiB converted %.3f GiB\n",
                 (double)gpu_timing.color_copy_bytes / 1073741824.0, (double)gpu_timing.color_publish_bytes / 1073741824.0,
                 (double)gpu_timing.depth_publish_bytes / 1073741824.0, (double)gpu_timing.depth_convert_bytes / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] coverage: %llu bitmap updates, %llu bitmap publication rows\n",
                 (unsigned long long)gpu_timing.coverage_mask_updates, (unsigned long long)gpu_timing.coverage_mask_rows);
    std::fprintf(stderr, "[GPU-D3D11] color refresh: %.3f GiB submitted, %.3f GiB full-surface equivalent\n",
                 (double)gpu_timing.color_refresh_bytes / 1073741824.0,
                 (double)gpu_timing.color_refresh_full_bytes / 1073741824.0);
}

static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<ID3D11Query> completion_event;
static bool pending_draws;

struct GpuDrawSample {
    ComPtr<ID3D11Query> disjoint, begin, end, statistics;
    uint32_t kind = 0;
    bool pending = false;
};

static std::array<GpuDrawSample,16> gpu_draw_samples;
struct GpuSampleBucket {
    uint64_t samples = 0, vertex_invocations = 0, geometry_invocations = 0, pixel_invocations = 0;
    double seconds = 0;
};
static struct {
    std::array<GpuSampleBucket,8> buckets;
    uint64_t draws = 0, busy = 0, disjoint = 0;
    uint32_t next = 0;
} gpu_samples;

static bool gpu_sampling_enabled()
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_NV2A_GPU_TIMING");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static void gpu_sample_failed(const char *operation, HRESULT result)
{
    std::fprintf(stderr, "[GPU-D3D11] GPU profiling %s failed: 0x%08X; device 0x%08X\n",
                 operation, (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

static bool collect_gpu_sample(GpuDrawSample &sample)
{
    if (!sample.pending) return true;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency = {};
    UINT64 begin = 0, end = 0;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics = {};
    auto read = [&](ID3D11Query *query, void *data, UINT bytes) {
        HRESULT result = context->GetData(query, data, bytes, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(result)) gpu_sample_failed("read", result);
        return result == S_OK;
    };
    if (!read(sample.disjoint.Get(), &frequency, sizeof frequency) ||
        !read(sample.begin.Get(), &begin, sizeof begin) ||
        !read(sample.end.Get(), &end, sizeof end) ||
        !read(sample.statistics.Get(), &statistics, sizeof statistics)) return false;
    sample.pending = false;
    if (frequency.Disjoint || !frequency.Frequency || end < begin) {
        gpu_samples.disjoint++;
        return true;
    }
    auto &bucket = gpu_samples.buckets[sample.kind];
    bucket.samples++;
    bucket.seconds += (double)(end - begin) / frequency.Frequency;
    bucket.vertex_invocations += statistics.VSInvocations;
    bucket.geometry_invocations += statistics.GSInvocations;
    bucket.pixel_invocations += statistics.PSInvocations;
    return true;
}

static GpuDrawSample *begin_gpu_sample(const Nv2aGpuDraw &state)
{
    if (!gpu_sampling_enabled() || (++gpu_samples.draws % 128) != 0) return nullptr;
    auto &sample = gpu_draw_samples[gpu_samples.next++ % gpu_draw_samples.size()];
    if (!collect_gpu_sample(sample)) {
        gpu_samples.busy++;
        return nullptr;
    }
    if (!sample.disjoint) {
        auto create = [&](D3D11_QUERY type, ComPtr<ID3D11Query> &query) {
            D3D11_QUERY_DESC description = {type, 0};
            HRESULT result = device->CreateQuery(&description, &query);
            if (FAILED(result)) gpu_sample_failed("create", result);
        };
        create(D3D11_QUERY_TIMESTAMP_DISJOINT, sample.disjoint);
        create(D3D11_QUERY_TIMESTAMP, sample.begin);
        create(D3D11_QUERY_TIMESTAMP, sample.end);
        create(D3D11_QUERY_PIPELINE_STATISTICS, sample.statistics);
    }
    sample.kind = (state.vertex_program ? 1u : 0u) |
                  (state.fixed_transform && state.lighting_enable ? 2u : 0u) |
                  (state.control0 & 0x10000u ? 4u : 0u);
    context->Begin(sample.disjoint.Get());
    context->Begin(sample.statistics.Get());
    context->End(sample.begin.Get());
    return &sample;
}

static void end_gpu_sample(GpuDrawSample *sample)
{
    if (!sample) return;
    context->End(sample->end.Get());
    context->End(sample->statistics.Get());
    context->End(sample->disjoint.Get());
    sample->pending = true;
}

static void report_gpu_samples()
{
    if (!gpu_sampling_enabled() || !context) return;
    for (auto &sample : gpu_draw_samples) collect_gpu_sample(sample);
    static std::array<GpuSampleBucket,8> previous;
    for (uint32_t kind = 0; kind < gpu_samples.buckets.size(); kind++) {
        const auto &bucket = gpu_samples.buckets[kind];
        uint64_t samples = bucket.samples - previous[kind].samples;
        if (!samples) continue;
        std::fprintf(stderr, "[GPU-D3D11] GPU sample: program=%u lit=%u wdepth=%u interval samples=%llu draw=%.3fus mean; VS=%llu GS=%llu PS=%llu invocations\n",
                     kind & 1u, (kind >> 1) & 1u, (kind >> 2) & 1u,
                     (unsigned long long)samples,
                     (bucket.seconds - previous[kind].seconds) * 1000000 / samples,
                     (unsigned long long)(bucket.vertex_invocations - previous[kind].vertex_invocations),
                     (unsigned long long)(bucket.geometry_invocations - previous[kind].geometry_invocations),
                     (unsigned long long)(bucket.pixel_invocations - previous[kind].pixel_invocations));
        previous[kind] = bucket;
    }
    std::fprintf(stderr, "[GPU-D3D11] GPU sampling: every 128th draw; %llu busy skips, %llu disjoint samples discarded\n",
                 (unsigned long long)gpu_samples.busy, (unsigned long long)gpu_samples.disjoint);
}
static ComPtr<ID3D11VertexShader> vertex_shader;
static ComPtr<ID3D11GeometryShader> geometry_shader;
static ComPtr<ID3D11GeometryShader> line_geometry_shader, point_geometry_shader;
static ComPtr<ID3D11PixelShader> uncombined_shader;
static ComPtr<ID3DBlob> vertex_code;

struct TargetExtent {
    uint32_t width = 0, height = 0;
};

struct Surface {
    uint8_t *memory;
    uint32_t width, height, pitch;
    bool dirty, needs_refresh, snapshot_valid;
    bool clear_value_valid;
    uint32_t clear_value;
    uint64_t last_used, content_serial, alias_copy_serial;
    D3D11_BOX written_region;
    std::vector<uint64_t> written_mask;
    std::vector<uint8_t> source_snapshot;
    ComPtr<ID3D11Texture2D> texture, staging;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
};

struct CachedTexture {
    const uint8_t *source;
    uint32_t width, height, format, cube, pitch, linear, face_stride, depth;
    uint32_t mip_levels;
    uint64_t bytes;
    uint64_t validated_serial;   /* frame serial this entry's memcmp last matched */
    bool force_validate;         /* a GPU blit overwrote this image; re-check now */
    std::vector<uint8_t> source_snapshot;
    ComPtr<ID3D11ShaderResourceView> view;
};

struct DepthSurface {
    uint8_t *memory;
    uint32_t width, height, pitch, format;
    bool dirty, needs_refresh, snapshot_valid;
    bool clear_value_valid;
    uint32_t clear_value;
    uint64_t last_used, content_serial;
    D3D11_BOX written_region;
    std::vector<uint64_t> written_mask;
    std::vector<uint8_t> source_snapshot;
    ComPtr<ID3D11Texture2D> texture, staging;
    ComPtr<ID3D11DepthStencilView> view;
};

struct Constants {
    uint32_t control[4], misc[4], stages[8][4], final_input[4];
    uint32_t texture_control[4];
    float factors[18][4], fog[4], viewport[4], texture_info[4][4];
    float bump_matrix[4][4], bump_luminance[4][4];
    float fog_parameters[4];
    uint32_t fog_control[4];
    float fog_plane[4];
    uint32_t texture_key[4][4];
    float depth_range[4];
    float depth_offset[4];
    float shader_eye_vector[4];
    float vertex_constants[192][4];
    /* Fixed-function lighting. */
    uint32_t light_state[4];     /* x=lighting_enable, y=specular_enable, z=light_enable_mask, w=color_material */
    uint32_t light_control[4];   /* flags, normalization, skin mode, texgen viewer */
    float material[4];           /* xyz=emission, w=alpha */
    float scene_ambient[4];      /* xyz */
    float specular_power[4];     /* x=specular exponent */
    float light_ambient[8][4];
    float light_diffuse[8][4];
    float light_specular[8][4];
    float light_local_position[8][4];    /* xyz=position, w=range */
    float light_local_attenuation[8][4]; /* xyz=constant/linear/quadratic */
    float light_infinite_direction[8][4];
    float light_infinite_half_vector[8][4];
    float light_spot_direction[8][4];    /* xyz=direction, w=spot parameter */
    uint32_t texgen[4][4], texture_matrix_enable[4];
    uint32_t window_clip_control[4], window_clip_rects[8][4];
};

static_assert(sizeof(Constants) % 16 == 0 && sizeof(Constants) <= 65536, "D3D11 constant-buffer extent");

static std::vector<Surface> surfaces;
static std::vector<DepthSurface> depth_surfaces;
static uint64_t surface_use_serial;
static uint64_t target_content_serial;
static std::vector<CachedTexture> textures;
/* Bind-time lookup: every texture bind used to scan the whole cache comparing
 * key fields. Guest addresses identify a texture, so index by source and fall
 * back to the linear scan only when the key fields differ. Rebuilt whenever the
 * cache size changes (creates and the rare eviction). */
static std::unordered_map<const void *, size_t> texture_index_by_source;
static size_t texture_index_count = (size_t)-1;
static void rebuild_texture_index()
{
    texture_index_by_source.clear();
    for (size_t i = 0; i < textures.size(); i++)
        texture_index_by_source.emplace(textures[i].source, i);
    texture_index_count = textures.size();
}
static ComPtr<ID3D11InputLayout> input_layout;
static ComPtr<ID3D11Buffer> constant_buffer;
static Constants uploaded_constants;
static bool uploaded_constants_valid;
struct DynamicStream {
    ComPtr<ID3D11Buffer> buffer;
    UINT capacity = 0, used = 0;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    bool active = false;   /* Map held across draws until the ring wraps / frame ends */
};
static std::array<DynamicStream,2> vertex_streams;
static DynamicStream index_stream;
static void release_stream(DynamicStream &stream);
static ComPtr<ID3D11RasterizerState> rasterizer;
static std::array<ComPtr<ID3D11RasterizerState>,12> raster_states;

struct BlendVariant {
    std::array<uint32_t,7> key;
    ComPtr<ID3D11BlendState> state;
};
static std::vector<BlendVariant> blend_variants;
static std::array<ComPtr<ID3D11DepthStencilState>,32> depth_states;
struct StencilVariant {
    std::array<uint32_t,7> key;
    ComPtr<ID3D11DepthStencilState> state;
};
static std::vector<StencilVariant> stencil_states;
struct SamplerVariant {
    std::array<uint32_t,9> key;
    ComPtr<ID3D11SamplerState> state;
};
static std::vector<SamplerVariant> sampler_cache;

struct ShaderVariant {
    std::array<uint32_t,43> key;
    ComPtr<ID3D11PixelShader> shader;
    uint64_t last_used, bytecode_bytes;
};
static std::vector<ShaderVariant> shader_variants;

struct VertexVariant {
    std::vector<uint32_t> key;
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11InputLayout> layout;
    uint64_t last_used, bytecode_bytes;
};
static std::vector<VertexVariant> vertex_variants;

static uint64_t shader_use_serial;

template<typename Variant>
static Variant *cache_shader_variant(std::vector<Variant> &cache, Variant &&variant)
{
    constexpr uint64_t budget = 16 * 1024 * 1024;
    constexpr size_t maximum_entries = 512;
    if (variant.bytecode_bytes > budget) {
        std::fprintf(stderr, "[GPU-D3D11] shader exceeds bytecode cache budget: %llu bytes\n",
                     (unsigned long long)variant.bytecode_bytes);
        return nullptr;
    }
    uint64_t bytes = 0;
    for (const auto &entry : cache) bytes += entry.bytecode_bytes;
    while (!cache.empty() && (cache.size() >= maximum_entries || bytes + variant.bytecode_bytes > budget)) {
        auto oldest = std::min_element(cache.begin(), cache.end(),
            [](const Variant &left, const Variant &right) { return left.last_used < right.last_used; });
        bytes -= oldest->bytecode_bytes;
        cache.erase(oldest);
        gpu_timing.shader_evictions++;
    }
    variant.last_used = ++shader_use_serial;
    cache.push_back(std::move(variant));
    return &cache.back();
}

static std::string vertex_mask(uint32_t mask)
{
    std::string result;
    const char components[] = "xyzw";
    for (uint32_t component = 0; component < 4; component++)
        if (mask & (8u >> component)) result += components[component];
    return result;
}

static std::string vertex_source(uint32_t mux, uint32_t temporary, uint32_t swizzle,
                                 uint32_t negate, uint32_t input, uint32_t constant, bool relative)
{
    std::string result;
    switch (mux) {
    case 1:
        if (temporary > 12) return {};
        result = temporary == 12 ? "outputs[0]" : "temporary[" + std::to_string(temporary) + "]";
        break;
    case 2: result = "input.attribute" + std::to_string(input); break;
    case 3:
        if (!relative && constant >= 192) return {};
        result = "vertexConstants[" + std::to_string(constant) + (relative ? "+address]" : "]");
        break;
    default: return {};
    }
    result += ".";
    const char components[] = "xyzw";
    for (uint32_t component = 0; component < 4; component++) result += components[(swizzle >> (6 - component * 2)) & 3];
    return negate ? "-(" + result + ")" : result;
}

static uint32_t vertex_program_word_count(const Nv2aGpuDraw &state)
{
    if (!state.vertex_program || !state.vertex_valid || !state.vertex_constants || state.vertex_start >= 136) return 0;
    for (uint32_t slot = state.vertex_start; slot < 136; slot++) {
        if (state.vertex_valid[slot] != 15) return 0;
        if (state.vertex_program[slot][3] & 1u) return (slot - state.vertex_start + 1) * 4;
    }
    return 0;
}

static bool vertex_program_key(const Nv2aGpuDraw &state, std::vector<uint32_t> &key)
{
    uint32_t words = vertex_program_word_count(state);
    if (!words) return false;
    key.resize(words);
    std::memcpy(key.data(), state.vertex_program + state.vertex_start, words * sizeof(uint32_t));
    return true;
}

static std::string vertex_program_source(const std::vector<uint32_t> &key, bool state_program)
{
    std::string source = state_program ? "" : "#define NV_FIXED_TRANSFORM 0\n";
    source += nv2a_gpu_shader_source;
    source += "\nstruct GuestVertex {\n";
    for (uint32_t attribute = 0; attribute < 16; attribute++)
        source += "float4 attribute" + std::to_string(attribute) + ":TEXCOORD" + std::to_string(attribute) + ";\n";
    source += "};\n";
    if (state_program) {
        source += "RWStructuredBuffer<float4> stateConstants:register(u0); cbuffer Attributes:register(b1) {float4 stateAttributes[16];};\n#define vertexConstants stateConstants\n[numthreads(1,1,1)]void cs_guest(uint3 dispatch:SV_DispatchThreadID) { GuestVertex input;\n";
        for (uint32_t attribute = 0; attribute < 16; attribute++)
            source += "input.attribute" + std::to_string(attribute) + "=stateAttributes[" + std::to_string(attribute) + "];\n";
    } else source += "Pixel vs_guest(GuestVertex input) {\n";
    source += "float4 temporary[13]; float4 outputs[16]; int address=0;\n";
    source += "[unroll]for(uint index=0;index<13;index++)temporary[index]=0; [unroll]for(uint index=0;index<16;index++)outputs[index]=float4(0,0,0,1);\n";
    for (size_t instruction = 0; instruction < key.size(); instruction += 4) {
        const uint32_t *words = key.data() + instruction;
        uint32_t mac = (words[1] >> 21) & 15, ilu = (words[1] >> 25) & 7;
        uint32_t input = (words[1] >> 9) & 15, constant = (words[1] >> 13) & 255;
        uint32_t destination = (words[3] >> 20) & 15, output = (words[3] >> 3) & 255;
        uint32_t mac_mask = (words[3] >> 24) & 15, ilu_mask = (words[3] >> 16) & 15, output_mask = (words[3] >> 12) & 15;
        bool relative = (words[3] & 2) != 0;
        if (mac > 13) return {};
        uint32_t usage = nv_cpu_vp_operand_usage(mac, ilu);
        std::string operand_a = usage & 1u ? vertex_source((words[2] >> 26) & 3, words[2] >> 28, words[1] & 255, (words[1] >> 8) & 1, input, constant, relative) : "float4(0,0,0,0)";
        std::string operand_b = usage & 2u ? vertex_source((words[2] >> 11) & 3, (words[2] >> 13) & 15, (words[2] >> 17) & 255, (words[2] >> 25) & 1, input, constant, relative) : "float4(0,0,0,0)";
        std::string operand_c = usage & 4u ? vertex_source((words[3] >> 28) & 3, ((words[2] & 3) << 2) | (words[3] >> 30), (words[2] >> 2) & 255, (words[2] >> 10) & 1, input, constant, relative) : "float4(0,0,0,0)";
        if (operand_a.empty() || operand_b.empty() || operand_c.empty()) return {};
        source += "{ float4 aa=" + operand_a + ",bb=" + operand_b + ",cc=" + operand_c + "; precise float4 macResult=0; float4 iluResult=0;\n";
        switch (mac) {
        case 0: case 13: break;
        case 1: source += "macResult=aa;\n"; break;
        case 2: source += "macResult=guest_multiply(aa,bb);\n"; break;
        case 3: source += "macResult=aa+cc;\n"; break;
        case 4: source += "macResult=guest_multiply(aa,bb)+cc;\n"; break;
        case 5: case 6: case 7:
            source += "float4 product=guest_multiply(aa,bb); macResult=product.x+product.y+product.z";
            source += mac == 6 ? "+bb.w;\n" : mac == 7 ? "+product.w;\n" : ";\n";
            break;
        case 8: source += "macResult=float4(1,guest_multiply(aa,bb).y,aa.z,bb.w);\n"; break;
        case 9: source += "macResult=min(aa,bb);\n"; break;
        case 10: source += "macResult=max(aa,bb);\n"; break;
        case 11: source += "macResult=float4(aa<bb);\n"; break;
        case 12: source += "macResult=float4(aa>=bb);\n"; break;
        }
        switch (ilu) {
        case 0: break;
        case 1: source += "iluResult=cc;\n"; break;
        case 2: source += "iluResult=1/cc.x;\n"; break;
        case 3: source += "float reciprocal=1/cc.x; iluResult=(reciprocal<0?-1:1)*clamp(abs(reciprocal),5.421010862427522e-20,1.8446744073709552e19);\n"; break;
        case 4: source += "iluResult=1/sqrt(abs(cc.x));\n"; break;
        case 5: source += "iluResult=float4(exp2(floor(cc.x)),cc.x-floor(cc.x),exp2(cc.x),1);\n"; break;
        case 6: source += "float scalar=abs(cc.x); float exponent=log2(scalar); iluResult=float4(floor(exponent),scalar==0?1:scalar/exp2(floor(exponent)),exponent,1);\n"; break;
        case 7: source += "iluResult=float4(1,max(cc.x,0),cc.x>0?pow(max(cc.y,0),clamp(cc.w,-127.99609375,127.99609375)):0,1);\n"; break;
        }
        auto write = [&](const std::string &target, const std::string &result, uint32_t mask) {
            std::string components = vertex_mask(mask);
            source += target + "." + components + "=" + result + "." + components + ";\n";
        };
        bool selected_ilu = (words[3] & 4) != 0;
        if (output_mask && (selected_ilu ? ilu : mac)) {
            std::string result = selected_ilu ? "iluResult" : "macResult";
            if (!(words[3] & 0x800)) {
                if (!state_program || output >= 192) return {};
                write("vertexConstants[" + std::to_string(output) + "]", result, output_mask);
            }
            else if ((output & 15) == 15) source += "address=(int)floor(" + result + ".x);\n";
            else if ((output & 15) == 5) source += "outputs[5].x=" + result + "." + vertex_mask(output_mask).substr(0,1) + ";\n";
            else write("outputs[" + std::to_string(output & 15) + "]", result, output_mask);
        }
        if (mac == 13) source += "address=(int)floor(aa.x);\n";
        else if (mac && mac_mask && !(ilu && destination == 1)) {
            if (destination > 12) return {};
            write(destination == 12 ? "outputs[0]" : "temporary[" + std::to_string(destination) + "]", "macResult", mac_mask);
        }
        if (ilu && ilu_mask) {
            uint32_t ilu_destination = mac ? 1 : destination;
            if (ilu_destination > 12) return {};
            write(ilu_destination == 12 ? "outputs[0]" : "temporary[" + std::to_string(ilu_destination) + "]", "iluResult", ilu_mask);
        }
        source += "}\n";
    }
    if (state_program) source += "}\n";
    else source += "Vertex bridge=(Vertex)0; bridge.position=outputs[0]; bridge.diffuse=saturate(outputs[3]); bridge.specular=saturate(outputs[4]); bridge.tex0=outputs[9]; bridge.tex1=outputs[10]; bridge.tex2=outputs[11]; bridge.tex3=outputs[12]; bridge.fogCoordinate=outputs[5].x; Pixel result=vs_main(bridge); result.fog=fog_factor(outputs[5].x); return result; }\n";
    return source;
}

static VertexVariant *get_vertex_shader(const Nv2aGpuDraw &state)
{
    uint32_t words = vertex_program_word_count(state);
    if (!words) return nullptr;
    for (auto &cached : vertex_variants) if (cached.key.size() == words &&
        !std::memcmp(cached.key.data(), state.vertex_program + state.vertex_start, words * sizeof(uint32_t))) {
        cached.last_used = ++shader_use_serial;
        gpu_timing.vertex_shader_hits++;
        return &cached;
    }
    VertexVariant variant;
    variant.key.resize(words);
    std::memcpy(variant.key.data(), state.vertex_program + state.vertex_start, words * sizeof(uint32_t));
    gpu_timing.vertex_shader_misses++;
    GpuTimer compile_timer(gpu_timing.vertex_compile);
    std::string source = vertex_program_source(variant.key, false);
    if (source.empty()) return nullptr;
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompile(source.data(), source.size(), "NV2A-vertex", nullptr, nullptr, "vs_guest", "vs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result)) {
        gpu_timing.vertex_shader_failures++;
        std::fprintf(stderr, "[GPU-D3D11] vertex compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return nullptr;
    }
    D3D11_INPUT_ELEMENT_DESC elements[16] = {};
    for (uint32_t attribute = 0; attribute < 16; attribute++) {
        elements[attribute].SemanticName = "TEXCOORD"; elements[attribute].SemanticIndex = attribute;
        elements[attribute].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        elements[attribute].AlignedByteOffset = (UINT)offsetof(Nv2aGpuVertex, attributes) + attribute * 16;
        elements[attribute].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    }
    if (FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &variant.shader)) ||
        FAILED(device->CreateInputLayout(elements, 16, code->GetBufferPointer(), code->GetBufferSize(), &variant.layout))) return nullptr;
    variant.bytecode_bytes = code->GetBufferSize();
    return cache_shader_variant(vertex_variants, std::move(variant));
}

extern "C" int nv2a_gpu_execute_state(const uint32_t program[136][4], const uint32_t valid[136], uint32_t start,
                                      const float attributes[16][4], float constants[192][4])
{
    GpuTimer state_timer(gpu_timing.state);
    struct StateVariant {
        std::vector<uint32_t> key;
        ComPtr<ID3D11ComputeShader> shader;
    };
    static std::vector<StateVariant> variants;
    if (!program || !valid || !attributes || !constants || !nv2a_gpu_available()) return 0;
    Nv2aGpuDraw state = {};
    state.vertex_program = program; state.vertex_valid = valid; state.vertex_start = start; state.vertex_constants = constants;
    StateVariant variant;
    if (!vertex_program_key(state, variant.key)) return 0;
    ComPtr<ID3D11ComputeShader> shader;
    for (auto &cached : variants) if (cached.key == variant.key) { shader = cached.shader; break; }
    if (!shader) {
        std::string source = vertex_program_source(variant.key, true);
        if (source.empty()) return 0;
        ComPtr<ID3DBlob> code, errors;
        HRESULT result = D3DCompile(source.data(), source.size(), "NV2A-state", nullptr, nullptr, "cs_guest", "cs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] state compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
            return 0;
        }
        if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &variant.shader))) return 0;
        shader = variant.shader;
        if (variants.size() >= 128) variants.erase(variants.begin());
        variants.push_back(std::move(variant));
    }
    ComPtr<ID3D11Buffer> storage, staging, attribute_buffer;
    ComPtr<ID3D11UnorderedAccessView> writable;
    D3D11_BUFFER_DESC description = {};
    description.ByteWidth = 192 * 4 * sizeof(float); description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS; description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = 4 * sizeof(float);
    D3D11_SUBRESOURCE_DATA data = {constants, 0, 0};
    if (FAILED(device->CreateBuffer(&description, &data, &storage))) return 0;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements = 192;
    if (FAILED(device->CreateUnorderedAccessView(storage.Get(), &view, &writable))) return 0;
    description.BindFlags = 0; description.MiscFlags = 0; description.StructureByteStride = 0;
    description.Usage = D3D11_USAGE_STAGING; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateBuffer(&description, nullptr, &staging))) return 0;
    description.ByteWidth = 16 * 4 * sizeof(float); description.Usage = D3D11_USAGE_IMMUTABLE;
    description.CPUAccessFlags = 0; description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    data.pSysMem = attributes;
    if (FAILED(device->CreateBuffer(&description, &data, &attribute_buffer))) return 0;
    ID3D11UnorderedAccessView *views[] = {writable.Get()}, *empty_views[] = {nullptr};
    ID3D11Buffer *buffers[] = {attribute_buffer.Get()}, *empty_buffers[] = {nullptr};
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(1, 1, buffers); context->CSSetUnorderedAccessViews(0, 1, views, nullptr);
    context->Dispatch(1, 1, 1);
    context->CSSetUnorderedAccessViews(0, 1, empty_views, nullptr);
    context->CSSetConstantBuffers(1, 1, empty_buffers); context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(staging.Get(), storage.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 0;
    std::memcpy(constants, mapped.pData, 192 * 4 * sizeof(float)); context->Unmap(staging.Get(), 0);
    return 1;
}

static ID3D11PixelShader *get_pixel_shader(const Nv2aGpuDraw &state, uint32_t window_clip_mode)
{
    ShaderVariant variant = {};
    variant.key[0] = state.combiners.control; variant.key[1] = state.stage_program;
    variant.key[2] = state.combiners.final_input[0]; variant.key[3] = state.combiners.final_input[1];
    variant.key[36] = (state.control0 & 0x10000) != 0;
    variant.key[37] = state.depth_enable != 0;
    variant.key[42] = window_clip_mode;
    for (uint32_t stage = 0; stage < 4; stage++)
        if (nv2a_gpu_texture_mode_samples((state.stage_program >> (stage * 5)) & 31u))
            variant.key[38 + stage] = state.textures[stage].filter >> 28;
    for (uint32_t stage = 0; stage < (state.combiners.control & 255u); stage++) {
        variant.key[4+stage*4] = state.combiners.rgb_input[stage]; variant.key[5+stage*4] = state.combiners.alpha_input[stage];
        variant.key[6+stage*4] = state.combiners.rgb_output[stage]; variant.key[7+stage*4] = state.combiners.alpha_output[stage];
    }
    for (auto &cached : shader_variants) if (cached.key == variant.key) {
        cached.last_used = ++shader_use_serial;
        gpu_timing.pixel_shader_hits++;
        return cached.shader.Get();
    }
    gpu_timing.pixel_shader_misses++;
    GpuTimer compile_timer(gpu_timing.pixel_compile);
    std::array<std::string,8> values;
    values[0] = std::to_string(variant.key[0] & 255u) + "u";
    values[1] = std::to_string(variant.key[0]) + "u"; values[2] = std::to_string(variant.key[1]) + "u";
    values[3] = std::to_string(variant.key[2]) + "u"; values[4] = std::to_string(variant.key[3]) + "u";
    values[5] = "{";
    for (uint32_t stage = 0; stage < 8; stage++) {
        if (stage) values[5] += ",";
        values[5] += "uint4(";
        for (uint32_t component = 0; component < 4; component++) {
            if (component) values[5] += ",";
            values[5] += std::to_string(variant.key[4+stage*4+component]) + "u";
        }
        values[5] += ")";
    }
    values[5] += "}";
    values[6] = "uint4(";
    for (uint32_t stage = 0; stage < 4; stage++) {
        if (stage) values[6] += ",";
        values[6] += std::to_string(variant.key[38 + stage]) + "u";
    }
    values[6] += ")";
    values[7] = std::to_string(variant.key[42]) + "u";
    const D3D_SHADER_MACRO definitions[] = {
        {"NV_COMPILED","1"}, {"NV_STAGE_COUNT",values[0].c_str()}, {"NV_COMBINER_CONTROL",values[1].c_str()},
        {"NV_STAGE_PROGRAM",values[2].c_str()}, {"NV_FINAL0",values[3].c_str()}, {"NV_FINAL1",values[4].c_str()},
        {"NV_STAGE_WORDS",values[5].c_str()}, {"NV_W_DEPTH",variant.key[36] ? "1" : "0"},
        {"NV_TEXTURE_SIGNS",values[6].c_str()},
        {"NV_WINDOW_CLIP_MODE",values[7].c_str()},
        {"NV_DEPTH_ENABLED",variant.key[37] ? "1" : "0"},
        {"NV_DEPTH_SEMANTIC",variant.key[36] ? "SV_DepthGreaterEqual" : "SV_DEPTH"}, {nullptr,nullptr}
    };
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A-guest", definitions, nullptr,
                               "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result)) {
        gpu_timing.pixel_shader_failures++;
        std::fprintf(stderr, "[GPU-D3D11] guest shader compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return nullptr;
    }
    if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &variant.shader))) return nullptr;
    variant.bytecode_bytes = code->GetBufferSize();
    auto *cached = cache_shader_variant(shader_variants, std::move(variant));
    return cached ? cached->shader.Get() : nullptr;
}

template<typename SurfaceType>
static void mark_written_mask(SurfaceType &surface, const D3D11_BOX &region)
{
    gpu_timing.coverage_mask_updates++;
    for (uint32_t row = region.top; row < region.bottom; row++) {
        size_t first = (size_t)row * surface.width + region.left;
        size_t end = (size_t)row * surface.width + region.right;
        size_t first_word = first / 64, last_word = (end - 1) / 64;
        uint64_t leading = ~uint64_t(0) << (first % 64);
        uint64_t trailing = end % 64 ? (uint64_t(1) << (end % 64)) - 1 : ~uint64_t(0);
        if (first_word == last_word) surface.written_mask[first_word] |= leading & trailing;
        else {
            surface.written_mask[first_word] |= leading;
            std::fill(surface.written_mask.begin() + first_word + 1, surface.written_mask.begin() + last_word, ~uint64_t(0));
            surface.written_mask[last_word] |= trailing;
        }
    }
}

template<typename SurfaceType>
static void mark_written_region(SurfaceType &surface, const D3D11_BOX &region)
{
    surface.clear_value_valid = false;
    if (!surface.dirty) {
        surface.written_region = region;
        surface.written_mask.clear();
    }
    else {
        auto &written = surface.written_region;
        bool contains = region.left <= written.left && region.top <= written.top &&
                        region.right >= written.right && region.bottom >= written.bottom;
        bool contained = written.left <= region.left && written.top <= region.top &&
                         written.right >= region.right && written.bottom >= region.bottom;
        bool rectangle = (written.left == region.left && written.right == region.right &&
                          region.top <= written.bottom && written.top <= region.bottom) ||
                         (written.top == region.top && written.bottom == region.bottom &&
                          region.left <= written.right && written.left <= region.right);
        if (contains) surface.written_mask.clear();
        else if (!surface.written_mask.empty()) mark_written_mask(surface, region);
        else if (!contained && !rectangle) {
            surface.written_mask.assign(((size_t)surface.width * surface.height + 63) / 64, 0);
            mark_written_mask(surface, written);
            mark_written_mask(surface, region);
        }
        if (region.left < written.left) written.left = region.left;
        if (region.top < written.top) written.top = region.top;
        if (region.right > written.right) written.right = region.right;
        if (region.bottom > written.bottom) written.bottom = region.bottom;
    }
    surface.dirty = true;
    surface.content_serial = ++target_content_serial;
}

template<typename SurfaceType, typename Publish>
static void for_each_written_span(const SurfaceType &surface, uint32_t row, const Publish &publish)
{
    const auto &region = surface.written_region;
    if (row < region.top || row >= region.bottom) return;
    if (surface.written_mask.empty()) { publish(region.left, region.right); return; }
    gpu_timing.coverage_mask_rows++;
    uint32_t column = region.left;
    auto covered = [&](uint32_t offset) {
        size_t bit = (size_t)row * surface.width + offset;
        return (surface.written_mask[bit / 64] >> (bit % 64)) & 1;
    };
    while (column < region.right) {
        while (column < region.right && !covered(column)) column++;
        uint32_t first = column;
        while (column < region.right && covered(column)) column++;
        if (first != column) publish(first, column);
    }
}

template<bool ToGuest>
static void rotate_depth_row(const uint8_t *source, uint8_t *destination, uint32_t columns)
{
    uint32_t column = 0;
#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    for (; columns - column >= 4; column += 4) {
        __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + (size_t)column * 4));
        if constexpr (ToGuest) value = _mm_or_si128(_mm_slli_epi32(value, 8), _mm_srli_epi32(value, 24));
        else value = _mm_or_si128(_mm_srli_epi32(value, 8), _mm_slli_epi32(value, 24));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + (size_t)column * 4), value);
    }
#endif
    for (; column < columns; column++) {
        uint32_t value;
        std::memcpy(&value, source + (size_t)column * 4, 4);
        if constexpr (ToGuest) value = (value << 8) | (value >> 24);
        else value = (value >> 8) | (value << 24);
        std::memcpy(destination + (size_t)column * 4, &value, 4);
    }
}

static void decode_depth_row(uint32_t format, const uint8_t *source, uint8_t *destination, uint32_t columns)
{
    if (format == 1) {
        if (source != destination) std::memcpy(destination, source, (size_t)columns * 2);
    } else rotate_depth_row<true>(source, destination, columns);
}

static void encode_depth_row(uint32_t format, const uint8_t *source, uint8_t *destination, uint32_t columns)
{
    if (format == 1) {
        if (source != destination) std::memcpy(destination, source, (size_t)columns * 2);
    } else rotate_depth_row<false>(source, destination, columns);
}

template<typename SurfaceType>
static bool prepare_surface_snapshot(SurfaceType &surface, uint32_t bytes)
{
    if (!surface.source_snapshot.empty()) return true;
    const size_t budget = 64 * 1024 * 1024;
    size_t required = (size_t)surface.width * surface.height * bytes;
    size_t allocated = 0;
    for (const auto &entry : surfaces) allocated += entry.source_snapshot.size();
    for (const auto &entry : depth_surfaces) allocated += entry.source_snapshot.size();
    if (required > budget - allocated) return false;
    surface.source_snapshot.resize(required);
    return true;
}

template<typename SurfaceType>
static bool surface_memory_matches(const SurfaceType &surface, uint32_t bytes)
{
    if (!surface.snapshot_valid || surface.dirty) return false;
    size_t row_bytes = (size_t)surface.width * bytes;
    for (uint32_t row = 0; row < surface.height; row++)
        if (std::memcmp(surface.memory + (size_t)row * surface.pitch,
                        surface.source_snapshot.data() + (size_t)row * row_bytes, row_bytes)) return false;
    return true;
}

static void refresh_surface(Surface &surface)
{
    if (!surface.needs_refresh) return;
    /* A dirty surface holds GPU content newer than guest RAM (it has been drawn
     * into but not published). Uploading RAM here would overwrite the rendered
     * image with the previous publish -- the corruption seen when publication
     * went lazy. Keep the GPU copy; the surface is published at the next real
     * fence, and the texture path samples the GPU view while it is dirty. */
    if (surface.dirty) { surface.needs_refresh = false; return; }
    if (surface.snapshot_valid && !surface.dirty) {
        size_t row_bytes = (size_t)surface.width * 4;
        D3D11_BOX changed = {surface.width, surface.height, 0, 0, 0, 1};
        for (uint32_t row = 0; row < surface.height; row++) {
            const uint8_t *source = surface.memory + (size_t)row * surface.pitch;
            const uint8_t *cached = surface.source_snapshot.data() + (size_t)row * row_bytes;
            if (!std::memcmp(source, cached, row_bytes)) continue;
            uint32_t first = 0, end = surface.width;
            while (first < end && !std::memcmp(source + first * 4, cached + first * 4, 4)) first++;
            while (end > first && !std::memcmp(source + (end - 1) * 4, cached + (end - 1) * 4, 4)) end--;
            if (first < changed.left) changed.left = first;
            if (end > changed.right) changed.right = end;
            if (row < changed.top) changed.top = row;
            changed.bottom = row + 1;
        }
        if (changed.left < changed.right) {
            surface.clear_value_valid = false;
            for (uint32_t row = changed.top; row < changed.bottom; row++)
                std::memcpy(surface.source_snapshot.data() + (size_t)row * row_bytes + changed.left * 4,
                            surface.memory + (size_t)row * surface.pitch + changed.left * 4,
                            (size_t)(changed.right - changed.left) * 4);
            context->UpdateSubresource(surface.texture.Get(), 0, &changed,
                                      surface.source_snapshot.data() + (size_t)changed.top * row_bytes + changed.left * 4,
                                      (UINT)row_bytes, 0);
            surface.alias_copy_serial = 0;
            gpu_timing.color_refreshed++;
            gpu_timing.color_refresh_bytes += (uint64_t)(changed.right - changed.left) * (changed.bottom - changed.top) * 4;
            gpu_timing.color_refresh_full_bytes += (uint64_t)surface.width * surface.height * 4;
        } else gpu_timing.color_refresh_reused++;
        surface.needs_refresh = false;
        return;
    }
    context->UpdateSubresource(surface.texture.Get(), 0, nullptr, surface.memory, surface.pitch, 0);
    surface.clear_value_valid = false;
    surface.alias_copy_serial = 0;
    gpu_timing.color_refresh_bytes += (uint64_t)surface.width * surface.height * 4;
    gpu_timing.color_refresh_full_bytes += (uint64_t)surface.width * surface.height * 4;
    surface.needs_refresh = false;
    surface.snapshot_valid = false;
    gpu_timing.color_refreshed++;
}

static bool target_operation_succeeded(const char *kind, const char *operation, HRESULT result)
{
    if (SUCCEEDED(result)) return true;
    std::fprintf(stderr, "[GPU-D3D11] %s target %s failed: 0x%08X; device 0x%08X\n",
                 kind, operation, (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
    std::fflush(stderr);
    return false;
}

static bool refresh_depth(DepthSurface &surface)
{
    if (!surface.needs_refresh) return true;
    if (surface_memory_matches(surface, surface.format == 1 ? 2 : 4)) {
        surface.needs_refresh = false;
        gpu_timing.depth_refresh_reused++;
        return true;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!target_operation_succeeded("depth", "upload map",
        context->Map(surface.staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;
    for (uint32_t row = 0; row < surface.height; row++) {
        const uint8_t *source = surface.memory + (size_t)row * surface.pitch;
        uint8_t *destination = (uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
        encode_depth_row(surface.format, source, destination, surface.width);
    }
    context->Unmap(surface.staging.Get(), 0);
    context->CopyResource(surface.texture.Get(), surface.staging.Get());
    surface.clear_value_valid = false;
    surface.needs_refresh = false;
    surface.snapshot_valid = false;
    gpu_timing.depth_refreshed++;
    return true;
}

static Surface *get_surface(const Nv2aGpuDraw &state, bool preserve = true, TargetExtent extent = {})
{
    if (!extent.width) extent = {state.width, state.height};
    for (auto &surface : surfaces)
        if (surface.memory == state.color && surface.width == extent.width && surface.height == extent.height && surface.pitch == state.pitch) {
            surface.last_used = ++surface_use_serial;
            if (preserve) refresh_surface(surface);
            else surface.needs_refresh = false;
            return &surface;
        }
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = extent.width; description.Height = extent.height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Surface surface = {};
    surface.memory = state.color; surface.width = extent.width; surface.height = extent.height; surface.pitch = state.pitch;
    surface.last_used = ++surface_use_serial;
    D3D11_SUBRESOURCE_DATA data = {state.color, state.pitch, 0};
    if (!target_operation_succeeded("color", "create texture", device->CreateTexture2D(&description, preserve ? &data : nullptr, &surface.texture)) ||
        !target_operation_succeeded("color", "create view", device->CreateRenderTargetView(surface.texture.Get(), nullptr, &surface.target)) ||
        !target_operation_succeeded("color", "create sample view", device->CreateShaderResourceView(surface.texture.Get(), nullptr, &surface.view))) return nullptr;
    description.BindFlags = 0; description.Usage = D3D11_USAGE_STAGING; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (!target_operation_succeeded("color", "create staging", device->CreateTexture2D(&description, nullptr, &surface.staging))) return nullptr;
    surfaces.push_back(std::move(surface));
    gpu_timing.color_created++;
    return &surfaces.back();
}

static DepthSurface *get_depth(const Nv2aGpuDraw &state, bool preserve = true, TargetExtent extent = {})
{
    if (!extent.width) extent = {state.width, state.height};
    if (!state.depth || (state.depth_format != 1 && state.depth_format != 2)) {
        std::fprintf(stderr, "[GPU-D3D11] invalid depth target: memory %p format %u\n",
                     (void *)state.depth, state.depth_format);
        return nullptr;
    }
    uint32_t bytes = state.depth_format == 1 ? 2 : 4;
    if (state.depth_pitch < extent.width * bytes) {
        std::fprintf(stderr, "[GPU-D3D11] depth target pitch %u cannot hold %u columns of format %u\n",
                     state.depth_pitch, extent.width, state.depth_format);
        return nullptr;
    }
    for (auto &surface : depth_surfaces)
        if (surface.memory == state.depth && surface.width == extent.width && surface.height == extent.height &&
            surface.pitch == state.depth_pitch && surface.format == state.depth_format) {
            surface.last_used = ++surface_use_serial;
            if (!preserve) { surface.needs_refresh = false; return &surface; }
            return refresh_depth(surface) ? &surface : nullptr;
        }
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = extent.width; description.Height = extent.height;
    description.MipLevels = description.ArraySize = 1; description.SampleDesc.Count = 1;
    description.Format = bytes == 2 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    DepthSurface surface = {};
    surface.memory = state.depth; surface.width = extent.width; surface.height = extent.height; surface.pitch = state.depth_pitch; surface.format = state.depth_format;
    surface.last_used = ++surface_use_serial;
    D3D11_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = bytes == 2 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT; view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (!target_operation_succeeded("depth", "create texture", device->CreateTexture2D(&description, nullptr, &surface.texture)) ||
        !target_operation_succeeded("depth", "create view", device->CreateDepthStencilView(surface.texture.Get(), &view, &surface.view))) return nullptr;
    description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (!target_operation_succeeded("depth", "create staging", device->CreateTexture2D(&description, nullptr, &surface.staging))) return nullptr;
    // Creation-data uploads can round D24 values; use the bit-preserving refresh path.
    surface.needs_refresh = preserve;
    if (preserve && !refresh_depth(surface)) return nullptr;
    depth_surfaces.push_back(std::move(surface)); gpu_timing.depth_surface_created++;
    return &depth_surfaces.back();
}

static void readback_failed(const char *kind, HRESULT result, const char *operation = "map")
{
    std::fprintf(stderr, "[GPU-D3D11] readback failed: %s %s 0x%08X; device 0x%08X\n",
                 kind, operation, (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

static void mark_target_aliases(const void *owner, const uint8_t *memory, size_t bytes)
{
    uintptr_t begin = (uintptr_t)memory, end = begin + bytes;
    auto mark = [&](auto &surface) {
        uintptr_t surface_begin = (uintptr_t)surface.memory;
        uintptr_t surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if ((const void *)&surface != owner && begin < surface_end && end > surface_begin)
            surface.needs_refresh = true;
    };
    for (auto &surface : surfaces) mark(surface);
    for (auto &surface : depth_surfaces) mark(surface);
}

static void fill_clear_row(uint8_t *destination, uint32_t columns, uint32_t value, uint32_t bytes)
{
    if (bytes == 4) {
        for (uint32_t column = 0; column < columns; column++)
            std::memcpy(destination + (size_t)column * 4, &value, 4);
    } else {
        uint16_t short_value = (uint16_t)value;
        for (uint32_t column = 0; column < columns; column++)
            std::memcpy(destination + (size_t)column * 2, &short_value, 2);
    }
}

template<typename SurfaceType>
static void publish_known_clear(SurfaceType &surface, uint32_t bytes)
{
    bool snapshot = prepare_surface_snapshot(surface, bytes);
    size_t row_bytes = (size_t)surface.width * bytes;
    if (snapshot) fill_clear_row(surface.source_snapshot.data(), surface.width, surface.clear_value, bytes);
    for (uint32_t row = 0; row < surface.height; row++) {
        uint8_t *destination = surface.memory + (size_t)row * surface.pitch;
        if (snapshot) {
            uint8_t *cached = surface.source_snapshot.data() + (size_t)row * row_bytes;
            if (row) std::memcpy(cached, surface.source_snapshot.data(), row_bytes);
            std::memcpy(destination, cached, row_bytes);
        } else fill_clear_row(destination, surface.width, surface.clear_value, bytes);
    }
    surface.snapshot_valid = snapshot;
    gpu_timing.clear_readback_bytes_avoided += row_bytes * surface.height;
    mark_target_aliases(&surface, surface.memory, (size_t)surface.pitch * surface.height);
    surface.dirty = false;
}

/* Texture re-validation pacing.
 *
 * Deciding whether a bound texture changed means memcmp'ing the whole image
 * from guest RAM against its snapshot. The menu bound textures ~2.7x their
 * total size per frame, so the same unchanged image was compared several times
 * a frame; measured, that was 62 GiB and ~24% of runtime for ~1,000 real
 * uploads. Validating each texture at most once per frame keeps the first
 * comparison and reuses its verdict for the rest of the frame.
 *
 * The serial advances on every flip (nv2a_gpu_sync). Disable with
 * RECOMP_TEX_VALIDATE_ONCE=0 if a title writes a texture mid-frame and samples
 * it again -- on hardware that is a race anyway, but a title might rely on it.
 */
static uint64_t s_texture_validate_serial = 1;
static int s_tex_validate_once = -1;
static bool tex_validate_once(void)
{
    if (s_tex_validate_once < 0) {
        const char *value = std::getenv("RECOMP_TEX_VALIDATE_ONCE");
        s_tex_validate_once = (value && value[0] == '0') ? 0 : 1;
    }
    return s_tex_validate_once != 0;
}

/* Depth surfaces are only ever consumed by a guest CPU read of their memory;
 * nothing presents them, and targeted publishes never carry depth anyway. The
 * full-sync publication (CopyResource + Map + de-swizzle) is otherwise pure
 * wall-clock per flip. Default on (other titles may read depth back);
 * RECOMP_DEPTH_PUBLISH=0 skips it, which this game does. */
static bool publish_depth(void)
{
    static int on = -1;
    if (on < 0) {
        const char *value = std::getenv("RECOMP_DEPTH_PUBLISH");
        on = (value && value[0] == '0') ? 0 : 1;
    }
    return on != 0;
}

/* Ordering events (WAIT_FOR_IDLE, semaphore release) only need the GPU to have
 * caught up; they do not read surfaces back. Waiting for every one serialises
 * the guest against the GPU -- ~39 full round trips per frame -- but draining
 * nothing at all was measured a net loss (the guest races ahead and the flip
 * drains a deep backlog). Keep a small ring of event queries and only wait for
 * the one from RECOMP_SYNC_LAG events ago, so the GPU runs a bounded distance
 * ahead. RECOMP_SYNC_LAG=0 restores the per-event wait. */
static void wait_ordering_completion(void)
{
    static ComPtr<ID3D11Query> ring[8];
    static unsigned produced;
    static int lag = -1;
    if (lag < 0) {
        const char *value = std::getenv("RECOMP_SYNC_LAG");
        lag = value ? std::atoi(value) : 4;
        if (lag < 0) lag = 0;
        if (lag > 7) lag = 7;
    }
    unsigned slot = produced++ & 7u;
    if (!ring[slot]) {
        D3D11_QUERY_DESC description = {D3D11_QUERY_EVENT, 0};
        HRESULT created = device->CreateQuery(&description, &ring[slot]);
        if (FAILED(created)) readback_failed("completion event", created, "create");
    }
    context->End(ring[slot].Get());
    context->Flush();
    if (produced > (unsigned)lag) {
        ID3D11Query *old = ring[(produced - 1 - (unsigned)lag) & 7u].Get();
        while (context->GetData(old, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE)
            SwitchToThread();
    }
}

static void gpu_sync_impl(bool publish, const void *only = nullptr, size_t only_bytes = 0)
{
    GpuTimer sync_timer(gpu_timing.sync);
    if (!context) return;
    const uintptr_t only_begin = (uintptr_t)only;
    const uintptr_t only_end = only_begin + only_bytes;
    auto wanted = [&](const Surface &surface) {
        if (!only) return true;
        uintptr_t begin = (uintptr_t)surface.memory;
        uintptr_t end = begin + (size_t)surface.pitch * surface.height;
        return only_begin < end && only_end > begin;
    };
    gpu_timing.sync_calls++;
    if (!publish) {
        /* Completion only: wait for the GPU to finish, but do not copy any
         * surface back to guest RAM. WAIT_FOR_IDLE and the semaphore release
         * only need ordering -- the guest is not about to read the surface --
         * and they fire ~50x a frame, so a full readback on each was most of
         * the sync cost. Nothing is marked clean, so the next real publish
         * still writes the final contents. The texture serial is deliberately
         * NOT advanced here: this is not a frame boundary. */
        if (pending_draws) {
            GpuTimer wait_timer(gpu_timing.sync_event_wait);
            gpu_timing.completion_waits++;
            wait_ordering_completion();
            pending_draws = false;
        }
        return;
    }
    ID3D11RenderTargetView *empty = nullptr;
    context->OMSetRenderTargets(1, &empty, nullptr);
    bool has_readback = false;
    bool has_known_clear = false;
    {
        GpuTimer copy_timer(gpu_timing.sync_copy);
        for (auto &surface : surfaces)
            if (surface.dirty) {
                if (!wanted(surface)) continue;
                if (surface.clear_value_valid) {
                    has_known_clear = true;
                    continue;
                }
                if (surface.snapshot_valid) {
                    const auto &region = surface.written_region;
                    context->CopySubresourceRegion(surface.staging.Get(), 0, region.left, region.top, 0,
                                                  surface.texture.Get(), 0, &region);
                    gpu_timing.color_copy_bytes += (uint64_t)(region.right - region.left) * (region.bottom - region.top) * 4;
                } else {
                    context->CopyResource(surface.staging.Get(), surface.texture.Get());
                    gpu_timing.color_copy_bytes += (uint64_t)surface.width * surface.height * 4;
                }
                has_readback = true;
            }
        if (!only && publish_depth())
        for (auto &surface : depth_surfaces)
            if (surface.dirty) {
                if (surface.clear_value_valid) {
                    has_known_clear = true;
                    continue;
                }
                context->CopyResource(surface.staging.Get(), surface.texture.Get());
                has_readback = true;
            }
    }
    /* Targeted publish with nothing overlapping dirty: nothing to pull back,
     * so don't pay a GPU-completion wait per blit. */
    if (only && !has_readback && !has_known_clear) return;
    if (pending_draws && (!has_readback || has_known_clear)) {
        GpuTimer wait_timer(gpu_timing.sync_event_wait);
        gpu_timing.completion_waits++;
        if (!completion_event) {
            D3D11_QUERY_DESC description = {D3D11_QUERY_EVENT, 0};
            HRESULT result = device->CreateQuery(&description, &completion_event);
            if (FAILED(result)) readback_failed("completion event", result, "create");
        }
        context->End(completion_event.Get());
        context->Flush();
        HRESULT result;
        while ((result = context->GetData(completion_event.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
            SwitchToThread();
        if (FAILED(result)) readback_failed("completion event", result, "wait");
    }
    for (auto &surface : surfaces) {
        if (!surface.dirty) continue;
        if (!wanted(surface)) continue;
        if (surface.clear_value_valid) {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer color_timer(gpu_timing.color_publish);
            publish_known_clear(surface, 4);
            gpu_timing.color_publish_bytes += (uint64_t)surface.width * surface.height * 4;
            gpu_timing.color_clear_publications++;
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result;
        {
            GpuTimer wait_timer(gpu_timing.sync_wait);
            result = context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
        if (FAILED(result)) readback_failed("color", result);
        gpu_timing.color_readbacks++;
        {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer color_timer(gpu_timing.color_publish);
            bool snapshot = prepare_surface_snapshot(surface, 4);
            bool initialize_snapshot = snapshot && !surface.snapshot_valid;
            const auto &region = surface.written_region;
            size_t row_bytes = (size_t)surface.width * 4;
            uint32_t first_row = initialize_snapshot ? 0 : region.top;
            uint32_t last_row = initialize_snapshot ? surface.height : region.bottom;
            if (surface.written_mask.empty() && region.left == 0 && region.right == surface.width &&
                mapped.RowPitch == row_bytes && surface.pitch == row_bytes) {
                const uint8_t *source = (const uint8_t *)mapped.pData;
                size_t offset = (size_t)region.top * row_bytes;
                size_t bytes = (size_t)(region.bottom - region.top) * row_bytes;
                if (snapshot) {
                    if (initialize_snapshot)
                        std::memcpy(surface.source_snapshot.data(), source, row_bytes * surface.height);
                    else
                        std::memcpy(surface.source_snapshot.data() + offset, source + offset, bytes);
                    source = surface.source_snapshot.data();
                }
                std::memcpy(surface.memory + offset, source + offset, bytes);
                gpu_timing.color_publish_bytes += bytes;
                gpu_timing.color_bulk_readbacks++;
            } else for (uint32_t row = first_row; row < last_row; row++) {
                const uint8_t *source = (const uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
                uint8_t *cached = snapshot ? surface.source_snapshot.data() + (size_t)row * row_bytes : nullptr;
                if (initialize_snapshot) std::memcpy(cached, source, row_bytes);
                for_each_written_span(surface, row, [&](uint32_t first, uint32_t end) {
                    size_t bytes = (size_t)(end - first) * 4;
                    if (cached && !initialize_snapshot) std::memcpy(cached + first * 4, source + first * 4, bytes);
                    std::memcpy(surface.memory + (size_t)row * surface.pitch + first * 4,
                                (cached ? cached : source) + first * 4, bytes);
                    gpu_timing.color_publish_bytes += bytes;
                });
            }
            surface.snapshot_valid = snapshot;
        }
        {
            GpuTimer unmap_timer(gpu_timing.sync_unmap);
            context->Unmap(surface.staging.Get(), 0);
        }
        mark_target_aliases(&surface, surface.memory, (size_t)surface.pitch * surface.height);
        surface.dirty = false;
    }
    if (!only && !publish_depth())
        for (auto &surface : depth_surfaces) surface.dirty = false;
    if (!only && publish_depth())
    for (auto &surface : depth_surfaces) {
        if (!surface.dirty) continue;
        if (surface.clear_value_valid) {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer depth_timer(gpu_timing.depth_publish);
            uint32_t bytes = surface.format == 1 ? 2 : 4;
            publish_known_clear(surface, bytes);
            gpu_timing.depth_publish_bytes += (uint64_t)surface.width * surface.height * bytes;
            gpu_timing.depth_clear_publications++;
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result;
        {
            GpuTimer wait_timer(gpu_timing.sync_wait);
            result = context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
        if (FAILED(result)) readback_failed("depth", result);
        gpu_timing.depth_readbacks++;
        {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer depth_timer(gpu_timing.depth_publish);
            uint32_t bytes = surface.format == 1 ? 2 : 4;
            bool snapshot = prepare_surface_snapshot(surface, bytes);
            bool initialize_snapshot = snapshot && !surface.snapshot_valid;
            const auto &region = surface.written_region;
            size_t row_bytes = (size_t)surface.width * bytes;
            uint32_t first_row = initialize_snapshot ? 0 : region.top;
            uint32_t last_row = initialize_snapshot ? surface.height : region.bottom;
            for (uint32_t row = first_row; row < last_row; row++) {
                uint8_t *cached = snapshot ? surface.source_snapshot.data() + (size_t)row * row_bytes : nullptr;
                const uint8_t *source = (const uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
                if (initialize_snapshot) {
                    decode_depth_row(surface.format, source, cached, surface.width);
                    if (surface.format == 2) gpu_timing.depth_convert_bytes += row_bytes;
                }
                for_each_written_span(surface, row, [&](uint32_t first, uint32_t end) {
                    uint8_t *destination = surface.memory + (size_t)row * surface.pitch + first * bytes;
                    uint8_t *decoded = cached ? cached + first * bytes : destination;
                    size_t span_bytes = (size_t)(end - first) * bytes;
                    if (!initialize_snapshot) {
                        decode_depth_row(surface.format, source + first * bytes, decoded, end - first);
                        if (surface.format == 2) gpu_timing.depth_convert_bytes += span_bytes;
                    }
                    if (cached) std::memcpy(destination, decoded, span_bytes);
                    gpu_timing.depth_publish_bytes += span_bytes;
                });
            }
            surface.snapshot_valid = snapshot;
        }
        {
            GpuTimer unmap_timer(gpu_timing.sync_unmap);
            context->Unmap(surface.staging.Get(), 0);
        }
        mark_target_aliases(&surface, surface.memory, (size_t)surface.pitch * surface.height);
        surface.dirty = false;
    }
    pending_draws = false;
    if (!only) s_texture_validate_serial++;
}

/* Full sync: publish every dirty surface to guest RAM (flips, invalidate). */
extern "C" void nv2a_gpu_sync(void)
{
    gpu_sync_impl(true);
}

/* Targeted publish: pull just the source surface a GPU-engine blit is about to
 * read from guest RAM back into guest RAM. A 2D blit reads guest RAM while the
 * renderer may still hold the surface in a D3D11 texture (publish normally only
 * happens on flip), so a mid-frame CopyRects -- the loading-screen back-buffer
 * capture -- read a stale/black image. No texture-serial bump: not a frame
 * boundary. */
extern "C" void nv2a_gpu_surface_publish(const void *addr, size_t bytes)
{
    gpu_sync_impl(true, addr, bytes);
}

/* Completion-only sync for ordering events that do not read surfaces back. */
extern "C" void nv2a_gpu_wait(void)
{
    gpu_sync_impl(false);
}

extern "C" void nv2a_gpu_flush(void)
{
    nv2a_gpu_sync();
    for (auto &surface : surfaces) surface.needs_refresh = true;
    for (auto &surface : depth_surfaces) surface.needs_refresh = true;
}

/* Frame boundary (FLIP_STALL): publish only the surface about to be shown,
 * advance the texture-validation serial once for the frame, and mark only CLEAN
 * surfaces for refresh.
 *
 * A flip only needs the presented surface in guest RAM, because that is the
 * only one the window and the title read back here; everything else can stay
 * GPU-resident until a fence that actually reads it. The full flush above
 * read back every dirty surface per flip. Dirty surfaces are deliberately not
 * marked needs_refresh: refreshing one would upload stale RAM over the rendered
 * image (see the guard in refresh_surface). If no surface is named, fall back
 * to the full flush. */
extern "C" void nv2a_gpu_flip(const void *presented, size_t bytes)
{
    if (!presented) { nv2a_gpu_flush(); return; }
    gpu_sync_impl(true, presented, bytes);
    /* Frame boundary: the GPU has consumed the stream; release the held Map. */
    for (auto &stream : vertex_streams) release_stream(stream);
    s_texture_validate_serial++;
    for (auto &surface : surfaces)
        if (!surface.dirty) surface.needs_refresh = true;
    for (auto &surface : depth_surfaces)
        if (!surface.dirty) surface.needs_refresh = true;
}

/* A GPU-engine blit wrote guest RAM a cached surface mirrors. Refresh only the
 * surfaces that overlap those bytes from guest RAM; never bump the global
 * texture-validation serial, which would re-upload GPU-only textures (UI). */
extern "C" void nv2a_gpu_surface_modified(const void *addr, size_t bytes)
{
    uintptr_t begin = (uintptr_t)addr, end = begin + bytes;
    for (auto &surface : surfaces) {
        uintptr_t surface_begin = (uintptr_t)surface.memory;
        uintptr_t surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin < surface_end && end > surface_begin) surface.needs_refresh = true;
    }
    for (auto &texture : textures) {
        uintptr_t texture_begin = (uintptr_t)texture.source;
        uintptr_t texture_end = texture_begin + texture.source_snapshot.size();
        if (begin < texture_end && end > texture_begin) texture.force_validate = true;
    }
}

extern "C" void nv2a_gpu_invalidate(void)
{
    nv2a_gpu_sync();
    for (auto &stream : vertex_streams) release_stream(stream);
    surfaces.clear();
    depth_surfaces.clear();
}

static bool matches_color_target(const Surface &surface, const Nv2aGpuDraw &state, TargetExtent extent)
{
    return surface.memory == state.color && surface.width == extent.width &&
        surface.height == extent.height && surface.pitch == state.pitch;
}

static bool matches_depth_target(const DepthSurface &surface, const Nv2aGpuDraw &state, TargetExtent extent)
{
    return surface.memory == state.depth && surface.width == extent.width &&
        surface.height == extent.height && surface.pitch == state.depth_pitch && surface.format == state.depth_format;
}

static TargetExtent reusable_target_extent(const Nv2aGpuDraw &state)
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_NV2A_COMPATIBLE_TARGETS");
        return !value || std::strcmp(value, "0") != 0;
    }();
    TargetExtent chosen = {state.width, state.height};
    if (!enabled) return chosen;
    uint64_t area = 0;
    bool needs_depth = state.depth_enable || state.stencil_enable;
    // Keep a covering owner even when an exact smaller alias is already cached.
    auto consider = [&](uint32_t width, uint32_t height) {
        if (width < state.width || height < state.height || (uint64_t)width * height <= area) return;
        TargetExtent candidate = {width, height};
        if (needs_depth && state.color && std::none_of(depth_surfaces.begin(), depth_surfaces.end(),
            [&](const DepthSurface &surface) { return matches_depth_target(surface, state, candidate); })) return;
        chosen = candidate;
        area = (uint64_t)width * height;
    };
    if (state.color) {
        for (const auto &surface : surfaces)
            if (surface.memory == state.color && surface.pitch == state.pitch)
                consider(surface.width, surface.height);
    } else if (needs_depth) {
        for (const auto &surface : depth_surfaces)
            if (surface.memory == state.depth && surface.pitch == state.depth_pitch && surface.format == state.depth_format)
                consider(surface.width, surface.height);
    }
    if (chosen.width != state.width || chosen.height != state.height)
        gpu_timing.compatible_target_views++;
    return chosen;
}

template<typename Target>
static bool overlaps_draw_target(const Target &surface, const Nv2aGpuDraw &state)
{
    uintptr_t begin = (uintptr_t)surface.memory, end = begin + (size_t)surface.pitch * surface.height;
    uintptr_t color_begin = (uintptr_t)state.color, color_end = color_begin + (size_t)state.pitch * state.height;
    if (state.color && begin < color_end && end > color_begin) return true;
    if (state.depth && (state.depth_enable || state.stencil_enable)) {
        uintptr_t depth_begin = (uintptr_t)state.depth, depth_end = depth_begin + (size_t)state.depth_pitch * state.height;
        if (begin < depth_end && end > depth_begin) return true;
    }
    return false;
}

static void prepare_target_cache(const Nv2aGpuDraw &state, TargetExtent extent = {})
{
    if (!extent.width) extent = {state.width, state.height};
    auto color_conflict = [&](const Surface &surface) {
        return surface.dirty && !matches_color_target(surface, state, extent) && overlaps_draw_target(surface, state);
    };
    auto depth_conflict = [&](const DepthSurface &surface) {
        return surface.dirty && !matches_depth_target(surface, state, extent) && overlaps_draw_target(surface, state);
    };
    bool new_color = state.color && std::none_of(surfaces.begin(), surfaces.end(),
        [&](const Surface &surface) { return matches_color_target(surface, state, extent); });
    bool new_depth = (state.depth_enable || state.stencil_enable) &&
        std::none_of(depth_surfaces.begin(), depth_surfaces.end(),
            [&](const DepthSurface &surface) { return matches_depth_target(surface, state, extent); });
    bool layout_conflict = std::any_of(surfaces.begin(), surfaces.end(), color_conflict) ||
        std::any_of(depth_surfaces.begin(), depth_surfaces.end(), depth_conflict);
    if (!layout_conflict &&
        !(new_color && surfaces.size() >= 64) && !(new_depth && depth_surfaces.size() >= 64)) return;
    if (layout_conflict) gpu_timing.target_layout_syncs++;
    nv2a_gpu_sync();
    size_t old_count = surfaces.size() + depth_surfaces.size();
    if (new_color && surfaces.size() >= 64) {
        auto oldest = std::min_element(surfaces.begin(), surfaces.end(),
            [](const Surface &left, const Surface &right) { return left.last_used < right.last_used; });
        surfaces.erase(oldest);
    }
    if (new_depth && depth_surfaces.size() >= 64) {
        auto oldest = std::min_element(depth_surfaces.begin(), depth_surfaces.end(),
            [](const DepthSurface &left, const DepthSurface &right) { return left.last_used < right.last_used; });
        depth_surfaces.erase(oldest);
    }
    gpu_timing.target_evictions += old_count - surfaces.size() - depth_surfaces.size();
}

static bool clear_target_rectangle(const Nv2aGpuDraw &state, Surface *surface, DepthSurface *depth,
                                   bool clear_depth, bool clear_stencil, uint32_t color, uint32_t depth_value);

extern "C" int nv2a_gpu_clear(const Nv2aGpuDraw *state, uint32_t flags, uint32_t color, uint32_t depth_value)
{
    if (!state) {
        std::fprintf(stderr, "[GPU-D3D11] native clear has no surface state\n");
        return -1;
    }
    bool clear_color = (flags & 0xF0u) != 0;
    bool clear_depth = (flags & 1u) != 0;
    bool clear_stencil = (flags & 2u) != 0 && state->depth_format == 2;
    uint32_t depth_bytes = state->depth_format == 1 ? 2u : 4u;
    auto cpu_path = []() { gpu_timing.clear_fallbacks++; return 0; };
    if ((flags & ~0xF3u) || !state->width || !state->height || state->width > 4096 || state->height > 4096 ||
        state->clip_x || state->clip_y || state->clip_width != state->width || state->clip_height != state->height)
        return cpu_path();
    if (clear_color && ((flags & 0xF0u) != 0xF0u || !state->color || state->bytes_per_pixel != 4 ||
        state->pitch < state->width * 4)) return cpu_path();
    if ((flags & 3u) && (!state->depth || (state->depth_format != 1 && state->depth_format != 2) ||
        (state->control0 & 0x1000u) || state->depth_pitch < state->width * depth_bytes)) return cpu_path();
    // Arbitrary Z24 integers need a bit-exact CPU clear, not a floating-point clear value.
    if (clear_depth && state->depth_format == 2 && (depth_value >> 8) != 0 && (depth_value >> 8) != 0xFFFFFFu)
        return cpu_path();
    if (!clear_color && !clear_depth && !clear_stencil) return 1;
    if (clear_color && (clear_depth || clear_stencil)) {
        uintptr_t color_begin = (uintptr_t)state->color, depth_begin = (uintptr_t)state->depth;
        if (color_begin < depth_begin + (size_t)state->depth_pitch * state->height &&
            depth_begin < color_begin + (size_t)state->pitch * state->height) return cpu_path();
    }
    if (!nv2a_gpu_available()) return -1;
    Nv2aGpuDraw targets = *state;
    if (!clear_color) targets.color = nullptr;
    targets.depth_enable = clear_depth || clear_stencil;
    targets.stencil_enable = clear_stencil;
    if (!targets.depth_enable) targets.depth = nullptr;
    TargetExtent extent = reusable_target_extent(targets);
    bool partial = extent.width != state->width || extent.height != state->height;
    prepare_target_cache(targets, extent);
    Surface *surface = clear_color ? get_surface(targets, partial, extent) : nullptr;
    if (clear_color && !surface) return -1;
    bool preserve_depth = state->depth_format == 2 && !(clear_depth && clear_stencil);
    DepthSurface *depth = targets.depth_enable ? get_depth(targets, preserve_depth || partial, extent) : nullptr;
    if (targets.depth_enable && !depth) return -1;
    static const bool known_clears = []() {
        const char *value = std::getenv("RECOMP_NV2A_KNOWN_CLEARS");
        return !value || std::strcmp(value, "0") != 0;
    }();
    D3D11_BOX region = {0, 0, 0, state->width, state->height, 1};
    if (partial) {
        if (!clear_target_rectangle(*state, surface, depth, clear_depth, clear_stencil, color, depth_value)) return -1;
        gpu_timing.partial_target_clears++;
    } else if (surface) {
        const float rgba[] = {(float)((color >> 16) & 255u) / 255.0f, (float)((color >> 8) & 255u) / 255.0f,
                              (float)(color & 255u) / 255.0f, (float)(color >> 24) / 255.0f};
        context->ClearRenderTargetView(surface->target.Get(), rgba);
    }
    if (depth && !partial) {
        UINT clear_flags = (clear_depth ? D3D11_CLEAR_DEPTH : 0u) | (clear_stencil ? D3D11_CLEAR_STENCIL : 0u);
        float normalized = state->depth_format == 1 ? (float)((depth_value >> 8) & 65535u) / 65535.0f :
            (float)(depth_value >> 8) / 16777215.0f;
        context->ClearDepthStencilView(depth->view.Get(), clear_flags, normalized, (UINT8)depth_value);
    }
    if (surface) {
        mark_written_region(*surface, region);
        if (known_clears && !partial) { surface->clear_value = color; surface->clear_value_valid = true; }
    }
    if (depth) {
        bool known = known_clears && !partial && (depth->format == 1 ? clear_depth :
            (clear_depth && clear_stencil) || depth->clear_value_valid);
        uint32_t value = depth->format == 1 ? (depth_value >> 8) & 65535u :
            (clear_depth ? depth_value & 0xFFFFFF00u : depth->clear_value & 0xFFFFFF00u) |
            (clear_stencil ? depth_value & 255u : depth->clear_value & 255u);
        mark_written_region(*depth, region);
        depth->clear_value = value;
        depth->clear_value_valid = known;
    }
    pending_draws = true;
    gpu_timing.native_clears++;
    return 1;
}

static void release_stream(DynamicStream &stream)
{
    if (stream.active && stream.buffer) {
        context->Unmap(stream.buffer.Get(), 0);
        stream.active = false;
    }
}

/* Keep one Map alive across draws. A Map/Unmap pair per draw was ~800 driver
 * round trips per frame (281 k appends / 10 s); only re-map (DISCARD) when the
 * ring wraps or the buffer must be recreated. Draws issued while the buffer is
 * mapped read the committed append region normally. */
static bool map_stream(DynamicStream &stream, UINT bytes, UINT bind, const char *kind, UINT &offset)
{
    if (!stream.buffer || bytes > stream.capacity) {
        release_stream(stream);
        /* The vertex+index ring resets every frame (release_stream at flip,
         * then the next map starts at 0). A frame carries ~15 MB of fixed
         * function vertices, so 512 KB wrapped it ~30-120x per frame and each
         * DISCARD became a DXVK rename/wait. Size it to hold a whole frame. */
        UINT capacity = (bind & D3D11_BIND_VERTEX_BUFFER) ? 32u * 1024 * 1024 : 64 * 1024;
        while (capacity < bytes) capacity *= 2;
        D3D11_BUFFER_DESC description = {};
        description.ByteWidth = capacity; description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = bind; description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        if (!target_operation_succeeded(kind, "create stream", device->CreateBuffer(&description, nullptr, &buffer)))
            return false;
        stream.buffer = std::move(buffer);
        stream.capacity = capacity;
        stream.used = 0;
    }
    UINT aligned = (stream.used + 15u) & ~15u;
    if (stream.active && (aligned > stream.capacity || bytes > stream.capacity - aligned)) {
        release_stream(stream);
        stream.used = 0;
        aligned = 0;
    }
    if (!stream.active) {
        if (!target_operation_succeeded(kind, "map stream",
            context->Map(stream.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &stream.mapped)))
            return false;
        stream.active = true;
        aligned = 0;
        gpu_timing.stream_discards++;
    } else {
        gpu_timing.stream_appends++;
    }
    offset = aligned;
    stream.used = aligned + bytes;
    return true;
}

static bool initialize_pipeline()
{
    if (input_layout) return true;
    if (!nv2a_gpu_compile()) return false;
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,0,96,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"FOG",0,DXGI_FORMAT_R32_FLOAT,0,(UINT)offsetof(Nv2aGpuVertex, fog_coordinate),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,(UINT)offsetof(Nv2aGpuVertex, normal),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,(UINT)offsetof(Nv2aGpuVertex, weights),D3D11_INPUT_PER_VERTEX_DATA,0}
    };
    D3D11_BUFFER_DESC buffer = {};
    buffer.ByteWidth = sizeof(Constants); buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.Usage = D3D11_USAGE_DYNAMIC; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE; raster.DepthClipEnable = FALSE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &constant_buffer)) ||
        FAILED(device->CreateRasterizerState(&raster, &rasterizer))) return false;
    uploaded_constants_valid = false;
    return SUCCEEDED(device->CreateInputLayout(elements, (UINT)(sizeof elements / sizeof elements[0]),
        vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), &input_layout));
}

/* A draw samples the surface it is rendering to (cloak refraction reads the
 * back buffer). D3D11 forbids one resource being SRV and RTV at once, so the
 * direct view samples as null. Snapshot the target's current content into a
 * scratch texture and sample that. One scratch is enough: in practice one
 * surface per draw feeds back, and the view is consumed immediately. */
static ComPtr<ID3D11Texture2D> feedback_texture;
static ComPtr<ID3D11ShaderResourceView> feedback_view;

static ID3D11ShaderResourceView *get_feedback_texture(const Surface &destination)
{
    if (!context || !destination.texture) return nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    destination.texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC have = {};
    if (feedback_texture) feedback_texture->GetDesc(&have);
    if (!feedback_texture || !feedback_view ||
            have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
        feedback_texture.Reset();
        feedback_view.Reset();
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &feedback_texture))) return nullptr;
        if (FAILED(device->CreateShaderResourceView(feedback_texture.Get(), nullptr, &feedback_view))) return nullptr;
    }
    context->CopyResource(feedback_texture.Get(), destination.texture.Get());
    return feedback_view.Get();
}

static ID3D11ShaderResourceView *get_texture(const Nv2aGpuTexture &binding, const Surface &destination)
{
    GpuTimer texture_timer(gpu_timing.texture);
    static const bool mirror_aliases = []() {
        const char *setting = std::getenv("RECOMP_NV2A_GPU_ALIAS_COPIES");
        return !setting || std::strcmp(setting, "0") != 0;
    }();
    if (!binding.source || (binding.cube ? !binding.decode_face : !binding.decode) || !binding.width || !binding.height || binding.width > 4096 || binding.height > 4096) return nullptr;
    if (binding.depth && (!binding.decode_volume || binding.cube ||
        std::max({binding.width, binding.height, binding.depth}) > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION)) return nullptr;
    uint32_t levels = binding.mip_levels ? binding.mip_levels : 1;
    uint32_t maximum_levels = 1, dimension = std::max(binding.width, binding.height);
    dimension = std::max(dimension, binding.depth);
    while (dimension > 1) { dimension >>= 1; maximum_levels++; }
    if (levels > maximum_levels || (levels > 1 && (!binding.decode_level || binding.linear))) return nullptr;
    if (binding.cube && (binding.width != binding.height || !binding.face_stride ||
        (uint64_t)binding.face_stride * 6 > binding.source_bytes)) return nullptr;
    uintptr_t begin = (uintptr_t)binding.source, end = begin + binding.source_bytes;
    uintptr_t destination_begin = (uintptr_t)destination.memory;
    uintptr_t destination_end = destination_begin + (size_t)destination.pitch * destination.height;
    if (begin < destination_end && end > destination_begin) {
        /* Same surface as the render target (back-buffer feedback). Sample a
         * snapshot of it instead of the illegal SRV+RTV bind. Only the exact
         * full-target case maps cleanly onto the scratch texture. */
        if (binding.width == destination.width && binding.height == destination.height &&
                binding.linear && (binding.format == 0x12 || binding.format == 0x1E))
            return get_feedback_texture(destination);
        std::fprintf(stderr, "[GPU-D3D11] texture target alias: source %p bytes %llu size %ux%u pitch %u format %X linear %u; target %p size %ux%u pitch %u\n",
                     (const void *)binding.source, (unsigned long long)binding.source_bytes,
                     binding.width, binding.height, binding.pitch, binding.format, binding.linear,
                     (const void *)destination.memory, destination.width, destination.height, destination.pitch);
        return nullptr;
    }
    Surface *direct = nullptr;
    /* A texture whose memory exactly matches a colour surface can be sampled
     * straight from that surface's view. 0x12/0x1E (linear A8R8G8B8/X8R8G8B8)
     * always qualify. 0x07 is the same 32bpp surface sampled as a swizzled
     * texture (the common render-target-as-texture case, e.g. 0x99753000):
     * the surface view already holds the resolved pixels, so the guest-side
     * swizzle is irrelevant and requiring binding.linear would strand it on a
     * full GPU sync every frame. */
    bool surface_viewable = (binding.format == 0x12 || binding.format == 0x1E)
                                ? binding.linear : binding.format == 0x07;
    if (levels == 1 && !binding.cube && !binding.depth && surface_viewable)
        for (auto &surface : surfaces)
            if (surface.memory == binding.source && surface.width == binding.width &&
                surface.height == binding.height && surface.pitch == binding.pitch) {
                direct = &surface;
                break;
            }
    bool dirty_alias = false, copyable_aliases = mirror_aliases && direct && !direct->dirty;
    uint64_t alias_serial = 0;
    for (auto &surface : surfaces) {
        uintptr_t surface_begin = (uintptr_t)surface.memory, surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin >= surface_end || end <= surface_begin) continue;
        if (&surface != direct && surface.dirty) {
            dirty_alias = true;
            if (!direct || surface.memory != direct->memory || surface.pitch != direct->pitch)
                copyable_aliases = false;
            alias_serial = std::max(alias_serial, surface.content_serial);
        }
    }
    for (auto &surface : depth_surfaces) {
        uintptr_t surface_begin = (uintptr_t)surface.memory, surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin < surface_end && end > surface_begin) {
            if (surface.dirty) { dirty_alias = true; copyable_aliases = false; }
        }
    }
    if (dirty_alias && copyable_aliases) {
        if (direct->alias_copy_serial != alias_serial || direct->needs_refresh) {
            direct->needs_refresh = true;
            refresh_surface(*direct);
            if (direct->alias_copy_serial != alias_serial) {
                for (const auto &surface : surfaces) {
                    if (&surface == direct || !surface.dirty || surface.memory != direct->memory ||
                        surface.pitch != direct->pitch) continue;
                    D3D11_BOX region = surface.written_region;
                    region.right = std::min(region.right, direct->width);
                    region.bottom = std::min(region.bottom, direct->height);
                    if (region.left >= region.right || region.top >= region.bottom) continue;
                    auto copy = [&](const D3D11_BOX &box) {
                        context->CopySubresourceRegion(direct->texture.Get(), 0, box.left, box.top, 0,
                                                      surface.texture.Get(), 0, &box);
                        direct->clear_value_valid = false;
                        gpu_timing.alias_copies++;
                        gpu_timing.alias_copy_bytes += (uint64_t)(box.right - box.left) * (box.bottom - box.top) * 4;
                    };
                    if (surface.written_mask.empty()) copy(region);
                    else for (uint32_t row = region.top; row < region.bottom; row++)
                        for_each_written_span(surface, row, [&](uint32_t first, uint32_t end) {
                            end = std::min(end, direct->width);
                            if (first < end) {
                                D3D11_BOX span = {first, row, 0, end, row + 1, 1};
                                copy(span);
                            }
                        });
                }
                // Sampling mirrors do not create guest writes; the dirty owner still publishes at real fences.
                direct->alias_copy_serial = alias_serial;
            } else gpu_timing.alias_copy_reuses++;
        } else gpu_timing.alias_copy_reuses++;
    } else if (dirty_alias) {
        gpu_timing.texture_alias_syncs++;
        /* Pull just this texture's source bytes back to guest RAM. A full sync
         * here republished every dirty surface once per frame, which alone made
         * the lazy flip worthless: the surface it was about to publish had
         * already been published by this call. */
        nv2a_gpu_surface_publish(binding.source, binding.source_bytes);
    }
    if (direct) {
        direct->last_used = ++surface_use_serial;
        /* A surface marked dirty holds GPU content newer than guest RAM (it has
         * been drawn into but not published). Re-uploading the snapshot would
         * overwrite that with the previous publish, so sample the GPU texture
         * as-is and only refresh from guest RAM when the surface is clean.
         * This is also what lets a sync skip publication without feeding the
         * GPU stale data -- see RECOMP_SYNC_LIGHT. */
        if (!direct->dirty)
            refresh_surface(*direct);
        return direct->view.Get();
    }
    CachedTexture *refresh = nullptr;
    {
        GpuTimer hash_timer(gpu_timing.hash);
        gpu_timing.hash_bytes += binding.source_bytes;
        auto key_matches = [&](const CachedTexture &texture) {
            return texture.source == binding.source && texture.width == binding.width &&
                   texture.height == binding.height && texture.format == binding.format &&
                   texture.cube == binding.cube && texture.depth == binding.depth &&
                   texture.pitch == binding.pitch && texture.linear == binding.linear &&
                   texture.face_stride == binding.face_stride && texture.mip_levels == levels &&
                   texture.source_snapshot.size() == binding.source_bytes;
        };
        auto check = [&](CachedTexture &texture) -> ID3D11ShaderResourceView * {
            /* Already compared this entry this frame: the verdict still holds -- unless a
             * GPU blit overwrote it since, which forces a fresh compare. */
            if (tex_validate_once() && !texture.force_validate &&
                    texture.validated_serial == s_texture_validate_serial)
                return texture.view.Get();
            if (std::memcmp(texture.source_snapshot.data(), binding.source, binding.source_bytes) == 0) {
                texture.validated_serial = s_texture_validate_serial;
                texture.force_validate = false;
                return texture.view.Get();
            }
            refresh = &texture;
            return nullptr;
        };
        if (texture_index_count != textures.size()) rebuild_texture_index();
        auto index = texture_index_by_source.find(binding.source);
        if (index != texture_index_by_source.end() && key_matches(textures[index->second])) {
            if (auto *view = check(textures[index->second])) return view;
        } else for (auto &texture : textures) {
            if (!key_matches(texture)) continue;
            if (auto *view = check(texture)) return view;
            break;
        }
    }
    GpuTimer upload_timer(gpu_timing.upload);
    uint32_t faces = binding.cube ? 6 : 1;
    DXGI_FORMAT format = binding.format == 0xC ? DXGI_FORMAT_BC1_UNORM : binding.format == 0xE ? DXGI_FORMAT_BC2_UNORM :
                         binding.format == 0xF ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    uint32_t block_bytes = binding.format == 0xC ? 8 : 16;
    bool compressed = format != DXGI_FORMAT_B8G8R8A8_UNORM;
    if (compressed && (binding.width % 4 || binding.height % 4)) {
        compressed = false; format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (binding.depth) {
        // D3D11 block-compressed resources are 2D; volumes need decoded texels.
        compressed = false;
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    std::vector<std::vector<uint32_t>> pixels(faces * levels);
    std::vector<D3D11_SUBRESOURCE_DATA> data(faces * levels);
    uint64_t resource_bytes = 0;
    for (uint32_t face = 0; face < faces; face++) {
        uint64_t source_offset = (uint64_t)face * binding.face_stride;
        uint32_t width = binding.width, height = binding.height, depth = binding.depth ? binding.depth : 1;
        for (uint32_t level = 0; level < levels; level++) {
            uint32_t subresource = face * levels + level;
            if (compressed) {
                uint32_t row_bytes = ((width + 3) / 4) * block_bytes;
                uint64_t level_bytes = (uint64_t)row_bytes * ((height + 3) / 4);
                if (source_offset + level_bytes > binding.source_bytes ||
                    (binding.cube && source_offset + level_bytes > (uint64_t)(face + 1) * binding.face_stride)) return nullptr;
                data[subresource].pSysMem = binding.source + source_offset;
                data[subresource].SysMemPitch = row_bytes;
                resource_bytes += level_bytes;
                source_offset += level_bytes;
            } else {
                auto &image = pixels[subresource];
                image.resize((size_t)width * height * depth);
                /* Linear 32bpp textures (0x12 A8R8G8B8, 0x1E X8R8G8B8) dominate
                 * the per-frame re-uploads. The generic path calls the decode
                 * callback once per texel; for these the guest bytes are already
                 * in the target layout, so copy whole rows instead (0x1E only
                 * needs alpha forced). Swizzled/compressed/format-shuffled ones
                 * keep the callback. */
                bool bulk = binding.linear && !binding.cube && !binding.depth && levels == 1 &&
                            (binding.format == 0x12 || binding.format == 0x1E);
                /* Swizzled 32bpp (0x06 A8R8G8B8, 0x07 X8R8G8B8) is the largest
                 * per-frame re-upload. The generic path calls the decode callback
                 * per texel; bulk-unswizzle by Morton order instead. */
                bool swizzled32 = !binding.linear && !binding.cube && !binding.depth && levels == 1 &&
                            (binding.format == 0x06 || binding.format == 0x07) &&
                            width && height && !(width & (width - 1)) && !(height & (height - 1)) &&
                            (uint64_t)width * height * 4 <= binding.source_bytes;
                if (bulk) {
                    const uint8_t *base = (const uint8_t *)binding.source;
                    for (uint32_t row = 0; row < height; row++) {
                        const uint32_t *source = (const uint32_t *)(base + (size_t)row * binding.pitch);
                        uint32_t *destination = &image[(size_t)row * width];
                        if (binding.format == 0x12) std::memcpy(destination, source, (size_t)width * 4);
                        else for (uint32_t column = 0; column < width; column++)
                            destination[column] = source[column] | 0xFF000000u;
                    }
                } else if (swizzled32) {
                    xbox_unswizzle_rect(image.data(), binding.source, width, height, 4);
                    if (binding.format == 0x07)
                        for (size_t pixel = 0; pixel < image.size(); pixel++) image[pixel] |= 0xFF000000u;
                } else {
                for (uint32_t slice = 0; slice < depth; slice++)
                 for (uint32_t row = 0; row < height; row++)
                    for (uint32_t column = 0; column < width; column++) {
                        uint32_t *pixel = &image[((size_t)slice * height + row) * width + column];
                        bool decoded = binding.depth ? binding.decode_volume(binding.decode_context, level, column, row, slice, pixel) :
                            binding.decode_level ? binding.decode_level(binding.decode_context, face, level, column, row, pixel) :
                            binding.cube ? binding.decode_face(binding.decode_context, face, column, row, pixel) :
                                           binding.decode(binding.decode_context, column, row, pixel);
                        if (!decoded) {
                            static unsigned logged;
                            if (logged++ < 16)
                                std::fprintf(stderr, "[GPU-D3D11] texture decode failed: fmt 0x%X %ux%u depth %u face %u level %u linear %u\n",
                                             binding.format, binding.width, binding.height, binding.depth, face, level, binding.linear);
                            gpu_timing.texture_decode_failures++;
                            return nullptr;
                        }
                        if (binding.depth) *pixel = (*pixel & 0xFF00FF00u) | ((*pixel & 255u) << 16) | ((*pixel >> 16) & 255u);
                    }
                }
                data[subresource].pSysMem = image.data(); data[subresource].SysMemPitch = width * 4;
                data[subresource].SysMemSlicePitch = width * height * 4;
                resource_bytes += (uint64_t)width * height * depth * 4;
            }
            if (width > 1) width >>= 1;
            if (height > 1) height >>= 1;
            if (depth > 1) depth >>= 1;
        }
    }
    if (refresh) {
        ComPtr<ID3D11Resource> resource;
        refresh->view->GetResource(&resource);
        for (uint32_t subresource = 0; subresource < faces * levels; subresource++)
            context->UpdateSubresource(resource.Get(), subresource, nullptr,
                                      data[subresource].pSysMem, data[subresource].SysMemPitch, data[subresource].SysMemSlicePitch);
        refresh->source_snapshot.assign(binding.source, binding.source + binding.source_bytes);
        refresh->validated_serial = s_texture_validate_serial;
        gpu_timing.texture_updated++;
        gpu_timing.texture_update_bytes += resource_bytes;
        return refresh->view.Get();
    }
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = binding.width; description.Height = binding.height;
    description.MipLevels = levels; description.ArraySize = faces; description.Format = format;
    description.MiscFlags = binding.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    description.SampleDesc.Count = 1; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    CachedTexture entry = {};
    entry.source = binding.source; entry.width = binding.width; entry.height = binding.height; entry.format = binding.format;
    entry.pitch = binding.pitch; entry.linear = binding.linear; entry.face_stride = binding.face_stride;
    entry.cube = binding.cube;
    entry.depth = binding.depth;
    entry.mip_levels = levels; entry.bytes = resource_bytes;
    entry.validated_serial = s_texture_validate_serial;
    if (binding.depth) {
        D3D11_TEXTURE3D_DESC volume_description = {};
        volume_description.Width = binding.width; volume_description.Height = binding.height;
        volume_description.Depth = binding.depth; volume_description.MipLevels = levels;
        volume_description.Format = format; volume_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture3D> volume;
        if (FAILED(device->CreateTexture3D(&volume_description, data.data(), &volume)) ||
            FAILED(device->CreateShaderResourceView(volume.Get(), nullptr, &entry.view))) {
            gpu_timing.texture_create_failures++;
            return nullptr;
        }
    } else if (FAILED(device->CreateTexture2D(&description, data.data(), &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &entry.view))) {
        gpu_timing.texture_create_failures++;
        return nullptr;
    }
    gpu_timing.texture_created++;
    entry.source_snapshot.assign(binding.source, binding.source + binding.source_bytes);
    entry.bytes += entry.source_snapshot.size();
    uint64_t cache_bytes = entry.bytes;
    for (const auto &cached : textures) cache_bytes += cached.bytes;
    while (!textures.empty() && (textures.size() >= 1024 || cache_bytes > 256ull * 1024 * 1024)) {
        cache_bytes -= textures.front().bytes;
        textures.erase(textures.begin());
        gpu_timing.texture_evicted++;
    }
    textures.push_back(std::move(entry));
    gpu_timing.texture_cache_entries = textures.size();
    gpu_timing.texture_cache_bytes = cache_bytes;
    return textures.back().view.Get();
}

static D3D11_BLEND blend_factor(uint32_t value, bool alpha)
{
    switch (value) {
    case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA; case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA; case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D11_BLEND_BLEND_FACTOR; case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return (D3D11_BLEND)0;
    }
}

static bool make_blend(const Nv2aGpuDraw &state, ComPtr<ID3D11BlendState> &result)
{
    D3D11_BLEND_DESC description = {};
    auto &target = description.RenderTarget[0];
    target.BlendEnable = state.blend_enable != 0;
    target.RenderTargetWriteMask = ((state.color_mask & 0x10000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
        ((state.color_mask & 0x100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((state.color_mask & 1) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
        ((state.color_mask & 0x1000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    target.SrcBlend = blend_factor(state.blend_source, false); target.DestBlend = blend_factor(state.blend_destination, false);
    target.SrcBlendAlpha = blend_factor(state.blend_source, true); target.DestBlendAlpha = blend_factor(state.blend_destination, true);
    switch (state.blend_equation) {
    case 0x8006: target.BlendOp = D3D11_BLEND_OP_ADD; break;
    case 0x800A: target.BlendOp = D3D11_BLEND_OP_SUBTRACT; break;
    case 0x800B: target.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
    case 0x8007: target.BlendOp = D3D11_BLEND_OP_MIN; break;
    case 0x8008: target.BlendOp = D3D11_BLEND_OP_MAX; break;
    default: if (state.blend_enable) return false; target.BlendOp = D3D11_BLEND_OP_ADD;
    }
    if (state.blend_enable && (!target.SrcBlend || !target.DestBlend)) return false;
    if (!state.blend_enable) target.SrcBlend = target.DestBlend = target.SrcBlendAlpha = target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOpAlpha = target.BlendOp;
    const std::array<uint32_t,7> key = {(uint32_t)target.BlendEnable, (uint32_t)target.SrcBlend,
        (uint32_t)target.DestBlend, (uint32_t)target.SrcBlendAlpha, (uint32_t)target.DestBlendAlpha,
        (uint32_t)target.BlendOp, target.RenderTargetWriteMask};
    for (const auto &variant : blend_variants) {
        if (variant.key == key) { result = variant.state; return true; }
    }
    if (FAILED(device->CreateBlendState(&description, &result))) return false;
    gpu_timing.blend_created++;
    if (blend_variants.size() >= 128) blend_variants.erase(blend_variants.begin());
    blend_variants.push_back({key, result});
    return true;
}

static D3D11_STENCIL_OP stencil_operation(uint32_t operation)
{
    switch (operation) {
    case 0x1E00: return D3D11_STENCIL_OP_KEEP;
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return (D3D11_STENCIL_OP)0;
    }
}

static D3D11_TEXTURE_ADDRESS_MODE texture_address(uint32_t address)
{
    switch (address) {
    case 0: case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: case 5: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return (D3D11_TEXTURE_ADDRESS_MODE)0;
    }
}

static bool make_sampler(const Nv2aGpuTexture &binding, ComPtr<ID3D11SamplerState> &result)
{
    uint32_t minimum = (binding.filter >> 16) & 255u, maximum = (binding.filter >> 24) & 15u;
    if (!minimum) minimum = 1;
    if (!maximum) maximum = 1;
    if (minimum > 6 || maximum > 2) return false;
    D3D11_SAMPLER_DESC description = {};
    description.AddressU = texture_address(binding.address_u);
    description.AddressV = texture_address(binding.address_v);
    description.AddressW = texture_address(binding.address_w);
    if (!description.AddressU || !description.AddressV || !description.AddressW) return false;
    description.Filter = (D3D11_FILTER)D3D11_ENCODE_BASIC_FILTER(
        minimum == 2 || minimum == 4 || minimum == 6 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
        maximum == 2 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
        minimum >= 5 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT, D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    int32_t bias = (int32_t)(binding.filter & 0x1FFFu);
    if (bias & 0x1000) bias -= 0x2000;
    description.MipLODBias = (float)bias / 256.0f;
    if (description.MipLODBias > D3D11_MIP_LOD_BIAS_MAX) return false;
    description.MaxAnisotropy = binding.control0_valid ? 1u << ((binding.control0 >> 4) & 3u) : 1u;
    if (description.MaxAnisotropy > 1) {
        if (maximum != 2 || (minimum != 2 && minimum != 6)) return false;
        description.Filter = D3D11_FILTER_ANISOTROPIC;
    }
    description.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    description.MaxLOD = D3D11_FLOAT32_MAX;
    if (minimum <= 2) {
        description.MinLOD = description.MaxLOD = 0;
    } else if (binding.control0_valid) {
        description.MinLOD = (float)((binding.control0 >> 18) & 0xFFFu) / 256.0f;
        description.MaxLOD = (float)((binding.control0 >> 6) & 0xFFFu) / 256.0f;
        if (description.MinLOD > description.MaxLOD) return false;
    }
    nv_cpu_unpack_argb(binding.border_color, description.BorderColor);
    std::array<uint32_t,9> key = {(uint32_t)description.Filter, (uint32_t)description.AddressU,
        (uint32_t)description.AddressV, (uint32_t)description.AddressW, binding.filter & 0x1FFFu,
        minimum > 2 && binding.control0_valid ? (binding.control0 >> 18) & 0xFFFu : 0,
        minimum <= 2 ? 0 : binding.control0_valid ? (binding.control0 >> 6) & 0xFFFu : UINT32_MAX,
        binding.border_color, description.MaxAnisotropy};
    for (const auto &entry : sampler_cache)
        if (entry.key == key) { result = entry.state; return true; }
    if (FAILED(device->CreateSamplerState(&description, &result))) return false;
    if (sampler_cache.size() >= 256) sampler_cache.clear();
    sampler_cache.push_back({key, result});
    gpu_timing.sampler_created++;
    return true;
}

static bool make_rasterizer(const Nv2aGpuDraw &state, ComPtr<ID3D11RasterizerState> &result)
{
    uint32_t front = state.front_face ? state.front_face : 0x900;
    uint32_t front_fill = state.polygon_front ? state.polygon_front : 0x1B02;
    uint32_t back_fill = state.polygon_back ? state.polygon_back : 0x1B02;
    if (front != 0x900 && front != 0x901) return false;
    D3D11_RASTERIZER_DESC description = {};
    description.CullMode = D3D11_CULL_NONE;
    if (state.cull_enable && state.topology == NV2A_GPU_TOPOLOGY_TRIANGLES) {
        switch (state.cull_face) {
        case 0x404: description.CullMode = D3D11_CULL_FRONT; break;
        case 0x405: description.CullMode = D3D11_CULL_BACK; break;
        case 0x408: break;
        default: return false;
        }
    }
    if (state.topology != NV2A_GPU_TOPOLOGY_TRIANGLES) front_fill = back_fill = 0x1B02;
    uint32_t fill = description.CullMode == D3D11_CULL_FRONT ? back_fill : front_fill;
    if (description.CullMode == D3D11_CULL_NONE && front_fill != back_fill && !(state.cull_enable && state.cull_face == 0x408)) return false;
    if (fill != 0x1B01 && fill != 0x1B02) return false;
    description.FillMode = fill == 0x1B01 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    description.FrontCounterClockwise = front == 0x901;
    description.ScissorEnable = TRUE; description.DepthClipEnable = FALSE;
    uint32_t key = ((uint32_t)description.CullMode - 1) * 4 + (description.FrontCounterClockwise ? 2u : 0u) + (fill == 0x1B01 ? 1u : 0u);
    auto &entry = raster_states[key];
    if (!entry && FAILED(device->CreateRasterizerState(&description, &entry))) return false;
    result = entry;
    return true;
}

static bool rectangle_contains(const D3D11_RECT &outer, const D3D11_RECT &inner)
{
    return outer.left <= inner.left && outer.top <= inner.top &&
           outer.right >= inner.right && outer.bottom >= inner.bottom;
}

static void configure_window_clip(const Nv2aGpuDraw &state, Constants &constants, D3D11_RECT &clip)
{
    if (!state.window_clip_valid || clip.left >= clip.right || clip.top >= clip.bottom) return;
    std::array<D3D11_RECT,8> rectangles = {};
    uint32_t count = 0;
    for (uint32_t rectangle = 0; rectangle < 8; rectangle++) {
        uint32_t horizontal = state.window_clip_horizontal[rectangle];
        uint32_t vertical = state.window_clip_vertical[rectangle];
        /* Guest maxima are inclusive; native scissors and shader boxes are half-open. */
        D3D11_RECT region = {(LONG)(horizontal & 4095u), (LONG)(vertical & 4095u),
                            (LONG)(((horizontal >> 16) & 4095u) + 1),
                            (LONG)(((vertical >> 16) & 4095u) + 1)};
        region.left = std::max(region.left, clip.left); region.top = std::max(region.top, clip.top);
        region.right = std::min(region.right, clip.right); region.bottom = std::min(region.bottom, clip.bottom);
        if (region.left >= region.right || region.top >= region.bottom) continue;
        bool contained = false;
        for (uint32_t previous = 0; previous < count; previous++)
            contained |= rectangle_contains(rectangles[previous], region);
        if (contained) continue;
        for (uint32_t previous = 0; previous < count;) {
            if (rectangle_contains(region, rectangles[previous])) rectangles[previous] = rectangles[--count];
            else previous++;
        }
        rectangles[count++] = region;
    }
    if (!count) {
        if (!state.window_clip_type) { clip.right = clip.left; clip.bottom = clip.top; }
        return;
    }
    if (!state.window_clip_type && count == 1) { clip = rectangles[0]; return; }
    if (state.window_clip_type && count == 1 && rectangle_contains(rectangles[0], clip)) {
        clip.right = clip.left; clip.bottom = clip.top;
        return;
    }
    constants.window_clip_control[0] = state.window_clip_type ? 2u : 1u;
    constants.window_clip_control[1] = count;
    for (uint32_t rectangle = 0; rectangle < count; rectangle++) {
        const auto &region = rectangles[rectangle];
        constants.window_clip_rects[rectangle][0] = (uint32_t)region.left;
        constants.window_clip_rects[rectangle][1] = (uint32_t)region.top;
        constants.window_clip_rects[rectangle][2] = (uint32_t)region.right;
        constants.window_clip_rects[rectangle][3] = (uint32_t)region.bottom;
    }
    if (!state.window_clip_type) {
        clip = rectangles[0];
        for (uint32_t rectangle = 1; rectangle < count; rectangle++) {
            clip.left = std::min(clip.left, rectangles[rectangle].left);
            clip.top = std::min(clip.top, rectangles[rectangle].top);
            clip.right = std::max(clip.right, rectangles[rectangle].right);
            clip.bottom = std::max(clip.bottom, rectangles[rectangle].bottom);
        }
    }
}

static bool clear_target_rectangle(const Nv2aGpuDraw &state, Surface *surface, DepthSurface *depth,
                                   bool clear_depth, bool clear_stencil, uint32_t color, uint32_t depth_value)
{
    struct ClearPipeline {
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11BlendState> blend[2];
        ComPtr<ID3D11DepthStencilState> depth[4];
    };
    static ClearPipeline pipeline;
    if (!pipeline.vertex) {
        static const char source[] =
            "cbuffer ClearConstants:register(b0){float4 clearColor;float4 clearDepth;};"
            "float4 vs(uint id:SV_VertexID):SV_Position{"
            "float2 p=id==0?float2(-1,-1):id==1?float2(-1,3):float2(3,-1);return float4(p,clearDepth.x,1);}"
            "float4 ps():SV_Target{return clearColor;}";
        ClearPipeline created;
        ComPtr<ID3DBlob> vertex, pixel, errors;
        HRESULT result = D3DCompile(source, sizeof source - 1, "NV2A clear", nullptr, nullptr,
                                   "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors);
        if (SUCCEEDED(result))
            result = D3DCompile(source, sizeof source - 1, "NV2A clear", nullptr, nullptr,
                                "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] rectangle clear shader compile: 0x%08X %s\n",
                         (unsigned)result, errors ? (const char *)errors->GetBufferPointer() : "");
            return false;
        }
        if (!target_operation_succeeded("rectangle clear", "create vertex shader",
                device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &created.vertex)) ||
            !target_operation_succeeded("rectangle clear", "create pixel shader",
                device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &created.pixel))) return false;
        D3D11_BUFFER_DESC buffer = {};
        buffer.ByteWidth = 32; buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!target_operation_succeeded("rectangle clear", "create constants",
                device->CreateBuffer(&buffer, nullptr, &created.constants))) return false;
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE; raster.ScissorEnable = TRUE;
        if (!target_operation_succeeded("rectangle clear", "create rasterizer",
                device->CreateRasterizerState(&raster, &created.raster))) return false;
        for (uint32_t index = 0; index < 2; index++) {
            D3D11_BLEND_DESC blend = {};
            blend.RenderTarget[0].RenderTargetWriteMask = index ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
            blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            if (!target_operation_succeeded("rectangle clear", "create blend state",
                    device->CreateBlendState(&blend, &created.blend[index]))) return false;
        }
        for (uint32_t index = 0; index < 4; index++) {
            D3D11_DEPTH_STENCIL_DESC description = {};
            description.DepthEnable = (index & 1u) != 0;
            description.DepthWriteMask = index & 1u ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            description.DepthFunc = D3D11_COMPARISON_ALWAYS;
            description.StencilEnable = (index & 2u) != 0;
            description.StencilReadMask = description.StencilWriteMask = 255;
            description.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            description.FrontFace.StencilFailOp = description.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            description.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            description.BackFace = description.FrontFace;
            if (!target_operation_succeeded("rectangle clear", "create depth/stencil state",
                    device->CreateDepthStencilState(&description, &created.depth[index]))) return false;
        }
        pipeline = std::move(created);
    }
    const float constants[8] = {
        (float)((color >> 16) & 255u) / 255.0f, (float)((color >> 8) & 255u) / 255.0f,
        (float)(color & 255u) / 255.0f, (float)(color >> 24) / 255.0f,
        state.depth_format == 1 ? (float)((depth_value >> 8) & 65535u) / 65535.0f :
            (float)(depth_value >> 8) / 16777215.0f, 0, 0, 0
    };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!target_operation_succeeded("rectangle clear", "map constants",
            context->Map(pipeline.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, constants, sizeof constants);
    context->Unmap(pipeline.constants.Get(), 0);
    ID3D11ShaderResourceView *empty[12] = {};
    context->PSSetShaderResources(0, 12, empty);
    ID3D11RenderTargetView *target = surface ? surface->target.Get() : nullptr;
    context->OMSetRenderTargets(surface ? 1 : 0, surface ? &target : nullptr, depth ? depth->view.Get() : nullptr);
    context->OMSetBlendState(pipeline.blend[surface ? 1 : 0].Get(), nullptr, UINT_MAX);
    context->OMSetDepthStencilState(pipeline.depth[(clear_depth ? 1 : 0) | (clear_stencil ? 2 : 0)].Get(), depth_value & 255u);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(pipeline.vertex.Get(), nullptr, 0);
    context->PSSetShader(pipeline.pixel.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer *buffer = pipeline.constants.Get();
    context->VSSetConstantBuffers(0, 1, &buffer);
    context->PSSetConstantBuffers(0, 1, &buffer);
    D3D11_VIEWPORT viewport = {0, 0, (float)state.width, (float)state.height, 0, 1};
    D3D11_RECT clip = {0, 0, (LONG)state.width, (LONG)state.height};
    context->RSSetState(pipeline.raster.Get());
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &clip);
    context->Draw(3, 0);
    return true;
}

extern "C" int nv2a_gpu_draw(const Nv2aGpuDraw *state, Nv2aGpuVertex *vertices, uint32_t count)
{
    GpuTimer draw_timer(gpu_timing.draw);
    GpuTimer phase_timer(gpu_timing.draw_setup);
    auto reject = [](const char *reason) {
        std::fprintf(stderr, "[GPU-D3D11] rejected: %s\n", reason);
        return 0;
    };
    if (!state || !vertices || !count || count > NV2A_GPU_MAX_VERTICES || !state->color || !state->width || !state->height ||
        state->width > 4096 || state->height > 4096 || state->pitch < state->width * 4 || state->bytes_per_pixel != 4 ||
        (state->combiners.control & 255u) > 8 || !initialize_pipeline()) return 0;
    if (state->topology > NV2A_GPU_TOPOLOGY_POINTS) return reject("primitive topology");
    uint32_t primitive_width = state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES ? 3u :
                               state->topology == NV2A_GPU_TOPOLOGY_LINES ? 2u : 1u;
    uint32_t element_count = state->indices ? state->index_count : count;
    if (element_count < primitive_width || element_count % primitive_width ||
        (state->indices && element_count > NV2A_GPU_MAX_INDICES)) return reject("primitive element count");
    if (state->topology == NV2A_GPU_TOPOLOGY_POINTS && state->point_size && state->point_size != 8)
        return reject("non-unit point size");
    if (state->indices) {
        for (uint32_t index = 0; index < state->index_count; index++)
            if (state->indices[index] >= count) return reject("vertex index extent");
    }
    if (state->window_clip_valid && state->window_clip_type > 1) return reject("window-clip type");
    if (state->clip_width > UINT32_MAX - state->clip_x || state->clip_height > UINT32_MAX - state->clip_y)
        return reject("surface scissor extent");
    if (state->fixed_transform) {
        if (state->vertex_program) return reject("conflicting vertex pipelines");
        if (state->skin_mode > 6) return reject("fixed-function skin mode");
        if (state->texgen_view_model > 1) return reject("texgen view model");
        for (uint32_t stage = 0; stage < 4; stage++) {
            for (uint32_t component = 0; component < 4; component++) {
                uint32_t mode = state->texgen[stage][component];
                if (mode == 0 || mode == 0x2400 || mode == 0x2401) continue;
                if ((mode == 0x2402 || mode == 0x8512) && state->texgen_view_model != 0)
                    return reject("infinite-viewer reflection texgen");
                if (mode == 0x2402 && component < 2) continue;
                if ((mode == 0x8511 || mode == 0x8512) && component < 3) continue;
                return reject("texgen mode or component");
            }
        }
        if (state->lighting_enable) {
            for (uint32_t source = 0; source < 4; source++)
                if (((state->color_material >> (source * 2)) & 3u) == 3u)
                    return reject("reserved color-material source");
            if (state->specular_enable && (!nv_cpu_finite(state->specular_power) || state->specular_power < 0))
                return reject("nonfinite or negative specular exponent");
        }
    }
    if (state->fog_enable) {
        if (state->fog_mode != 0x2601 && (state->fog_mode < 0x800 || state->fog_mode > 0x804)) return reject("fog mode");
        if (!state->vertex_program && state->fog_gen_mode > 3 && state->fog_gen_mode != 6) return reject("fog coordinate generation");
        if (!state->vertex_program && state->fog_gen_mode >= 1 && state->fog_gen_mode <= 3 && !state->fixed_transform)
            return reject("eye-space fog requires fixed transform");
    }
    if (state->depth_enable && ((state->control0 & 0x1000u) || state->depth_function < 0x200 || state->depth_function > 0x207)) return 0;
    uint32_t depth_clamp = (state->zmin_max_control >> 4) & 15u;
    float depth_clip_min = state->depth_range_valid & 1u ? state->depth_clip_min : 0.0f;
    float depth_clip_max = state->depth_range_valid & 2u ? state->depth_clip_max : state->depth_format == 1 ? 65535.0f : 16777215.0f;
    if (depth_clamp > 1 || !nv_cpu_finite(depth_clip_min) || !nv_cpu_finite(depth_clip_max) ||
        (depth_clamp && depth_clip_min > depth_clip_max)) return reject("depth clip range or clamp mode");
    auto offset_enabled = [&](uint32_t mode) {
        if (!mode) mode = 0x1B02;
        return mode >= 0x1B00 && mode <= 0x1B02 && ((state->polygon_offset_enable >> (mode - 0x1B00)) & 1u) != 0;
    };
    bool front_offset = state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES &&
                        offset_enabled(state->polygon_front) && (!state->cull_enable || state->cull_face == 0x405);
    bool back_offset = state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES &&
                       offset_enabled(state->polygon_back) && (!state->cull_enable || state->cull_face == 0x404);
    if (front_offset || back_offset) {
        if (!nv_cpu_finite(state->polygon_offset_scale) || !nv_cpu_finite(state->polygon_offset_bias))
            return reject("nonfinite polygon offset");
    }
    if (state->stencil_enable && (state->depth_format != 2 || state->stencil_function < 0x200 || state->stencil_function > 0x207 ||
        !stencil_operation(state->stencil_fail) || !stencil_operation(state->stencil_depth_fail) || !stencil_operation(state->stencil_pass)))
        return reject("stencil format, comparison or operation");
    for (uint32_t stage = 0; stage < (state->combiners.control & 255u); stage++) {
        const uint32_t outputs[2] = {state->combiners.rgb_output[stage], state->combiners.alpha_output[stage]};
        for (uint32_t path = 0; path < 2; path++) {
            uint32_t mapping = (outputs[path] >> 15) & 7;
            if (mapping == 5 || mapping == 7) return reject("combiner output mapping");
            for (uint32_t product = 0; product < 3; product++) {
                uint32_t destination = (outputs[path] >> (product * 4)) & 15;
                if (destination && destination != 4 && destination != 5 && (destination < 8 || destination > 13)) return reject("combiner destination register");
            }
        }
    }
    ComPtr<ID3D11BlendState> blending;
    if (!make_blend(*state, blending)) return reject("blend state");
    ComPtr<ID3D11RasterizerState> raster_state;
    if (!make_rasterizer(*state, raster_state)) return reject("culling, front face or polygon fill");
    for (uint32_t vertex = 0; !state->vertex_program && !state->fixed_transform && vertex < count; vertex++)
        if (!nv_cpu_finite(vertices[vertex].position[0]) || !nv_cpu_finite(vertices[vertex].position[1]) ||
            !nv_cpu_finite(vertices[vertex].position[2]) || !nv_cpu_finite(vertices[vertex].position[3])) return 0;
    TargetExtent extent = reusable_target_extent(*state);
    prepare_target_cache(*state, extent);
    Surface *surface = get_surface(*state, true, extent);
    if (!surface) return 0;
    DepthSurface *depth = state->depth_enable || state->stencil_enable ? get_depth(*state, true, extent) : nullptr;
    if ((state->depth_enable || state->stencil_enable) && !depth) return reject("depth/stencil surface");
    D3D11_DEPTH_STENCIL_DESC depth_description = {};
    depth_description.DepthEnable = state->depth_enable != 0;
    depth_description.DepthWriteMask = state->depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_description.DepthFunc = state->depth_enable ? (D3D11_COMPARISON_FUNC)(state->depth_function - 0x1FF) : D3D11_COMPARISON_ALWAYS;
    uint32_t depth_key = (depth_description.DepthEnable ? 8u : 0u) +
                        (depth_description.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? 16u : 0u) +
                        (uint32_t)depth_description.DepthFunc - 1u;
    depth_description.StencilEnable = state->stencil_enable != 0;
    depth_description.StencilReadMask = (UINT8)state->stencil_read_mask;
    depth_description.StencilWriteMask = (UINT8)state->stencil_write_mask;
    depth_description.FrontFace.StencilFunc = state->stencil_enable ? (D3D11_COMPARISON_FUNC)(state->stencil_function - 0x1FF) : D3D11_COMPARISON_ALWAYS;
    depth_description.FrontFace.StencilFailOp = state->stencil_enable ? stencil_operation(state->stencil_fail) : D3D11_STENCIL_OP_KEEP;
    depth_description.FrontFace.StencilDepthFailOp = state->stencil_enable ? stencil_operation(state->stencil_depth_fail) : D3D11_STENCIL_OP_KEEP;
    depth_description.FrontFace.StencilPassOp = state->stencil_enable ? stencil_operation(state->stencil_pass) : D3D11_STENCIL_OP_KEEP;
    depth_description.BackFace = depth_description.FrontFace;
    std::array<uint32_t,7> stencil_key = {depth_key, (uint32_t)depth_description.FrontFace.StencilFunc,
        depth_description.StencilReadMask, depth_description.StencilWriteMask,
        (uint32_t)depth_description.FrontFace.StencilFailOp, (uint32_t)depth_description.FrontFace.StencilDepthFailOp,
        (uint32_t)depth_description.FrontFace.StencilPassOp};
    ComPtr<ID3D11DepthStencilState> depth_state;
    if (!state->stencil_enable) depth_state = depth_states[depth_key];
    else for (const auto &entry : stencil_states)
        if (entry.key == stencil_key) { depth_state = entry.state; break; }
    if (!depth_state) {
        if (FAILED(device->CreateDepthStencilState(&depth_description, &depth_state))) return 0;
        if (!state->stencil_enable) depth_states[depth_key] = depth_state;
        else {
            if (stencil_states.size() >= 256) stencil_states.clear();
            stencil_states.push_back({stencil_key, depth_state});
        }
        gpu_timing.depth_created++;
    }
    std::array<ComPtr<ID3D11ShaderResourceView>,4> views;
    std::array<ComPtr<ID3D11SamplerState>,4> samplers;
    uint32_t signed_texture_stages = 0, packed_texture_stages = 0;
    Constants constants = {};
    D3D11_RECT clip = {(LONG)std::min(state->clip_x, state->width), (LONG)std::min(state->clip_y, state->height),
                      (LONG)std::min(state->clip_x + state->clip_width, state->width),
                      (LONG)std::min(state->clip_y + state->clip_height, state->height)};
    if (state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES && state->cull_enable && state->cull_face == 0x408) { clip.right = clip.left; clip.bottom = clip.top; }
    configure_window_clip(*state, constants, clip);
    constants.control[0] = state->combiners.control & 255u; constants.control[1] = state->stage_program;
    constants.control[2] = state->alpha_enable; constants.control[3] = state->alpha_function; constants.misc[0] = state->alpha_reference;
    constants.misc[1] = state->control0;
    constants.misc[2] = state->fixed_transform; constants.misc[3] = state->flat_shading;
    constants.final_input[0] = state->combiners.final_input[0]; constants.final_input[1] = state->combiners.final_input[1];
    constants.final_input[2] = state->combiners.control;
    constants.final_input[3] = state->shader_clip_mode;
    constants.texture_control[0] = state->shader_other_stage_input;
    constants.texture_control[1] = state->shader_dot_mapping;
    std::memcpy(constants.shader_eye_vector, state->shader_eye_vector, sizeof state->shader_eye_vector);
    constants.texture_control[2] = state->fog_enable;
    constants.texture_control[3] = state->fog_mode;
    std::memcpy(constants.fog_parameters, state->fog_parameters, sizeof state->fog_parameters);
    constants.fog_control[0] = state->fog_gen_mode;
    std::memcpy(constants.fog_plane, state->fog_plane, sizeof state->fog_plane);
    std::memcpy(constants.factors, state->combiners.factors, sizeof constants.factors);
    std::memcpy(constants.fog, state->combiners.fog_color, sizeof constants.fog);
    if (state->vertex_constants) std::memcpy(constants.vertex_constants, state->vertex_constants, sizeof constants.vertex_constants);
    if (state->fixed_transform) {
        constants.light_state[0] = state->lighting_enable; constants.light_state[1] = state->specular_enable;
        constants.light_state[2] = state->light_enable_mask; constants.light_state[3] = state->color_material;
        constants.light_control[0] = state->light_control;
        constants.light_control[1] = state->normalization_enable;
        constants.light_control[2] = state->skin_mode;
        constants.light_control[3] = state->texgen_view_model;
        std::memcpy(constants.texgen, state->texgen, sizeof constants.texgen);
        std::memcpy(constants.texture_matrix_enable, state->texture_matrix_enable, sizeof constants.texture_matrix_enable);
        std::memcpy(constants.material, state->material_emission, sizeof constants.material);
        constants.material[3] = state->material_alpha;
        std::memcpy(constants.scene_ambient, state->scene_ambient, sizeof constants.scene_ambient);
        constants.specular_power[0] = state->specular_power;
        std::memcpy(constants.light_ambient, state->light_ambient, sizeof constants.light_ambient);
        std::memcpy(constants.light_diffuse, state->light_diffuse, sizeof constants.light_diffuse);
        std::memcpy(constants.light_specular, state->light_specular, sizeof constants.light_specular);
        std::memcpy(constants.light_local_position, state->light_local_position, sizeof constants.light_local_position);
        std::memcpy(constants.light_local_attenuation, state->light_local_attenuation, sizeof constants.light_local_attenuation);
        std::memcpy(constants.light_infinite_direction, state->light_infinite_direction, sizeof constants.light_infinite_direction);
        std::memcpy(constants.light_infinite_half_vector, state->light_infinite_half_vector, sizeof constants.light_infinite_half_vector);
        std::memcpy(constants.light_spot_direction, state->light_spot_direction, sizeof constants.light_spot_direction);
    }
    if (state->fixed_transform && state->vertex_constants &&
            ((state->stage_program & 31u) == 1u)) {
        static int shadow_diag = -1;
        if (shadow_diag < 0) shadow_diag = getenv("RECOMP_SHADOW_DIAG") != NULL;
        if (shadow_diag) {
            static std::set<uint64_t> seen;
            uint64_t key = ((uint64_t)state->stage_program << 24)
                         | ((uint64_t)(state->textures[0].format & 0xFFu) << 16)
                         | ((uint64_t)(state->texgen[0][0] & 0xFFFFu) << 0);
            if (seen.size() < 32 && seen.insert(key).second) {
                const float (*vc)[4] = state->vertex_constants;
                fprintf(stderr, "[SHADOW] stages=%X ctl0=%X blend=%u/%X/%X depth=%u/%X tm0=%u tgen=%X,%X,%X,%X "
                        "tex0=%X addr=%X/%X/%X filt=%X ctl=%X/%u %ux%u w=%u h=%u "
                        "pl64=%.4f,%.4f,%.4f,%.4f pl68=%.4f,%.4f,%.4f,%.4f\n",
                        state->stage_program, state->control0, state->blend_enable,
                        state->blend_source, state->blend_destination, state->depth_enable,
                        state->depth_function, state->texture_matrix_enable[0],
                        state->texgen[0][0], state->texgen[0][1], state->texgen[0][2], state->texgen[0][3],
                        state->textures[0].format, state->textures[0].address_u,
                        state->textures[0].address_v, state->textures[0].address_w,
                        state->textures[0].filter, state->textures[0].control0,
                        state->textures[0].control0_valid, state->textures[0].width, state->textures[0].height,
                        state->width, state->height,
                        vc[64][0], vc[64][1], vc[64][2], vc[64][3],
                        vc[68][0], vc[68][1], vc[68][2], vc[68][3]);
            }
        }
    }
    constants.viewport[0] = (float)state->width; constants.viewport[1] = (float)state->height;
    constants.viewport[2] = state->depth_format == 1 ? 65535.0f : 16777215.0f;
    constants.depth_range[0] = depth_clip_min; constants.depth_range[1] = depth_clip_max;
    constants.depth_range[2] = (float)depth_clamp;
    if (front_offset || back_offset) {
        constants.depth_offset[0] = state->polygon_offset_scale; constants.depth_offset[1] = state->polygon_offset_bias;
        constants.depth_offset[2] = front_offset ? 1.0f : 0.0f; constants.depth_offset[3] = back_offset ? 1.0f : 0.0f;
    }
    for (uint32_t stage = 0; stage < 8; stage++) {
        constants.stages[stage][0] = state->combiners.rgb_input[stage]; constants.stages[stage][1] = state->combiners.alpha_input[stage];
        constants.stages[stage][2] = state->combiners.rgb_output[stage]; constants.stages[stage][3] = state->combiners.alpha_output[stage];
    }
    phase_timer.next_phase(gpu_timing.draw_textures);
    for (uint32_t stage = 0; stage < 4; stage++) {
        uint32_t mode = (state->stage_program >> (stage * 5)) & 31;
        bool dependent = mode == 15 || mode == 16;
        bool bump = mode == 6 || mode == 7;
        bool diffuse_reflection = mode == 11;
        bool reflection = diffuse_reflection || mode == 12 || mode == 14 || mode == 18;
        bool dot_stage = mode == 9 || mode == 17 || reflection;
        if (mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5 && !dependent && !bump && !dot_stage) return reject("texture shader mode");
        if (dot_stage) {
            if ((mode == 17 && stage != 1 && stage != 2) || (mode == 9 && stage < 2)) return reject("dot texture stage");
            if (diffuse_reflection) {
                if (stage != 2 || ((state->stage_program >> 5) & 31u) != 17) return reject("diffuse reflection dot predecessor");
                uint32_t next_mode = (state->stage_program >> 15) & 31u;
                if (next_mode != 12 && next_mode != 18) return reject("diffuse reflection following specular stage");
                if (((state->shader_other_stage_input >> 20) & 15u) >= 2) return reject("diffuse reflection look-ahead input");
                if (((state->shader_dot_mapping >> 8) & 15u) > 4) return reject("diffuse reflection look-ahead mapping");
                for (uint32_t vertex = 0; !state->vertex_program && vertex < count; vertex++)
                    for (uint32_t component = 0; component < 3; component++)
                        if (!nv_cpu_finite(vertices[vertex].texture[3][component])) return reject("nonfinite diffuse reflection coordinate");
            } else if (reflection && (stage != 3 || ((state->stage_program >> 5) & 31u) != 17 ||
                (((state->stage_program >> 10) & 31u) != 17 && (mode == 14 || ((state->stage_program >> 10) & 31u) != 11))))
                return reject("reflection dot predecessors");
            if (mode == 18) {
                if (state->shader_eye_vector_valid != 7u) return reject("unprogrammed constant eye vector");
                for (float component : state->shader_eye_vector)
                    if (!nv_cpu_finite(component)) return reject("nonfinite constant eye vector");
            }
            if (((state->shader_dot_mapping >> ((stage - 1) * 4)) & 15u) > 4) return reject("dot texture mapping");
            if (mode == 9) {
                uint32_t previous_mode = (state->stage_program >> ((stage - 1) * 5)) & 31u;
                if (previous_mode != 9 && previous_mode != 17) return reject("dot texture predecessor");
            }
        }
        if (dependent || bump || dot_stage) {
            uint32_t source_stage = stage == 1 ? 0 : (state->shader_other_stage_input >> (stage * 4 + 8)) & 15u;
            if (!stage || source_stage >= stage) return reject("dependent texture input stage");
            if (dot_stage) {
                uint32_t source_mode = (state->stage_program >> (source_stage * 5)) & 31u;
                if (source_mode == 0 || source_mode == 5 || source_mode == 17) return reject("dot texture input has no color result");
                uint32_t mapping = (state->shader_dot_mapping >> ((stage - 1) * 4)) & 15u;
                uint32_t sign_mask = source_mode == 4 ? 0 :
                    (state->textures[source_stage].filter >> 28) & (mapping == 4 ? 15u : 14u);
                if (mapping != 0 && sign_mask) return reject("signed texture input with encoded dot mapping");
                if (diffuse_reflection && source_mode != 4 && ((state->shader_dot_mapping >> 8) & 15u) != 0 &&
                    ((state->textures[0].filter >> 28) & 15u))
                    return reject("signed reflection look-ahead with encoded dot mapping");
                if ((mapping == 4 || (diffuse_reflection && ((state->shader_dot_mapping >> 8) & 15u) == 4)) &&
                    (source_mode == 4 || source_mode == 7)) return reject("HILO requires normalized sampled color input");
                if (reflection && !diffuse_reflection && source_stage != 0) return reject("reflection normal input stage");
                if (diffuse_reflection && ((state->shader_other_stage_input >> 20) & 15u) != 0)
                    return reject("diffuse reflection look-ahead has no color result");
            }
        }
        if (!nv2a_gpu_texture_mode_samples(mode)) continue;
        const auto &binding = state->textures[stage];
        if (!nv2a_gpu_texture_enabled(&binding)) return reject("disabled texture used by sampling shader mode");
        if ((mode == 2) != (binding.depth != 0) || (binding.depth && (binding.cube || binding.linear)))
            return reject("projective volume texture dimensionality");
        uint32_t sign_mask = binding.filter >> 28;
        uint32_t key_mode = binding.control0_valid ? binding.control0 & 3u : 0;
        if (sign_mask && key_mode) return reject("signed-channel texture color-key ordering");
        if (reflection && binding.linear) return reject("reflection texture dimensionality");
        if (bump) {
            if (binding.cube) return reject("bump texture dimensionality");
            for (float component : binding.bump_matrix)
                if (!nv_cpu_finite(component)) return reject("nonfinite bump matrix");
            if (mode == 7 && (!nv_cpu_finite(binding.bump_scale) || !nv_cpu_finite(binding.bump_offset)))
                return reject("nonfinite bump luminance");
            std::memcpy(constants.bump_matrix[stage], binding.bump_matrix, sizeof binding.bump_matrix);
            constants.bump_luminance[stage][0] = binding.bump_scale;
            constants.bump_luminance[stage][1] = binding.bump_offset;
        }
        if (dependent && (binding.cube || binding.linear)) return reject("dependent texture dimensionality");
        if (mode == 9 && (binding.cube || binding.linear)) return reject("dot texture dimensionality");
        for (uint32_t vertex = 0; !dependent && !state->vertex_program && vertex < count; vertex++) {
            for (uint32_t component = 0; component < 4; component++)
                if (state->texgen[stage][component] == 0 &&
                    !nv_cpu_finite(vertices[vertex].texture[stage][component])) {
                    static uint32_t shown;
                    if (shown++ < 16)
                        std::fprintf(stderr, "[TEXCOORD] sanitize stage %u vertex %u comp %u tex=(%.3f %.3f %.3f %.3f) texgen=(%u %u %u %u) matrix_en %u mode %u fmt %08X\n",
                            stage, vertex, component,
                            vertices[vertex].texture[stage][0], vertices[vertex].texture[stage][1],
                            vertices[vertex].texture[stage][2], vertices[vertex].texture[stage][3],
                            state->texgen[stage][0], state->texgen[stage][1], state->texgen[stage][2], state->texgen[stage][3],
                            state->texture_matrix_enable[stage], mode, binding.format);
                    /* Hardware does not reject a NaN/inf texcoord: it samples
                     * texel 0. Dropping the whole batch deletes geometry, so
                     * fold the bad component to 0 and keep drawing. */
                    vertices[vertex].texture[stage][component] = 0.0f;
                }
            if ((mode == 1 || mode == 2) && vertices[vertex].texture[stage][3] == 0) return reject("zero projective texture coordinate");
        }
        if (!binding.source_bytes) return reject("missing texture data");
        views[stage] = get_texture(binding, *surface);
        if (!views[stage]) return reject("texture upload or resource alias");
        if (!make_sampler(binding, samplers[stage])) return reject("texture filter, addressing or LOD state");
        constants.texture_info[stage][0] = (float)binding.width; constants.texture_info[stage][1] = (float)binding.height; constants.texture_info[stage][2] = (float)binding.linear;
        constants.texture_key[stage][0] = binding.color_key;
        constants.texture_key[stage][1] = binding.format == 0x03 || binding.format == 0x07 ||
            binding.format == 0x1C || binding.format == 0x1E ? 0x00FFFFFFu : UINT32_MAX;
        constants.texture_key[stage][2] = binding.control0_valid ? binding.control0 & 3u : 0;
        constants.texture_key[stage][3] = sign_mask;
        constants.texture_info[stage][3] = (float)((binding.format == 0x1E ? 1u : 0u) | (binding.cube ? 2u : 0u) |
            (binding.address_u == 5 ? 4u : 0u) | (binding.address_v == 5 ? 8u : 0u) |
            (binding.control0_valid && (binding.control0 & 4u) ? 16u : 0u) |
            (binding.address_w == 5 ? 32u : 0u));
        if (sign_mask) signed_texture_stages++;
        switch (binding.format) {
        case 0x01: case 0x16: case 0x17: case 0x1A: case 0x1B: case 0x20: case 0x28: case 0x29:
        case 0x3A: case 0x3B: case 0x3C: packed_texture_stages++; break;
        default: break;
        }
    }
    phase_timer.next_phase(gpu_timing.draw_streams);
    UINT vertex_stride = state->vertex_program ? sizeof(Nv2aGpuVertex) : sizeof(Nv2aGpuVertex) - sizeof(vertices[0].attributes);
    auto &vertex_stream = vertex_streams[state->vertex_program ? 1 : 0];
    UINT vertex_bytes = count * vertex_stride;
    UINT index_bytes = state->indices ? state->index_count * sizeof(uint32_t) : 0;
    UINT vertex_offset;
    /* Vertex and index data share one dynamic buffer, written under one Map
     * that is held across draws and released at the frame boundary. */
    if (!map_stream(vertex_stream, vertex_bytes + index_bytes,
                    D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER, "geometry", vertex_offset))
        return 0;
    uint8_t *vertex_destination = (uint8_t *)vertex_stream.mapped.pData + vertex_offset;
    if (state->vertex_program) std::memcpy(vertex_destination, vertices, vertex_bytes);
    else for (uint32_t vertex = 0; vertex < count; vertex++)
        std::memcpy(vertex_destination + vertex * vertex_stride, &vertices[vertex], offsetof(Nv2aGpuVertex, attributes));
    UINT index_offset = 0;
    if (state->indices) {
        index_offset = vertex_offset + vertex_bytes;
        std::memcpy((uint8_t *)vertex_stream.mapped.pData + index_offset, state->indices, index_bytes);
    }
    if (state->indices) context->IASetIndexBuffer(vertex_stream.buffer.Get(), DXGI_FORMAT_R32_UINT, index_offset);
    else context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
    {
        GpuTimer constants_timer(gpu_timing.constants);
        if (uploaded_constants_valid && !std::memcmp(&uploaded_constants, &constants, sizeof constants)) {
            gpu_timing.constant_reuses++;
        } else {
            D3D11_MAPPED_SUBRESOURCE constant_map = {};
            if (!target_operation_succeeded("constants", "upload map",
                context->Map(constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &constant_map))) return 0;
            std::memcpy(constant_map.pData, &constants, sizeof constants);
            context->Unmap(constant_buffer.Get(), 0);
            uploaded_constants = constants;
            uploaded_constants_valid = true;
            gpu_timing.constant_uploads++;
        }
    }
    phase_timer.next_phase(gpu_timing.draw_shaders);
    bool uncombined = !(state->combiners.control & 255u) && !state->combiners.final_input[0] && !state->combiners.final_input[1];
    for (uint32_t stage = 0; stage < 4; stage++)
        if (((state->stage_program >> (stage * 5)) & 31u) == 5u ||
            ((uint32_t)constants.texture_info[stage][3] & 16u) ||
            constants.texture_key[stage][2] == 3u) uncombined = false;
    ID3D11PixelShader *program = uncombined ? uncombined_shader.Get() : get_pixel_shader(*state, constants.window_clip_control[0]);
    if (!program) return 0;
    VertexVariant *guest_vertex = state->vertex_program ? get_vertex_shader(*state) : nullptr;
    if (state->vertex_program && !guest_vertex) return reject("unsupported vertex program");
    phase_timer.next_phase(gpu_timing.draw_submit);
    UINT stride = vertex_stride, offset = vertex_offset;
    ID3D11Buffer *buffers[] = {vertex_stream.buffer.Get()}, *constants_buffer[] = {constant_buffer.Get()};
    ID3D11RenderTargetView *target = surface->target.Get();
    ID3D11ShaderResourceView *resources[12] = {}; ID3D11SamplerState *sampler_states[4];
    for (uint32_t stage = 0; stage < 4; stage++) {
        resources[state->textures[stage].depth ? stage + 8 : state->textures[stage].cube ? stage + 4 : stage] = views[stage].Get();
        sampler_states[stage] = samplers[stage].Get();
    }
    float blend_color[4]; nv_cpu_unpack_argb(state->blend_constant, blend_color);
    context->OMSetRenderTargets(1, &target, depth ? depth->view.Get() : nullptr);
    context->OMSetDepthStencilState(depth_state.Get(), state->stencil_reference & 255u); context->OMSetBlendState(blending.Get(), blend_color, ~0u);
    context->IASetInputLayout(guest_vertex ? guest_vertex->layout.Get() : input_layout.Get()); context->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context->IASetPrimitiveTopology(state->topology == NV2A_GPU_TOPOLOGY_LINES ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST :
                                    state->topology == NV2A_GPU_TOPOLOGY_POINTS ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST :
                                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(guest_vertex ? guest_vertex->shader.Get() : vertex_shader.Get(), nullptr, 0);
    ID3D11GeometryShader *geometry = state->topology == NV2A_GPU_TOPOLOGY_LINES ? line_geometry_shader.Get() :
        state->topology == NV2A_GPU_TOPOLOGY_POINTS ? point_geometry_shader.Get() :
        guest_vertex || state->fixed_transform || state->flat_shading ? geometry_shader.Get() : nullptr;
    context->GSSetShader(geometry, nullptr, 0);
    context->PSSetShader(program, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, constants_buffer); context->PSSetConstantBuffers(0, 1, constants_buffer);
    context->GSSetConstantBuffers(0, 1, constants_buffer);
    context->PSSetShaderResources(0, 12, resources); context->PSSetSamplers(0, 4, sampler_states);
    D3D11_VIEWPORT viewport = {0,0,(float)state->width,(float)state->height,0,1};
    D3D11_BOX region = {};
    region.left = clip.left > 0 ? (UINT)clip.left : 0;
    region.top = clip.top > 0 ? (UINT)clip.top : 0;
    region.right = clip.right > 0 ? (UINT)clip.right : 0;
    region.bottom = clip.bottom > 0 ? (UINT)clip.bottom : 0;
    if (region.right > state->width) region.right = state->width;
    if (region.bottom > state->height) region.bottom = state->height;
    region.back = 1;
    bool writes = region.left < region.right && region.top < region.bottom;
    context->RSSetState(raster_state.Get()); context->RSSetViewports(1, &viewport); context->RSSetScissorRects(1, &clip);
    GpuDrawSample *sample = begin_gpu_sample(*state);
    if (state->indices) context->DrawIndexed(state->index_count, 0, 0);
    else context->Draw(count, 0);
    end_gpu_sample(sample);
    phase_timer.next_phase(gpu_timing.draw_bookkeeping);
    if (state->topology == NV2A_GPU_TOPOLOGY_LINES) gpu_timing.line_primitives += element_count / 2;
    if (state->topology == NV2A_GPU_TOPOLOGY_POINTS) gpu_timing.point_primitives += element_count;
    gpu_timing.signed_texture_stages += signed_texture_stages;
    gpu_timing.packed_texture_stages += packed_texture_stages;
    if (state->window_clip_valid) {
        gpu_timing.window_clip_draws++;
        if (constants.window_clip_control[0]) gpu_timing.window_shader_draws++;
        else gpu_timing.window_scissor_draws++;
        if (state->window_clip_type) gpu_timing.window_exclusive_draws++;
    }
    if (state->fixed_transform) {
        gpu_timing.fixed_draws++;
        if (state->lighting_enable) gpu_timing.lit_draws++;
        if (state->skin_mode) gpu_timing.skinned_draws++;
        bool texgen_used = false, texture_matrix_used = false;
        for (uint32_t stage = 0; stage < 4; stage++) {
            texture_matrix_used |= state->texture_matrix_enable[stage] != 0;
            for (uint32_t component = 0; component < 4; component++)
                texgen_used |= state->texgen[stage][component] != 0;
        }
        if (texgen_used) gpu_timing.texgen_draws++;
        if (texture_matrix_used) gpu_timing.texture_matrix_draws++;
    }
    pending_draws = true;
    if (state->color_mask & 0x01010101u) { if (writes) mark_written_region(*surface, region); }
    else gpu_timing.color_masked_draws++;
    bool stencil_writes = state->stencil_enable && (state->stencil_write_mask & 255u) &&
        (state->stencil_fail != 0x1E00 || state->stencil_depth_fail != 0x1E00 || state->stencil_pass != 0x1E00);
    if (depth && ((state->depth_enable && state->depth_mask) || stencil_writes) && writes) mark_written_region(*depth, region);
    ID3D11ShaderResourceView *empty[12] = {};
    context->PSSetShaderResources(0, 12, empty);
    return 1;
}

extern "C" int nv2a_gpu_compile(void)
{
    if (vertex_shader && uncombined_shader && geometry_shader && line_geometry_shader && point_geometry_shader) return 1;
    if (!nv2a_gpu_available()) return 0;
    ComPtr<ID3DBlob> pixel_code, errors;
    HRESULT result = D3DCompile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr, nullptr,
                               "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &vertex_code, &errors);
    if (FAILED(result)) {
        std::fprintf(stderr, "[GPU-D3D11] shader compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return 0;
    }
    result = device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr, &vertex_shader);
    if (FAILED(result)) {
        std::fprintf(stderr, "[GPU-D3D11] base vertex shader creation failed: 0x%08X; device 0x%08X\n",
                     (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
        return 0;
    }
    pixel_code.Reset(); errors.Reset();
    result = D3DCompile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr, nullptr,
                       "ps_uncombined", "ps_5_0", D3DCOMPILE_IEEE_STRICTNESS, 0, &pixel_code, &errors);
    if (FAILED(result)) return 0;
    if (FAILED(device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &uncombined_shader))) return 0;
    const struct { const char *entry; ComPtr<ID3D11GeometryShader> *shader; } geometries[] = {
        {"gs_main", &geometry_shader}, {"gs_lines_main", &line_geometry_shader}, {"gs_points_main", &point_geometry_shader}
    };
    for (const auto &geometry : geometries) {
        pixel_code.Reset(); errors.Reset();
        result = D3DCompile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr, nullptr,
                           geometry.entry, "gs_5_0", D3DCOMPILE_IEEE_STRICTNESS, 0, &pixel_code, &errors);
        if (SUCCEEDED(result))
            result = device->CreateGeometryShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr,
                                                   geometry.shader->ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] geometry %s failed (0x%08lX): %s\n", geometry.entry,
                         (unsigned long)result, errors ? (const char *)errors->GetBufferPointer() : "shader creation failed");
            return 0;
        }
    }
    return 1;
}

extern "C" int nv2a_gpu_available(void)
{
    static bool attempted;
    if (!attempted) {
        attempted = true;
        D3D_FEATURE_LEVEL level;
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &device, &level, &context);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] device unavailable: 0x%08X\n", (unsigned)result);
            return 0;
        }
        std::fprintf(stderr, "[GPU-D3D11] device ready: feature level 0x%X, hardware\n", (unsigned)level);
    }
    return device != nullptr;
}