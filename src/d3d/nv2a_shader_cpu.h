#ifndef NV2A_SHADER_CPU_H
#define NV2A_SHADER_CPU_H

#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct Nv2aCpuVertex {
    float output[16][4];
    uint32_t failed_slot;
    int failed_constant;
} Nv2aCpuVertex;

static inline int nv_cpu_compare(uint32_t function, uint32_t incoming, uint32_t stored)
{
    switch (function) {
    case 0x0200: return 0;
    case 0x0201: return incoming < stored;
    case 0x0202: return incoming == stored;
    case 0x0203: return incoming <= stored;
    case 0x0204: return incoming > stored;
    case 0x0205: return incoming != stored;
    case 0x0206: return incoming >= stored;
    case 0x0207: return 1;
    default: return 0;
    }
}

static inline int nv_cpu_edge_inside(float edge, float delta_x, float delta_y, float area)
{
    if (area > 0.0f)
        return edge > 0.0f || (edge == 0.0f && (delta_y < 0.0f || (delta_y == 0.0f && delta_x > 0.0f)));
    return edge < 0.0f || (edge == 0.0f && (delta_y > 0.0f || (delta_y == 0.0f && delta_x < 0.0f)));
}

static inline float nv_cpu_clamp(float value, float low, float high)
{
    if (!(value >= low)) return low;
    return value > high ? high : value;
}

static inline int nv_cpu_finite(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return (bits & 0x7F800000u) != 0x7F800000u;
}

static inline int32_t nv_cpu_floor_coordinate(float value)
{
    int32_t truncated = (int32_t)value;
    return (float)truncated > value ? truncated - 1 : truncated;
}

static inline void nv_cpu_mask_write(float destination[4], const float source[4], uint32_t mask)
{
    uint32_t component;
    for (component = 0; component < 4; component++)
        if (mask & (8u >> component))
            destination[component] = source[component];
}

static inline int nv_cpu_vp_source(uint32_t mux, uint32_t temporary, uint32_t swizzle,
                                    uint32_t negate, uint32_t input, int constant,
                                    const float attributes[16][4], const float constants[192][4],
                                    const float temporaries[13][4], const Nv2aCpuVertex *vertex,
                                    float result[4])
{
    const float *source;
    uint32_t component;
    switch (mux) {
    case 1:
        if (temporary > 12) return 0;
        source = temporary == 12 ? vertex->output[0] : temporaries[temporary];
        break;
    case 2: source = attributes[input]; break;
    case 3:
        if (constant < 0 || constant >= 192) return 0;
        source = constants[constant];
        break;
    default: return 0;
    }
    for (component = 0; component < 4; component++) {
        uint32_t selected = (swizzle >> (6u - component * 2u)) & 3u;
        result[component] = negate ? -source[selected] : source[selected];
    }
    return 1;
}

static inline float nv_cpu_multiply(float left, float right)
{
    return left == 0.0f || right == 0.0f ? 0.0f : left * right;
}

static inline void nv_cpu_unpack_normal(uint32_t packed, float result[4])
{
    uint32_t component;
    for (component = 0; component < 3; component++) {
        uint32_t bits = component == 2 ? 10u : 11u;
        uint32_t field = (packed >> (component * 11u)) & ((1u << bits) - 1u);
        int32_t signed_value = (int32_t)field;
        if (field & (1u << (bits - 1u))) signed_value -= (int32_t)(1u << bits);
        result[component] = (float)signed_value / (float)((1u << (bits - 1u)) - 1u);
    }
    result[3] = 1.0f;
}

static inline uint32_t nv_cpu_vp_operand_usage(uint32_t mac, uint32_t ilu)
{
    return (mac != 0 ? 1u : 0u) |
           (mac == 2 || (mac >= 4 && mac <= 12) ? 2u : 0u) |
           (mac == 3 || mac == 4 || ilu != 0 ? 4u : 0u);
}

static inline uint32_t nv_cpu_vertex_input_mask(const uint32_t program[136][4],
                                                const uint32_t valid[136], uint32_t start)
{
    uint32_t mask = 0, slot;
    for (slot = start; slot < 136; slot++) {
        const uint32_t *words = program[slot];
        uint32_t usage = nv_cpu_vp_operand_usage((words[1] >> 21) & 15u,
                                                (words[1] >> 25) & 7u);
        if (((usage & 1u) && ((words[2] >> 26) & 3u) == 2u) ||
            ((usage & 2u) && ((words[2] >> 11) & 3u) == 2u) ||
            ((usage & 4u) && ((words[3] >> 28) & 3u) == 2u))
            mask |= 1u << ((words[1] >> 9) & 15u);
        if (valid[slot] == 15 && (words[3] & 1u)) break;
    }
    return mask;
}

static inline int nv_cpu_vertex_execute(const uint32_t program[136][4],
                                        const uint32_t valid[136], uint32_t start,
                                        const float attributes[16][4], float constants[192][4],
                                        Nv2aCpuVertex *vertex)
{
    float temporaries[13][4] = {{0}};
    int address = 0;
    uint32_t slot, component;
    memset(vertex, 0, sizeof(*vertex));
    vertex->failed_slot = start;
    for (component = 0; component < 16; component++)
        vertex->output[component][3] = 1.0f;
    for (slot = start; slot < 136; slot++) {
        const uint32_t *words = program[slot];
        uint32_t mac = (words[1] >> 21) & 15u, ilu = (words[1] >> 25) & 7u;
        uint32_t input = (words[1] >> 9) & 15u;
        uint32_t destination = (words[3] >> 20) & 15u;
        uint32_t mac_mask = (words[3] >> 24) & 15u;
        uint32_t ilu_mask = (words[3] >> 16) & 15u;
        uint32_t output_mask = (words[3] >> 12) & 15u;
        uint32_t output_address = (words[3] >> 3) & 255u;
        float source_a[4] = {0}, source_b[4] = {0}, source_c[4] = {0};
        float mac_result[4] = {0}, ilu_result[4] = {0};
        float scalar = 0.0f;
        int constant = (int)((words[1] >> 13) & 255u);
        uint32_t usage = nv_cpu_vp_operand_usage(mac, ilu);
        int use_a = (usage & 1u) != 0, use_b = (usage & 2u) != 0;
        int use_c = (usage & 4u) != 0;
        vertex->failed_slot = slot;
        if (valid[slot] != 15 || mac > 13) return 0;
        if (words[3] & 2u) constant += address;
        vertex->failed_constant = constant;
        if (use_a && !nv_cpu_vp_source((words[2] >> 26) & 3u, words[2] >> 28,
                 words[1] & 255u, (words[1] >> 8) & 1u, input, constant,
                 attributes, constants, temporaries, vertex, source_a)) return 0;
        if (use_b && !nv_cpu_vp_source((words[2] >> 11) & 3u, (words[2] >> 13) & 15u,
                 (words[2] >> 17) & 255u, (words[2] >> 25) & 1u, input, constant,
                 attributes, constants, temporaries, vertex, source_b)) return 0;
        if (use_c && !nv_cpu_vp_source((words[3] >> 28) & 3u,
                 ((words[2] & 3u) << 2) | (words[3] >> 30),
                 (words[2] >> 2) & 255u, (words[2] >> 10) & 1u, input, constant,
                 attributes, constants, temporaries, vertex, source_c)) return 0;
        if (mac >= 5 && mac <= 7) {
            uint32_t count = mac == 7 ? 4u : 3u;
            for (component = 0; component < count; component++)
                scalar += nv_cpu_multiply(source_a[component], source_b[component]);
            if (mac == 6) scalar += source_b[3];
        }
        for (component = 0; component < 4; component++) {
            switch (mac) {
            case 0: break;
            case 1: mac_result[component] = source_a[component]; break;
            case 2: mac_result[component] = nv_cpu_multiply(source_a[component], source_b[component]); break;
            case 3: mac_result[component] = source_a[component] + source_c[component]; break;
            case 4: mac_result[component] = nv_cpu_multiply(source_a[component], source_b[component]) + source_c[component]; break;
            case 5: case 6: case 7: mac_result[component] = scalar; break;
            case 8:
                mac_result[component] = component == 0 ? 1.0f : component == 1 ?
                    nv_cpu_multiply(source_a[1], source_b[1]) : component == 2 ? source_a[2] : source_b[3];
                break;
            case 9: mac_result[component] = fminf(source_a[component], source_b[component]); break;
            case 10: mac_result[component] = fmaxf(source_a[component], source_b[component]); break;
            case 11: mac_result[component] = source_a[component] < source_b[component] ? 1.0f : 0.0f; break;
            case 12: mac_result[component] = source_a[component] >= source_b[component] ? 1.0f : 0.0f; break;
            case 13: break;
            }
        }
        scalar = source_c[0];
        switch (ilu) {
        case 0: break;
        case 1: memcpy(ilu_result, source_c, sizeof ilu_result); break;
        case 2: case 3: case 4:
            scalar = ilu == 4 ? 1.0f / sqrtf(fabsf(scalar)) : 1.0f / scalar;
            if (ilu == 3)
                scalar = copysignf(nv_cpu_clamp(fabsf(scalar), 0x1p-64f, 0x1p64f), scalar);
            for (component = 0; component < 4; component++) ilu_result[component] = scalar;
            break;
        case 5:
            ilu_result[0] = exp2f(floorf(scalar));
            ilu_result[1] = scalar - floorf(scalar);
            ilu_result[2] = exp2f(scalar);
            ilu_result[3] = 1.0f;
            break;
        case 6:
            scalar = fabsf(scalar);
            ilu_result[2] = log2f(scalar);
            ilu_result[0] = floorf(ilu_result[2]);
            ilu_result[1] = scalar == 0.0f ? 1.0f : scalar / exp2f(ilu_result[0]);
            ilu_result[3] = 1.0f;
            break;
        case 7:
            ilu_result[0] = ilu_result[3] = 1.0f;
            ilu_result[1] = fmaxf(source_c[0], 0.0f);
            ilu_result[2] = source_c[0] > 0.0f ?
                powf(fmaxf(source_c[1], 0.0f), nv_cpu_clamp(source_c[3], -127.99609375f, 127.99609375f)) : 0.0f;
            break;
        }
        if (output_mask) {
            int selected_ilu = (words[3] & 4u) != 0;
            const float *result = selected_ilu ? ilu_result : mac_result;
            if ((selected_ilu ? ilu : mac) != 0) {
                if (words[3] & 0x800u) {
                    uint32_t output = output_address & 15u;
                    if (output == 15) {
                        if (!isfinite(result[0]) || fabsf(result[0]) > 32767.0f) return 0;
                        address = (int)floorf(result[0]);
                    }
                    else if (output == 5) {
                        for (component = 0; component < 4; component++)
                            if (output_mask & (8u >> component)) {
                                vertex->output[5][0] = result[component];
                                break;
                            }
                    } else nv_cpu_mask_write(vertex->output[output], result, output_mask);
                } else {
                    if (output_address >= 192) return 0;
                    nv_cpu_mask_write(constants[output_address], result, output_mask);
                }
            }
        }
        if (mac == 13) {
            if (!isfinite(source_a[0]) || fabsf(source_a[0]) > 32767.0f) return 0;
            address = (int)floorf(source_a[0]);
        } else if (mac && mac_mask && !(ilu && destination == 1)) {
            if (destination > 12) return 0;
            nv_cpu_mask_write(destination == 12 ? vertex->output[0] : temporaries[destination], mac_result, mac_mask);
        }
        if (ilu && ilu_mask) {
            uint32_t ilu_destination = mac ? 1u : destination;
            if (ilu_destination > 12) return 0;
            nv_cpu_mask_write(ilu_destination == 12 ? vertex->output[0] : temporaries[ilu_destination], ilu_result, ilu_mask);
        }
        if (words[3] & 1u) return 1;
    }
    return 0;
}

typedef struct Nv2aCpuCombiners {
    uint32_t control, rgb_input[8], alpha_input[8], rgb_output[8], alpha_output[8];
    uint32_t final_input[2];
    float factors[9][2][4], fog_color[4];
} Nv2aCpuCombiners;

static inline void nv_cpu_unpack_argb(uint32_t color, float result[4])
{
    result[0] = (float)((color >> 16) & 255u) / 255.0f;
    result[1] = (float)((color >> 8) & 255u) / 255.0f;
    result[2] = (float)(color & 255u) / 255.0f;
    result[3] = (float)(color >> 24) / 255.0f;
}

static inline uint32_t nv_cpu_pack_argb(const float color[4])
{
    return ((uint32_t)(nv_cpu_clamp(color[3], 0.0f, 1.0f) * 255.0f + 0.5f) << 24)
         | ((uint32_t)(nv_cpu_clamp(color[0], 0.0f, 1.0f) * 255.0f + 0.5f) << 16)
         | ((uint32_t)(nv_cpu_clamp(color[1], 0.0f, 1.0f) * 255.0f + 0.5f) << 8)
         | (uint32_t)(nv_cpu_clamp(color[2], 0.0f, 1.0f) * 255.0f + 0.5f);
}

static inline float nv_cpu_combiner_input(const float registers[16][4], uint32_t input,
                                           uint32_t component, int alpha_path)
{
    float value = registers[input & 15u][(input & 16u) ? 3u : alpha_path ? 2u : component];
    float positive = value > 0.0f ? value : 0.0f;
    switch ((input >> 5) & 7u) {
    case 0: return positive;
    case 1: return 1.0f - nv_cpu_clamp(value, 0.0f, 1.0f);
    case 2: return 2.0f * positive - 1.0f;
    case 3: return 1.0f - 2.0f * positive;
    case 4: return positive - 0.5f;
    case 5: return 0.5f - positive;
    case 6: return value;
    default: return -value;
    }
}

static inline float nv_cpu_combiner_output(float value, uint32_t output)
{
    switch ((output >> 15) & 7u) {
    case 1: value -= 0.5f; break;
    case 2: value *= 2.0f; break;
    case 3: value = (value - 0.5f) * 2.0f; break;
    case 4: value *= 4.0f; break;
    case 6: value *= 0.5f; break;
    default: break;
    }
    return nv_cpu_clamp(value, -1.0f, 1.0f);
}

static inline void nv_cpu_combiner_products(const float registers[16][4], uint32_t input,
                                             uint32_t output, int alpha_path, int mux_select,
                                             float products[3][4])
{
    uint32_t component, count = alpha_path ? 1u : 3u;
    float ab_dot = 0.0f, cd_dot = 0.0f;
    for (component = 0; component < count; component++) {
        float input_a = nv_cpu_combiner_input(registers, input >> 24, component, alpha_path);
        float input_b = nv_cpu_combiner_input(registers, input >> 16, component, alpha_path);
        float input_c = nv_cpu_combiner_input(registers, input >> 8, component, alpha_path);
        float input_d = nv_cpu_combiner_input(registers, input, component, alpha_path);
        products[0][component] = input_a * input_b;
        products[1][component] = input_c * input_d;
        ab_dot += products[0][component];
        cd_dot += products[1][component];
    }
    for (component = 0; component < count; component++) {
        float ab = !alpha_path && (output & 0x2000u) ? ab_dot : products[0][component];
        float cd = !alpha_path && (output & 0x1000u) ? cd_dot : products[1][component];
        float sum = output & 0x4000u ? (mux_select ? cd : ab) : ab + cd;
        products[0][component] = nv_cpu_combiner_output(ab, output);
        products[1][component] = nv_cpu_combiner_output(cd, output);
        products[2][component] = nv_cpu_combiner_output(sum, output);
    }
}

static inline int nv_cpu_combiner_store(float registers[16][4], uint32_t output,
                                        const float products[3][4], int alpha_path)
{
    uint32_t product, component;
    const uint32_t destinations[3] = {(output >> 4) & 15u, output & 15u, (output >> 8) & 15u};
    for (product = 0; product < 3; product++) {
        uint32_t destination = destinations[product];
        if (!destination) continue;
        if (!(destination == 4 || destination == 5 || (destination >= 8 && destination <= 13))) return 0;
        if (alpha_path) registers[destination][3] = products[product][0];
        else {
            for (component = 0; component < 3; component++)
                registers[destination][component] = products[product][component];
            if (product < 2 && (output & (product == 0 ? 0x80000u : 0x40000u)))
                registers[destination][3] = products[product][2];
        }
    }
    return 1;
}

static inline int nv_cpu_combiners_execute(const Nv2aCpuCombiners *state,
                                            const float diffuse[4], const float specular[4],
                                            const float textures[4][4], float fog, float result[4])
{
    float registers[16][4] = {{0}};
    uint32_t stage, component, count = state->control & 255u;
    if (count > 8) return 0;
    memcpy(registers[4], diffuse, sizeof registers[4]);
    memcpy(registers[5], specular, sizeof registers[5]);
    memcpy(registers[8], textures, sizeof(float) * 16);
    memcpy(registers[3], state->fog_color, sizeof registers[3]);
    registers[3][3] = nv_cpu_clamp(fog, 0.0f, 1.0f);
    registers[12][3] = textures[0][3];
    for (stage = 0; stage < count; stage++) {
        float rgb[3][4] = {{0}}, alpha[3][4] = {{0}};
        uint32_t rgb_map = (state->rgb_output[stage] >> 15) & 7u;
        uint32_t alpha_map = (state->alpha_output[stage] >> 15) & 7u;
        int mux_select = state->control & 0x100u ? registers[12][3] >= 0.5f :
            ((uint32_t)(nv_cpu_clamp(registers[12][3], 0.0f, 1.0f) * 255.0f) & 1u) != 0;
        if (rgb_map == 5 || rgb_map == 7 || alpha_map == 5 || alpha_map == 7) return 0;
        memcpy(registers[1], state->factors[state->control & 0x1000u ? stage : 0][0], sizeof registers[1]);
        memcpy(registers[2], state->factors[state->control & 0x10000u ? stage : 0][1], sizeof registers[2]);
        nv_cpu_combiner_products(registers, state->rgb_input[stage], state->rgb_output[stage], 0, mux_select, rgb);
        nv_cpu_combiner_products(registers, state->alpha_input[stage], state->alpha_output[stage], 1, mux_select, alpha);
        if (!nv_cpu_combiner_store(registers, state->rgb_output[stage], rgb, 0) ||
            !nv_cpu_combiner_store(registers, state->alpha_output[stage], alpha, 1)) return 0;
    }
    if (state->final_input[0] || state->final_input[1]) {
        uint32_t final_flags = state->final_input[1] & 255u;
        memcpy(registers[1], state->factors[8][0], sizeof registers[1]);
        memcpy(registers[2], state->factors[8][1], sizeof registers[2]);
        for (component = 0; component < 3; component++) {
            float spec = final_flags & 0x40u ? 1.0f - registers[5][component] : registers[5][component];
            float spare = final_flags & 0x20u ? 1.0f - registers[12][component] : registers[12][component];
            registers[14][component] = final_flags & 0x80u ? nv_cpu_clamp(spec + spare, 0.0f, 1.0f) : spec + spare;
            registers[15][component] =
                nv_cpu_combiner_input(registers, state->final_input[1] >> 24, component, 0) *
                nv_cpu_combiner_input(registers, state->final_input[1] >> 16, component, 0);
        }
        for (component = 0; component < 3; component++) {
            float input_a = nv_cpu_combiner_input(registers, state->final_input[0] >> 24, component, 0);
            float input_b = nv_cpu_combiner_input(registers, state->final_input[0] >> 16, component, 0);
            float input_c = nv_cpu_combiner_input(registers, state->final_input[0] >> 8, component, 0);
            float input_d = nv_cpu_combiner_input(registers, state->final_input[0], component, 0);
            result[component] = nv_cpu_clamp(input_a * input_b + (1.0f - input_a) * input_c + input_d, 0.0f, 1.0f);
        }
        result[3] = nv_cpu_clamp(nv_cpu_combiner_input(registers, state->final_input[1] >> 8, 0, 1), 0.0f, 1.0f);
    } else memcpy(result, registers[count ? 12 : 4], sizeof(float) * 4);
    return 1;
}

static inline int nv_cpu_blend_factor(uint32_t factor, uint32_t source, uint32_t destination,
                                      uint32_t constant, uint32_t shift)
{
    switch (factor) {
    case 0: return 0;
    case 1: return 255;
    case 0x0300: return (source >> shift) & 255u;
    case 0x0301: return 255 - ((source >> shift) & 255u);
    case 0x0302: return source >> 24;
    case 0x0303: return 255 - (source >> 24);
    case 0x0304: return destination >> 24;
    case 0x0305: return 255 - (destination >> 24);
    case 0x0306: return (destination >> shift) & 255u;
    case 0x0307: return 255 - ((destination >> shift) & 255u);
    case 0x0308:
        return shift == 24 ? 255 : (int)fminf((float)(source >> 24), (float)(255 - (destination >> 24)));
    case 0x8001: return (constant >> shift) & 255u;
    case 0x8002: return 255 - ((constant >> shift) & 255u);
    case 0x8003: return constant >> 24;
    case 0x8004: return 255 - (constant >> 24);
    default: return -1;
    }
}

static inline int nv_cpu_blend(uint32_t source, uint32_t destination, uint32_t source_factor,
                               uint32_t destination_factor, uint32_t equation, uint32_t constant,
                               uint32_t *result)
{
    uint32_t shift;
    *result = 0;
    for (shift = 0; shift < 32; shift += 8) {
        int source_value = (source >> shift) & 255u, destination_value = (destination >> shift) & 255u;
        int source_scale = nv_cpu_blend_factor(source_factor, source, destination, constant, shift);
        int destination_scale = nv_cpu_blend_factor(destination_factor, source, destination, constant, shift);
        int value;
        if (source_scale < 0 || destination_scale < 0) return 0;
        switch (equation) {
        case 0x8006: value = (source_value * source_scale + destination_value * destination_scale + 127) / 255; break;
        case 0x800A: value = (source_value * source_scale - destination_value * destination_scale + 127) / 255; break;
        case 0x800B: value = (destination_value * destination_scale - source_value * source_scale + 127) / 255; break;
        case 0x8007: value = source_value < destination_value ? source_value : destination_value; break;
        case 0x8008: value = source_value > destination_value ? source_value : destination_value; break;
        default: return 0;
        }
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        *result |= (uint32_t)value << shift;
    }
    return 1;
}

static inline int nv_cpu_combiner_is_product(uint32_t input, uint32_t output,
                                            uint32_t source_a, uint32_t source_b)
{
    uint32_t input_a = input >> 24, input_b = (input >> 16) & 255u;
    uint32_t input_c = (input >> 8) & 255u, input_d = input & 255u;
    uint32_t ab = (output >> 4) & 15u, cd = output & 15u, sum = (output >> 8) & 15u;
    int ab_matches = (input_a == source_a && input_b == source_b) ||
                     (input_a == source_b && input_b == source_a);
    int cd_matches = (input_c == source_a && input_d == source_b) ||
                     (input_c == source_b && input_d == source_a);
    if ((output & 0xFFFFF000u) || (sum && sum != 12)) return 0;
    if (ab_matches && (!input_c || !input_d))
        return sum == 12 || (ab == 12 && cd != 12);
    if (cd_matches && (!input_a || !input_b))
        return sum == 12 || cd == 12;
    return 0;
}

static inline int nv_cpu_combiner_fast_mode(const Nv2aCpuCombiners *state)
{
    uint32_t rgb = state->final_input[0], alpha = (state->final_input[1] >> 8) & 255u;
    if ((state->control & 255u) != 1 || alpha != 0x1Cu) return 0;
    if (rgb != 0x0000000Cu && rgb != 0x00000C00u) return 0;
    if (nv_cpu_combiner_is_product(state->rgb_input[0], state->rgb_output[0], 8, 0x20) &&
        (state->alpha_output[0] == 0 ||
         nv_cpu_combiner_is_product(state->alpha_input[0], state->alpha_output[0], 0x18, 0x20))) return 1;
    if (nv_cpu_combiner_is_product(state->rgb_input[0], state->rgb_output[0], 8, 0x20) &&
        nv_cpu_combiner_is_product(state->alpha_input[0], state->alpha_output[0], 0x14, 0x20)) return 3;
    if (nv_cpu_combiner_is_product(state->rgb_input[0], state->rgb_output[0], 8, 4) &&
        nv_cpu_combiner_is_product(state->alpha_input[0], state->alpha_output[0], 0x18, 0x14)) return 2;
    if (nv_cpu_combiner_is_product(state->rgb_input[0], state->rgb_output[0], 8, 4) &&
        nv_cpu_combiner_is_product(state->alpha_input[0], state->alpha_output[0], 0x14, 0x20)) return 4;
    if (nv_cpu_combiner_is_product(state->rgb_input[0], state->rgb_output[0], 8, 4) &&
        (state->alpha_output[0] == 0 ||
         nv_cpu_combiner_is_product(state->alpha_input[0], state->alpha_output[0], 0x18, 0x20))) return 5;
    return 0;
}

#endif