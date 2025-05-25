/* See LICENSE for license details. */
#ifndef _UTIL_H_
#define _UTIL_H_

#include <stddef.h>
#include <stdint.h>

#ifndef asm
#define asm __asm__
#endif

#ifndef typeof
#define typeof __typeof__
#endif

#define alignof _Alignof

#if COMPILER_CLANG || COMPILER_GCC
  #define force_inline inline __attribute__((always_inline))
  #define unreachable() __builtin_unreachable()
#elif COMPILER_MSVC
  #define force_inline  __forceinline
  #define unreachable() __assume(0)
#endif

#if COMPILER_MSVC || (COMPILER_CLANG && OS_WINDOWS)
  #pragma section(".rdata$", read)
  #define read_only __declspec(allocate(".rdata$"))
#elif COMPILER_CLANG
  #define read_only __attribute__((section(".rodata")))
#elif COMPILER_GCC
  /* TODO(rnp): how do we do this with gcc, putting it in rodata causes warnings and writing to
   * it doesn't cause a fault */
  #define read_only
#endif

/* TODO(rnp): msvc probably won't build this but there are other things preventing that as well */
#define sqrt_f32(a)     __builtin_sqrtf(a)
#define atan2_f32(y, x) __builtin_atan2f(y, x)
#define sin_f32(x)      __builtin_sinf(x)
#define cos_f32(x)      __builtin_cosf(x)
#define tan_f32(x)      __builtin_tanf(x)

#if ARCH_ARM64
  /* TODO? debuggers just loop here forever and need a manual PC increment (step over) */
  #define debugbreak() asm volatile ("brk 0xf000")
#elif ARCH_X64
  #define debugbreak() asm volatile ("int3; nop")
#endif

#ifdef _DEBUG
	#ifdef OS_WINDOWS
		#define DEBUG_EXPORT __declspec(dllexport)
	#else
		#define DEBUG_EXPORT
	#endif
	#define DEBUG_DECL(a) a
	#define assert(c) do { if (!(c)) debugbreak(); } while (0)
#else
	#define DEBUG_EXPORT function
	#define DEBUG_DECL(a)
	#define assert(c)
#endif

#define InvalidCodePath assert(0)
#define InvalidDefaultCase default: assert(0); break

#define function      static
#define global        static
#define local_persist static

#define static_assert _Static_assert

/* NOTE: garbage to get the prepocessor to properly stringize the value of a macro */
#define str_(...) #__VA_ARGS__
#define str(...) str_(__VA_ARGS__)

#define countof(a)       (sizeof(a) / sizeof(*a))
#define ARRAY_COUNT(a)   (sizeof(a) / sizeof(*a))
#define ABS(x)           ((x) < 0 ? (-x) : (x))
#define BETWEEN(x, a, b) ((x) >= (a) && (x) <= (b))
#define CLAMP(x, a, b)   ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define CLAMP01(x)       CLAMP(x, 0, 1)
#define ISPOWEROF2(a)    (((a) & ((a) - 1)) == 0)
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define MAX(a, b)        ((a) > (b) ? (a) : (b))
#define ORONE(x)         ((x)? (x) : 1)
#define SIGN(x)          ((x) < 0? -1 : 1)
#define SWAP(a, b)       {typeof(a) __tmp = (a); (a) = (b); (b) = __tmp;}

/* NOTE(rnp): no guarantees about actually getting an element */
#define SLLPop(list)     list; list = list ? list->next : 0
#define SLLPush(v, list) do { \
	(v)->next = (list); \
	(list)    = v;      \
} while (0)

#define DLLPushDown(v, list) do { \
	(v)->next = (list);                   \
	if ((v)->next) (v)->next->prev = (v); \
	(list) = (v);                         \
} while (0)

#define DLLRemove(v) do { \
	if ((v)->next) (v)->next->prev = (v)->prev; \
	if ((v)->prev) (v)->prev->next = (v)->next; \
} while (0)

#define KB(a)            ((u64)(a) << 10ULL)
#define MB(a)            ((u64)(a) << 20ULL)
#define GB(a)            ((u64)(a) << 30ULL)

#define I32_MAX          (0x7FFFFFFFL)
#define U32_MAX          (0xFFFFFFFFUL)
#define F32_INFINITY     (__builtin_inff())

#define PI (3.14159265358979323846)

#define INVALID_FILE (-1)

typedef char      c8;
typedef uint8_t   u8;
typedef int16_t   s16;
typedef uint16_t  u16;
typedef int32_t   s32;
typedef uint32_t  u32;
typedef int64_t   s64;
typedef uint64_t  u64;
typedef uint32_t  b32;
typedef float     f32;
typedef double    f64;
typedef ptrdiff_t sz;
typedef size_t    uz;
typedef ptrdiff_t sptr;
typedef size_t    uptr;

typedef struct { u8 *beg, *end; } Arena;

typedef struct { sz len; u8 *data; } str8;
#define str8(s) (str8){.len = ARRAY_COUNT(s) - 1, .data = (u8 *)s}

typedef struct { sz len; u16 *data; } str16;

typedef struct { u32 cp, consumed; } UnicodeDecode;

typedef union {
	struct { s32 x, y; };
	struct { s32 w, h; };
	s32 E[2];
} sv2;

typedef union {
	struct { s32 x, y, z; };
	struct { s32 w, h, d; };
	sv2 xy;
	s32 E[3];
} sv3;

typedef union {
	struct { u32 x, y; };
	struct { u32 w, h; };
	u32 E[2];
} uv2;

typedef union {
	struct { u32 x, y, z; };
	struct { u32 w, h, d; };
	uv2 xy;
	u32 E[3];
} uv3;

typedef union {
	struct { u32 x, y, z, w; };
	struct { uv3 xyz; u32 _w; };
	u32 E[4];
} uv4;

typedef union {
	struct { f32 x, y; };
	struct { f32 w, h; };
	f32 E[2];
} v2;

typedef union {
	struct { f32 x, y, z; };
	struct { f32 w, h, d; };
	f32 E[3];
} v3;

typedef union {
	struct { f32 x, y, z, w; };
	struct { f32 r, g, b, a; };
	struct { v3 xyz; f32 _1; };
	struct { f32 _2; v3 yzw; };
	struct { v2 xy, zw; };
	f32 E[4];
} v4;

#define XZ(v) (v2){.x = v.x, .y = v.z}
#define YZ(v) (v2){.x = v.y, .y = v.z}
#define XY(v) (v2){.x = v.x, .y = v.y}

typedef union {
	struct { v4 x, y, z, w; };
	v4  c[4];
	f32 E[16];
} m4;

typedef struct { v2 pos, size; } Rect;
#define INVERTED_INFINITY_RECT (Rect){.pos  = {.x = -F32_INFINITY, .y = -F32_INFINITY}, \
                                      .size = {.x = -F32_INFINITY, .y = -F32_INFINITY}}

typedef struct {
	u8   *data;
	u32   widx;
	u32   cap;
	b32   errors;
} Stream;

typedef struct OS OS;

#define FILE_WATCH_CALLBACK_FN(name) b32 name(OS *os, str8 path, sptr user_data, Arena tmp)
typedef FILE_WATCH_CALLBACK_FN(file_watch_callback);

typedef struct {
	sptr user_data;
	u64  hash;
	file_watch_callback *callback;
} FileWatch;

typedef struct {
	u64       hash;
	sptr      handle;
	str8      name;

	FileWatch *data;
	sz         count;
	sz         capacity;
	Arena      buffer;
} FileWatchDirectory;

typedef struct {
	FileWatchDirectory *data;
	sz    count;
	sz    capacity;
	sptr  handle;
} FileWatchContext;

#define OS_ALLOC_ARENA_FN(name) Arena name(sz capacity)
typedef OS_ALLOC_ARENA_FN(os_alloc_arena_fn);

#define OS_ADD_FILE_WATCH_FN(name) void name(OS *os, Arena *a, str8 path, \
                                             file_watch_callback *callback, sptr user_data)
typedef OS_ADD_FILE_WATCH_FN(os_add_file_watch_fn);

#define OS_READ_WHOLE_FILE_FN(name) str8 name(Arena *arena, char *file)
typedef OS_READ_WHOLE_FILE_FN(os_read_whole_file_fn);

#define OS_WRITE_NEW_FILE_FN(name) b32 name(char *fname, str8 raw)
typedef OS_WRITE_NEW_FILE_FN(os_write_new_file_fn);

#define OS_WRITE_FILE_FN(name) b32 name(sptr file, str8 raw)
typedef OS_WRITE_FILE_FN(os_write_file_fn);

struct OS {
	FileWatchContext file_watch_context;
	sptr             context;
	sptr             error_handle;
};

typedef struct {
	u32 shader;
	u32 vao;
	u32 vbo;
} RenderContext;

typedef struct {
	u32 fb;
	u32 textures[2];
	sv2 size;
} RenderTarget;

typedef struct {
	sptr elements_offset;
	s32  elements;
	u32  buffer;
	u32  vao;
} RenderModel;

typedef struct {
	Arena arena;
	OS    os;

	RenderContext model_render_context;
	RenderContext overlay_render_context;

	RenderTarget multisample_target;
	RenderTarget output_target;
	RenderModel  unit_cube;

	sv2 window_size;

	b32 demo_mode;
	f32 cycle_t;
	f32 camera_angle;
	f32 camera_fov;
	f32 camera_radius;
	v3  camera_position;

	u32 output_frames_count;

	f32 last_time;
	f32 input_dt;

	b32 should_exit;

	void *window;
} ViewerContext;

#define LABEL_GL_OBJECT(type, id, s) {str8 _s = (s); glObjectLabel(type, id, _s.len, (c8 *)_s.data);}

#include "util.c"

#endif /* _UTIL_H_ */
