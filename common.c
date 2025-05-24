/* See LICENSE for license details. */
#define GLAD_GL_IMPLEMENTATION
#include "glad/gl.h"
#include "GLFW/glfw3.h"

#define BG_CLEAR_COLOUR    (v4){{0.12, 0.1, 0.1, 1}}
#define RENDER_TARGET_SIZE 1024, 1024

#define CAMERA_ELEVATION_ANGLE 30.0f

/* NOTE(rnp): for video output we will render a full rotation in this much time at the
 * the specified frame rate */
#define OUTPUT_TIME_SECONDS     8.0f
#define OUTPUT_FRAME_RATE      30.0f
#define OUTPUT_BG_CLEAR_COLOUR (v4){{0.0, 0.2, 0.0, 1}}

#define MODEL_RENDER_MODEL_MATRIX_LOC  (0)
#define MODEL_RENDER_VIEW_MATRIX_LOC   (1)
#define MODEL_RENDER_PROJ_MATRIX_LOC   (2)
#define MODEL_RENDER_LOG_SCALE_LOC     (3)
#define MODEL_RENDER_DYNAMIC_RANGE_LOC (4)
#define MODEL_RENDER_THRESHOLD_LOC     (5)
#define MODEL_RENDER_GAMMA_LOC         (6)

typedef struct {
	v3  normal;
	v3  position;
	v3  colour;
	v3  texture_coordinate;
	u32 flags;
} Vertex;

struct gl_debug_ctx {
	Stream  stream;
	OS     *os;
};

function void
gl_debug_logger(u32 src, u32 type, u32 id, u32 lvl, s32 len, const char *msg, const void *userctx)
{
	(void)src; (void)type; (void)id;

	struct gl_debug_ctx *ctx = (struct gl_debug_ctx *)userctx;
	Stream *e = &ctx->stream;
	stream_append_str8(e, str8("[GL DEBUG "));
	switch (lvl) {
	case GL_DEBUG_SEVERITY_HIGH:         stream_append_str8(e, str8("HIGH]: "));         break;
	case GL_DEBUG_SEVERITY_MEDIUM:       stream_append_str8(e, str8("MEDIUM]: "));       break;
	case GL_DEBUG_SEVERITY_LOW:          stream_append_str8(e, str8("LOW]: "));          break;
	case GL_DEBUG_SEVERITY_NOTIFICATION: stream_append_str8(e, str8("NOTIFICATION]: ")); break;
	default:                             stream_append_str8(e, str8("INVALID]: "));      break;
	}
	stream_append(e, (char *)msg, len);
	stream_append_byte(e, '\n');
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

function RenderModel
load_render_model(Arena arena, c8 *positions_file_name, c8 *indices_file_name, c8 *normals_file_name)
{
	RenderModel result = {0};

	str8 positions = os_read_whole_file(&arena, positions_file_name);
	str8 normals   = os_read_whole_file(&arena, normals_file_name);
	str8 indices   = os_read_whole_file(&arena, indices_file_name);

	result.elements = indices.len / sizeof(u16);

	s32 buffer_size = positions.len + indices.len + normals.len;

	s32 el_offset = positions.len + normals.len;
	result.elements_offset = el_offset;

	glCreateBuffers(1, &result.buffer);
	glNamedBufferStorage(result.buffer, buffer_size, 0, GL_DYNAMIC_STORAGE_BIT);
	glNamedBufferSubData(result.buffer, 0,             positions.len, positions.data);
	glNamedBufferSubData(result.buffer, positions.len, normals.len,   normals.data);
	glNamedBufferSubData(result.buffer, el_offset,     indices.len,   indices.data);

	glCreateVertexArrays(1, &result.vao);
	glVertexArrayVertexBuffer(result.vao, 0, result.buffer, 0,             3 * sizeof(f32));
	glVertexArrayVertexBuffer(result.vao, 1, result.buffer, positions.len, 3 * sizeof(f32));
	glVertexArrayElementBuffer(result.vao, result.buffer);

	glEnableVertexArrayAttrib(result.vao, 0);
	glEnableVertexArrayAttrib(result.vao, 1);

	glVertexArrayAttribFormat(result.vao, 0, 3, GL_FLOAT, 0, 0);
	glVertexArrayAttribFormat(result.vao, 1, 3, GL_FLOAT, 0, positions.len);

	glVertexArrayAttribBinding(result.vao, 0, 0);
	glVertexArrayAttribBinding(result.vao, 1, 0);

	return result;
}

function void
scroll_callback(GLFWwindow *window, f64 x, f64 y)
{
	ViewerContext *ctx  = glfwGetWindowUserPointer(window);
	ctx->camera_radius += 0.2 * y;
}

function void
key_callback(GLFWwindow *window, s32 key, s32 scancode, s32 action, s32 modifiers)
{
	ViewerContext *ctx = glfwGetWindowUserPointer(window);
	if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		ctx->should_exit = 1;

	ctx->camera_angle += (key == GLFW_KEY_W && action != GLFW_RELEASE) * 5 * PI / 180.0f;
	ctx->camera_angle -= (key == GLFW_KEY_S && action != GLFW_RELEASE) * 5 * PI / 180.0f;
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
	ctx->window_size   = (sv2){.w = 640, .h = 640};
	ctx->camera_radius = 5;
	ctx->camera_angle  = CAMERA_ELEVATION_ANGLE * PI / 180.0f;

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

	if (!gladLoadGL(glfwGetProcAddress)) os_fatal(str8("failed to load glad\n"));

	/* NOTE: set up OpenGL debug logging */
	struct gl_debug_ctx *gl_debug_ctx = push_struct(&ctx->arena, typeof(*gl_debug_ctx));
	gl_debug_ctx->stream = stream_alloc(&ctx->arena, KB(4));
	gl_debug_ctx->os     = &ctx->os;
	glDebugMessageCallback(gl_debug_logger, gl_debug_ctx);
#ifdef _DEBUG
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
#endif

	glEnable(GL_DEPTH_TEST);

	RenderContext *rc = &ctx->model_render_context;

	RenderTarget *rt = &ctx->output_target;
	glCreateTextures(GL_TEXTURE_2D, countof(rt->textures), rt->textures);
	rt->size = (sv2){{RENDER_TARGET_SIZE}};
	glTextureStorage2D(rt->textures[0], 1, GL_RGBA8,             RENDER_TARGET_SIZE);
	glTextureStorage2D(rt->textures[1], 1, GL_DEPTH_COMPONENT24, RENDER_TARGET_SIZE);

	glCreateFramebuffers(1, &rt->fb);
	glNamedFramebufferTexture(rt->fb, GL_COLOR_ATTACHMENT0, rt->textures[0], 0);
	glNamedFramebufferTexture(rt->fb, GL_DEPTH_ATTACHMENT,  rt->textures[1], 0);

	ShaderReloadContext *model_rc = push_struct(&ctx->arena, ShaderReloadContext);
	model_rc->render_context = rc;
	model_rc->vertex_text = str8(""
	"#version 460 core\n"
	"\n"
	"layout(location = 0) in vec3 v_position;\n"
	"layout(location = 1) in vec3 v_normal;\n"
	"\n"
	"layout(location = 0) out vec3 f_normal;\n"
	"\n"
	"layout(location = " str(MODEL_RENDER_MODEL_MATRIX_LOC) ") uniform mat4 u_model;\n"
	"layout(location = " str(MODEL_RENDER_VIEW_MATRIX_LOC)  ") uniform mat4 u_view;\n"
	"layout(location = " str(MODEL_RENDER_PROJ_MATRIX_LOC)  ") uniform mat4 u_projection;\n"
	"\n"
	"\n"
	"void main()\n"
	"{\n"
	//"\tf_normal    = normalize(mat3(u_model) * v_normal);\n"
	"\tf_normal    = v_normal;\n"
	"\tgl_Position = u_projection * u_view * u_model * vec4(v_position, 1);\n"
	"}\n");

	model_rc->fragment_header = str8(""
	"#version 460 core\n\n"
	"layout(location = 0) in  vec3 normal;\n\n"
	"layout(location = 0) out vec4 out_colour;\n\n"
	"layout(location = " str(MODEL_RENDER_DYNAMIC_RANGE_LOC) ") uniform float u_db_cutoff = 60;\n"
	"layout(location = " str(MODEL_RENDER_THRESHOLD_LOC)     ") uniform float u_threshold = 40;\n"
	"layout(location = " str(MODEL_RENDER_GAMMA_LOC)         ") uniform float u_gamma     = 1;\n"
	"layout(location = " str(MODEL_RENDER_LOG_SCALE_LOC)     ") uniform bool  u_log_scale;\n"
	"\n#line 1\n");

	str8 render_model = str8("render_model.frag.glsl");
	reload_shader(&ctx->os, render_model, (sptr)model_rc, ctx->arena);
	os_add_file_watch(&ctx->os, &ctx->arena, render_model, reload_shader, (sptr)model_rc);

	/* TODO(rnp): think about a reasonable region (probably min_coord -> max_coord + 10%) */
	f32 n = 1;
	f32 f = 20;
	f32 a = -f / (f - n);
	f32 b = -f * n / (f - n);
	m4 projection;
	projection.c[0] = (v4){{1, 0, 0,  0}};
	projection.c[1] = (v4){{0, 1, 0,  0}};
	projection.c[2] = (v4){{0, 0, a, -1}};
	projection.c[3] = (v4){{0, 0, b,  0}};
	glProgramUniformMatrix4fv(rc->shader, MODEL_RENDER_PROJ_MATRIX_LOC, 1, 0, projection.E);

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
	"\tf_texture_coordinate = 1 - v_texture_coordinate;\n"
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
	glGenVertexArrays(1, &rc->vao);
	glBindVertexArray(rc->vao);
	glGenBuffers(1, &rc->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, rc->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(overlay_vertices), overlay_vertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 4 * sizeof(f32), 0);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, 4 * sizeof(f32), (void *)(2 * sizeof(f32)));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glBindVertexArray(0);

	str8 render_overlay = str8("render_overlay.frag.glsl");
	reload_shader(&ctx->os, render_overlay, (sptr)overlay_rc, ctx->arena);
	os_add_file_watch(&ctx->os, &ctx->arena, render_overlay, reload_shader, (sptr)overlay_rc);

	ctx->unit_cube = load_render_model(ctx->arena, "unit_cube_positions.bin",
	                                   "unit_cube_indices.bin", "unit_cube_normals.bin");
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
viewer_frame_step(ViewerContext *ctx, f32 dt)
{
	ctx->cycle_t += 0.25 * dt;
	if (ctx->cycle_t > 1) ctx->cycle_t -= 1;

	f32 angle = ctx->cycle_t * 2 * PI;
	ctx->camera_position.x =  ctx->camera_radius * cos_f32(angle);
	ctx->camera_position.z = -ctx->camera_radius * sin_f32(angle);
	ctx->camera_position.y =  ctx->camera_radius * tan_f32(ctx->camera_angle);

	//////////////
	// 3D Models
	f32 one = 1;
	glBindFramebuffer(GL_FRAMEBUFFER, ctx->output_target.fb);
	glClearNamedFramebufferfv(ctx->output_target.fb, GL_COLOR, 0, OUTPUT_BG_CLEAR_COLOUR.E);
	glClearNamedFramebufferfv(ctx->output_target.fb, GL_DEPTH, 0, &one);
	glViewport(0, 0, ctx->output_target.size.w, ctx->output_target.size.h);

	glUseProgram(ctx->model_render_context.shader);

	v3 camera = ctx->camera_position;
	set_camera(ctx->model_render_context.shader, MODEL_RENDER_VIEW_MATRIX_LOC,
	           camera, v3_normalize(v3_sub(camera, (v3){0})), (v3){{0, 1, 0}});

	f32 scale = 2;
	m4 model_transform;
	model_transform.c[0] = (v4){{scale, 0,     0,     0}};
	model_transform.c[1] = (v4){{0,     scale, 0,     0}};
	model_transform.c[2] = (v4){{0,     0,     scale, 0}};
	model_transform.c[3] = (v4){{0,     0,     0,     1}};
	glProgramUniformMatrix4fv(ctx->model_render_context.shader, MODEL_RENDER_MODEL_MATRIX_LOC,
	                          1, 0, model_transform.E);

	glBindVertexArray(ctx->unit_cube.vao);
	glDrawElements(GL_TRIANGLES, ctx->unit_cube.elements, GL_UNSIGNED_SHORT,
	               (void *)ctx->unit_cube.elements_offset);

	////////////////
	// UI Overlay
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

	glfwSwapBuffers(ctx->window);
	glfwPollEvents();
}
