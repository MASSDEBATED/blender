/*
 * Copyright 2016, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Blender Institute
 *
 */

#include "DRW_render.h"

#include "BKE_icons.h"

#include "BLI_rand.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "UI_resources.h"
#include "UI_interface_icons.h"

#include "clay.h"

/* Shaders */

extern char datatoc_clay_frag_glsl[];
extern char datatoc_clay_vert_glsl[];

/* Storage */

static struct CLAY_data {
	/* Depth Pre Pass */
	struct GPUShader *depth_sh;
	struct DRWInterface *depth_itf;
	/* Shading Pass */
	struct GPUShader *clay_sh;
	struct DRWInterface *clay_itf;

	/* Matcap textures */
	struct GPUTexture *matcap_array;
	float matcap_colors[24][3];
	float matcap_rot[2];
	int matcap_id;

	/* Ssao */
	float dfdyfac[2];
	float winmat[4][4];
	float viewvecs[3][4];
	float ssao_params_var[4];
	float ssao_params[4];
	struct GPUTexture *jitter_tx;
	struct GPUTexture *sampling_tx;
} data = {NULL};

/* keep it under MAX_BUFFERS */
typedef struct CLAY_FramebufferList{
	/* default */
	struct GPUFrameBuffer *default_fb;
	/* engine specific */
	struct GPUFrameBuffer *downsample_depth;
} CLAY_FramebufferList;

/* keep it under MAX_TEXTURES */
typedef struct CLAY_TextureList{
	/* default */
	struct GPUTexture *color;
	struct GPUTexture *depth;
	/* engine specific */
	struct GPUTexture *depth_low;
} CLAY_TextureList;

/* for clarity follow the same layout as CLAY_TextureList */
#define SCENE_COLOR 0
#define SCENE_DEPTH 1
#define SCENE_DEPTH_LOW 2

/* keep it under MAX_PASSES */
typedef struct CLAY_PassList{
	struct DRWPass *depth_pass;
	struct DRWPass *clay_pass;
} CLAY_PassList;

/* Functions */

static void add_icon_to_rect(PreviewImage *prv, float *final_rect, int layer)
{
	int image_size = prv->w[0] * prv->h[0];
	float *new_rect = &final_rect[image_size * 4 * layer];

	IMB_buffer_float_from_byte(new_rect, (unsigned char *)prv->rect[0], IB_PROFILE_SRGB, IB_PROFILE_SRGB,
	                           false, prv->w[0], prv->h[0], prv->w[0], prv->w[0]);

	/* Find overall color */
	for (int y = 0; y < 4; ++y)	{
		for (int x = 0; x < 4; ++x) {
			data.matcap_colors[layer][0] += new_rect[y * 512 * 128 * 4 + x * 128 * 4 + 0];
			data.matcap_colors[layer][1] += new_rect[y * 512 * 128 * 4 + x * 128 * 4 + 1];
			data.matcap_colors[layer][2] += new_rect[y * 512 * 128 * 4 + x * 128 * 4 + 2];
		}
	}

	data.matcap_colors[layer][0] /= 16.0f * 2.0f; /* the * 2 is to darken for shadows */
	data.matcap_colors[layer][1] /= 16.0f * 2.0f;
	data.matcap_colors[layer][2] /= 16.0f * 2.0f;
}

static struct GPUTexture *load_matcaps(PreviewImage *prv[24], int nbr)
{
	struct GPUTexture *tex;
	int w = prv[0]->w[0];
	int h = prv[0]->h[0];
	float *final_rect = MEM_callocN(sizeof(float) * 4 * w * h * nbr, "Clay Matcap array rect");

	for (int i = 0; i < nbr; ++i) {
		add_icon_to_rect(prv[i], final_rect, i);
		BKE_previewimg_free(&prv[i]);
	}

	tex = DRW_texture_create_2D_array(w, h, nbr, DRW_TEX_RGBA_8, DRW_TEX_FILTER, final_rect);
	MEM_freeN(final_rect);

	return tex;
}

static int matcap_to_index(int matcap)
{
	if (matcap == ICON_MATCAP_02) return 1;
	else if (matcap == ICON_MATCAP_03) return 2;
	else if (matcap == ICON_MATCAP_04) return 3;
	else if (matcap == ICON_MATCAP_05) return 4;
	else if (matcap == ICON_MATCAP_06) return 5;
	else if (matcap == ICON_MATCAP_07) return 6;
	else if (matcap == ICON_MATCAP_08) return 7;
	else if (matcap == ICON_MATCAP_09) return 8;
	else if (matcap == ICON_MATCAP_10) return 9;
	else if (matcap == ICON_MATCAP_11) return 10;
	else if (matcap == ICON_MATCAP_12) return 11;
	else if (matcap == ICON_MATCAP_13) return 12;
	else if (matcap == ICON_MATCAP_14) return 13;
	else if (matcap == ICON_MATCAP_15) return 14;
	else if (matcap == ICON_MATCAP_16) return 15;
	else if (matcap == ICON_MATCAP_17) return 16;
	else if (matcap == ICON_MATCAP_18) return 17;
	else if (matcap == ICON_MATCAP_19) return 18;
	else if (matcap == ICON_MATCAP_20) return 19;
	else if (matcap == ICON_MATCAP_21) return 20;
	else if (matcap == ICON_MATCAP_22) return 21;
	else if (matcap == ICON_MATCAP_23) return 22;
	else if (matcap == ICON_MATCAP_24) return 23;
	return 0;
}

static struct GPUTexture *create_spiral_sample_texture(int numsaples)
{
	struct GPUTexture *tex;
	float (*texels)[2] = MEM_mallocN(sizeof(float[2]) * numsaples, "concentric_tex");
	const float numsaples_inv = 1.0f / numsaples;
	int i;
	/* arbitrary number to ensure we don't get conciding samples every circle */
	const float spirals = 7.357;

	for (i = 0; i < numsaples; i++) {
		float r = (i + 0.5f) * numsaples_inv;
		float phi = r * spirals * (float)(2.0 * M_PI);
		texels[i][0] = r * cosf(phi);
		texels[i][1] = r * sinf(phi);
	}

	tex = DRW_texture_create_1D(numsaples, DRW_TEX_RG_16, 0, (float *)texels);

	MEM_freeN(texels);
	return tex;
}

static struct GPUTexture *create_jitter_texture(void)
{
	float jitter[64 * 64][2];
	int i;

	/* TODO replace by something more evenly distributed like blue noise */
	for (i = 0; i < 64 * 64; i++) {
		jitter[i][0] = 2.0f * BLI_frand() - 1.0f;
		jitter[i][1] = 2.0f * BLI_frand() - 1.0f;
		normalize_v2(jitter[i]);
	}

	return DRW_texture_create_2D(64, 64, DRW_TEX_RG_16, DRW_TEX_FILTER | DRW_TEX_WRAP, &jitter[0][0]);
}

static void clay_engine_init(void)
{
	/* Create Texture Array */
	if (!data.matcap_array) {
		PreviewImage *prv[24]; /* For now use all of the 24 internal matcaps */

		/* TODO only load used matcaps */
		prv[0]  = UI_icon_to_preview(ICON_MATCAP_01);
		prv[1]  = UI_icon_to_preview(ICON_MATCAP_02);
		prv[2]  = UI_icon_to_preview(ICON_MATCAP_03);
		prv[3]  = UI_icon_to_preview(ICON_MATCAP_04);
		prv[4]  = UI_icon_to_preview(ICON_MATCAP_05);
		prv[5]  = UI_icon_to_preview(ICON_MATCAP_06);
		prv[6]  = UI_icon_to_preview(ICON_MATCAP_07);
		prv[7]  = UI_icon_to_preview(ICON_MATCAP_08);
		prv[8]  = UI_icon_to_preview(ICON_MATCAP_09);
		prv[9]  = UI_icon_to_preview(ICON_MATCAP_10);
		prv[10] = UI_icon_to_preview(ICON_MATCAP_11);
		prv[11] = UI_icon_to_preview(ICON_MATCAP_12);
		prv[12] = UI_icon_to_preview(ICON_MATCAP_13);
		prv[13] = UI_icon_to_preview(ICON_MATCAP_14);
		prv[14] = UI_icon_to_preview(ICON_MATCAP_15);
		prv[15] = UI_icon_to_preview(ICON_MATCAP_16);
		prv[16] = UI_icon_to_preview(ICON_MATCAP_17);
		prv[17] = UI_icon_to_preview(ICON_MATCAP_18);
		prv[18] = UI_icon_to_preview(ICON_MATCAP_19);
		prv[19] = UI_icon_to_preview(ICON_MATCAP_20);
		prv[20] = UI_icon_to_preview(ICON_MATCAP_21);
		prv[21] = UI_icon_to_preview(ICON_MATCAP_22);
		prv[22] = UI_icon_to_preview(ICON_MATCAP_23);
		prv[23] = UI_icon_to_preview(ICON_MATCAP_24);

		data.matcap_array = load_matcaps(prv, 24);
	}

	/* AO Jitter */
	if (!data.jitter_tx) {
		data.jitter_tx = create_jitter_texture();
	}

	/* AO Samples */
	/* TODO use hammersley sequence */
	if (!data.sampling_tx) {
		data.sampling_tx = create_spiral_sample_texture(500);
	}

	/* Depth prepass */
	if (!data.depth_sh) {
		data.depth_sh = DRW_shader_create_3D_depth_only();
	}

	/* Shading pass */
	if (!data.clay_sh) {
		const char *defines =
		        "#define USE_AO;\n"
		        "#define USE_ROTATION;\n";

		/* TODO Optimisation : Create shader combinations and bind the right 
		 * shader depending on the material settings */
		data.clay_sh = DRW_shader_create(datatoc_clay_vert_glsl, NULL, datatoc_clay_frag_glsl, defines);
	}
}

#if 0
static void clay_init_view(CLAY_FramebufferList *buffers, CLAY_TextureList *textures)
{
	int *viewsize = DRW_viewport_size_get();

	DRWFboTexture depth = {&textures->depth_low, DRW_BUF_R_16};

	DRW_framebuffer_init(&buffers->downsample_depth, viewsize[0]/2, viewsize[1]/2, &depth, 1);
}
#endif

static void clay_populate_passes(CLAY_PassList *passes, const struct bContext *C)
{
	SceneLayer *sl = CTX_data_scene_layer(C);
	DRWBatch *matcapbatch, *depthbatch;
	bool pop_depth = false;
	bool pop_clay = false;

	if (!passes->depth_pass) {
		passes->depth_pass = DRW_pass_create("Clay Pass", DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS);

		depthbatch = DRW_batch_create(data.depth_sh, passes->depth_pass);
		pop_depth = true;
	}

	if (!passes->clay_pass) {
		DRWBatch *batch;
		const int depthloc = 0;
		const int matcaploc = 1;
		const int jitterloc = 2;
		const int sampleloc = 3;

		passes->clay_pass = DRW_pass_create("Clay Pass", DRW_STATE_WRITE_COLOR);

		batch = DRW_batch_create(data.clay_sh, passes->clay_pass);

		DRW_batch_uniform_ivec2(batch, "screenres", DRW_viewport_size_get(), 1);
		DRW_batch_uniform_buffer(batch, "depthtex", SCENE_DEPTH, depthloc);
		DRW_batch_uniform_texture(batch, "matcaps", data.matcap_array, matcaploc);
		DRW_batch_uniform_vec3(batch, "matcaps_color", (float *)data.matcap_colors, 24);
		DRW_batch_uniform_vec2(batch, "matcap_rotation", (float *)data.matcap_rot, 1);
		DRW_batch_uniform_int(batch, "matcap_index", &data.matcap_id, 1);

		/* SSAO */
		DRW_batch_uniform_mat4(batch, "WinMatrix", (float *)data.winmat);
		DRW_batch_uniform_vec4(batch, "viewvecs", (float *)data.viewvecs, 3);
		DRW_batch_uniform_vec4(batch, "ssao_params_var", data.ssao_params_var, 1);
		DRW_batch_uniform_vec4(batch, "ssao_params", data.ssao_params, 1);
		DRW_batch_uniform_texture(batch, "ssao_jitter", data.jitter_tx, jitterloc);
		DRW_batch_uniform_texture(batch, "ssao_samples", data.sampling_tx, sampleloc);

		matcapbatch = batch;
		pop_clay = true;
	}

	/* Update default material */
	{
		EngineDataClay *settings = DRW_render_settings();

		data.matcap_rot[0] = cosf(settings->matcap_rot * 3.14159f * 2.0f);
		data.matcap_rot[1] = sinf(settings->matcap_rot * 3.14159f * 2.0f);

		data.ssao_params_var[0] = settings->ssao_distance;
		data.ssao_params_var[1] = settings->ssao_factor_cavity;
		data.ssao_params_var[2] = settings->ssao_factor_edge;
		data.ssao_params_var[3] = settings->ssao_attenuation;
	}

	static bool first = true;
	if (first) {
		first = false;

		/* TODO Why this is crashing? we want to start from an empty list
		 * Seems like ob genericNode (see below) is freed two times */
		// DRW_batch_surface_clear(matcapbatch);
		// DRW_batch_surface_clear(depthbatch);

		Object *ob;
		FOREACH_OBJECT(sl, ob)
		{
			/* Create hash table of batch based on material id*/

			/* Add everything for now */
			if (pop_clay) {
				DRW_batch_surface_add(matcapbatch, ob);
			}

			if (pop_depth) {
				DRW_batch_surface_add(depthbatch, ob);
			}

			/* Free hash table */
		}
		FOREACH_OBJECT_END
	}
}

static void clay_ssao_setup(void)
{
	float invproj[4][4];
	float dfdyfacs[2];
	bool is_persp = DRW_viewport_is_persp();
	/* view vectors for the corners of the view frustum. Can be used to recreate the world space position easily */
	float viewvecs[3][4] = {
	    {-1.0f, -1.0f, -1.0f, 1.0f},
	    {1.0f, -1.0f, -1.0f, 1.0f},
	    {-1.0f, 1.0f, -1.0f, 1.0f}
	};
	int i;
	int *size = DRW_viewport_size_get();
	EngineDataClay *settings = DRW_render_settings();

	DRW_get_dfdy_factors(dfdyfacs);

	data.ssao_params[0] = settings->ssao_samples;
	data.ssao_params[1] = size[0] / 64.0;
	data.ssao_params[2] = size[1] / 64.0;
	data.ssao_params[3] = dfdyfacs[1]; /* dfdy sign for offscreen */

	/* invert the view matrix */
	DRW_viewport_matrix_get(data.winmat, DRW_MAT_WIN);
	invert_m4_m4(invproj, data.winmat);

	/* convert the view vectors to view space */
	for (i = 0; i < 3; i++) {
		mul_m4_v4(invproj, viewvecs[i]);
		/* normalized trick see http://www.derschmale.com/2014/01/26/reconstructing-positions-from-the-depth-buffer */
		mul_v3_fl(viewvecs[i], 1.0f / viewvecs[i][3]);
		if (is_persp)
			mul_v3_fl(viewvecs[i], 1.0f / viewvecs[i][2]);
		viewvecs[i][3] = 1.0;

		copy_v4_v4(data.viewvecs[i], viewvecs[i]);
	}

	/* we need to store the differences */
	data.viewvecs[1][0] -= data.viewvecs[0][0];
	data.viewvecs[1][1] = data.viewvecs[2][1] - data.viewvecs[0][1];

	/* calculate a depth offset as well */
	if (!is_persp) {
		float vec_far[] = {-1.0f, -1.0f, 1.0f, 1.0f};
		mul_m4_v4(invproj, vec_far);
		mul_v3_fl(vec_far, 1.0f / vec_far[3]);
		data.viewvecs[1][2] = vec_far[2] - data.viewvecs[0][2];
	}
}

static void clay_view_draw(RenderEngine *UNUSED(engine), const struct bContext *context)
{
	/* This function may run for multiple viewports
	 * so get the current viewport buffers */
	CLAY_FramebufferList *buffers = NULL;
	CLAY_TextureList *textures = NULL;
	CLAY_PassList *passes = NULL;

	DRW_viewport_init(context, (void **)&buffers, (void **)&textures, (void **)&passes);

	clay_engine_init();

	/* Settings */
	EngineDataClay *engine_data = &CTX_data_scene(context)->claydata;

	if (engine_data->matcap_icon < ICON_MATCAP_01 ||
	    engine_data->matcap_icon > ICON_MATCAP_24)
	{
		engine_data->matcap_icon = ICON_MATCAP_01;
	}

	data.matcap_id = matcap_to_index(engine_data->matcap_icon);

	/* TODO : tag to refresh by the deps graph */
	/* ideally only refresh when objects are added/removed */
	/* or render properties / materials change */
	clay_populate_passes(passes, context);

	DRW_draw_background();

	/* Pass 1 : Depth pre-pass */
	DRW_draw_pass(passes->depth_pass);

	/* Pass 2 : Shading */
	clay_ssao_setup();
	DRW_framebuffer_texture_detach(textures->depth);
	DRW_draw_pass(passes->clay_pass);
	DRW_framebuffer_texture_attach(buffers->default_fb, textures->depth, 0);

	/* Always finish by this */
	DRW_state_reset();
}

void clay_engine_free(void)
{
	/* data.depth_sh Is builtin so it's automaticaly freed */
	if (data.clay_sh) {
		DRW_shader_free(data.clay_sh);
	}

	if (data.matcap_array) {
		DRW_texture_free(data.matcap_array);
	}
}

RenderEngineType viewport_clay_type = {
	NULL, NULL,
	"BLENDER_CLAY", N_("Clay"), RE_INTERNAL | RE_USE_OGL_PIPELINE,
	NULL, NULL, NULL, NULL, &clay_view_draw, NULL,
	{NULL, NULL, NULL}
};