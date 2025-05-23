/* See LICENSE for license details. */
#define GLAD_GL_IMPLEMENTATION
#include "glad/gl.h"
#include "GLFW/glfw3.h"

#define static_path_join(a, b) (a OS_PATH_SEPARATOR b)

#define VIEWER_RENDER_DYNAMIC_RANGE_LOC (0)
#define VIEWER_RENDER_THRESHOLD_LOC     (1)
#define VIEWER_RENDER_GAMMA_LOC         (2)
#define VIEWER_RENDER_LOG_SCALE_LOC     (3)

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
	ctx->os->write_file(ctx->os->error_handle, stream_to_str8(e));
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
		os->write_file(os->error_handle, stream_to_str8(&buf));

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
		os->write_file(os->error_handle, stream_to_str8(&buf));
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
		os->write_file(os->error_handle, stream_to_str8(&buf));
		LABEL_GL_OBJECT(GL_PROGRAM, result, label);
	}

	return result;
}

function FILE_WATCH_CALLBACK_FN(reload_render_shader)
{
	RenderContext *ctx = (typeof(ctx))user_data;

	local_persist str8 vertex = str8(""
	"#version 460 core\n"
	"\n"
	"layout(location = 0) in vec3 v_normal;\n"
	"layout(location = 1) in vec3 v_position;\n"
	"layout(location = 2) in vec3 v_texture_coordinate;\n"
	"layout(location = 3) in vec3 v_colour;\n"
	"layout(location = 4) in uint v_flags;\n"
	"\n"
	"layout(location = 0) out vec3 f_texture_coordinate;\n"
	"layout(location = 1) out vec3 f_colour;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tf_texture_coordinate = v_texture_coordinate;\n"
	"\tf_colour             = v_colour;\n"
	"\tgl_Position = vec4(v_position, 1);\n"
	"}\n");

	str8 header = push_str8(&tmp, str8(""
	"#version 460 core\n\n"
	"layout(location = 0) in  vec3 texture_coordinate;\n"
	"layout(location = 1) in  vec3 colour;\n"
	"layout(location = 0) out vec4 out_colour;\n\n"
	"layout(location = " str(VIEWER_RENDER_DYNAMIC_RANGE_LOC) ") uniform float u_db_cutoff = 60;\n"
	"layout(location = " str(VIEWER_RENDER_THRESHOLD_LOC)     ") uniform float u_threshold = 40;\n"
	"layout(location = " str(VIEWER_RENDER_GAMMA_LOC)         ") uniform float u_gamma     = 1;\n"
	"layout(location = " str(VIEWER_RENDER_LOG_SCALE_LOC)     ") uniform bool  u_log_scale;\n"
	"\n#line 1\n"));

	str8 fragment  = os->read_whole_file(&tmp, (c8 *)path.data);
	fragment.data -= header.len;
	fragment.len  += header.len;
	assert(fragment.data == header.data);
	u32 new_program = load_shader(os, tmp, vertex, fragment, path, str8("Render Shader"));
	if (new_program) {
		glDeleteProgram(ctx->shader);
		ctx->shader = new_program;
	}

	return 1;
}

function void
key_callback(GLFWwindow *window, s32 key, s32 scancode, s32 action, s32 modifiers)
{
	ViewerContext *ctx = glfwGetWindowUserPointer(window);
	if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		ctx->should_exit = 1;
}

function void
fb_callback(GLFWwindow *window, s32 w, s32 h)
{
	ViewerContext *ctx = glfwGetWindowUserPointer(window);
	ctx->window_size   = (sv2){.w = w, .h = h};
	glViewport(0, 0, w, h);
}

function void
init_viewer(ViewerContext *ctx)
{
	ctx->window_size  = (sv2){.w = 640, .h = 640};

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

	RenderContext *rc = &ctx->render_context;

	Vertex vertices[] = {
		{.position = {{-0.5, -0.5, 0.0}}, .colour = {{1.0, 0.0, 0.0}}},
		{.position = {{   0,  0.5, 0.0}}, .colour = {{0.0, 1.0, 0.0}}},
		{.position = {{ 0.5, -0.5, 0.0}}, .colour = {{0.0, 0.0, 1.0}}},
	};

	glGenVertexArrays(1, &rc->vao);
	glBindVertexArray(rc->vao);
	glGenBuffers(1, &rc->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, rc->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, normal));
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, position));
	glVertexAttribPointer(2, 3, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, texture_coordinate));
	glVertexAttribPointer(3, 3, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, colour));
	glVertexAttribPointer(4, 1, GL_INT,   0, sizeof(Vertex), (void *)offsetof(Vertex, flags));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);
	glEnableVertexAttribArray(4);

	str8 render = str8("render.frag.glsl");
	reload_render_shader(&ctx->os, render, (sptr)rc, ctx->arena);
	os_add_file_watch(&ctx->os, &ctx->arena, render, reload_render_shader, (sptr)rc);
}

function void
viewer_frame_step(ViewerContext *ctx)
{
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(ctx->render_context.shader);
	glBindVertexArray(ctx->render_context.vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glfwSwapBuffers(ctx->window);
	glfwPollEvents();
}
