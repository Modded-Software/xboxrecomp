#ifndef NV2A_GPU_H
#define NV2A_GPU_H

#include <stdint.h>
#include "../d3d/nv2a_shader_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

int nv2a_gpu_available(void);
int nv2a_gpu_compile(void);
void nv2a_gpu_sync(void);
void nv2a_gpu_flush(void);
void nv2a_gpu_invalidate(void);
void nv2a_gpu_report(void);

#define NV2A_GPU_MAX_VERTICES 65536u
#define NV2A_GPU_MAX_INDICES 196608u
#define NV2A_GPU_TOPOLOGY_TRIANGLES 0u
#define NV2A_GPU_TOPOLOGY_LINES 1u
#define NV2A_GPU_TOPOLOGY_POINTS 2u

typedef struct Nv2aGpuVertex {
	float position[4], diffuse[4], specular[4], texture[4][4];
	float fog_coordinate;
	float normal[4];
	float weights[4];
	float attributes[16][4];
} Nv2aGpuVertex;

typedef struct Nv2aGpuTexture {
	const uint8_t *source;
	uint32_t source_bytes, width, height, pitch, format, linear, address_u, address_v;
	uint32_t cube, face_stride;
	uint32_t depth;
	uint32_t address_w, filter, control0, control0_valid, border_color;
	uint32_t color_key;
	uint32_t mip_levels;
	float bump_matrix[4], bump_scale, bump_offset;
	void *decode_context;
	int (*decode)(void *, uint32_t, uint32_t, uint32_t *);
	int (*decode_face)(void *, uint32_t, uint32_t, uint32_t, uint32_t *);
	int (*decode_level)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
	int (*decode_volume)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
} Nv2aGpuTexture;

static inline int nv2a_gpu_texture_enabled(const Nv2aGpuTexture *binding)
{
	return !binding->control0_valid || (binding->control0 & (1u << 30)) != 0;
}

static inline int nv2a_gpu_texture_mode_samples(uint32_t mode)
{
	switch (mode) {
	case 1: case 2: case 3: case 6: case 7: case 9: case 11: case 12: case 14: case 15: case 16: case 18:
		return 1;
	default:
		return 0;
	}
}

typedef struct Nv2aGpuDraw {
	uint8_t *color, *depth;
	uint32_t width, height, pitch, bytes_per_pixel, depth_pitch, depth_format;
	uint32_t clip_x, clip_y, clip_width, clip_height;
	/* Unspecified window clipping preserves the direct-call surface-scissor contract. */
	uint32_t window_clip_valid, window_clip_type;
	uint32_t window_clip_horizontal[8], window_clip_vertical[8];
	uint32_t stage_program, alpha_enable, alpha_function, alpha_reference, control0;
	uint32_t shader_clip_mode, shader_other_stage_input;
	uint32_t shader_dot_mapping;
	float shader_eye_vector[3];
	uint32_t shader_eye_vector_valid;
	uint32_t fog_enable, fog_mode;
	float fog_parameters[3];
	uint32_t fog_gen_mode;
	float fog_plane[4];
	uint32_t depth_enable, depth_function, depth_mask;
	uint32_t zmin_max_control, depth_range_valid;
	float depth_clip_min, depth_clip_max;
	uint32_t stencil_enable, stencil_function, stencil_reference, stencil_read_mask, stencil_write_mask;
	uint32_t stencil_fail, stencil_depth_fail, stencil_pass;
	uint32_t cull_enable, cull_face, front_face, polygon_front, polygon_back;
	uint32_t polygon_offset_enable;
	float polygon_offset_scale, polygon_offset_bias;
	uint32_t blend_enable, blend_source, blend_destination, blend_equation, blend_constant, color_mask;
	Nv2aCpuCombiners combiners;
	Nv2aGpuTexture textures[4];
	const uint32_t (*vertex_program)[4];
	const uint32_t *vertex_valid;
	const float (*vertex_constants)[4];
	uint32_t vertex_start, fixed_transform, flat_shading;
	const uint32_t *indices;
	uint32_t index_count;
	uint32_t topology, point_size;
	/* Fixed-function state; guest vertex programs do not consult it. */
	uint32_t lighting_enable, specular_enable, light_enable_mask, color_material, light_control;
	uint32_t normalization_enable, skin_mode, texgen_view_model;
	uint32_t texgen[4][4], texture_matrix_enable[4];
	float material_alpha, specular_power;
	float scene_ambient[4], material_emission[4];
	float light_ambient[8][4], light_diffuse[8][4], light_specular[8][4];
	float light_local_position[8][4];    /* xyz = position, w = range */
	float light_local_attenuation[8][4]; /* xyz = constant/linear/quadratic coefficients */
	float light_infinite_direction[8][4];
	float light_infinite_half_vector[8][4];
	float light_spot_direction[8][4];    /* xyz = direction, w = spot cone parameter */
} Nv2aGpuDraw;

int nv2a_gpu_draw(const Nv2aGpuDraw *state, const Nv2aGpuVertex *vertices, uint32_t count);
int nv2a_gpu_execute_state(const uint32_t program[136][4], const uint32_t valid[136], uint32_t start,
						  const float attributes[16][4], float constants[192][4]);

#ifdef __cplusplus
}
#endif

#endif
