/* See LICENSE for license details. */
#include "compiler.h"

#if !OS_WINDOWS
#error This file is only meant to be compiled for Win32
#endif

#include "os_win32.c"
#include "common.c"

function void
dispatch_file_watch(OS *os, FileWatchDirectory *fw_dir, u8 *buf, Arena arena)
{
	s64 offset = 0;
	Arena start_arena = arena;
	w32_file_notify_info *fni = (w32_file_notify_info *)buf;
	do {
		arena = start_arena;

		Stream path = {.data = arena_commit(&arena, KB(1)), .cap = KB(1)};

		if (fni->action != FILE_ACTION_MODIFIED) {
			stream_append_str8(&path, str8("unknown file watch event: "));
			stream_append_u64(&path, fni->action);
			stream_append_byte(&path, '\n');
			os_write_file(os->error_handle, stream_to_str8(&path));
			stream_reset(&path, 0);
		}

		stream_append_str8(&path, fw_dir->name);
		stream_append_byte(&path, '\\');

		str8 file_name = str16_to_str8(&arena, (str16){.data = fni->filename,
		                                               .len  = fni->filename_size / 2});
		stream_append_str8(&path, file_name);
		stream_append_byte(&path, 0);
		stream_commit(&path, -1);

		u64 hash = str8_hash(file_name);
		for (u32 i = 0; i < fw_dir->count; i++) {
			FileWatch *fw = fw_dir->data + i;
			if (fw->hash == hash) {
				fw->callback(os, stream_to_str8(&path), fw->user_data, arena);
				break;
			}
		}

		offset = fni->next_entry_offset;
		fni    = (w32_file_notify_info *)((u8 *)fni + offset);
	} while (offset);
}

function void
clear_io_queue(OS *os, Arena arena)
{
	w32_context *ctx = (w32_context *)os->context;

	sptr handle = ctx->io_completion_handle;
	w32_overlapped *overlapped;
	u32  bytes_read;
	uptr user_data;
	while (GetQueuedCompletionStatus(handle, &bytes_read, &user_data, &overlapped, 0)) {
		w32_io_completion_event *event = (w32_io_completion_event *)user_data;
		switch (event->tag) {
		case W32_IO_FILE_WATCH: {
			FileWatchDirectory *dir = (FileWatchDirectory *)event->context;
			dispatch_file_watch(os, dir, dir->buffer.beg, arena);
			zero_struct(overlapped);
			ReadDirectoryChangesW(dir->handle, dir->buffer.beg, 4096, 0,
			                      FILE_NOTIFY_CHANGE_LAST_WRITE, 0, overlapped, 0);
		} break;
		case W32_IO_PIPE: break;
		}
	}
}

extern s32
main(void)
{
	Arena memory       = os_alloc_arena(GB(1));
	ViewerContext *ctx = push_struct(&memory, ViewerContext);
	ctx->arena         = memory;

	w32_context w32_ctx = {0};
	w32_ctx.io_completion_handle = CreateIoCompletionPort(INVALID_FILE, 0, 0, 0);

	ctx->os.context      = (sptr)&w32_ctx;
	ctx->os.error_handle = GetStdHandle(STD_ERROR_HANDLE);

	init_viewer(ctx);

	while (!ctx->should_exit) {
		clear_io_queue(&ctx->os, ctx->arena);
		viewer_frame_step(ctx, get_frame_time_step(ctx));
		glfwSwapBuffers(ctx->window);
		glfwPollEvents();
	}
}
