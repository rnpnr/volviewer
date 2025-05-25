/* See LICENSE for license details. */
/* NOTE: inspired by nob: https://github.com/tsoding/nob.h */

/* TODO(rnp):
 * [ ]: bake shaders and font data into binary
 *      - for shaders there is a way of making a separate data section and referring
 *        to it with extern from the C source (bake both data and size)
 *      - use objcopy, maybe need linker script maybe command line flags for ld will work
 * [ ]: cross compile/override baked compiler
 */

#define COMMON_FLAGS "-std=c11", "-Wall", "-Iexternal/include"

#define OUTDIR    "out"
#define OUTPUT(s) OUTDIR "/" s

#include "compiler.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>

#define is_aarch64 ARCH_ARM64
#define is_amd64   ARCH_X64
#define is_unix    OS_LINUX
#define is_w32     OS_WINDOWS
#define is_clang   COMPILER_CLANG

#if OS_LINUX

  #include <errno.h>
  #include <string.h>
  #include <sys/select.h>
  #include <sys/wait.h>

  #include "os_linux.c"

  #define OS_MAIN "main_linux.c"

#elif OS_WINDOWS

  #include "os_win32.c"

  #define OS_MAIN "main_w32.c"

#else
  #error Unsupported Platform
#endif

#if COMPILER_CLANG
  #define COMPILER "clang"
#elif COMPILER_MSVC
  #define COMPILER "cl"
#else
  #define COMPILER "cc"
#endif

#define shift(list, count) ((count)--, *(list)++)

#define da_append_count(a, s, items, item_count) do { \
	da_reserve((a), (s), (s)->count + (item_count));                            \
	mem_copy((s)->data + (s)->count, (items), sizeof(*(items)) * (item_count)); \
	(s)->count += (item_count);                                                 \
} while (0)

#define cmd_append_count da_append_count
#define cmd_append(a, s, ...) da_append_count(a, s, ((char *[]){__VA_ARGS__}), \
                                             (sizeof((char *[]){__VA_ARGS__}) / sizeof(char *)))

typedef struct {
	char **data;
	sz     count;
	sz     capacity;
} CommandList;

typedef struct {
	b32   debug;
	b32   generic;
	b32   report;
	b32   sanitize;
} Options;

#define die(fmt, ...) die_("%s: " fmt, __FUNCTION__, ##__VA_ARGS__)
function void __attribute__((noreturn))
die_(char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	/* TODO(rnp): proper log */
	vfprintf(stderr, format, ap);
	va_end(ap);
	os_fatal(str8(""));
}

function b32
str8_contains(str8 s, u8 byte)
{
	b32 result = 0;
	for (sz i = 0 ; !result && i < s.len; i++)
		result |= s.data[i] == byte;
	return result;
}

function void
stream_push_command(Stream *s, CommandList *c)
{
	if (!s->errors) {
		for (sz i = 0; i < c->count; i++) {
			str8 item = c_str_to_str8(c->data[i]);
			if (item.len) {
				b32 escape = str8_contains(item, ' ') || str8_contains(item, '"');
				if (escape) stream_append_byte(s, '\'');
				stream_append_str8(s, item);
				if (escape) stream_append_byte(s, '\'');
				if (i != c->count - 1) stream_append_byte(s, ' ');
			}
		}
	}
}

#if OS_LINUX

function b32
os_rename_file(char *name, char *new)
{
	b32 result = rename(name, new) != -1;
	return result;
}

function b32
os_remove_file(char *name)
{
	b32 result = remove(name) != -1;
	return result;
}

function u64
os_get_filetime(char *file)
{
	struct stat sb;
	u64 result = (u64)-1;
	if (stat(file, &sb) != -1)
		result = sb.st_mtim.tv_sec;
	return result;
}

function sptr
os_spawn_process(CommandList *cmd, Stream sb)
{
	pid_t result = fork();
	switch (result) {
	case -1: die("failed to fork command: %s: %s\n", cmd->data[0], strerror(errno)); break;
	case  0: {
		if (execvp(cmd->data[0], cmd->data) == -1)
			die("failed to exec command: %s: %s\n", cmd->data[0], strerror(errno));
		unreachable();
	} break;
	}
	return (sptr)result;
}

function b32
os_wait_close_process(sptr handle)
{
	b32 result = 0;
	for (;;) {
		s32  status;
		sptr wait_pid = (sptr)waitpid(handle, &status, 0);
		if (wait_pid == -1)
			die("failed to wait on child process: %s\n", strerror(errno));
		if (wait_pid == handle) {
			if (WIFEXITED(status)) {
				status = WEXITSTATUS(status);
				/* TODO(rnp): logging */
				result = status == 0;
				break;
			}
			if (WIFSIGNALED(status)) {
				/* TODO(rnp): logging */
				result = 0;
				break;
			}
		} else {
			/* TODO(rnp): handle multiple children */
			InvalidCodePath;
		}
	}
	return result;
}

#elif OS_WINDOWS

enum {
	MOVEFILE_REPLACE_EXISTING = 0x01,
};

W32(b32) CreateProcessA(u8 *, u8 *, sptr, sptr, b32, u32, sptr, u8 *, sptr, sptr);
W32(b32) GetExitCodeProcess(iptr handle, u32 *);
W32(b32) GetFileTime(sptr, sptr, sptr, sptr);
W32(b32) MoveFileExA(c8 *, c8 *, u32);
W32(u32) WaitForSingleObject(sptr, u32);

function b32
os_rename_file(char *name, char *new)
{
	b32 result = MoveFileExA(name, new, MOVEFILE_REPLACE_EXISTING) != 0;
	return result;
}

function b32
os_remove_file(char *name)
{
	b32 result = DeleteFileA(name);
	return result;
}

function u64
os_get_filetime(char *file)
{
	u64 result = (u64)-1;
	iptr h = CreateFileA(file, 0, 0, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
	if (h != INVALID_FILE) {
		struct { u32 low, high; } w32_filetime;
		GetFileTime(h, 0, 0, (iptr)&w32_filetime);
		result = (u64)w32_filetime.high << 32ULL | w32_filetime.low;
		CloseHandle(h);
	}
	return result;
}

function sptr
os_spawn_process(CommandList *cmd, Stream sb)
{
	struct {
		u32 cb;
		u8 *reserved, *desktop, *title;
		u32 x, y, x_size, y_size, x_count_chars, y_count_chars;
		u32 fill_attr, flags;
		u16 show_window, reserved_2;
		u8 *reserved_3;
		sptr std_input, std_output, std_error;
	} w32_startup_info = {
		.cb = sizeof(w32_startup_info),
		.flags = 0x100,
		.std_input  = GetStdHandle(STD_INPUT_HANDLE),
		.std_output = GetStdHandle(STD_OUTPUT_HANDLE),
		.std_error  = GetStdHandle(STD_ERROR_HANDLE),
	};

	struct {
		iptr phandle, thandle;
		u32  pid, tid;
	} w32_process_info = {0};

	/* TODO(rnp): warn if we need to clamp last string */
	sb.widx = MIN(sb.widx, KB(32) - 1);
	if (sb.widx < sb.cap) sb.data[sb.widx]     = 0;
	else                  sb.data[sb.widx - 1] = 0;

	iptr result = INVALID_FILE;
	if (CreateProcessA(0, sb.data, 0, 0, 1, 0, 0, 0, (iptr)&w32_startup_info,
	                   (iptr)&w32_process_info))
	{
		CloseHandle(w32_process_info.thandle);
		result = w32_process_info.phandle;
	}
	return result;
}

function b32
os_wait_close_process(iptr handle)
{
	b32 result = WaitForSingleObject(handle, -1) != 0xFFFFFFFFUL;
	if (result) {
		u32 status;
		GetExitCodeProcess(handle, &status);
		result = status == 0;
	}
	CloseHandle(handle);
	return result;
}

#endif

#define needs_rebuild(b, ...) needs_rebuild_(b, ((char *[]){__VA_ARGS__}), \
                                             (sizeof((char *[]){__VA_ARGS__}) / sizeof(char *)))
function b32
needs_rebuild_(char *binary, char *deps[], sz deps_count)
{
	u64 binary_filetime = os_get_filetime(binary);
	b32 result = binary_filetime == (u64)-1;
	for (sz i = 0; i < deps_count; i++) {
		u64 filetime = os_get_filetime(deps[i]);
		result |= (filetime == (u64)-1) | (filetime > binary_filetime);
	}
	return result;
}

function b32
run_synchronous(Arena a, CommandList *command)
{
	Stream sb = arena_stream(a);
	stream_push_command(&sb, command);
	printf("%.*s\n", (s32)sb.widx, sb.data);
	return os_wait_close_process(os_spawn_process(command, sb));
}

function void
check_rebuild_self(Arena arena, s32 argc, char *argv[])
{
	char *binary = shift(argv, argc);
	if (needs_rebuild(binary, __FILE__, "os_win32.c", "os_linux.c", "util.c", "util.h")) {
		Stream name_buffer = arena_stream(arena);
		stream_append_str8s(&name_buffer, c_str_to_str8(binary), str8(".old"));
		char *old_name = (char *)arena_stream_commit_zero(&arena, &name_buffer).data;

		if (!os_rename_file(binary, old_name))
			die("failed to move: %s -> %s\n", binary, old_name);

		CommandList c = {0};
		cmd_append(&arena, &c, COMPILER, "-march=native", "-O3", COMMON_FLAGS);
		cmd_append(&arena, &c, "-Wno-unused-function", __FILE__, "-o", binary, (void *)0);
		if (!run_synchronous(arena, &c)) {
			os_rename_file(old_name, binary);
			die("failed to rebuild self\n");
		}
		os_remove_file(old_name);

		c.count = 0;
		cmd_append(&arena, &c, binary);
		cmd_append_count(&arena, &c, argv, argc);
		cmd_append(&arena, &c, (void *)0);
		if (!run_synchronous(arena, &c))
			os_exit(1);

		os_exit(0);
	}
}

function b32
str8_equal(str8 a, str8 b)
{
	b32 result = a.len == b.len;
	for (sz i = 0; result && i < a.len; i++)
		result = a.data[i] == b.data[i];
	return result;
}

function void
usage(char *argv0)
{
	die("%s [--debug] [--report] [--sanitize]\n"
	    "    --debug:       dynamically link and build with debug symbols\n"
	    "    --generic:     compile for a generic target (x86-64-v3 or armv8 with NEON)\n"
	    "    --report:      print compilation stats (clang only)\n"
	    "    --sanitize:    build with ASAN and UBSAN\n"
	    , argv0);
}

function Options
parse_options(s32 argc, char *argv[])
{
	Options result = {0};

	char *argv0 = shift(argv, argc);
	while (argc > 0) {
		char *arg = shift(argv, argc);
		str8 str    = c_str_to_str8(arg);
		if (str8_equal(str, str8("--debug"))) {
			result.debug = 1;
		} else if (str8_equal(str, str8("--generic"))) {
			result.generic = 1;
		} else if (str8_equal(str, str8("--report"))) {
			result.report = 1;
		} else if (str8_equal(str, str8("--sanitize"))) {
			result.sanitize = 1;
		} else {
			usage(argv0);
		}
	}

	return result;
}

function CommandList
cmd_base(Arena *a, Options *o)
{
	CommandList result = {0};
	cmd_append(a, &result, COMPILER);

	/* TODO(rnp): support cross compiling with clang */
	if (!o->generic)     cmd_append(a, &result, "-march=native");
	else if (is_amd64)   cmd_append(a, &result, "-march=x86-64-v3");
	else if (is_aarch64) cmd_append(a, &result, "-march=armv8");

	cmd_append(a, &result, COMMON_FLAGS);

	if (o->debug) cmd_append(a, &result, "-O0", "-D_DEBUG", "-Wno-unused-function");
	else          cmd_append(a, &result, "-O3");

	if (is_w32 && is_clang) cmd_append(a, &result, "-fms-extensions");

	if (o->debug && is_unix) cmd_append(a, &result, "-gdwarf-4");

	if (o->sanitize) cmd_append(a, &result, "-fsanitize=address,undefined");

	if (o->report) {
		if (is_clang) cmd_append(a, &result, "-fproc-stat-report");
		else printf("warning: timing not supported with this compiler\n");
		/* TODO(rnp): basic timing */
	}

	return result;
}

/* NOTE(rnp): produce pdbs on w32 */
function void
cmd_pdb(Arena *a, CommandList *cmd)
{
	if (is_w32 && is_clang)
		cmd_append(a, cmd, "-fuse-ld=lld", "-g", "-gcodeview", "-Wl,--pdb=");
}

/* NOTE(rnp): gcc requires these to appear at the end for no reason at all */
function void
cmd_append_ldflags(Arena *a, CommandList *cc, b32 shared)
{
	cmd_pdb(a, cc);
	if (is_w32) cmd_append(a, cc, "-lgdi32", "-lwinmm");
}

extern s32
main(s32 argc, char *argv[])
{
	Arena arena = os_alloc_arena(MB(8));
	check_rebuild_self(arena, argc, argv);

	Options options = parse_options(argc, argv);

	CommandList c = cmd_base(&arena, &options);
	if (is_unix) cmd_append(&arena, &c, "-D_GLFW_X11");
	cmd_append(&arena, &c,  "-I external/include", "-I external/glfw/include");

	cmd_append(&arena, &c, OS_MAIN, "external/rglfw.c", "-o", "volviewer");
	cmd_append_ldflags(&arena, &c, options.debug);
	cmd_append(&arena, &c, (void *)0);

	return !run_synchronous(arena, &c);
}
