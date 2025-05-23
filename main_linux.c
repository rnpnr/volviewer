/* See LICENSE for license details. */
#include "compiler.h"

#if !OS_LINUX
#error This file is only meant to be compiled for Linux
#endif

#include "os_linux.c"
#include "common.c"

function void
dispatch_file_watch_events(OS *os, Arena arena)
{
	FileWatchContext *fwctx = &os->file_watch_context;
	u8 *mem     = arena_alloc(&arena, 4096, 16, 1);
	Stream path = stream_alloc(&arena, 256);
	struct inotify_event *event;

	sz rlen;
	while ((rlen = read(fwctx->handle, mem, 4096)) > 0) {
		for (u8 *data = mem; data < mem + rlen; data += sizeof(*event) + event->len) {
			event = (struct inotify_event *)data;
			for (u32 i = 0; i < fwctx->count; i++) {
				FileWatchDirectory *dir = fwctx->data + i;
				if (event->wd != dir->handle)
					continue;

				str8 file = c_str_to_str8(event->name);
				u64  hash = str8_hash(file);
				for (u32 i = 0; i < dir->count; i++) {
					FileWatch *fw = dir->data + i;
					if (fw->hash == hash) {
						stream_append_str8s(&path, dir->name, str8("/"), file);
						stream_append_byte(&path, 0);
						stream_commit(&path, -1);
						fw->callback(os, stream_to_str8(&path),
						             fw->user_data, arena);
						stream_reset(&path, 0);
						break;
					}
				}
			}
		}
	}
}

extern s32
main(void)
{
	Arena memory       = os_alloc_arena(MB(16));
	ViewerContext *ctx = push_struct(&memory, ViewerContext);
	ctx->arena         = memory;

	#define X(name) ctx->os.name = os_ ## name;
	OS_FNS
	#undef X

	ctx->os.file_watch_context.handle = inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
	ctx->os.error_handle              = STDERR_FILENO;

	init_viewer(ctx);

	struct pollfd fds[1] = {{0}};
	fds[0].fd     = ctx->os.file_watch_context.handle;
	fds[0].events = POLLIN;

	while (!ctx->should_exit) {
		poll(fds, countof(fds), 0);
		if (fds[0].revents & POLLIN)
			dispatch_file_watch_events(&ctx->os, ctx->arena);
		viewer_frame_step(ctx);
	}
}
