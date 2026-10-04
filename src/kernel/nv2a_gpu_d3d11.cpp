#include "nv2a_gpu.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <d3dcompiler.h>
#include "nv2a_gpu_shader.h"
#include <vector>
#include <array>
#include <cstring>
#include <string>
#include <cstddef>
#include <chrono>
#include <algorithm>

using Microsoft::WRL::ComPtr;

static struct {
    double draw, texture, hash, upload, sync, state;
    double sync_copy, sync_wait, sync_publish, sync_unmap;
    double color_publish, depth_publish;
    double sync_event_wait;
    double constants;
    uint64_t hash_bytes;
    uint64_t blend_created, depth_created, sampler_created;
    uint64_t color_created, depth_surface_created, color_refreshed, depth_refreshed;
    uint64_t color_refresh_reused, depth_refresh_reused;
    uint64_t color_readbacks, depth_readbacks, color_masked_draws, completion_waits;
    uint64_t constant_uploads;
    uint64_t color_copy_bytes, color_publish_bytes, depth_publish_bytes, depth_convert_bytes;
    uint64_t coverage_mask_updates, coverage_mask_rows;
    uint64_t color_refresh_bytes, color_refresh_full_bytes;
    uint64_t color_bulk_readbacks;
    uint64_t texture_created, texture_updated, texture_update_bytes;
    uint64_t texture_evicted, texture_cache_entries, texture_cache_bytes;
    uint64_t fixed_draws, lit_draws, skinned_draws, texgen_draws, texture_matrix_draws;
    uint64_t signed_texture_stages, packed_texture_stages;
    uint64_t window_clip_draws, window_scissor_draws, window_shader_draws, window_exclusive_draws;
    uint64_t line_primitives, point_primitives;
} gpu_timing;

struct GpuTimer {
    double &total;
    std::chrono::steady_clock::time_point start;
    explicit GpuTimer(double &target) : total(target), start(std::chrono::steady_clock::now()) {}
    ~GpuTimer() { total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
};

extern "C" void nv2a_gpu_report(void)
{
    std::fprintf(stderr, "[GPU-D3D11] time: draw %.3fs texture %.3fs hash %.3fs upload %.3fs sync %.3fs state %.3fs; hashed %.3f GiB\n",
                 gpu_timing.draw, gpu_timing.texture, gpu_timing.hash, gpu_timing.upload, gpu_timing.sync, gpu_timing.state,
                 (double)gpu_timing.hash_bytes / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] states created: %llu blend, %llu depth, %llu sampler\n",
                 (unsigned long long)gpu_timing.blend_created, (unsigned long long)gpu_timing.depth_created,
                 (unsigned long long)gpu_timing.sampler_created);
    std::fprintf(stderr, "[GPU-D3D11] texture uploads: %llu created, %llu updated; %.3f GiB updated\n",
                 (unsigned long long)gpu_timing.texture_created, (unsigned long long)gpu_timing.texture_updated,
                 (double)gpu_timing.texture_update_bytes / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] texture cache: %llu entries, %.1f MiB, %llu evictions\n",
                 (unsigned long long)gpu_timing.texture_cache_entries,
                 (double)gpu_timing.texture_cache_bytes / 1048576.0,
                 (unsigned long long)gpu_timing.texture_evicted);
    std::fprintf(stderr, "[GPU-D3D11] surfaces: %llu color created, %llu depth created; %llu color refreshed, %llu depth refreshed\n",
                 (unsigned long long)gpu_timing.color_created, (unsigned long long)gpu_timing.depth_surface_created,
                 (unsigned long long)gpu_timing.color_refreshed, (unsigned long long)gpu_timing.depth_refreshed);
    std::fprintf(stderr, "[GPU-D3D11] unchanged refreshes: %llu color, %llu depth\n",
                 (unsigned long long)gpu_timing.color_refresh_reused, (unsigned long long)gpu_timing.depth_refresh_reused);
    std::fprintf(stderr, "[GPU-D3D11] readback: copy %.3fs wait %.3fs publish %.3fs unmap %.3fs\n",
                 gpu_timing.sync_copy, gpu_timing.sync_wait, gpu_timing.sync_publish, gpu_timing.sync_unmap);
    std::fprintf(stderr, "[GPU-D3D11] publication: color %.3fs depth %.3fs\n",
                 gpu_timing.color_publish, gpu_timing.depth_publish);
    std::fprintf(stderr, "[GPU-D3D11] bulk color readbacks: %llu\n",
                 (unsigned long long)gpu_timing.color_bulk_readbacks);
    std::fprintf(stderr, "[GPU-D3D11] completion: %.3fs in %llu event waits; %llu color-masked draws; readbacks %llu color, %llu depth\n",
                 gpu_timing.sync_event_wait, (unsigned long long)gpu_timing.completion_waits,
                 (unsigned long long)gpu_timing.color_masked_draws, (unsigned long long)gpu_timing.color_readbacks,
                 (unsigned long long)gpu_timing.depth_readbacks);
    std::fprintf(stderr, "[GPU-D3D11] constants: %llu uploads, %.3fs\n",
                 (unsigned long long)gpu_timing.constant_uploads, gpu_timing.constants);
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
static ComPtr<ID3D11VertexShader> vertex_shader;
static ComPtr<ID3D11GeometryShader> geometry_shader;
static ComPtr<ID3D11GeometryShader> line_geometry_shader, point_geometry_shader;
static ComPtr<ID3D11PixelShader> pixel_shader;
static ComPtr<ID3D11PixelShader> uncombined_shader;
static ComPtr<ID3DBlob> vertex_code;

struct Surface {
    uint8_t *memory;
    uint32_t width, height, pitch;
    bool dirty, needs_refresh, snapshot_valid;
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
    std::vector<uint8_t> source_snapshot;
    ComPtr<ID3D11ShaderResourceView> view;
};

struct DepthSurface {
    uint8_t *memory;
    uint32_t width, height, pitch, format;
    bool dirty, needs_refresh, snapshot_valid;
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
static std::vector<CachedTexture> textures;
static ComPtr<ID3D11InputLayout> input_layout;
static ComPtr<ID3D11Buffer> constant_buffer;
static std::array<std::array<ComPtr<ID3D11Buffer>,17>,2> vertex_buffers;
static std::array<ComPtr<ID3D11Buffer>,19> index_buffers;
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
};
static std::vector<ShaderVariant> shader_variants;

struct VertexVariant {
    std::vector<uint32_t> key;
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11InputLayout> layout;
};
static std::vector<VertexVariant> vertex_variants;

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

static bool vertex_program_key(const Nv2aGpuDraw &state, std::vector<uint32_t> &key)
{
    if (!state.vertex_program || !state.vertex_valid || !state.vertex_constants || state.vertex_start >= 136) return false;
    for (uint32_t slot = state.vertex_start; slot < 136; slot++) {
        if (state.vertex_valid[slot] != 15) return false;
        key.insert(key.end(), state.vertex_program[slot], state.vertex_program[slot] + 4);
        if (state.vertex_program[slot][3] & 1u) return true;
    }
    return false;
}

static std::string vertex_program_source(const std::vector<uint32_t> &key, bool state_program)
{
    std::string source = nv2a_gpu_shader_source;
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
        std::string operand_a = mac ? vertex_source((words[2] >> 26) & 3, words[2] >> 28, words[1] & 255, (words[1] >> 8) & 1, input, constant, relative) : "float4(0,0,0,0)";
        std::string operand_b = mac == 2 || (mac >= 4 && mac <= 12) ? vertex_source((words[2] >> 11) & 3, (words[2] >> 13) & 15, (words[2] >> 17) & 255, (words[2] >> 25) & 1, input, constant, relative) : "float4(0,0,0,0)";
        std::string operand_c = mac == 3 || mac == 4 || ilu ? vertex_source((words[3] >> 28) & 3, ((words[2] & 3) << 2) | (words[3] >> 30), (words[2] >> 2) & 255, (words[2] >> 10) & 1, input, constant, relative) : "float4(0,0,0,0)";
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
    VertexVariant variant;
    if (!vertex_program_key(state, variant.key)) return nullptr;
    for (auto &cached : vertex_variants) if (cached.key == variant.key) return &cached;
    std::string source = vertex_program_source(variant.key, false);
    if (source.empty()) return nullptr;
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompile(source.data(), source.size(), "NV2A-vertex", nullptr, nullptr, "vs_guest", "vs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result)) {
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
    if (vertex_variants.size() >= 128) vertex_variants.erase(vertex_variants.begin());
    vertex_variants.push_back(std::move(variant));
    return &vertex_variants.back();
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
    for (auto &cached : shader_variants) if (cached.key == variant.key) return cached.shader.Get();
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
        std::fprintf(stderr, "[GPU-D3D11] guest shader compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return nullptr;
    }
    if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &variant.shader))) return nullptr;
    if (shader_variants.size() >= 128) shader_variants.erase(shader_variants.begin());
    shader_variants.push_back(std::move(variant));
    return shader_variants.back().shader.Get();
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

static void decode_depth_row(uint32_t format, const uint8_t *source, uint8_t *destination, uint32_t columns)
{
    if (format == 1) std::memcpy(destination, source, (size_t)columns * 2);
    else {
        if (source != destination) std::memcpy(destination, source, (size_t)columns * 4);
        for (uint32_t column = 0; column < columns; column++) {
            uint32_t value; std::memcpy(&value, destination + column * 4, 4);
            value = (value << 8) | (value >> 24); std::memcpy(destination + column * 4, &value, 4);
        }
    }
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
            for (uint32_t row = changed.top; row < changed.bottom; row++)
                std::memcpy(surface.source_snapshot.data() + (size_t)row * row_bytes + changed.left * 4,
                            surface.memory + (size_t)row * surface.pitch + changed.left * 4,
                            (size_t)(changed.right - changed.left) * 4);
            context->UpdateSubresource(surface.texture.Get(), 0, &changed,
                                      surface.source_snapshot.data() + (size_t)changed.top * row_bytes + changed.left * 4,
                                      (UINT)row_bytes, 0);
            gpu_timing.color_refreshed++;
            gpu_timing.color_refresh_bytes += (uint64_t)(changed.right - changed.left) * (changed.bottom - changed.top) * 4;
            gpu_timing.color_refresh_full_bytes += (uint64_t)surface.width * surface.height * 4;
        } else gpu_timing.color_refresh_reused++;
        surface.needs_refresh = false;
        return;
    }
    context->UpdateSubresource(surface.texture.Get(), 0, nullptr, surface.memory, surface.pitch, 0);
    gpu_timing.color_refresh_bytes += (uint64_t)surface.width * surface.height * 4;
    gpu_timing.color_refresh_full_bytes += (uint64_t)surface.width * surface.height * 4;
    surface.needs_refresh = false;
    surface.snapshot_valid = false;
    gpu_timing.color_refreshed++;
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
    if (FAILED(context->Map(surface.staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;
    for (uint32_t row = 0; row < surface.height; row++) {
        const uint8_t *source = surface.memory + (size_t)row * surface.pitch;
        uint8_t *destination = (uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
        if (surface.format == 1) std::memcpy(destination, source, surface.width * 2);
        else for (uint32_t column = 0; column < surface.width; column++) {
            uint32_t value; std::memcpy(&value, source + column * 4, 4);
            value = (value >> 8) | (value << 24); std::memcpy(destination + column * 4, &value, 4);
        }
    }
    context->Unmap(surface.staging.Get(), 0);
    context->CopyResource(surface.texture.Get(), surface.staging.Get());
    surface.needs_refresh = false;
    surface.snapshot_valid = false;
    gpu_timing.depth_refreshed++;
    return true;
}

static Surface *get_surface(const Nv2aGpuDraw &state)
{
    for (auto &surface : surfaces)
        if (surface.memory == state.color && surface.width == state.width && surface.height == state.height && surface.pitch == state.pitch) {
            refresh_surface(surface);
            return &surface;
        }
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = state.width; description.Height = state.height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Surface surface = {};
    surface.memory = state.color; surface.width = state.width; surface.height = state.height; surface.pitch = state.pitch;
    D3D11_SUBRESOURCE_DATA data = {state.color, state.pitch, 0};
    if (FAILED(device->CreateTexture2D(&description, &data, &surface.texture)) ||
        FAILED(device->CreateRenderTargetView(surface.texture.Get(), nullptr, &surface.target)) ||
        FAILED(device->CreateShaderResourceView(surface.texture.Get(), nullptr, &surface.view))) return nullptr;
    description.BindFlags = 0; description.Usage = D3D11_USAGE_STAGING; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &surface.staging))) return nullptr;
    surfaces.push_back(std::move(surface));
    gpu_timing.color_created++;
    return &surfaces.back();
}

static DepthSurface *get_depth(const Nv2aGpuDraw &state)
{
    if (!state.depth || (state.depth_format != 1 && state.depth_format != 2)) return nullptr;
    uint32_t bytes = state.depth_format == 1 ? 2 : 4;
    if (state.depth_pitch < state.width * bytes) return nullptr;
    for (auto &surface : depth_surfaces)
        if (surface.memory == state.depth && surface.width == state.width && surface.height == state.height &&
            surface.pitch == state.depth_pitch && surface.format == state.depth_format) return refresh_depth(surface) ? &surface : nullptr;
    std::vector<uint8_t> pixels((size_t)state.width * state.height * bytes);
    for (uint32_t row = 0; row < state.height; row++) {
        const uint8_t *source = state.depth + (size_t)row * state.depth_pitch;
        uint8_t *destination = pixels.data() + (size_t)row * state.width * bytes;
        if (bytes == 2) std::memcpy(destination, source, state.width * bytes);
        else for (uint32_t column = 0; column < state.width; column++) {
            uint32_t value; std::memcpy(&value, source + column * 4, 4);
            value = (value >> 8) | (value << 24); std::memcpy(destination + column * 4, &value, 4);
        }
    }
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = state.width; description.Height = state.height;
    description.MipLevels = description.ArraySize = 1; description.SampleDesc.Count = 1;
    description.Format = bytes == 2 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_SUBRESOURCE_DATA data = {pixels.data(), state.width * bytes, 0};
    DepthSurface surface = {};
    surface.memory = state.depth; surface.width = state.width; surface.height = state.height; surface.pitch = state.depth_pitch; surface.format = state.depth_format;
    D3D11_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = bytes == 2 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT; view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateTexture2D(&description, &data, &surface.texture)) ||
        FAILED(device->CreateDepthStencilView(surface.texture.Get(), &view, &surface.view))) return nullptr;
    description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &surface.staging))) return nullptr;
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

extern "C" void nv2a_gpu_sync(void)
{
    GpuTimer sync_timer(gpu_timing.sync);
    if (!context) return;
    ID3D11RenderTargetView *empty = nullptr;
    context->OMSetRenderTargets(1, &empty, nullptr);
    bool has_readback = false;
    {
        GpuTimer copy_timer(gpu_timing.sync_copy);
        for (auto &surface : surfaces)
            if (surface.dirty) {
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
        for (auto &surface : depth_surfaces)
            if (surface.dirty) {
                context->CopyResource(surface.staging.Get(), surface.texture.Get());
                has_readback = true;
            }
    }
    if (pending_draws && !has_readback) {
        GpuTimer wait_timer(gpu_timing.sync_event_wait);
        gpu_timing.completion_waits++;
        if (!completion_event) {
            D3D11_QUERY_DESC description = {D3D11_QUERY_EVENT, 0};
            HRESULT result = device->CreateQuery(&description, &completion_event);
            if (FAILED(result)) readback_failed("completion event", result, "create");
        }
        context->End(completion_event.Get());
        HRESULT result;
        while ((result = context->GetData(completion_event.Get(), nullptr, 0, 0)) == S_FALSE)
            SwitchToThread();
        if (FAILED(result)) readback_failed("completion event", result, "wait");
    }
    for (auto &surface : surfaces) {
        if (!surface.dirty) continue;
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
        surface.dirty = false;
    }
    for (auto &surface : depth_surfaces) {
        if (!surface.dirty) continue;
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
        surface.dirty = false;
    }
    pending_draws = false;
}

extern "C" void nv2a_gpu_flush(void)
{
    nv2a_gpu_sync();
    for (auto &surface : surfaces) surface.needs_refresh = true;
    for (auto &surface : depth_surfaces) surface.needs_refresh = true;
}

extern "C" void nv2a_gpu_invalidate(void)
{
    nv2a_gpu_sync();
    surfaces.clear();
    depth_surfaces.clear();
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
    return SUCCEEDED(device->CreateInputLayout(elements, (UINT)(sizeof elements / sizeof elements[0]),
        vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), &input_layout));
}

static ID3D11ShaderResourceView *get_texture(const Nv2aGpuTexture &binding, const Surface &destination)
{
    GpuTimer texture_timer(gpu_timing.texture);
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
    for (auto &surface : surfaces) {
        uintptr_t surface_begin = (uintptr_t)surface.memory, surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin >= surface_end || end <= surface_begin) continue;
        if (&surface == &destination) {
            std::fprintf(stderr, "[GPU-D3D11] texture target alias: source %p bytes %llu size %ux%u pitch %u format %X linear %u; target %p size %ux%u pitch %u\n",
                         (const void *)binding.source, (unsigned long long)binding.source_bytes,
                         binding.width, binding.height, binding.pitch, binding.format, binding.linear,
                         (const void *)surface.memory, surface.width, surface.height, surface.pitch);
            return nullptr;
        }
        if (levels == 1 && !binding.cube && !binding.depth && surface.memory == binding.source && surface.width == binding.width && surface.height == binding.height &&
            surface.pitch == binding.pitch && binding.linear && (binding.format == 0x12 || binding.format == 0x1E)) {
            refresh_surface(surface);
            return surface.view.Get();
        }
        nv2a_gpu_sync();
    }
    for (auto &surface : depth_surfaces) {
        uintptr_t surface_begin = (uintptr_t)surface.memory, surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin < surface_end && end > surface_begin) nv2a_gpu_sync();
    }
    CachedTexture *refresh = nullptr;
    {
        GpuTimer hash_timer(gpu_timing.hash);
        gpu_timing.hash_bytes += binding.source_bytes;
        for (auto &texture : textures)
            if (texture.source == binding.source && texture.width == binding.width && texture.height == binding.height &&
                texture.format == binding.format && texture.cube == binding.cube && texture.depth == binding.depth && texture.pitch == binding.pitch &&
                texture.linear == binding.linear && texture.face_stride == binding.face_stride &&
                texture.mip_levels == levels &&
                texture.source_snapshot.size() == binding.source_bytes) {
                if (std::memcmp(texture.source_snapshot.data(), binding.source, binding.source_bytes) == 0)
                    return texture.view.Get();
                refresh = &texture;
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
                for (uint32_t slice = 0; slice < depth; slice++)
                 for (uint32_t row = 0; row < height; row++)
                    for (uint32_t column = 0; column < width; column++) {
                        uint32_t *pixel = &image[((size_t)slice * height + row) * width + column];
                        bool decoded = binding.depth ? binding.decode_volume(binding.decode_context, level, column, row, slice, pixel) :
                            binding.decode_level ? binding.decode_level(binding.decode_context, face, level, column, row, pixel) :
                            binding.cube ? binding.decode_face(binding.decode_context, face, column, row, pixel) :
                                           binding.decode(binding.decode_context, column, row, pixel);
                        if (!decoded) return nullptr;
                        if (binding.depth) *pixel = (*pixel & 0xFF00FF00u) | ((*pixel & 255u) << 16) | ((*pixel >> 16) & 255u);
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
    if (binding.depth) {
        D3D11_TEXTURE3D_DESC volume_description = {};
        volume_description.Width = binding.width; volume_description.Height = binding.height;
        volume_description.Depth = binding.depth; volume_description.MipLevels = levels;
        volume_description.Format = format; volume_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture3D> volume;
        if (FAILED(device->CreateTexture3D(&volume_description, data.data(), &volume)) ||
            FAILED(device->CreateShaderResourceView(volume.Get(), nullptr, &entry.view))) return nullptr;
    } else if (FAILED(device->CreateTexture2D(&description, data.data(), &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &entry.view))) return nullptr;
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

extern "C" int nv2a_gpu_draw(const Nv2aGpuDraw *state, const Nv2aGpuVertex *vertices, uint32_t count)
{
    GpuTimer draw_timer(gpu_timing.draw);
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
    bool incompatible = false;
    for (const auto &surface : surfaces) {
        uintptr_t begin = (uintptr_t)state->color, end = begin + (size_t)state->pitch * state->height;
        uintptr_t old_begin = (uintptr_t)surface.memory, old_end = old_begin + (size_t)surface.pitch * surface.height;
        if (begin < old_end && end > old_begin &&
            (surface.memory != state->color || surface.width != state->width || surface.height != state->height || surface.pitch != state->pitch)) incompatible = true;
    }
    for (const auto &surface : depth_surfaces)
        if (surface.memory == state->depth && (surface.width != state->width || surface.height != state->height || surface.pitch != state->depth_pitch || surface.format != state->depth_format)) incompatible = true;
    if (incompatible || surfaces.size() >= 64 || depth_surfaces.size() >= 64) nv2a_gpu_invalidate();
    Surface *surface = get_surface(*state);
    if (!surface) return 0;
    DepthSurface *depth = state->depth_enable || state->stencil_enable ? get_depth(*state) : nullptr;
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
                if (!nv_cpu_finite(vertices[vertex].texture[stage][component])) return reject("nonfinite texture coordinate");
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
    uint32_t buffer_index = 0, capacity = 1;
    while (capacity < count) { capacity *= 2; buffer_index++; }
    UINT vertex_stride = state->vertex_program ? sizeof(Nv2aGpuVertex) : sizeof(Nv2aGpuVertex) - sizeof(vertices[0].attributes);
    auto &vertex_buffer = vertex_buffers[state->vertex_program ? 1 : 0][buffer_index];
    if (!vertex_buffer) {
        D3D11_BUFFER_DESC buffer = {};
        buffer.ByteWidth = capacity * vertex_stride; buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &vertex_buffer))) return 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(vertex_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return 0;
    if (state->vertex_program) std::memcpy(mapped.pData, vertices, count * vertex_stride);
    else for (uint32_t vertex = 0; vertex < count; vertex++)
        std::memcpy((uint8_t *)mapped.pData + vertex * vertex_stride, &vertices[vertex], vertex_stride);
    context->Unmap(vertex_buffer.Get(), 0);
    if (state->indices) {
        uint32_t index_bucket = 0, index_capacity = 1;
        while (index_capacity < state->index_count) { index_capacity *= 2; index_bucket++; }
        auto &index_buffer = index_buffers[index_bucket];
        if (!index_buffer) {
            D3D11_BUFFER_DESC buffer = {};
            buffer.ByteWidth = index_capacity * sizeof(uint32_t); buffer.Usage = D3D11_USAGE_DYNAMIC;
            buffer.BindFlags = D3D11_BIND_INDEX_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&buffer, nullptr, &index_buffer))) return reject("index buffer creation");
        }
        if (FAILED(context->Map(index_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return reject("index buffer upload");
        std::memcpy(mapped.pData, state->indices, state->index_count * sizeof(uint32_t)); context->Unmap(index_buffer.Get(), 0);
        context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    } else context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
    {
        GpuTimer constants_timer(gpu_timing.constants);
        if (FAILED(context->Map(constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return 0;
        std::memcpy(mapped.pData, &constants, sizeof constants); context->Unmap(constant_buffer.Get(), 0);
        gpu_timing.constant_uploads++;
    }
    bool uncombined = !(state->combiners.control & 255u) && !state->combiners.final_input[0] && !state->combiners.final_input[1];
    for (uint32_t stage = 0; stage < 4; stage++)
        if (((state->stage_program >> (stage * 5)) & 31u) == 5u ||
            ((uint32_t)constants.texture_info[stage][3] & 16u) ||
            constants.texture_key[stage][2] == 3u) uncombined = false;
    ID3D11PixelShader *program = uncombined ? uncombined_shader.Get() : get_pixel_shader(*state, constants.window_clip_control[0]);
    if (!program) return 0;
    VertexVariant *guest_vertex = state->vertex_program ? get_vertex_shader(*state) : nullptr;
    if (state->vertex_program && !guest_vertex) return reject("unsupported vertex program");
    UINT stride = vertex_stride, offset = 0;
    ID3D11Buffer *buffers[] = {vertex_buffer.Get()}, *constants_buffer[] = {constant_buffer.Get()};
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
    if (state->indices) context->DrawIndexed(state->index_count, 0, 0);
    else context->Draw(count, 0);
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
    if (pixel_shader && uncombined_shader && geometry_shader && line_geometry_shader && point_geometry_shader) return 1;
    if (!nv2a_gpu_available()) return 0;
    ComPtr<ID3DBlob> pixel_code, errors;
    HRESULT result = D3DCompile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr, nullptr,
                               "vs_main", "vs_5_0", D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_IEEE_STRICTNESS, 0, &vertex_code, &errors);
    if (SUCCEEDED(result)) {
        errors.Reset();
        result = D3DCompile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr, nullptr,
                            "ps_main", "ps_5_0", D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_IEEE_STRICTNESS, 0, &pixel_code, &errors);
    }
    if (FAILED(result)) {
        std::fprintf(stderr, "[GPU-D3D11] shader compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return 0;
    }
    if (FAILED(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr, &vertex_shader)) ||
        FAILED(device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &pixel_shader))) return 0;
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