#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>
#include "kernel/nv2a_gpu.h"
#include "kernel/xbox_memory_layout.h"
#include "d3d/d3d8_swizzle.h"

void nv2a_pb_exec_method(uint32_t subchannel, uint32_t method, uint32_t parameter);
void nv2a_pb_scan(uint32_t start_address, uint32_t end_address);

static uint32_t memory[32768];
static uint32_t nv2a_registers[0x1000000 / sizeof(uint32_t)];
static int failures;
uint32_t g_xbox_image_lo, g_xbox_image_hi;
size_t g_xbox_total_ram = sizeof memory;
size_t g_xbox_map_size;

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)memory; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
uint32_t xbox_ContiguousBlockSize(uint32_t guest_va)
{
    (void)guest_va;
    return 0; /* The harness has no contiguous allocations. */
}

uint8_t *xbox_DmaPhysicalPointer(uint64_t physical, uint32_t bytes)
{
    if (physical >= sizeof memory || bytes > sizeof memory - physical)
        return NULL;
    return (uint8_t *)memory + (size_t)physical;
}

volatile uint32_t *xbox_Nv2aRegisterPointer(uint32_t offset, uint32_t bytes)
{
    if ((offset & 3u) || offset >= sizeof nv2a_registers
            || bytes > sizeof nv2a_registers - offset)
        return NULL;
    return nv2a_registers + offset / sizeof(uint32_t);
}

int xbox_Nv2aSoftwareMethod(uint32_t parameter, uint32_t depth_clear,
                            uint32_t color_clear)
{
    fprintf(stderr, "Unexpected guest software method in shader harness: "
            "0x%08X (depth=0x%08X, color=0x%08X)\n",
            parameter, depth_clear, color_clear);
    exit(EXIT_FAILURE);
}

void xbox_Nv2aSoftwareMethodReport(void)
{
    /* No guest software callbacks are registered by this harness. */
}
void xbox_FramebufferWindowSet(uint32_t address, uint32_t pitch) { (void)address; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowPresent(uint32_t address, uint32_t pitch) { (void)address; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}

typedef struct TestVertex {
    float position[4], diffuse[4], texture0[4], texture1[4];
} TestVertex;

static void check(const char *name, int condition)
{
    if (!condition) { printf("FAIL: %s\n", name); failures++; }
}

static void method(uint32_t address, uint32_t parameter)
{
    nv2a_pb_exec_method(0, address, parameter);
}

static void float_method(uint32_t address, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    method(address, bits);
}

static void upload_program(void)
{
    const uint32_t program[4][4] = {
        {0, (3u << 21) | 0x1Bu, (2u << 26) | (0x1Bu << 2), (3u << 28) | 0xF800u},
        {0, (1u << 21) | (3u << 9) | 0x1Bu, 2u << 26, 0xF800u | (3u << 3)},
        {0, (1u << 21) | (9u << 9) | 0x1Bu, 2u << 26, 0xF800u | (9u << 3)},
        {0, (1u << 21) | (10u << 9) | 0x1Bu, 2u << 26, 0xF800u | (10u << 3) | 1u}
    };
    uint32_t instruction, component;
    method(0x1E9C, 0);
    for (instruction = 0; instruction < 4; instruction++)
        for (component = 0; component < 4; component++)
            method(0x0B00 + component * 4, program[instruction][component]);
    method(0x1EA0, 0);
    method(0x1E94, 6);
    method(0x1EA4, 0);
    float_method(0x0B80, 2);
    float_method(0x0B84, 2);
    float_method(0x0B88, 0);
    float_method(0x0B8C, 0);
}

static void bind_texture(uint32_t stage, uint32_t address, uint32_t color)
{
    uint32_t base = 0x1B00 + stage * 64;
    memory[address / 4] = color;
    method(base, address);
    method(base + 4, 0x12u << 8);
    method(base + 8, 0x303);
    method(base + 12, 1u << 30);
    method(base + 16, 4u << 16);
    method(base + 28, 0x00010001);
}

static void draw(uint32_t primitive, uint32_t count)
{
    method(0x17FC, primitive);
    method(0x1810, (count - 1) << 24);
    method(0x17FC, 0);
    {
        extern void nv2a_pb_exec_flush(void);
        nv2a_pb_exec_flush();
    }
}

static int invalid_pushbuffer_case(const char *name)
{
    const uint32_t address = 0x18000;
    uint32_t *packet = memory + address / 4;
    if (!strcmp(name, "--pb-discontinuous")) {
        packet[0] = (2u << 18) | 0x1D90;
        nv2a_pb_scan(address, address + 4);
        nv2a_pb_scan(address + 8, address + 12);
    } else if (!strcmp(name, "--pb-invalid-jump")) {
        packet[0] = 0x1FFFFFFD;
        nv2a_pb_scan(address, address + 4);
    } else if (!strcmp(name, "--pb-cycle")) {
        packet[0] = address | 1u;
        nv2a_pb_scan(address, address + 4);
    } else if (!strcmp(name, "--pb-nested-call")) {
        packet[0] = (address + 8) | 2u;
        packet[2] = (address + 16) | 2u;
        nv2a_pb_scan(address, address + 4);
    } else if (!strcmp(name, "--pb-invalid-return")) {
        packet[0] = 0x00020000;
        nv2a_pb_scan(address, address + 4);
    } else return 2;
    return 0;
}

static void test_invalid_pushbuffer_packets(void)
{
    static const char *cases[] = {"--pb-discontinuous", "--pb-invalid-jump", "--pb-cycle", "--pb-nested-call", "--pb-invalid-return"};
    char *executable = NULL;
    size_t index;
    if (_get_pgmptr(&executable) || !executable) {
        check("pushbuffer negative test executable path", 0);
        return;
    }
    for (index = 0; index < sizeof cases / sizeof cases[0]; index++)
        check(cases[index], _spawnl(_P_WAIT, executable, executable, cases[index], NULL) == EXIT_FAILURE);
}

static void test_split_pushbuffer_packet(void)
{
    const uint32_t address = 0x18000;
    uint32_t *packet = memory + address / 4;
    uint32_t split, index;
    method(0x0200, 8u << 16); method(0x0204, 8u << 16);
    method(0x0208, 0x28); method(0x020C, 32u | (32u << 16));
    method(0x0210, 0x1000); method(0x0214, 0x2000);
    packet[0] = (3u << 18) | 0x1D8C;
    packet[1] = 0xFFFFFFA6; packet[2] = 0xFFA1B2C3; packet[3] = 0xF0;
    for (split = 1; split < 4; split++) {
        for (index = 0; index < 64; index++) memory[0x1000 / 4 + index] = 0x11223344;
        nv2a_pb_scan(address, address + split * 4);
        check("partial method packet does not clear prematurely", memory[0x1000 / 4] == 0x11223344);
        nv2a_pb_scan(address + split * 4, address + 16);
        check("split incrementing packet preserves parameters", memory[0x1000 / 4] == 0xFFA1B2C3 && memory[0x1000 / 4 + 63] == 0xFFA1B2C3);
    }
    packet[0] = 0x40000000u | (2u << 18) | 0x1D90;
    packet[1] = 0xFF123456; packet[2] = 0xFFABCDEF;
    packet[3] = (1u << 18) | 0x1D94; packet[4] = 0xF0;
    nv2a_pb_scan(address, address + 4);
    nv2a_pb_scan(address + 4, address + 8);
    nv2a_pb_scan(address + 8, address + 20);
    check("split nonincrementing packet retains method", memory[0x1000 / 4] == 0xFFABCDEF && memory[0x1000 / 4 + 63] == 0xFFABCDEF);
    for (index = 0; index < 2; index++) {
        uint32_t *tail = packet + 16;
        tail[0] = (3u << 18) | 0x1D8C;
        tail[1] = 0xFFFFFFA6; tail[2] = 0xFF102030; tail[3] = 0xF0;
        tail[4] = index ? 0x20000000u | address : address | 1u;
        packet[0] = (1u << 18) | 0x1D90; packet[1] = 0xFF506070;
        packet[2] = (1u << 18) | 0x1D94; packet[3] = 0xF0;
        nv2a_pb_scan(address + 64, address + 72);
        nv2a_pb_scan(address + 72, address + 16);
        check("ring jump completes tail packet and wrapped head", memory[0x1000 / 4] == 0xFF506070 && memory[0x1000 / 4 + 63] == 0xFF506070);
    }
    packet[0] = (address + 64) | 2u;
    packet[1] = (1u << 18) | 0x1D90; packet[2] = 0xFFABCDEF;
    packet[3] = (1u << 18) | 0x1D94; packet[4] = 0xF0;
    packet[16] = (1u << 18) | 0x1D90; packet[17] = 0xFF102030;
    packet[18] = (1u << 18) | 0x1D94; packet[19] = 0xF0; packet[20] = 0x00020000;
    nv2a_pb_scan(address, address + 4);
    check("DMA call executes subroutine before reaching caller PUT", memory[0x1000 / 4] == 0xFF102030);
    nv2a_pb_scan(address + 4, address + 20);
    check("DMA return resumes caller packet", memory[0x1000 / 4] == 0xFFABCDEF);
}

static uint32_t pixel(uint32_t horizontal, uint32_t vertical)
{
    return memory[0x1000 / 4 + vertical * 8 + horizontal];
}

static uint32_t gpu_decode_calls;

static int gpu_decode(void *context, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    (void)horizontal; (void)vertical;
    gpu_decode_calls++;
    memcpy(color, context, sizeof *color);
    return 1;
}

static void test_gpu_depth_minimal(void)
{
    uint32_t color[64], depth[64], iteration, vertex;
    uint32_t saved_color[64], saved_depth[64];
    uint32_t texel = 0xFFFF0000;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 32; state.bytes_per_pixel = 4;
    state.depth_format = 2; state.depth_enable = state.depth_mask = 1; state.depth_function = 0x201;
    state.color_mask = 0x01010101;
    state.textures[0].source = (const uint8_t *)&texel; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].pitch = 4; state.textures[0].linear = 1;
    state.textures[0].decode_context = &texel; state.textures[0].decode = gpu_decode;
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 100; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = 1; vertices[vertex].diffuse[3] = 1;
        vertices[vertex].texture[0][3] = 1;
    }
    for (iteration = 0; iteration < 20; iteration++) {
        uint32_t pixel;
        uint32_t scenario = iteration % 4, path = iteration / 4;
        state.width = state.clip_width = 2u + iteration % 7;
        state.combiners.control = path ? 1 : 0; state.stage_program = path >= 2 ? 1 : 0;
        state.combiners.rgb_input[0] = path == 4 ? 0x08040000 : path >= 2 ? 0x08200000 : 0x04200000;
        state.combiners.final_input[0] = path >= 3 ? 0x0000000C : 0;
        state.combiners.final_input[1] = path >= 3 ? 0x00001C80 : 0;
        state.combiners.alpha_input[0] = 0x00002014;
        state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
        for (pixel = 0; pixel < 64; pixel++) { color[pixel] = 0xFF000000; depth[pixel] = 0xFFFFFF5A; }
        state.depth_format = scenario < 2 ? 2 : 1;
        state.control0 = scenario & 1 ? 0x10000 : 0;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 100;
            vertices[vertex].position[3] = scenario & 1 ? 100.0f : 1.0f;
        }
        if (path == 4) {
            state.depth_enable = 0;
            for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[2] = 0;
            check("minimal warm shader without depth", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            state.depth_enable = 1;
            for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[2] = 100;
        }
        check("minimal hardware depth draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("minimal hardware depth draw color", color[9] == 0xFFFF0000);
        if (state.depth_format == 2) {
            printf("depth path %u Z24/W%u: %08X\n", path, scenario & 1, depth[9]);
            check("minimal hardware Z24/W-depth", depth[9] == 0x0000645A);
        } else {
            uint16_t stored; memcpy(&stored, (uint8_t *)depth + 32 + 2, 2);
            printf("depth path %u Z16/W%u: %u\n", path, scenario & 1, stored);
            check("minimal hardware Z16/W-depth", stored == 100);
        }
                memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
                nv2a_gpu_flush(); nv2a_gpu_flush();
                check("unchanged repeated flush draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                check("unchanged refresh preserves complete color and pitched depth",
                            memcmp(saved_color, color, sizeof color) == 0 && memcmp(saved_depth, depth, sizeof depth) == 0);
        nv2a_gpu_flush();
        color[9] = 0xFF010203; color[63] = 0xFF123456;
        if (state.depth_format == 2) depth[9] = 0x000032A6;
        else { uint16_t near_depth = 50; memcpy((uint8_t *)depth + 34, &near_depth, 2); }
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 75;
            vertices[vertex].position[3] = scenario & 1 ? 75.0f : 1.0f;
        }
        check("retained surface observes CPU depth mutation", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("retained CPU color survives updated depth occlusion", color[9] == 0xFF010203 && color[63] == 0xFF123456);
        nv2a_gpu_flush();
        if (state.depth_format == 2) depth[9] = 0x000096A6;
        else { uint16_t far_depth = 150; memcpy((uint8_t *)depth + 34, &far_depth, 2); }
        check("retained surface observes subsequent CPU depth clear", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("retained refreshed depth admits nearer fragment", color[9] == 0xFFFF0000 && color[63] == 0xFF123456);
        if (state.depth_format == 2) check("retained Z24 refresh preserves CPU stencil", depth[9] == 0x00004BA6);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 34, 2); check("retained Z16 refresh writes guest depth", stored == 75); }
        nv2a_gpu_flush(); state.depth_mask = 0;
        color[9] = 0xFF010203;
        if (state.depth_format == 2) depth[9] = 0x000032A6;
        else { uint16_t near_depth = 50; memcpy((uint8_t *)depth + 34, &near_depth, 2); }
        check("read-only depth observes CPU mutation", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("read-only mutated depth occludes fragment", color[9] == 0xFF010203);
        nv2a_gpu_flush();
        if (state.depth_format == 2) depth[9] = 0x00004BA6;
        else { uint16_t restored_depth = 75; memcpy((uint8_t *)depth + 34, &restored_depth, 2); }
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 60;
            vertices[vertex].position[3] = scenario & 1 ? 60.0f : 1.0f;
        }
        check("read-only depth restored to old snapshot draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("old snapshot cannot hide a subsequent CPU depth change", color[9] == 0xFFFF0000 && color[63] == 0xFF123456);
        if (state.depth_format == 2) check("read-only depth retains restored stencil and depth", depth[9] == 0x00004BA6);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 34, 2); check("read-only Z16 depth remains restored", stored == 75); }
        state.depth_mask = 1;
        nv2a_gpu_flush();
        state.color_mask = iteration & 1 ? 0xFEFEFEFE : 0;
        check("color-masked depth draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[9] = 0xFF314159;
        memcpy(saved_color, color, sizeof color);
        nv2a_gpu_flush();
        check("color-masked draw does not publish stale color", memcmp(saved_color, color, sizeof color) == 0);
        if (state.depth_format == 2) check("color-masked draw still publishes Z24 depth", depth[9] == 0x00003CA6);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 34, 2); check("color-masked draw still publishes Z16 depth", stored == 60); }
          state.depth_mask = 0;
          check("draw with both output write masks disabled accepted", nv2a_gpu_draw(&state, vertices, 3));
          color[9] = 0xFF271828;
          memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
          nv2a_gpu_flush(); nv2a_gpu_flush();
          check("output-masked draw preserves complete guest color and depth",
              memcmp(saved_color, color, sizeof color) == 0 && memcmp(saved_depth, depth, sizeof depth) == 0);
          state.depth_mask = 1;
        state.color_mask = 0x01010101;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 45;
            vertices[vertex].position[3] = scenario & 1 ? 45.0f : 1.0f;
        }
        check("pending color write draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        state.color_mask = 0;
        check("masked draw after pending color write accepted", nv2a_gpu_draw(&state, vertices, 3));
        nv2a_gpu_flush();
        check("masked draw preserves pending color publication", color[9] == 0xFFFF0000);
        state.color_mask = 0x01010101;
        if (iteration & 1) nv2a_gpu_invalidate();
        state.clip_x = state.clip_y = state.clip_width = state.clip_height = 1;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 30;
            vertices[vertex].position[3] = scenario & 1 ? 30.0f : 1.0f;
        }
        check("one-pixel scissor depth draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[0] = 0xFF314159;
        color[9] = 0xFF010203;
        memcpy(saved_color, color, sizeof color); saved_color[9] = 0xFFFF0000;
        if (state.depth_format == 2) depth[0] = 0x000005B7;
        else { uint16_t outside_depth = 5; memcpy(depth, &outside_depth, 2); }
        memcpy(saved_depth, depth, sizeof depth);
        if (state.depth_format == 2) saved_depth[9] = 0x00001EA6;
        else { uint16_t inside_depth = 30; memcpy((uint8_t *)saved_depth + 34, &inside_depth, 2); }
        nv2a_gpu_flush();
        check("scissored color publication preserves all outside pixels", memcmp(saved_color, color, sizeof color) == 0);
        check("scissored depth publication preserves all outside pixels", memcmp(saved_depth, depth, sizeof depth) == 0);
        state.clip_x = state.clip_y = 0; state.clip_height = 8;
        state.clip_width = state.width;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 15;
            vertices[vertex].position[3] = scenario & 1 ? 15.0f : 1.0f;
        }
        check("CPU mutation outside scissor refreshes retained resources", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
        check("outside CPU depth mutation occludes later fragment", color[0] == 0xFF314159 && color[9] == 0xFFFF0000);
        state.clip_width = state.clip_height = 1;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 2;
            vertices[vertex].position[3] = scenario & 1 ? 2.0f : 1.0f;
        }
        check("first accumulated scissor draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        state.clip_x = state.clip_y = 1;
        check("second accumulated scissor draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[63] = 0xFF271828;
        color[1] = 0xFF173205;
        if (state.depth_format == 2) depth[1] = 0x000011CD;
        else { uint16_t gap_depth = 17; memcpy((uint8_t *)depth + 2, &gap_depth, 2); }
        nv2a_gpu_flush();
        check("accumulated scissors publish both color writes", color[0] == 0xFFFF0000 && color[9] == 0xFFFF0000 && color[63] == 0xFF271828);
        check("disjoint scissors preserve CPU color in their gap", color[1] == 0xFF173205);
        if (state.depth_format == 2) check("disjoint scissors preserve CPU Z24 and stencil in their gap", depth[1] == 0x000011CD);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 2, 2); check("disjoint scissors preserve CPU Z16 in their gap", stored == 17); }
        if (state.depth_format == 2) check("accumulated scissors publish both depth writes", depth[0] == 0x000002B7 && depth[9] == 0x000002A6);
        else {
            uint16_t first_depth, second_depth;
            memcpy(&first_depth, depth, 2); memcpy(&second_depth, (uint8_t *)depth + 34, 2);
            check("accumulated scissors publish both Z16 writes", first_depth == 2 && second_depth == 2);
        }
        state.clip_x = state.width + 1; state.clip_y = 0;
        check("scissor outside viewport draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[9] = 0xFF161803;
        memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
        nv2a_gpu_flush(); nv2a_gpu_flush();
        check("empty effective scissor preserves complete guest output",
              memcmp(saved_color, color, sizeof color) == 0 && memcmp(saved_depth, depth, sizeof depth) == 0);
        state.clip_x = state.clip_y = 0; state.clip_height = 8;
        nv2a_gpu_invalidate();
    }
}

static void test_gpu_scissor_coverage(void)
{
    uint32_t color[544], depth[544], expected_color[544], expected_depth[544];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t vertex, pixel, pass;
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = 67; state.height = 4; state.pitch = state.depth_pitch = 272;
    state.bytes_per_pixel = 4; state.depth_format = 2;
    state.color_mask = 0x01010101; state.depth_enable = state.depth_mask = 1; state.depth_function = 0x207;
    for (pixel = 0; pixel < 544; pixel++) { color[pixel] = 0xFF000000; depth[pixel] = 0xFFFFFFA5; }
    vertices[1].position[0] = vertices[2].position[1] = 128;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 3; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
    }
    for (pass = 0; pass < 3; pass++) {
        state.clip_x = 61; state.clip_y = 0; state.clip_width = 5; state.clip_height = 1;
        check("word-boundary scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
        state.clip_x = 3; state.clip_y = 1; state.clip_width = 1;
        check("non-word-aligned second row scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
        if (pass == 2) {
            state.clip_x = state.clip_y = 0; state.clip_width = 67; state.clip_height = 2;
            check("containing scissor replaces fragmented coverage", nv2a_gpu_draw(&state, vertices, 3));
        }
        color[60] = 0xFF314159; depth[60] = 0x000011CD;
        color[67] = 0xFF271828; depth[67] = 0x000012B7;
        memcpy(expected_color, color, sizeof color); memcpy(expected_depth, depth, sizeof depth);
        if (pass == 2) {
            uint32_t row;
            for (row = 0; row < 2; row++)
                for (pixel = 0; pixel < 67; pixel++) {
                    expected_color[row * 68 + pixel] = 0xFFFF0000;
                    expected_depth[row * 68 + pixel] = 0x00000300 | (expected_depth[row * 68 + pixel] & 255);
                }
        } else {
            for (pixel = 61; pixel < 66; pixel++) {
                expected_color[pixel] = 0xFFFF0000; expected_depth[pixel] = 0x000003A5;
            }
            expected_color[71] = 0xFFFF0000; expected_depth[71] = 0x000003A5;
        }
        nv2a_gpu_flush();
        check("coverage spans preserve exact color and row padding", memcmp(expected_color, color, sizeof color) == 0);
        check("coverage spans preserve exact depth stencil and row padding", memcmp(expected_depth, depth, sizeof depth) == 0);
    }
    nv2a_gpu_invalidate();
    state.width = 64; state.pitch = 256;
    state.depth = NULL; state.depth_enable = state.depth_mask = 0;
    state.clip_x = 0; state.clip_y = 1; state.clip_width = 64; state.clip_height = 2;
    for (pass = 0; pass < 3; pass++) {
        if (pass == 2) nv2a_gpu_invalidate();
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].diffuse[0] = pass == 1 ? 0 : 1;
            vertices[vertex].diffuse[1] = pass == 1 ? 1 : 0;
        }
        check("packed full-width partial-height scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[0] = 0xFF314159 + pass; color[255] = 0xFF271828 + pass;
        memcpy(expected_color, color, sizeof color);
        for (pixel = 64; pixel < 192; pixel++) expected_color[pixel] = pass == 1 ? 0xFF00FF00 : 0xFFFF0000;
        nv2a_gpu_flush();
        check("packed publication preserves outside rows and allocation tail", memcmp(expected_color, color, sizeof color) == 0);
    }
    nv2a_gpu_invalidate();
    state.width = state.clip_width = 128; state.pitch = 512;
    state.depth = (uint8_t *)depth; state.depth_enable = state.depth_mask = 1;
    vertices[1].position[0] = vertices[2].position[1] = 256;
    for (uint32_t format = 1; format <= 2; format++) {
        state.depth_format = format; state.depth_pitch = format == 1 ? 256 : 512;
        for (pixel = 0; pixel < 544; pixel++) {
            color[pixel] = 0xFF000000; depth[pixel] = 0xFFFFFF00 | (pixel & 255);
        }
        for (pass = 0; pass < 3; pass++) {
            if (pass == 2) nv2a_gpu_invalidate();
            for (vertex = 0; vertex < 3; vertex++) {
                vertices[vertex].position[2] = (float)(3 + pass);
                vertices[vertex].diffuse[0] = 1; vertices[vertex].diffuse[1] = 0;
            }
            check("packed depth partial-height scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
            color[0] = 0xFF314159 + pass; color[511] = 0xFF271828 + pass;
            if (format == 1) {
                ((uint16_t *)depth)[0] = (uint16_t)(123 + pass);
                ((uint16_t *)depth)[511] = (uint16_t)(456 + pass);
            } else {
                depth[0] = 0x000011CD + pass; depth[511] = 0x000012B7 + pass;
            }
            memcpy(expected_color, color, sizeof color); memcpy(expected_depth, depth, sizeof depth);
            for (pixel = 128; pixel < 384; pixel++) {
                expected_color[pixel] = 0xFFFF0000;
                if (format == 1) ((uint16_t *)expected_depth)[pixel] = (uint16_t)(3 + pass);
                else expected_depth[pixel] = ((3 + pass) << 8) | (expected_depth[pixel] & 255);
            }
            nv2a_gpu_flush();
            check("packed depth draw preserves outside color rows and tail", memcmp(expected_color, color, sizeof color) == 0);
            check("packed depth publication preserves stencil outside rows and tail", memcmp(expected_depth, depth, sizeof depth) == 0);
        }
        nv2a_gpu_invalidate();
    }
}

static int gpu_decode_cube(void *context, uint32_t face, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    (void)horizontal; (void)vertical;
    *color = ((const uint32_t *)context)[face * 32];
    return 1;
}

static int gpu_decode_grid(void *context, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    *color = ((const uint32_t *)context)[vertical * 4 + horizontal];
    return 1;
}

static void test_gpu_cube(void)
{
    static const float directions[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const uint32_t colors[6] = {0xFFFF0000,0xFF00FF00,0xFF0000FF,0xFFFFFF00,0xFF00FFFF,0xFFFF00FF};
    uint32_t color[64] = {0}, cube[192] = {0}, face, vertex;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.stage_program = 3; state.combiners.control = 1;
    state.combiners.rgb_input[0] = 0x08200000; state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    state.textures[0].source = (const uint8_t *)cube; state.textures[0].source_bytes = sizeof cube;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].format = 6;
    state.textures[0].cube = 1; state.textures[0].face_stride = 128;
    state.textures[0].decode_context = cube; state.textures[0].decode_face = gpu_decode_cube;
    for (face = 0; face < 6; face++) cube[face * 32] = colors[face];
    for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[3] = 1;
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    for (face = 0; face < 6; face++) {
        for (vertex = 0; vertex < 3; vertex++) memcpy(vertices[vertex].texture[0], directions[face], sizeof directions[face]);
        check("native cube face accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native cube face orientation and color", color[9] == colors[face]);
    }
    cube[5 * 32] = 0xFF804020;
    check("native mutated cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native cube cache observes source mutation", color[9] == 0xFF804020);
    nv2a_gpu_invalidate();
    {
        const float projective_coordinates[5][4] = {{1.5f,0,0,2},{0,4,0,2},{0,-4,0,2},{-4,0,0,2},{4,0,0,2}};
        const uint32_t expected_faces[5] = {0,2,3,4,5};
        state.stage_program = 1;
        for (face = 0; face < 5; face++) {
            for (vertex = 0; vertex < 3; vertex++) memcpy(vertices[vertex].texture[0], projective_coordinates[face], sizeof projective_coordinates[face]);
            check("projective coordinates on cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            check("projective cube coordinates divide by Q", color[9] == cube[expected_faces[face] * 32]);
        }
        state.stage_program = 3;
        nv2a_gpu_invalidate();
    }
    {
        const float projected_directions[6][3] = {{2,0.5f,-0.5f},{-2,0.5f,1},{0.5f,2,-1},
                                                  {1,-2,1},{0.5f,0.5f,2},{-1,0.5f,-2}};
        const uint32_t expected_texels[6] = {5,6,9,10,5,6};
        for (face = 0; face < 16; face++) cube[face] = 0xFF000000u | ((face + 1) * 0x00090703u);
        state.textures[0].cube = 0; state.textures[0].face_stride = 0;
        state.textures[0].width = state.textures[0].height = 4; state.textures[0].pitch = 16;
        state.textures[0].source_bytes = 64; state.textures[0].decode = gpu_decode_grid;
        for (face = 0; face < 6; face++) {
            for (vertex = 0; vertex < 3; vertex++) memcpy(vertices[vertex].texture[0], projected_directions[face], sizeof projected_directions[face]);
            check("cube coordinates on 2D texture accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            check("cube coordinates project to correct 2D texel", color[9] == cube[expected_texels[face]]);
        }
        cube[6] = 0x00804020; state.textures[0].format = 0x1E;
        check("projected XRGB texture accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("projected XRGB texture forces opaque alpha", color[9] == 0xFF804020);
        nv2a_gpu_invalidate();
        state.stage_program = 1; state.textures[0].format = 6;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].texture[0][0] = 1.375f; vertices[vertex].texture[0][1] = -0.375f;
            vertices[vertex].texture[0][3] = 1;
        }
        for (uint32_t repetition = 0; repetition < 2; repetition++) {
            for (uint32_t address_u = 1; address_u <= 3; address_u++) {
                for (uint32_t address_v = 1; address_v <= 3; address_v++) {
                    const uint32_t columns[3] = {1,2,3}, rows[3] = {2,1,0};
                    state.textures[0].address_u = address_u; state.textures[0].address_v = address_v;
                    check("cached sampler transition accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                    check("cached sampler preserves wrap mirror clamp", color[9] == cube[rows[address_v - 1] * 4 + columns[address_u - 1]]);
                }
            }
        }
        nv2a_gpu_invalidate();
    }
}

static void test_gpu_dxt(void)
{
    const uint32_t formats[3] = {0xC,0xE,0xF};
    uint32_t color[64] = {0}, decoded = 0xFFFF00FF, format, vertex;
    uint8_t blocks[768] = {0};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.stage_program = 1; state.combiners.control = 1;
    state.combiners.rgb_input[0] = 0x08200000; state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    state.textures[0].source = blocks; state.textures[0].width = state.textures[0].height = 4;
    state.textures[0].decode = gpu_decode; state.textures[0].decode_context = &decoded;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = vertices[vertex].texture[0][3] = 1;
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0.5f;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    for (format = 0; format < 3; format++) {
        uint32_t offset = format ? 8 : 0;
        memset(blocks, 0, sizeof blocks);
        if (format == 1) memset(blocks, 255, 8);
        if (format == 2) blocks[0] = blocks[1] = 255;
        blocks[offset + 1] = 0xF8;
        state.textures[0].format = formats[format]; state.textures[0].source_bytes = format ? 16 : 8;
        check("native DXT texture accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native DXT samples raw blocks without CPU expansion", color[9] == 0xFFFF0000);
        blocks[offset] = 0x1F; blocks[offset + 1] = 0;
        check("native mutated DXT accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native DXT source mutation updates pixels", color[9] == 0xFF0000FF);
    }
    memset(blocks, 0, sizeof blocks);
    for (format = 0; format < 6; format++) blocks[format * 128 + 1] = 0xF8;
    state.stage_program = 3; state.textures[0].format = 0xC;
    state.textures[0].source_bytes = sizeof blocks; state.textures[0].cube = 1; state.textures[0].face_stride = 128;
    state.textures[0].decode_face = gpu_decode_cube;
    state.textures[0].decode_context = blocks;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0;
        vertices[vertex].texture[0][2] = -1;
    }
    check("native DXT cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native DXT cube samples aligned raw face", color[9] == 0xFFFF0000);
    blocks[5 * 128] = 0x1F; blocks[5 * 128 + 1] = 0;
    check("native mutated DXT cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native DXT cube observes changed face", color[9] == 0xFF0000FF);
    nv2a_gpu_invalidate();
}

static void test_gpu_vertex(void)
{
    uint32_t color[64] = {0}, valid[136] = {15,15,15,15};
    uint32_t program[136][4] = {
        {0, (3u << 21) | 0x1Bu, (2u << 26) | (0x1Bu << 2), (3u << 28) | 0xF800u},
        {0, (1u << 21) | (3u << 9) | 0x1Bu, 2u << 26, 0xF800u | (3u << 3)},
        {0, (1u << 21) | (9u << 9) | 0x1Bu, 2u << 26, 0xF800u | (9u << 3)},
        {0, (1u << 21) | (10u << 9) | 0x1Bu, 2u << 26, 0xF800u | (10u << 3) | 1u}
    };
    float constants[192][4] = {{2,2,0,0}};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[4] = {0};
    uint32_t vertex;
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.vertex_program = program; state.vertex_valid = valid; state.vertex_constants = constants;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].attributes[0][3] = 1;
        vertices[vertex].attributes[3][0] = 0.5f; vertices[vertex].attributes[3][1] = 0.25f;
        vertices[vertex].attributes[3][2] = vertices[vertex].attributes[3][3] = 1;
    }
    vertices[1].attributes[0][0] = 4; vertices[2].attributes[0][1] = 4;
    check("native vertex MOV/ADD accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native vertex constants and diffuse output", color[3 * 8 + 3] == 0xFF8040FF && color[9] == 0);
    nv2a_gpu_flush(); memset(color, 0, sizeof color);
    for (vertex = 0; vertex < 3; vertex++) vertices[vertex].attributes[3][1] = 0.5f;
    check("unchanged constants with changed vertices accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("reused constants still draw changed vertex color", color[3 * 8 + 3] == 0xFF8080FF && color[9] == 0);
    for (vertex = 0; vertex < 3; vertex++) vertices[vertex].attributes[3][1] = 0.25f;
    nv2a_gpu_invalidate(); memset(color, 0, sizeof color);
    constants[0][0] = constants[0][1] = 4;
    check("cached native vertex program accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("cached native vertex program observes constants", color[5 * 8 + 5] == 0xFF8040FF && color[3 * 8 + 3] == 0);
    nv2a_gpu_flush(); memset(color, 0, sizeof color);
    constants[0][0] = constants[0][1] = 2;
    check("restored native constants accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("constant mutation and restoration update geometry", color[3 * 8 + 3] == 0xFF8040FF && color[5 * 8 + 5] == 0);
    nv2a_gpu_invalidate();
    memset(color, 0, sizeof color); constants[0][0] = constants[0][1] = 2;
    for (vertex = 0; vertex < 4; vertex++) constants[1][vertex] = 0.5f;
    program[1][1] = (2u << 21) | (1u << 13) | (3u << 9) | 0x1Bu;
    program[1][2] = (2u << 26) | (0x1Bu << 17) | (3u << 11);
    check("mutated native vertex MUL program accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native vertex MUL and instruction cache invalidation", color[3 * 8 + 3] == 0x80402080);
    nv2a_gpu_invalidate(); memset(color, 0, sizeof color);
    program[1][1] = (1u << 21) | (3u << 9) | 0x1Bu; program[1][2] = 2u << 26;
    for (vertex = 0; vertex < 3; vertex++) {
        memset(vertices[vertex].attributes[3], 0, sizeof vertices[vertex].attributes[3]);
        vertices[vertex].attributes[3][vertex] = 1; vertices[vertex].attributes[3][3] = 1;
    }
    state.flat_shading = 1;
    check("native flat-shaded vertex program accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native flat shading uses final provoking vertex", color[3 * 8 + 3] == 0xFF0000FF);
    nv2a_gpu_invalidate();
    {
        uint32_t indices[6] = {0,1,2,2,1,3};
        vertices[3] = vertices[1]; vertices[3].attributes[0][1] = 4;
        state.indices = indices; state.index_count = 6;
        memset(color, 0, sizeof color);
        check("native indexed quad accepted", nv2a_gpu_draw(&state, vertices, 4)); nv2a_gpu_sync();
        check("indexed flat shading preserves provoking vertices", color[3 * 8 + 3] == 0xFF0000FF && color[5 * 8 + 5] == 0xFF00FF00);
        nv2a_gpu_invalidate(); memset(color, 0, sizeof color); indices[5] = 0;
        check("mutated native indices accepted", nv2a_gpu_draw(&state, vertices, 4)); nv2a_gpu_sync();
        check("native indices mutation changes geometry", color[3 * 8 + 3] == 0xFFFF0000 && color[5 * 8 + 5] == 0);
        indices[5] = 4;
        check("native out-of-range vertex index rejected", !nv2a_gpu_draw(&state, vertices, 4));
        state.indices = NULL; state.index_count = 0;
        nv2a_gpu_invalidate();
    }
    {
        uint32_t state_program[136][4] = {{0, (1u << 21) | 0x1Bu, 2u << 26, 0x3001u}};
        float attributes[16][4] = {{9,8,7,6}};
        check("native masked state program accepted", nv2a_gpu_execute_state(state_program, valid, 0, attributes, constants));
        check("native state mask preserves untouched constants", constants[0][0] == 2 && constants[0][1] == 2 &&
              constants[0][2] == 7 && constants[0][3] == 6 && constants[1][0] == 0.5f);
    }
}

static void test_gpu_dot_textures(void)
{
    const uint32_t expected_texels[4] = {13,9,10,6};
    const uint32_t boundary_bytes[4] = {0,127,128,255};
    const uint32_t boundary_texels[4][4] = {{0,5,10,0},{15,15,0,0},{0,0,0,15},{0,0,15,15}};
    uint32_t color[64] = {0}, texels[16];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.combiners.control = 1; state.combiners.rgb_input[0] = 0x0A200000; state.combiners.alpha_input[0] = 0x0000201A;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    for (uint32_t texel = 0; texel < 16; texel++) texels[texel] = 0xFF000000u | ((texel + 1) * 0x000B0703u);
    for (uint32_t stage = 2; stage < 4; stage++) {
        state.textures[stage].source = (const uint8_t *)texels; state.textures[stage].source_bytes = sizeof texels;
        state.textures[stage].width = state.textures[stage].height = 4; state.textures[stage].pitch = 16;
        state.textures[stage].format = 6; state.textures[stage].decode = gpu_decode_grid; state.textures[stage].decode_context = texels;
        state.textures[stage].address_u = state.textures[stage].address_v = 1;
    }
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = 1;
        vertices[vertex].texture[0][0] = 64.0f / 255; vertices[vertex].texture[0][1] = 192.0f / 255;
        vertices[vertex].texture[0][2] = 32.0f / 255;
        vertices[vertex].texture[1][0] = 1; vertices[vertex].texture[2][1] = 1;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    state.stage_program = 4u | (17u << 5) | (9u << 10);
    nv2a_gpu_invalidate();
    for (uint32_t mapping = 0; mapping < 4; mapping++) {
        state.shader_dot_mapping = mapping | (mapping << 4);
        check("native dot texture mapping accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native dot texture mapping selects distinct texel", color[9] == texels[expected_texels[mapping]]);
        for (uint32_t sample = 0; sample < 4; sample++) {
            for (uint32_t vertex = 0; vertex < 3; vertex++) {
                vertices[vertex].texture[0][0] = boundary_bytes[sample] / 255.0f;
                vertices[vertex].texture[0][1] = boundary_bytes[sample] / 255.0f;
            }
            check("native dot texture byte boundary accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            check("native dot mapping handles sign endpoints", color[9] == texels[boundary_texels[mapping][sample]]);
        }
        for (uint32_t vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].texture[0][0] = 64.0f / 255; vertices[vertex].texture[0][1] = 192.0f / 255;
        }
    }
    state.shader_dot_mapping = 0;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[1][1] = 0.25f; vertices[vertex].texture[1][2] = 0.5f;
        vertices[vertex].texture[2][0] = -0.25f; vertices[vertex].texture[2][2] = 0.25f;
    }
    check("multicomponent dot coordinates accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot coordinates include weighted signed XYZ contributions", color[9] == texels[10]);
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[1][1] = vertices[vertex].texture[1][2] = 0;
        vertices[vertex].texture[2][0] = vertices[vertex].texture[2][2] = 0;
    }
    state.combiners.rgb_input[0] = 0x0B200000; state.combiners.alpha_input[0] = 0x0000201B;
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[3][2] = 1;
    state.stage_program = 4u | (17u << 5) | (17u << 10) | (9u << 15);
    check("dot ST at stage three accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("stage three consumes stage two dot intermediate", color[9] == texels[3]);
    state.shader_dot_mapping = 2u << 8;
    check("cached stage three dot mapping accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("stage three mapping uses independent high field", color[9] == texels[7]);
    state.shader_dot_mapping = 0; state.stage_program = 4u | (17u << 5) | (9u << 10);
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[3][0] = 1; vertices[vertex].texture[3][2] = 0;
    }
    state.stage_program |= 9u << 15;
    check("chained dot texture stages accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot texture chain retains preceding dot value", color[9] == texels[7]);
    state.shader_other_stage_input = 2u << 20;
    check("cached dot texture selector change accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot texture selector uses sampled stage output", color[9] == texels[11]);
    state.shader_other_stage_input = 0; state.stage_program = 4u | (17u << 5) | (17u << 10);
    state.combiners.rgb_input[0] = 0x0A200000; state.combiners.alpha_input[0] = 0x0000201A;
    check("dot product zero output accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot product texture register is zero including alpha", color[9] == 0);
    state.stage_program = 4u | (17u << 5) | (9u << 10); state.shader_dot_mapping = 4;
    check("unresolved HILO dot mapping rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_dot_mapping = 0; state.stage_program = 4u | (9u << 10);
    check("dot texture missing predecessor rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 4u | (9u << 5);
    check("dot texture invalid stage rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 17;
    check("dot product stage zero rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 4u | (17u << 15);
    check("dot product stage three rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 4u | (17u << 5) | (9u << 10); state.shader_other_stage_input = 2u << 16;
    check("dot texture self input rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_other_stage_input = 0; state.textures[2].cube = 1;
    check("dot ST cube resource rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.textures[2].cube = 0; state.textures[2].linear = 1;
    check("dot ST linear resource rejected", !nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_invalidate();
}

static void test_gpu_dependent_textures(void)
{
    const float producer_coordinates[3][4] = {{0.125f,0.375f,0.625f,0.875f},
                                              {0.375f,0.625f,0.875f,0.125f},
                                              {0.625f,0.875f,0.125f,0.375f}};
    const uint32_t expected_texels[2][3] = {{3,4,9},{9,14,3}};
    uint32_t color[64] = {0}, texels[16], producer_texel = 0xDF205F9F;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.combiners.control = 1; state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    for (uint32_t texel = 0; texel < 16; texel++) texels[texel] = 0xFF000000u | ((texel + 1) * 0x00090703u);
    for (uint32_t stage = 0; stage < 4; stage++) {
        state.textures[stage].source = (const uint8_t *)texels; state.textures[stage].source_bytes = sizeof texels;
        state.textures[stage].width = state.textures[stage].height = 4; state.textures[stage].pitch = 16;
        state.textures[stage].format = 6; state.textures[stage].decode = gpu_decode_grid; state.textures[stage].decode_context = texels;
    }
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = 1;
        for (uint32_t stage = 0; stage < 3; stage++) memcpy(vertices[vertex].texture[stage], producer_coordinates[stage], sizeof producer_coordinates[stage]);
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    nv2a_gpu_invalidate();
    for (uint32_t mode = 0; mode < 2; mode++) {
        for (uint32_t stage = 1; stage < 4; stage++) {
            state.stage_program = (15u + mode) << (stage * 5);
            for (uint32_t producer = 0; producer < stage; producer++) state.stage_program |= 4u << (producer * 5);
            state.combiners.rgb_input[0] = ((8u + stage) << 24) | 0x00200000;
            state.combiners.alpha_input[0] = 0x00002010u | (8u + stage);
            for (uint32_t vertex = 0; vertex < 3; vertex++) memset(vertices[vertex].texture[stage], 0, sizeof vertices[vertex].texture[stage]);
            for (uint32_t source_stage = 0; source_stage < stage; source_stage++) {
                state.shader_other_stage_input = stage == 1 ? 0xF000u : source_stage << (stage * 4 + 8);
                check("native dependent texture read accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                check("dependent texture uses selected AR or GB channels", color[9] == texels[expected_texels[mode][source_stage]]);
            }
            for (uint32_t vertex = 0; vertex < 3; vertex++) {
                if (stage < 3) memcpy(vertices[vertex].texture[stage], producer_coordinates[stage], sizeof producer_coordinates[stage]);
            }
        }
    }
    state.textures[0].source = (const uint8_t *)&producer_texel; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].pitch = 4;
    state.textures[0].decode = gpu_decode; state.textures[0].decode_context = &producer_texel;
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[0][3] = 1;
    state.stage_program = 1u | (16u << 5) | (15u << 10); state.shader_other_stage_input = 1u << 16;
    state.combiners.rgb_input[0] = 0x0A200000; state.combiners.alpha_input[0] = 0x0000201A;
    check("chained dependent texture reads accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dependent reads consume sampled producer output", color[9] == texels[7]);
    state.shader_other_stage_input = 0;
    check("cached dependent producer mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("cached dependent shader observes producer mutation", color[9] == texels[3]);
    state.shader_other_stage_input = 2u << 16;
    check("same-stage dependent input rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_other_stage_input = 15u << 16;
    check("out-of-range dependent input rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_other_stage_input = 0; state.textures[2].linear = 1;
    check("dependent linear texture rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.textures[2].linear = 0; state.textures[2].cube = 1;
    check("dependent cube texture rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.textures[2].cube = 0; state.stage_program = 15;
    check("dependent read in stage zero rejected", !nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_invalidate();
}

static void test_gpu_clip_planes(void)
{
    const float values[4] = {-1, -0.0f, 0, 1};
    uint32_t color[64], depth[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.depth_enable = state.depth_mask = 1; state.depth_format = 2; state.depth_function = 0x207;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = 1; vertices[vertex].position[2] = 100;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
        for (uint32_t stage = 0; stage < 4; stage++)
            for (uint32_t component = 0; component < 4; component++) vertices[vertex].texture[stage][component] = 1;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    nv2a_gpu_invalidate();
    for (uint32_t stage = 0; stage < 4; stage++) {
        state.stage_program = 5u << (stage * 5);
        for (uint32_t component = 0; component < 4; component++) {
            for (uint32_t polarity = 0; polarity < 2; polarity++) {
                state.shader_clip_mode = polarity << (stage * 4 + component);
                for (uint32_t sample = 0; sample < 4; sample++) {
                    int discarded = polarity ? values[sample] >= 0 : values[sample] < 0;
                    for (uint32_t pixel_index = 0; pixel_index < 64; pixel_index++) {
                        color[pixel_index] = 0xFF123456; depth[pixel_index] = 0xFFFFFF5A;
                    }
                    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[stage][component] = values[sample];
                    check("native clip-plane comparison accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                    check("clip-plane comparison preserves or writes color", color[9] == (discarded ? 0xFF123456 : 0xFFFF0000));
                    check("clip-plane discard preserves depth and stencil", depth[9] == (discarded ? 0xFFFFFF5A : 0x0000645A));
                    nv2a_gpu_invalidate();
                }
            }
            for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[stage][component] = 1;
        }
    }
    state.stage_program = 5; state.shader_clip_mode = 0;
    for (uint32_t pixel_index = 0; pixel_index < 64; pixel_index++) {
        color[pixel_index] = 0xFF123456; depth[pixel_index] = 0xFFFFFF5A;
    }
    vertices[0].texture[0][0] = vertices[2].texture[0][0] = -1;
    check("interpolated clip plane accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("clip plane discards per fragment rather than per triangle", color[9] == 0xFF123456 && color[13] == 0xFFFF0000);
    check("interpolated clip plane preserves discarded depth", depth[9] == 0xFFFFFF5A && depth[13] == 0x0000645A);
    nv2a_gpu_invalidate();
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[0][0] = 1;
    state.combiners.control = 1; state.combiners.rgb_input[0] = 0x08200000;
    state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    check("clip-plane output register accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("clip-plane output is zero including alpha", color[9] == 0);
    nv2a_gpu_invalidate();
}

static void test_gpu(void)
{
    uint32_t framebuffer[64] = {0}, texels[18] = {0xFF804020,0xFF202040};
    uint32_t depthbuffer[64];
    uint32_t resolved[64] = {0};
    uint16_t depth16[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t index;
    state.color = (uint8_t *)framebuffer;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.stage_program = 1; state.combiners.control = 1;
    state.combiners.rgb_input[0] = 0x08040000; state.combiners.rgb_output[0] = 0xC00;
    state.combiners.alpha_input[0] = 0x00002014; state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0x0000000C; state.combiners.final_input[1] = 0x00001C80;
    for (index = 0; index < 2; index++) {
        state.textures[index].source = (const uint8_t *)&texels[index]; state.textures[index].source_bytes = 4;
        state.textures[index].width = state.textures[index].height = 1; state.textures[index].pitch = 4;
        state.textures[index].linear = 1; state.textures[index].decode = gpu_decode; state.textures[index].decode_context = &texels[index];
    }
    for (index = 0; index < 3; index++) {
        vertices[index].position[3] = 1; vertices[index].diffuse[0] = 0.5f; vertices[index].diffuse[1] = 0.25f;
        vertices[index].diffuse[2] = vertices[index].diffuse[3] = 1;
        vertices[index].texture[0][3] = vertices[index].texture[1][3] = 1;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    check("hardware textured triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware texture/diffuse pixel", framebuffer[9] == 0xFF401020);
    state.stage_program = 0x21; state.combiners.control = 2;
    state.combiners.rgb_input[1] = 0x0C090000; state.combiners.rgb_output[1] = 0xC00;
    state.combiners.alpha_input[1] = 0x0000201C; state.combiners.alpha_output[1] = 0xC00;
    check("hardware second combiner stage accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware two-stage pixel", framebuffer[9] == 0xFF080208);
    texels[0] = 0xFF202040;
    check("hardware mutated source accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware observes source mutation", framebuffer[9] == 0xFF020110);
    {
        uint32_t decode_before = gpu_decode_calls;
        state.textures[0].source_bytes = sizeof texels;
        check("expanded texture validation extent accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("texture extent change invalidates cached resource", gpu_decode_calls == decode_before + 1);
        decode_before = gpu_decode_calls;
        check("unchanged full texture snapshot accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("unchanged texture snapshot avoids redecoding", gpu_decode_calls == decode_before);
        texels[17] ^= 0x01000000u;
        check("texture tail mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("texture snapshot checks final source byte", gpu_decode_calls == decode_before + 1);
        decode_before = gpu_decode_calls; state.textures[0].pitch = 8;
        check("texture pitch mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("texture layout participates in cache identity", gpu_decode_calls == decode_before + 1);
        state.textures[0].pitch = 4; state.textures[0].source_bytes = 4;
    }
    state.alpha_enable = 1; state.alpha_function = 0x204; state.alpha_reference = 128;
    for (index = 0; index < 3; index++) vertices[index].diffuse[3] = 0.25f;
    check("hardware alpha-tested triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware alpha discard preserves color", framebuffer[9] == 0xFF020110);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) framebuffer[index] = 0xFF000000;
    texels[0] = 0xFFFF0000;
    state.alpha_enable = 0; state.stage_program = 1; state.combiners.control = 1;
    state.blend_enable = 1; state.blend_source = 0x302; state.blend_destination = 0x303; state.blend_equation = 0x8006;
    for (index = 0; index < 3; index++) { vertices[index].diffuse[0] = 1; vertices[index].diffuse[3] = 0.5f; }
    check("hardware blended triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware source-alpha blend pixel", framebuffer[9] == 0xBF800000);
    state.blend_enable = 0; state.color_mask = 0x01000001; texels[0] = 0xFF00FF00;
    check("hardware masked triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware channel mask preserves red and green", (framebuffer[9] & 0x00FFFF00) == 0x00800000);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) { framebuffer[index] = 0xFF000000; depthbuffer[index] = 0xFFFFFF5A; }
    state.color_mask = 0x01010101; texels[0] = 0xFF804020;
    state.depth = (uint8_t *)depthbuffer; state.depth_pitch = 32; state.depth_format = 2;
    state.depth_enable = state.depth_mask = 1; state.depth_function = 0x201;
    for (index = 0; index < 3; index++) { vertices[index].position[2] = 100; vertices[index].diffuse[0] = 0.5f; vertices[index].diffuse[3] = 1; }
    check("hardware depth-tested triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    if (depthbuffer[9] != 0x0000645A) printf("hardware depth readback: %08X (expected 0000645A)\n", depthbuffer[9]);
    check("hardware Z24 write preserves stencil", depthbuffer[9] == 0x0000645A);
    check("hardware depth-tested color", framebuffer[9] == 0xFF401020);
    for (index = 0; index < 3; index++) { vertices[index].position[2] = 200; vertices[index].diffuse[0] = 1; }
    check("hardware occluded triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware occlusion preserves color and depth", framebuffer[9] == 0xFF401020 && depthbuffer[9] == 0x0000645A);
    state.alpha_enable = 1; state.alpha_function = 0x204; state.alpha_reference = 128;
    for (index = 0; index < 3; index++) { vertices[index].position[2] = 50; vertices[index].diffuse[3] = 0.25f; }
    check("hardware alpha-tested depth triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware alpha discard preserves depth", depthbuffer[9] == 0x0000645A && framebuffer[9] == 0xFF401020);
    state.alpha_enable = 0;
    check("hardware nearer triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware nearer depth updates color", (framebuffer[9] & 0x00FFFFFF) == 0x00801020);
    if ((framebuffer[9] & 0x00FFFFFF) != 0x00801020) printf("hardware nearer triangle color: %08X depth %08X\n", framebuffer[9], depthbuffer[9]);
    state.alpha_enable = state.depth_enable = 0;
    state.combiners.rgb_input[0] = 0x08200000; state.combiners.alpha_input[0] = 0x00002018;
    check("hardware resolve source draw accepted", nv2a_gpu_draw(&state, vertices, 3));
    state.color = (uint8_t *)resolved;
    state.textures[0].source = (const uint8_t *)framebuffer; state.textures[0].source_bytes = sizeof framebuffer;
    state.textures[0].width = state.textures[0].height = 8; state.textures[0].pitch = 32; state.textures[0].format = 0x12;
    state.textures[0].decode_context = framebuffer;
    check("hardware resident render-target resolve accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware resolve reads GPU result rather than stale host pixels", resolved[9] == 0xFF804020);
    nv2a_gpu_flush();
    for (index = 0; index < 64; index++) framebuffer[index] = 0xFF204080;
    resolved[63] = 0xFFABCDEF;
    check("retained resolve observes CPU source pixels", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("retained resolve refreshes source SRV and target pixels", resolved[9] == 0xFF204080 && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush();
    for (index = 0; index < 3; index++) {
        vertices[index].texture[0][0] = 3.5f; vertices[index].texture[0][1] = 2.5f;
    }
    framebuffer[19] = 0xFF112233;
    check("single-texel resident source mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("boxed color refresh samples changed nonzero texel", resolved[9] == 0xFF112233 && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush(); framebuffer[19] = 0xFF204080;
    check("resident source restoration accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("updated upload snapshot observes restoration", resolved[9] == 0xFF204080 && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush();
    framebuffer[10] = 0xFF556677; framebuffer[35] = 0xFF99AABB;
    for (index = 0; index < 3; index++) {
        vertices[index].texture[0][0] = 2.5f; vertices[index].texture[0][1] = 1.5f;
    }
    check("multirow resident source mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("boxed color refresh samples first changed row", resolved[9] == 0xFF556677);
    nv2a_gpu_flush();
    for (index = 0; index < 3; index++) {
        vertices[index].texture[0][0] = 3.5f; vertices[index].texture[0][1] = 4.5f;
    }
    check("unchanged resident source reuse accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("boxed upload preserves source pitch and final changed row", resolved[9] == 0xFF99AABB && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush(); framebuffer[10] = framebuffer[35] = 0xFF204080;
    check("multirow resident source restoration accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("multirow upload snapshot observes restoration", resolved[9] == 0xFF204080);
    for (index = 0; index < 3; index++) vertices[index].texture[0][0] = vertices[index].texture[0][1] = 0;
    state.color = (uint8_t *)framebuffer;
    check("hardware feedback alias safely falls back", !nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_invalidate();
    state.textures[0].source = (const uint8_t *)texels; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].pitch = 4; state.textures[0].decode_context = texels;
    state.depth_enable = 1; state.depth_format = 1; state.depth_pitch = 16; state.depth = (uint8_t *)depth16;
    for (index = 0; index < 64; index++) depth16[index] = 65535;
    for (index = 0; index < 3; index++) vertices[index].position[2] = 100;
    check("hardware Z16 triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware Z16 guest depth write", depth16[9] == 100);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) depth16[index] = 65535;
    for (index = 0; index < 3; index++) vertices[index].position[2] = 100.75f;
    check("hardware fractional Z16 triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware Z16 truncates guest fractional depth", depth16[9] == 100);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) depthbuffer[index] = 0xFFFFFF5A;
    state.depth_format = 2; state.depth_pitch = 32; state.depth = (uint8_t *)depthbuffer; state.control0 = 0x10000;
    for (index = 0; index < 3; index++) { vertices[index].position[3] = 100; vertices[index].position[2] = 500; }
    check("hardware W-depth triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    if (depthbuffer[9] != 0x0000645A) printf("hardware W-depth readback: %08X (expected 0000645A)\n", depthbuffer[9]);
    check("hardware W-depth uses reciprocal clip W rather than Z", depthbuffer[9] == 0x0000645A);
    for (index = 0; index < 3; index++) vertices[index].position[3] = 200;
    check("hardware W-depth occluded triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware W-depth rejection preserves guest depth", depthbuffer[9] == 0x0000645A);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) depthbuffer[index] = 0xFFFFFF5A;
    vertices[0].position[3] = 100; vertices[1].position[3] = 200; vertices[2].position[3] = 400;
    check("hardware varying W-depth triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware W-depth matches integer harmonic interpolation", depthbuffer[9] == 0x0000825A);
    nv2a_gpu_invalidate();
}

int main(int argument_count, char **arguments)
{
    TestVertex *vertices = (TestVertex *)((uint8_t *)memory + 0x4000);
    const uint32_t attributes[] = {0, 3, 9, 10};
    uint32_t index, attribute;
    check("DMA resolves harness memory",
          xbox_DmaPhysicalPointer(0x1000, 4) == (uint8_t *)memory + 0x1000);
    check("DMA rejects out-of-bounds span",
          xbox_DmaPhysicalPointer(sizeof memory - 1, 4) == NULL);
    check("DMA rejects overflowing address",
          xbox_DmaPhysicalPointer(UINT64_MAX, 4) == NULL);
    check("register aperture resolves DMA registers",
          xbox_Nv2aRegisterPointer(0x800040, 8) == nv2a_registers + 0x800040 / 4);
    check("register aperture rejects unaligned address",
          xbox_Nv2aRegisterPointer(1, 4) == NULL);
    check("register aperture rejects out-of-bounds span",
          xbox_Nv2aRegisterPointer(sizeof nv2a_registers - 4, 8) == NULL);
    if (argument_count == 2 && !strncmp(arguments[1], "--pb-", 5)) return invalid_pushbuffer_case(arguments[1]);
    if (getenv("NV2A_GPU_DEPTH_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("minimal D3D11 device", nv2a_gpu_available());
        test_gpu_depth_minimal();
        printf("nv2a_shader_depth: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_PROBE")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("D3D11 device initialization", nv2a_gpu_available());
        check("hardware shader compilation", nv2a_gpu_compile());
        test_gpu_vertex();
        test_gpu_cube();
        test_gpu_dxt();
        test_gpu_clip_planes();
        test_gpu_dependent_textures();
        test_gpu_dot_textures();
        test_gpu();
        test_gpu_depth_minimal();
        test_gpu_scissor_coverage();
    }
    _putenv_s("RECOMP_FB_DUMP", "");
    _putenv_s("RECOMP_TEX_DUMP", "");
    _putenv_s("RECOMP_RASTER_TEST", "");
    test_invalid_pushbuffer_packets();
    test_split_pushbuffer_packet();
    method(0x0200, 8u << 16);
    method(0x0204, 8u << 16);
    method(0x0208, 0x28);
    method(0x020C, 32u | (32u << 16));
    method(0x0210, 0x1000);
    method(0x0214, 0x2000);
    method(0x1D90, 0xFF000000);
    method(0x1D8C, 0xFFFFFF00);
    method(0x1D94, 0xF3);
    method(0x037C, 0x1D01);
    for (attribute = 0; attribute < 4; attribute++) {
        method(0x1720 + attributes[attribute] * 4, 0x4000 + attribute * 16);
        method(0x1760 + attributes[attribute] * 4, (sizeof(TestVertex) << 8) | 0x42);
    }
    for (index = 0; index < 4; index++) {
        vertices[index].position[3] = 1;
        vertices[index].diffuse[0] = 0.5f;
        vertices[index].diffuse[1] = 0.25f;
        vertices[index].diffuse[2] = vertices[index].diffuse[3] = 1;
        vertices[index].texture0[3] = vertices[index].texture1[3] = 1;
    }
    vertices[1].position[0] = 8;
    vertices[2].position[1] = 8;
    upload_program();
    bind_texture(0, 0x3000, 0xFF804020);
    method(0x1E60, 1);
    method(0x1E70, 1);
    method(0x0AC0, 0x08040000);
    method(0x0260, 0x00002014);
    method(0x1E40, 0xC00);
    method(0x0AA0, 0xC00);
    method(0x0288, 0x0000000C);
    method(0x028C, 0x00001C80);
    draw(5, 3);
    check("uploaded vertex program translates geometry", pixel(1, 1) == 0xFF000000);
    check("guest combiner computes lit pixel", pixel(3, 3) == 0xFF401020);
    bind_texture(1, 0x3010, 0xFF202040);
    method(0x1E70, 0x21);
    method(0x0AC0, 0x08090000);
    draw(5, 3);
    check("second guest texture sampled", pixel(3, 3) == 0xFF100808);
    method(0x1E70, 1);
    method(0x0AC0, 0x08040000);
    method(0x030C, 1);
    method(0x0354, 0x0201);
    method(0x035C, 1);
    for (index = 0; index < 3; index++) vertices[index].position[2] = 100;
    draw(5, 3);
    check("depth written to guest surface", (memory[0x2000 / 4 + 3 * 8 + 3] >> 8) == 100);
    for (index = 0; index < 3; index++) {
        vertices[index].position[2] = 200;
        vertices[index].diffuse[0] = 1;
    }
    draw(5, 3);
    check("occluded geometry does not overwrite color", pixel(3, 3) == 0xFF401020);
    method(0x0300, 1);
    method(0x033C, 0x0204);
    method(0x0340, 128);
    for (index = 0; index < 3; index++) {
        vertices[index].position[2] = 50;
        vertices[index].diffuse[3] = 0.25f;
    }
    draw(5, 3);
    check("alpha discard preserves depth", (memory[0x2000 / 4 + 3 * 8 + 3] >> 8) == 100);
    check("alpha discard preserves color", pixel(3, 3) == 0xFF401020);
    method(0x0300, 0);
    method(0x030C, 0);
    method(0x1D94, 0xF0);
    method(0x0304, 1);
    method(0x0344, 0x0302);
    method(0x0348, 0x0303);
    method(0x0350, 0x8006);
    method(0x0AC0, 0x08200000);
    method(0x1EA4, 0);
    float_method(0x0B80, 0); float_method(0x0B84, 0);
    memory[0x3000 / 4] = 0xFFFF0000;
    for (index = 0; index < 4; index++) vertices[index].diffuse[3] = 0.5f;
    vertices[2].position[0] = 8;
    vertices[3].position[1] = 8;
    draw(8, 4);
    check("shared edge blended only once", pixel(3, 3) == 0xBF800000);
    method(0x1D94, 0xF0);
    vertices[1].position[3] = -1;
    draw(8, 4);
    check("invalid triangle does not discard valid batch neighbor", pixel(3, 5) == 0xBF800000);
    check("invalid clip-W triangle remains rejected", pixel(5, 3) == 0xFF000000);
    vertices[1].position[3] = 1;
    method(0x0304, 0);
    method(0x1D94, 0xF0);
    for (index = 0; index < 3; index++) vertices[index].diffuse[3] = 1;
    vertices[2].position[0] = 0;
    method(0x1E9C, 0);
    method(0x0B00, 0);
    method(0x0B04, (3u << 21) | (59u << 13) | 0x1Bu);
    method(0x0B08, (2u << 26) | (0x1Bu << 2));
    method(0x0B0C, (3u << 28) | 0xF800u);
    float_method(0x0A20, 2); float_method(0x0A24, 2);
    float_method(0x0A28, 0); float_method(0x0A2C, 0);
    draw(5, 3);
    check("viewport methods alias shader constants", pixel(1, 1) == 0xFF000000 && pixel(3, 3) == 0xFFFF0000);
    method(0x1D94, 0xF0);
    method(0x1E9C, 0);
    method(0x0B00, 0);
    method(0x0B04, (3u << 21) | 0x1Bu);
    method(0x0B08, (2u << 26) | (0x1Bu << 2));
    method(0x0B0C, (3u << 28) | 0xF800u);
    float_method(0x0680, 4); float_method(0x0684, 4);
    float_method(0x0688, 0); float_method(0x068C, 0);
    draw(5, 3);
    check("matrix methods alias shader constants", pixel(3, 3) == 0xFF000000 && pixel(5, 5) == 0xFFFF0000);
    method(0x1D94, 0xF0);
    method(0x1E9C, 130);
    method(0x0B00, 0);
    method(0x0B04, (1u << 21) | 0x1Bu);
    method(0x0B08, 2u << 26);
    method(0x0B0C, 0xF001u);
    float_method(0x1E80, 2); float_method(0x1E84, 2);
    float_method(0x1E88, 0); float_method(0x1E8C, 0);
    method(0x1E90, 130);
    draw(5, 3);
    check("state shader writes shared constants", pixel(1, 1) == 0xFF000000 && pixel(3, 3) == 0xFFFF0000);
    method(0x1D94, 0xF0);
    method(0x1E94, 4);
    for (index = 0; index < 16; index++)
        float_method(0x0680 + index * 4, index == 0 || index == 5 ? 80.0f : index == 10 || index == 15 ? 1.0f : 0.0f);
    vertices[1].position[0] = 0.1f;
    vertices[2].position[1] = 0.1f;
    draw(5, 3);
    check("fixed-function object vertices use composite transform", pixel(1, 1) == 0xFF000000 && pixel(3, 3) == 0xFFFF0000);
    method(0x176C, 2);
    method(0x194C, 0xFF804020);
    method(0x0AC0, 0x08040000);
    memory[0x3000 / 4] = 0xFFFFFFFF;
    draw(5, 3);
    check("immediate byte color uses hardware channel order", pixel(3, 3) == 0xFF204080);
    memory[0x1000 / 4 + 48] = 0x12345678;
    method(0x020C, 24u | (32u << 16));
    method(0x1D94, 0xF0);
    draw(5, 3);
    check("surface format is independent of clip-to-pitch ratio", memory[0x1000 / 4 + 3 * 6 + 3] == 0xFF204080);
    check("narrow render target preserves following memory", memory[0x1000 / 4 + 48] == 0x12345678);
    method(0x020C, 32u | (32u << 16));
    if (getenv("NV2A_GPU_EXEC")) {
        for (index = 0; index < 6; index++) memory[0x6000 / 4 + index * 32] = 0xFF804020;
        for (index = 0; index < 3; index++) {
            vertices[index].texture0[0] = 1; vertices[index].texture0[1] = vertices[index].texture0[2] = 0;
        }
        method(0x1B00, 0x6000); method(0x1B04, (6u << 8) | (1u << 16) | 4u);
        method(0x1E70, 3); method(0x0AC0, 0x08200000); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest cube metadata and aligned face decoding", pixel(3, 3) == 0xFF804020);
        method(0x1E70, 1); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest projective coordinates select cube storage", pixel(3, 3) == 0xFF804020);
        method(0x1E70, 3);
        method(0x1B04, (6u << 8) | (1u << 16)); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest cube coordinates accept non-cube storage", pixel(3, 3) == 0xFF804020);
        bind_texture(0, 0x3000, 0xFFFFFFFF); method(0x1E70, 1); method(0x0AC0, 0x08040000);
        method(0x1E60, 0); method(0x0288, 0); method(0x028C, 0);
        method(0x1E70, 5); method(0x17F8, 8); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest clip register discards nonnegative Q", pixel(3, 3) == 0xFF000000);
        method(0x17F8, 0); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest clip register mutation restores diffuse draw", pixel(3, 3) == 0xFF204080);
        method(0x1E60, 1); method(0x0288, 0xC); method(0x028C, 0x1C80); method(0x1E70, 1);
        {
            uint32_t texels[16];
            float saved_texture1[3][4];
            const float producer_coordinates[4] = {0.375f,0.625f,0.875f,0.125f};
            for (index = 0; index < 16; index++) texels[index] = 0xFF000000u | ((index + 1) * 0x00090703u);
            xbox_swizzle_rect((uint8_t *)memory + 0x7000, texels, 4, 4, 4);
            for (index = 0; index < 3; index++) {
                memcpy(saved_texture1[index], vertices[index].texture1, sizeof saved_texture1[index]);
                memcpy(vertices[index].texture1, producer_coordinates, sizeof producer_coordinates);
            }
            bind_texture(0, 0x7200, 0xDF205F9F); method(0x1B04, (6u << 8) | (1u << 16) | 0x20u);
            method(0x1B80, 0x7000); method(0x1B84, (6u << 8) | (1u << 16) | (2u << 20) | (2u << 24) | 0x20u);
            method(0x1B88, 0x303); method(0x1B90, 16u << 16);
            method(0x0AC0, 0x0A200000); method(0x0260, 0x0000201A);
            method(0x1E70, 1u | (4u << 5) | (15u << 10)); method(0x1E78, 0); method(0x1D94, 0xF0);
            draw(5, 3);
            check("guest dependent AR consumes sampled stage zero", pixel(3, 3) == texels[3]);
            method(0x1E78, 1u << 16); method(0x1D94, 0xF0);
            draw(5, 3);
            check("guest dependent input register selects stage one", pixel(3, 3) == texels[4]);
            method(0x1E70, 1u | (4u << 5) | (16u << 10)); method(0x1D94, 0xF0);
            draw(5, 3);
            check("guest dependent GB reads green and blue", pixel(3, 3) == texels[14]);
            {
                const uint32_t expected_dot_texels[4] = {13,9,10,6};
                uint32_t mapping;
                float_method(0x1AB0, 0); float_method(0x1AB4, 1); float_method(0x1AB8, 0); float_method(0x1ABC, 0);
                for (index = 0; index < 3; index++) {
                    vertices[index].texture1[0] = 1; vertices[index].texture1[1] = vertices[index].texture1[2] = vertices[index].texture1[3] = 0;
                }
                bind_texture(0, 0x7200, 0xFF40C020); method(0x1B04, (6u << 8) | (1u << 16) | 0x20u);
                method(0x1B88, 0x101); method(0x1E78, 0); method(0x1E70, 1u | (17u << 5) | (9u << 10));
                for (mapping = 0; mapping < 4; mapping++) {
                    method(0x1E74, mapping | (mapping << 4)); method(0x1D94, 0xF0);
                    draw(5, 3);
                    check("guest dot mapping register samples swizzled texture", pixel(3, 3) == texels[expected_dot_texels[mapping]]);
                }
                method(0x1E74, 0); method(0x1E78, 1u << 16); method(0x1D94, 0xF0);
                draw(5, 3);
                check("guest dot source register reads zero DOTPRODUCT output", pixel(3, 3) == texels[1]);
                method(0x1E78, 0); method(0x1E74, 1u << 4); method(0x1D94, 0xF0);
                draw(5, 3);
                check("guest dot mappings retain independent stage fields", pixel(3, 3) == texels[9]);
                method(0x1E74, 0); float_method(0x1AB4, 0); float_method(0x1ABC, 1);
            }
            for (index = 0; index < 3; index++) memcpy(vertices[index].texture1, saved_texture1[index], sizeof saved_texture1[index]);
            bind_texture(0, 0x3000, 0xFFFFFFFF); method(0x1E70, 1); method(0x1E78, 0);
            method(0x0AC0, 0x08040000); method(0x0260, 0x00002014);
        }
    }
    if (getenv("NV2A_SHADER_BENCH")) {
        clock_t started;
        uint32_t checksum = 0;
        method(0x0200, 128u << 16);
        method(0x0204, 128u << 16);
        method(0x020C, 512u | (512u << 16));
        method(0x0210, 0x8000);
        method(0x176C, (sizeof(TestVertex) << 8) | 0x42);
        upload_program();
        method(0x0AC0, 0x08040000);
        method(0x0260, 0x18200000);
        if (getenv("NV2A_SHADER_BENCH_GENERAL")) {
            method(0x1E60, 2);
            method(0x0AC4, 0x0C200000);
            method(0x0264, 0x1C200000);
            method(0x1E44, 0xC00);
            method(0x0AA4, 0xC00);
        }
        method(0x0304, 1);
        method(0x0344, 0x0302);
        method(0x0348, 0x0303);
        memory[0x3000 / 4] = 0x80804020;
        vertices[1].position[0] = 128;
        vertices[2].position[1] = 128;
        for (index = 0; index < 3; index++) {
            vertices[index].diffuse[0] = 0.5f;
            vertices[index].diffuse[1] = 0.25f;
            vertices[index].diffuse[2] = vertices[index].diffuse[3] = 1;
        }
        started = clock();
        for (index = 0; index < 256; index++) draw(5, 3);
        for (index = 0; index < 128 * 128; index++)
            checksum = checksum * 33u + memory[0x8000 / 4 + index];
        check("benchmark preserves menu shader color", memory[0x8000 / 4 + 3 * 128 + 3] == 0x80401020);
        printf("menu shader benchmark: %.3f ms checksum %08X\n",
                1000.0 * (double)(clock() - started) / CLOCKS_PER_SEC, checksum);
        method(0x0304, 0);
        method(0x1E60, 1);
        method(0x0260, 0x18140000);
        draw(5, 3);
        check("affine modulation preserves texture times diffuse alpha", memory[0x8000 / 4 + 3 * 128 + 3] == 0x80401020);
        method(0x0260, 0x14200000);
        draw(5, 3);
        check("affine modulation preserves diffuse-only alpha", memory[0x8000 / 4 + 3 * 128 + 3] == 0xFF401020);
    }
    printf("nv2a_shader_smoke: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}