/* See LICENSE for license details. */
#include "opengl.h"
#include "GLFW/glfw3.h"

#include <stdio.h>

#include "options.h"

#define RENDER_TARGET_SIZE   RENDER_TARGET_WIDTH, RENDER_TARGET_HEIGHT
#define TOTAL_OUTPUT_FRAMES (OUTPUT_FRAME_RATE * OUTPUT_TIME_SECONDS - 1)

#define MODEL_RENDER_MODEL_MATRIX_LOC   0
#define MODEL_RENDER_VIEW_MATRIX_LOC    1
#define MODEL_RENDER_PROJ_MATRIX_LOC    2
#define MODEL_RENDER_CLIP_FRACTION_LOC  3
#define MODEL_RENDER_SWIZZLE_LOC        4
#define MODEL_RENDER_LOG_SCALE_LOC      5
#define MODEL_RENDER_DYNAMIC_RANGE_LOC  6
#define MODEL_RENDER_THRESHOLD_LOC      7
#define MODEL_RENDER_GAMMA_LOC          8
#define MODEL_RENDER_BB_COLOUR_LOC      9
#define MODEL_RENDER_BB_FRACTION_LOC   10
#define MODEL_RENDER_GAIN_LOC          11

#define CYCLE_T_UPDATE_SPEED 0.25f
#define BG_CLEAR_COLOUR      (v4){{0.12, 0.1, 0.1, 1}}

struct gl_debug_ctx {
	Stream  stream;
	OS     *os;
};

function f32
get_frame_time_step(ViewerContext *ctx)
{
	f32 result = 0;
	/* NOTE(rnp): if we are outputting frames do a constant time step */
	if (ctx->output_frames_count > 0) {
		result = 1.0f / (OUTPUT_FRAME_RATE * OUTPUT_TIME_SECONDS * CYCLE_T_UPDATE_SPEED);
	} else {
		f64 now = glfwGetTime();
		result = ctx->demo_mode * (now - ctx->last_time);
		ctx->last_time = now;
	}
	ctx->do_update |= result != 0;
	return result;
}

function void
gl_debug_logger(u32 src, u32 type, u32 id, u32 lvl, s32 len, const char *msg, const void *userctx)
{
	(void)src; (void)type; (void)id;

	struct gl_debug_ctx *ctx = (struct gl_debug_ctx *)userctx;
	Stream *e = &ctx->stream;
	stream_append_str8s(e, str8("[OpenGL] "), (str8){.len = len, .data = (u8 *)msg}, str8("\n"));
	os_write_file(ctx->os->error_handle, stream_to_str8(e));
	stream_reset(e, 0);
}

function u32
compile_shader(OS *os, Arena a, u32 type, str8 shader, str8 name)
{
	u32 sid = glCreateShader(type);
	glShaderSource(sid, 1, (const char **)&shader.data, (int *)&shader.len);
	glCompileShader(sid);

	s32 res = 0;
	glGetShaderiv(sid, GL_COMPILE_STATUS, &res);

	if (res == GL_FALSE) {
		Stream buf = arena_stream(a);
		stream_append_str8s(&buf, name, str8(": failed to compile\n"));

		s32 len = 0, out_len = 0;
		glGetShaderiv(sid, GL_INFO_LOG_LENGTH, &len);
		glGetShaderInfoLog(sid, len, &out_len, (char *)(buf.data + buf.widx));
		stream_commit(&buf, out_len);
		glDeleteShader(sid);
		os_write_file(os->error_handle, stream_to_str8(&buf));

		sid = 0;
	}

	return sid;
}

function u32
link_program(OS *os, Arena a, u32 *shader_ids, u32 shader_id_count)
{
	s32 success = 0;
	u32 result  = glCreateProgram();
	for (u32 i = 0; i < shader_id_count; i++)
		glAttachShader(result, shader_ids[i]);
	glLinkProgram(result);
	glGetProgramiv(result, GL_LINK_STATUS, &success);
	if (success == GL_FALSE) {
		s32 len    = 0;
		Stream buf = arena_stream(a);
		stream_append_str8(&buf, str8("shader link error: "));
		glGetProgramInfoLog(result, buf.cap - buf.widx, &len, (c8 *)(buf.data + buf.widx));
		stream_reset(&buf, len);
		stream_append_byte(&buf, '\n');
		os_write_file(os->error_handle, stream_to_str8(&buf));
		glDeleteProgram(result);
		result = 0;
	}
	return result;
}

function u32
load_shader(OS *os, Arena arena, str8 vs_text, str8 fs_text, str8 info_name, str8 label)
{
	u32 result = 0;
	u32 fs_id = compile_shader(os, arena, GL_FRAGMENT_SHADER, fs_text, info_name);
	u32 vs_id = compile_shader(os, arena, GL_VERTEX_SHADER,   vs_text, info_name);
	if (fs_id && vs_id) result = link_program(os, arena, (u32 []){vs_id, fs_id}, 2);
	glDeleteShader(fs_id);
	glDeleteShader(vs_id);

	if (result) {
		Stream buf = arena_stream(arena);
		stream_append_str8s(&buf, str8("loaded: "), info_name, str8("\n"));
		os_write_file(os->error_handle, stream_to_str8(&buf));
		LABEL_GL_OBJECT(GL_PROGRAM, result, label);
	}

	return result;
}

typedef struct {
	RenderContext *render_context;
	str8 vertex_text;
	str8 fragment_header;
} ShaderReloadContext;

function FILE_WATCH_CALLBACK_FN(reload_shader)
{
	ShaderReloadContext *ctx = (typeof(ctx))user_data;
	str8 header    = push_str8(&tmp, ctx->fragment_header);
	str8 fragment  = os_read_whole_file(&tmp, (c8 *)path.data);
	fragment.data -= header.len;
	fragment.len  += header.len;
	assert(fragment.data == header.data);
	u32 new_program = load_shader(os, tmp, ctx->vertex_text, fragment, path, path);
	if (new_program) {
		glDeleteProgram(ctx->render_context->shader);
		ctx->render_context->shader = new_program;
	}
	return 1;
}

function u32
load_complex_texture(Arena arena, c8 *file_path, u32 width, u32 height, u32 depth)
{
	u32 result = 0;
	glCreateTextures(GL_TEXTURE_3D, 1, &result);
	glTextureStorage3D(result, 1, GL_RG32F, width, height, depth);
	glTextureParameteri(result, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
	glTextureParameteri(result, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
	glTextureParameteri(result, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);
	glTextureParameteri(result, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTextureParameteri(result, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	str8 raw = os_read_whole_file(&arena, file_path);
	if (raw.len) glTextureSubImage3D(result, 0, 0, 0, 0, width, height, depth, GL_RG, GL_FLOAT, raw.data);

	return result;
}

function RenderModel
render_model_from_arrays(f32 *vertices, f32 *normals, u16 *indices, u32 index_count)
{
	RenderModel result = {0};

	s32 buffer_size    = index_count * (6 * sizeof(f32) + sizeof(u16));
	s32 indices_offset = index_count * (6 * sizeof(f32));
	s32 vert_size      = index_count * 3 * sizeof(f32);
	s32 ind_size       = index_count * sizeof(u16);

	result.elements        = index_count;
	result.elements_offset = indices_offset;

	glCreateBuffers(1, &result.buffer);
	glNamedBufferStorage(result.buffer, buffer_size, 0, GL_DYNAMIC_STORAGE_BIT);
	glNamedBufferSubData(result.buffer, 0,              vert_size, vertices);
	glNamedBufferSubData(result.buffer, vert_size,      vert_size, normals);
	glNamedBufferSubData(result.buffer, indices_offset, ind_size,  indices);

	glCreateVertexArrays(1, &result.vao);
	glVertexArrayVertexBuffer(result.vao, 0, result.buffer, 0,         3 * sizeof(f32));
	glVertexArrayVertexBuffer(result.vao, 1, result.buffer, vert_size, 3 * sizeof(f32));
	glVertexArrayElementBuffer(result.vao, result.buffer);

	glEnableVertexArrayAttrib(result.vao, 0);
	glEnableVertexArrayAttrib(result.vao, 1);

	glVertexArrayAttribFormat(result.vao, 0, 3, GL_FLOAT, 0, 0);
	glVertexArrayAttribFormat(result.vao, 1, 3, GL_FLOAT, 0, vert_size);

	glVertexArrayAttribBinding(result.vao, 0, 0);
	glVertexArrayAttribBinding(result.vao, 1, 0);

	return result;
}

function RenderModel
load_render_model(Arena arena, c8 *positions_file_name, c8 *indices_file_name, c8 *normals_file_name)
{
	str8 positions = os_read_whole_file(&arena, positions_file_name);
	str8 normals   = os_read_whole_file(&arena, normals_file_name);
	str8 indices   = os_read_whole_file(&arena, indices_file_name);

	RenderModel result = render_model_from_arrays((f32 *)positions.data, (f32 *)normals.data,
	                                              (u16 *)indices.data, indices.len / sizeof(u16));
	return result;
}

function void
scroll_callback(GLFWwindow *window, f64 x, f64 y)
{
	ViewerContext *ctx  = glfwGetWindowUserPointer(window);
	ctx->camera_fov += y;
	ctx->do_update   = 1;
}

function void
key_callback(GLFWwindow *window, s32 key, s32 scancode, s32 action, s32 modifiers)
{
	ViewerContext *ctx = glfwGetWindowUserPointer(window);
	if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		ctx->should_exit = 1;

	if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
		ctx->demo_mode = !ctx->demo_mode;

	if (key == GLFW_KEY_F12 && action == GLFW_PRESS && ctx->output_frames_count == 0) {
		sz frames       = TOTAL_OUTPUT_FRAMES;
		sz needed_bytes = sizeof(u32) * RENDER_TARGET_HEIGHT * RENDER_TARGET_WIDTH * frames;
		if (!ctx->video_arena.beg) {
			ctx->video_arena = os_alloc_arena(needed_bytes);
			if (!ctx->video_arena.beg) {
				fputs("failed to allocate space for output video, video "
				      "won't be saved\n", stderr);
			}
		}
		if (ctx->video_arena.beg) {
			ctx->output_frames_count = TOTAL_OUTPUT_FRAMES;
			ctx->cycle_t = 0;
		}
	}

	if (key == GLFW_KEY_A && action != GLFW_RELEASE)
		ctx->cycle_t += 4.0f / (OUTPUT_TIME_SECONDS * OUTPUT_FRAME_RATE);
	if (key == GLFW_KEY_D && action != GLFW_RELEASE)
		ctx->cycle_t -= 4.0f / (OUTPUT_TIME_SECONDS * OUTPUT_FRAME_RATE);
	if (key == GLFW_KEY_W && action != GLFW_RELEASE)
		ctx->camera_angle += 5 * PI / 180.0f;
	if (key == GLFW_KEY_S && action != GLFW_RELEASE)
		ctx->camera_angle -= 5 * PI / 180.0f;

	ctx->do_update = 1;
}

function void
fb_callback(GLFWwindow *window, s32 w, s32 h)
{
	ViewerContext *ctx = glfwGetWindowUserPointer(window);
	ctx->window_size   = (sv2){.w = w, .h = h};
}

function void
init_viewer(ViewerContext *ctx)
{
	ctx->demo_mode     = 1;
	ctx->window_size   = (sv2){.w = 640, .h = 640};
	ctx->camera_radius = CAMERA_RADIUS;
	ctx->camera_angle  = -CAMERA_ELEVATION_ANGLE * PI / 180.0f;
	ctx->camera_fov    = 60.0f;

	if (!glfwInit()) os_fatal(str8("failed to start glfw\n"));

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	ctx->window = glfwCreateWindow(ctx->window_size.w, ctx->window_size.h, "3D Viewer", 0, 0);
	if (!ctx->window) os_fatal(str8("failed to open window\n"));
	glfwMakeContextCurrent(ctx->window);
	glfwSetWindowUserPointer(ctx->window, ctx);
	glfwSwapInterval(1);

	glfwSetKeyCallback(ctx->window, key_callback);
	glfwSetScrollCallback(ctx->window, scroll_callback);
	glfwSetFramebufferSizeCallback(ctx->window, fb_callback);

	#define X(name, ret, params) name = (name##_fn *)glfwGetProcAddress(#name);
	OGLProcedureList
	#undef X

	/* NOTE: set up OpenGL debug logging */
	struct gl_debug_ctx *gl_debug_ctx = push_struct(&ctx->arena, typeof(*gl_debug_ctx));
	gl_debug_ctx->stream = stream_alloc(&ctx->arena, KB(4));
	gl_debug_ctx->os     = &ctx->os;
	glDebugMessageCallback(gl_debug_logger, gl_debug_ctx);
#ifdef _DEBUG
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
#endif

	glEnable(GL_MULTISAMPLE);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	RenderContext *rc = &ctx->model_render_context;

	RenderTarget *rt = &ctx->multisample_target;
	rt->size = (sv2){{RENDER_TARGET_SIZE}};
	glCreateRenderbuffers(countof(rt->textures), rt->textures);
	glNamedRenderbufferStorageMultisample(rt->textures[0], RENDER_MSAA_SAMPLES,
	                                      GL_RGBA8, RENDER_TARGET_SIZE);
	glNamedRenderbufferStorageMultisample(rt->textures[1], RENDER_MSAA_SAMPLES,
	                                      GL_DEPTH_COMPONENT24, RENDER_TARGET_SIZE);
	glCreateFramebuffers(1, &rt->fb);
	glNamedFramebufferRenderbuffer(rt->fb, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rt->textures[0]);
	glNamedFramebufferRenderbuffer(rt->fb, GL_DEPTH_ATTACHMENT,  GL_RENDERBUFFER, rt->textures[1]);

	rt = &ctx->output_target;
	glCreateTextures(GL_TEXTURE_2D, countof(rt->textures), rt->textures);
	rt->size = (sv2){{RENDER_TARGET_SIZE}};
	glTextureStorage2D(rt->textures[0], 8, GL_RGBA8,             RENDER_TARGET_SIZE);
	glTextureStorage2D(rt->textures[1], 1, GL_DEPTH_COMPONENT24, RENDER_TARGET_SIZE);

	glCreateFramebuffers(1, &rt->fb);
	glNamedFramebufferTexture(rt->fb, GL_COLOR_ATTACHMENT0, rt->textures[0], 0);
	glNamedFramebufferTexture(rt->fb, GL_DEPTH_ATTACHMENT,  rt->textures[1], 0);

	glTextureParameteri(rt->textures[0], GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTextureParameteri(rt->textures[0], GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

	ShaderReloadContext *model_rc = push_struct(&ctx->arena, ShaderReloadContext);
	model_rc->render_context = rc;
	model_rc->vertex_text = str8(""
	"#version 460 core\n"
	"\n"
	"layout(location = 0) in vec3 v_position;\n"
	"layout(location = 1) in vec3 v_normal;\n"
	"\n"
	"layout(location = 0) out vec3 f_normal;\n"
	"layout(location = 1) out vec3 f_texture_coordinate;\n"
	"layout(location = 2) out vec3 f_orig_texture_coordinate;\n"
	"\n"
	"layout(location = " str(MODEL_RENDER_MODEL_MATRIX_LOC)  ") uniform mat4  u_model;\n"
	"layout(location = " str(MODEL_RENDER_VIEW_MATRIX_LOC)   ") uniform mat4  u_view;\n"
	"layout(location = " str(MODEL_RENDER_PROJ_MATRIX_LOC)   ") uniform mat4  u_projection;\n"
	"layout(location = " str(MODEL_RENDER_CLIP_FRACTION_LOC) ") uniform float u_clip_fraction = 1;\n"
	"layout(location = " str(MODEL_RENDER_SWIZZLE_LOC)       ") uniform bool  u_swizzle;\n"
	"\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec3 pos = v_position;\n"
	"\tf_orig_texture_coordinate = (v_position + 1) / 2;\n"
	"\tif (v_position.y == -1) pos.x = clamp(v_position.x, -u_clip_fraction, u_clip_fraction);\n"
	"\tvec3 tex_coord = (pos + 1) / 2;\n"
	"\tf_texture_coordinate = u_swizzle? tex_coord.xzy : tex_coord;\n"
	//"\tf_normal    = normalize(mat3(u_model) * v_normal);\n"
	"\tf_normal    = v_normal;\n"
	"\tgl_Position = u_projection * u_view * u_model * vec4(pos, 1);\n"
	"}\n");

	model_rc->fragment_header = str8(""
	"#version 460 core\n\n"
	"layout(location = 0) in  vec3 normal;\n"
	"layout(location = 1) in  vec3 texture_coordinate;\n\n"
	"layout(location = 2) in  vec3 test_texture_coordinate;\n\n"
	"layout(location = 0) out vec4 out_colour;\n\n"
	"layout(location = " str(MODEL_RENDER_DYNAMIC_RANGE_LOC) ") uniform float u_db_cutoff = 60;\n"
	"layout(location = " str(MODEL_RENDER_THRESHOLD_LOC)     ") uniform float u_threshold = 40;\n"
	"layout(location = " str(MODEL_RENDER_GAMMA_LOC)         ") uniform float u_gamma     = 1;\n"
	"layout(location = " str(MODEL_RENDER_LOG_SCALE_LOC)     ") uniform bool  u_log_scale;\n"
	"layout(location = " str(MODEL_RENDER_BB_COLOUR_LOC)     ") uniform vec4  u_bb_colour   = vec4(" str(BOUNDING_BOX_COLOUR) ");\n"
	"layout(location = " str(MODEL_RENDER_BB_FRACTION_LOC)   ") uniform float u_bb_fraction = " str(BOUNDING_BOX_FRACTION) ";\n"
	"layout(location = " str(MODEL_RENDER_GAIN_LOC)          ") uniform float u_gain        = 1.0f;\n"
	"\n"
	"layout(binding = 0) uniform sampler3D u_texture;\n"
	"\n#line 1\n");

	str8 render_model = str8("render_model.frag.glsl");
	reload_shader(&ctx->os, render_model, (sptr)model_rc, ctx->arena);
	os_add_file_watch(&ctx->os, &ctx->arena, render_model, reload_shader, (sptr)model_rc);

	rc = &ctx->overlay_render_context;
	ShaderReloadContext *overlay_rc = push_struct(&ctx->arena, ShaderReloadContext);
	overlay_rc->render_context = rc;
	overlay_rc->vertex_text = str8(""
	"#version 460 core\n"
	"\n"
	"layout(location = 0) in vec2 v_position;\n"
	"layout(location = 1) in vec2 v_texture_coordinate;\n"
	"layout(location = 2) in vec3 v_colour;\n"
	"layout(location = 3) in uint v_flags;\n"
	"\n"
	"layout(location = 0) out vec2 f_texture_coordinate;\n"
	"layout(location = 1) out vec3 f_colour;\n"
	"layout(location = 2) out uint f_flags;\n"
	"\n"
	"layout(location = 0) uniform ivec2 u_screen_size;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tf_texture_coordinate = v_texture_coordinate;\n"
	"\tf_colour             = v_colour;\n"
	"\tf_flags              = v_flags;\n"
	"\tgl_Position = vec4(v_position, 0, 1);\n"
	//"\tgl_Position = vec4(2 * (v_position / vec2(u_screen_size)) - 1, 0, 1);\n"
	"}\n");

	overlay_rc->fragment_header = str8(""
	"#version 460 core\n\n"
	"layout(location = 0) in  vec2 texture_coordinate;\n"
	"layout(location = 1) in  vec3 colour;\n"
	"layout(location = 0) out vec4 out_colour;\n"
	"\n#line 1\n");

	f32 overlay_vertices[] = {
		-1,  1, 0, 0,
		-1, -1, 0, 1,
		 1, -1, 1, 1,
		-1,  1, 0, 0,
		 1, -1, 1, 1,
		 1,  1, 1, 0,
	};
	glCreateVertexArrays(1, &rc->vao);
	glCreateBuffers(1, &rc->vbo);

	glNamedBufferData(rc->vbo, sizeof(overlay_vertices), overlay_vertices, GL_STATIC_DRAW);

	glEnableVertexArrayAttrib(rc->vao, 0);
	glEnableVertexArrayAttrib(rc->vao, 1);
	glVertexArrayVertexBuffer(rc->vao, 0, rc->vbo, 0,               4 * sizeof(f32));
	glVertexArrayVertexBuffer(rc->vao, 1, rc->vbo, 2 * sizeof(f32), 4 * sizeof(f32));
	glVertexArrayAttribFormat(rc->vao, 0, 2, GL_FLOAT, 0, 0);
	glVertexArrayAttribFormat(rc->vao, 1, 2, GL_FLOAT, 0, 2 * sizeof(f32));
	glVertexArrayAttribBinding(rc->vao, 0, 0);
	glVertexArrayAttribBinding(rc->vao, 1, 0);

	str8 render_overlay = str8("render_overlay.frag.glsl");
	reload_shader(&ctx->os, render_overlay, (sptr)overlay_rc, ctx->arena);
	os_add_file_watch(&ctx->os, &ctx->arena, render_overlay, reload_shader, (sptr)overlay_rc);

	f32 unit_cube_vertices[] = {
		 1.0f,  1.0f, -1.0f,
		 1.0f,  1.0f, -1.0f,
		 1.0f,  1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f, -1.0f,  1.0f,
		 1.0f, -1.0f,  1.0f,
		 1.0f, -1.0f,  1.0f,
		-1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,
		-1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f, -1.0f,
		-1.0f,  1.0f,  1.0f,
		-1.0f,  1.0f,  1.0f,
		-1.0f,  1.0f,  1.0f,
		-1.0f, -1.0f,  1.0f,
		-1.0f, -1.0f,  1.0f,
		-1.0f, -1.0f,  1.0f
	};
	f32 unit_cube_normals[] = {
		 0.0f,  0.0f, -1.0f,
		 0.0f,  1.0f,  0.0f,
		 1.0f,  0.0f,  0.0f,
		 0.0f,  0.0f, -1.0f,
		 0.0f, -1.0f,  0.0f,
		 1.0f,  0.0f,  0.0f,
		 0.0f,  0.0f,  1.0f,
		 0.0f,  1.0f,  0.0f,
		 1.0f,  0.0f,  0.0f,
		 0.0f,  0.0f,  1.0f,
		 0.0f, -1.0f,  0.0f,
		 1.0f,  0.0f,  0.0f,
		 0.0f,  0.0f, -1.0f,
		 0.0f,  1.0f,  0.0f,
		-1.0f,  0.0f,  0.0f,
		 0.0f,  0.0f, -1.0f,
		 0.0f, -1.0f,  0.0f,
		-1.0f,  0.0f,  0.0f,
		 0.0f,  0.0f,  1.0f,
		 0.0f,  1.0f,  0.0f,
		-1.0f,  0.0f,  0.0f,
		 0.0f,  0.0f,  1.0f,
		 0.0f, -1.0f,  0.0f,
		-1.0f,  0.0f,  0.0f
	};
	u16 unit_cube_indices[] = {
		1,  13, 19,
		1,  19, 7,
		9,  6,  18,
		9,  18, 21,
		23, 20, 14,
		23, 14, 17,
		16, 4,  10,
		16, 10, 22,
		5,  2,  8,
		5,  8,  11,
		15, 12, 0,
		15, 0,  3
	};

	ctx->unit_cube = render_model_from_arrays(unit_cube_vertices, unit_cube_normals,
	                                          unit_cube_indices, countof(unit_cube_indices));
}

function void
set_camera(u32 program, u32 location, v3 position, v3 normal, v3 orthogonal)
{
	v3 right = cross(orthogonal, normal);
	v3 up    = cross(normal,     right);

	v3 translate;
	position    = v3_sub((v3){0}, position);
	translate.x = v3_dot(position, right);
	translate.y = v3_dot(position, up);
	translate.z = v3_dot(position, normal);

	m4 transform;
	transform.c[0] = (v4){{right.x,     up.x,        normal.x,    0}};
	transform.c[1] = (v4){{right.y,     up.y,        normal.y,    0}};
	transform.c[2] = (v4){{right.z,     up.z,        normal.z,    0}};
	transform.c[3] = (v4){{translate.x, translate.y, translate.z, 1}};
	glProgramUniformMatrix4fv(program, location, 1, 0, transform.E);
}

function void
draw_volume_item(ViewerContext *ctx, VolumeDisplayItem *v, f32 rotation, f32 translate_x)
{
	if (!v->texture) {
		v->texture = load_complex_texture(ctx->arena, v->file_path,
		                                  v->width, v->height, v->depth);
	}

	u32 program = ctx->model_render_context.shader;
	v3 scale = v3_sub(v->max_coord_mm, v->min_coord_mm);
	m4 S;
	S.c[0] = (v4){{scale.x, 0,       0,       0}};
	S.c[1] = (v4){{0,       scale.z, 0,       0}};
	S.c[2] = (v4){{0,       0,       scale.y, 0}};
	S.c[3] = (v4){{0,       0,       0,       1}};

	m4 T;
	T.c[0] = (v4){{1, 0, 0, translate_x}};
	T.c[1] = (v4){{0, 1, 0, 0}};
	T.c[2] = (v4){{0, 0, 1, 0}};
	T.c[3] = (v4){{0, 0, 0, 1}};

	f32 sa = sin_f32(rotation);
	f32 ca = cos_f32(rotation);
	m4 R;
	R.c[0] = (v4){{ ca, 0, sa, 0}};
	R.c[1] = (v4){{ 0,  1, 0,  0}};
	R.c[2] = (v4){{-sa, 0, ca, 0}};
	R.c[3] = (v4){{ 0,  0, 0,  1}};

	m4 model_transform = m4_mul(m4_mul(R, S), T);
	glProgramUniformMatrix4fv(program, MODEL_RENDER_MODEL_MATRIX_LOC, 1, 0, model_transform.E);

	glProgramUniform1f(program,  MODEL_RENDER_CLIP_FRACTION_LOC, 1 - v->clip_fraction);
	glProgramUniform1f(program,  MODEL_RENDER_THRESHOLD_LOC,     v->threshold);
	glProgramUniform1f(program,  MODEL_RENDER_GAIN_LOC,          v->gain);
	glProgramUniform1ui(program, MODEL_RENDER_SWIZZLE_LOC,       v->swizzle);

	glBindTextureUnit(0, v->texture);
	glBindVertexArray(ctx->unit_cube.vao);
	glDrawElements(GL_TRIANGLES, ctx->unit_cube.elements, GL_UNSIGNED_SHORT,
	               (void *)ctx->unit_cube.elements_offset);
}

function void
update_scene(ViewerContext *ctx, f32 dt)
{
	ctx->cycle_t += CYCLE_T_UPDATE_SPEED * dt;
	if (ctx->cycle_t > 1) ctx->cycle_t -= 1;

	f32 angle = ctx->cycle_t * 2 * PI;
	ctx->camera_position.x =  0;
	ctx->camera_position.z = -ctx->camera_radius;
	ctx->camera_position.y =  ctx->camera_radius * tan_f32(ctx->camera_angle);

	RenderTarget *rt = &ctx->multisample_target;
	f32 one = 1;
	glBindFramebuffer(GL_FRAMEBUFFER, rt->fb);
	glClearNamedFramebufferfv(rt->fb, GL_COLOR, 0, OUTPUT_BG_CLEAR_COLOUR.E);
	glClearNamedFramebufferfv(rt->fb, GL_DEPTH, 0, &one);
	glViewport(0, 0, rt->size.w, rt->size.h);

	u32 program = ctx->model_render_context.shader;
	glUseProgram(program);

	/* TODO(rnp): set this on hot reload instead of every frame */
	v2 points = {{RENDER_TARGET_SIZE}};
	f32 n = 0.1f;
	f32 f = 400.0f;
	f32 r = n * tan_f32(ctx->camera_fov / 2 * PI / 180.0f);
	f32 t = r * points.h / points.w;
	f32 a = -(f + n) / (f - n);
	f32 b = -2 * f * n / (f - n);

	m4 projection;
	projection.c[0] = (v4){{n / r, 0,     0,  0}};
	projection.c[1] = (v4){{0,     n / t, 0,  0}};
	projection.c[2] = (v4){{0,     0,     a, -1}};
	projection.c[3] = (v4){{0,     0,     b,  0}};
	glProgramUniformMatrix4fv(program, MODEL_RENDER_PROJ_MATRIX_LOC, 1, 0, projection.E);

	v3 camera = ctx->camera_position;
	set_camera(program, MODEL_RENDER_VIEW_MATRIX_LOC, camera,
	           v3_normalize(v3_sub(camera, (v3){0})), (v3){{0, 1, 0}});

	glProgramUniform1ui(program, MODEL_RENDER_LOG_SCALE_LOC,     LOG_SCALE);
	glProgramUniform1f(program,  MODEL_RENDER_DYNAMIC_RANGE_LOC, DYNAMIC_RANGE);

	#if DRAW_ALL_VOLUMES
	for (u32 i = 0; i < countof(volumes); i++)
		draw_volume_item(ctx, volumes + i, angle, volumes[i].translate_x);
	#else
	draw_volume_item(ctx, volumes + single_volume_index, angle, 0);
	#endif

	/* NOTE(rnp): resolve multisampled scene */
	glBlitNamedFramebuffer(rt->fb, ctx->output_target.fb, 0, 0, rt->size.w, rt->size.h,
	                       0, 0, rt->size.w, rt->size.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glGenerateTextureMipmap(ctx->output_target.textures[0]);
}

function void
viewer_frame_step(ViewerContext *ctx, f32 dt)
{
	if (ctx->do_update) {
		update_scene(ctx, dt);
		if (ctx->output_frames_count) {
			u32 frame_index  = TOTAL_OUTPUT_FRAMES - ctx->output_frames_count--;
			printf("Reading Frame: [%u/%u]\n", frame_index, (u32)TOTAL_OUTPUT_FRAMES - 1);
			sz needed = ctx->output_target.size.w * ctx->output_target.size.h * sizeof(u32);
			glGetTextureImage(ctx->output_target.textures[0], 0, GL_RGBA,
			                  GL_UNSIGNED_INT_8_8_8_8, needed,
			                  ctx->video_arena.beg + ctx->video_arena_offset);
			ctx->video_arena_offset += needed;
			if (!ctx->output_frames_count) {
				str8 raw = {.len  = TOTAL_OUTPUT_FRAMES * needed,
				            .data = ctx->video_arena.beg};
				ctx->video_arena_offset = 0;
				os_write_new_file(RAW_OUTPUT_PATH, raw);
			}
		}
		ctx->do_update = 0;
	}

	////////////////
	// UI Overlay
	f32 one = 1;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glClearNamedFramebufferfv(0, GL_COLOR, 0, BG_CLEAR_COLOUR.E);
	glClearNamedFramebufferfv(0, GL_DEPTH, 0, &one);

	f32 aspect_ratio = (f32)ctx->output_target.size.w / (f32)ctx->output_target.size.h;
	sv2 target_size  = ctx->window_size;
	if (aspect_ratio > 1) target_size.h = target_size.w / aspect_ratio;
	else                  target_size.w = target_size.h * aspect_ratio;

	if (target_size.w > ctx->window_size.w) {
		target_size.w = ctx->window_size.w;
		target_size.h = ctx->window_size.w / aspect_ratio;
	} else if (target_size.h > ctx->window_size.h) {
		target_size.h = ctx->window_size.h;
		target_size.w = target_size.h * aspect_ratio;
	}

	sv2 size_delta = sv2_sub(ctx->window_size, target_size);

	glViewport(size_delta.x / 2 + 0.5, size_delta.y / 2 + 0.5, target_size.w, target_size.h);

	glUseProgram(ctx->overlay_render_context.shader);
	glBindTextureUnit(0, ctx->output_target.textures[0]);
	glBindVertexArray(ctx->overlay_render_context.vao);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	ctx->should_exit |= glfwWindowShouldClose(ctx->window);
}
