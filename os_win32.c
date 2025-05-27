/* See LICENSE for license details. */
#define OS_PATH_SEPARATOR_CHAR '\\'
#define OS_PATH_SEPARATOR      "\\"

#include "util.h"

#define STD_INPUT_HANDLE  -10
#define STD_OUTPUT_HANDLE -11
#define STD_ERROR_HANDLE  -12

#define PAGE_READWRITE 0x04
#define MEM_COMMIT     0x1000
#define MEM_RESERVE    0x2000
#define MEM_RELEASE    0x8000

#define GENERIC_WRITE  0x40000000
#define GENERIC_READ   0x80000000

#define FILE_SHARE_READ            0x00000001
#define FILE_MAP_ALL_ACCESS        0x000F001F
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_OVERLAPPED       0x40000000

#define FILE_NOTIFY_CHANGE_LAST_WRITE 0x00000010

#define FILE_ACTION_MODIFIED 0x00000003

#define CREATE_ALWAYS  2
#define OPEN_EXISTING  3

#define THREAD_SET_LIMITED_INFORMATION 0x0400

/* NOTE: this is packed because the w32 api designers are dumb and ordered the members
 * incorrectly. They worked around it be making the ft* members a struct {u32, u32} which
 * is aligned on a 4-byte boundary. Then in their documentation they explicitly tell you not
 * to cast to u64 because "it can cause alignment faults on 64-bit Windows" - go figure */
typedef struct __attribute__((packed)) {
	u32 dwFileAttributes;
	u64 ftCreationTime;
	u64 ftLastAccessTime;
	u64 ftLastWriteTime;
	u32 dwVolumeSerialNumber;
	u32 nFileSizeHigh;
	u32 nFileSizeLow;
	u32 nNumberOfLinks;
	u32 nFileIndexHigh;
	u32 nFileIndexLow;
} w32_file_info;

typedef struct {
	u32 next_entry_offset;
	u32 action;
	u32 filename_size;
	u16 filename[];
} w32_file_notify_info;

typedef struct {
	uptr internal, internal_high;
	union {
		struct {u32 off, off_high;};
		sptr pointer;
	};
	sptr event_handle;
} w32_overlapped;

typedef struct {
	sptr io_completion_handle;
	u64  timer_start_time;
	u64  timer_frequency;
} w32_context;

typedef enum {
	W32_IO_FILE_WATCH,
	W32_IO_PIPE,
} W32_IO_Event;

typedef struct {
	u64  tag;
	sptr context;
} w32_io_completion_event;

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(sptr);
W32(sptr)   CreateFileA(c8 *, u32, u32, void *, u32, u32, void *);
W32(sptr)   CreateFileMappingA(sptr, void *, u32, u32, u32, c8 *);
W32(sptr)   CreateIoCompletionPort(sptr, sptr, uptr, u32);
W32(sptr)   CreateThread(sptr, uz, sptr, sptr, u32, u32 *);
W32(void)   ExitProcess(s32);
W32(b32)    GetFileInformationByHandle(sptr, void *);
W32(s32)    GetLastError(void);
W32(b32)    GetQueuedCompletionStatus(sptr, u32 *, uptr *, w32_overlapped **, u32);
W32(sptr)   GetStdHandle(s32);
W32(void)   GetSystemInfo(void *);
W32(void *) MapViewOfFile(sptr, u32, u32, u32, u64);
W32(b32)    ReadDirectoryChangesW(sptr, u8 *, u32, b32, u32, u32 *, void *, void *);
W32(b32)    ReadFile(sptr, u8 *, s32, s32 *, void *);
W32(b32)    ReleaseSemaphore(sptr, s64, s64 *);
W32(s32)    SetThreadDescription(sptr, u16 *);
W32(b32)    WaitOnAddress(void *, void *, uz, u32);
W32(s32)    WakeByAddressAll(void *);
W32(b32)    WriteFile(sptr, u8 *, s32, s32 *, void *);
W32(void *) VirtualAlloc(u8 *, sz, u32, u32);
W32(b32)    VirtualFree(u8 *, sz, u32);

function OS_WRITE_FILE_FN(os_write_file)
{
	s32 wlen = 0;
	if (raw.len > 0 && raw.len <= U32_MAX) WriteFile(file, raw.data, raw.len, &wlen, 0);
	return raw.len == wlen;
}

function void __attribute__((noreturn))
os_exit(s32 code)
{
	ExitProcess(code);
	unreachable();
}

function void __attribute__((noreturn))
os_fatal(str8 msg)
{
	os_write_file(GetStdHandle(STD_ERROR_HANDLE), msg);
	os_exit(1);
	unreachable();
}

function OS_ALLOC_ARENA_FN(os_alloc_arena)
{
	Arena result = {0};

	struct {
		u16  architecture;
		u16  _pad1;
		u32  page_size;
		sz   minimum_application_address;
		sz   maximum_application_address;
		u64  active_processor_mask;
		u32  number_of_processors;
		u32  processor_type;
		u32  allocation_granularity;
		u16  processor_level;
		u16  processor_revision;
	} info;

	GetSystemInfo(&info);

	if (capacity % info.page_size != 0)
		capacity += (info.page_size - capacity % info.page_size);

	void *beg = VirtualAlloc(0, capacity, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
	if (beg) {
		result.beg = beg;
		result.end = result.beg + capacity;
	}
	return result;
}

function OS_READ_WHOLE_FILE_FN(os_read_whole_file)
{
	str8 result = str8("");

	w32_file_info fileinfo;
	sptr h = CreateFileA(file, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
	if (h >= 0 && GetFileInformationByHandle(h, &fileinfo)) {
		sz filesize  = (sz)fileinfo.nFileSizeHigh << 32;
		filesize    |= (sz)fileinfo.nFileSizeLow;
		result       = str8_alloc(arena, filesize);

		assert(filesize <= (sz)U32_MAX);

		s32 rlen;
		if (!ReadFile(h, result.data, result.len, &rlen, 0) || rlen != result.len)
			result = str8("");
	}
	if (h >= 0) CloseHandle(h);

	return result;
}

function OS_WRITE_NEW_FILE_FN(os_write_new_file)
{
	enum { CHUNK_SIZE = GB(2) };

	b32 result = 0;
	sptr h = CreateFileA(fname, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (h >= 0) {
		while (raw.len > 0) {
			str8 chunk  = raw;
			chunk.len = MIN(chunk.len, CHUNK_SIZE);
			result    = os_write_file(h, chunk);
			if (!result) break;
			raw = str8_cut_head(raw, chunk.len);
		}
		CloseHandle(h);
	}
	return result;
}

function OS_ADD_FILE_WATCH_FN(os_add_file_watch)
{
	str8 directory  = path;
	directory.len = str8_scan_backwards(path, '\\');
	if (directory.len < 0) {
		directory = str8(".");
	} else {
		path = str8_cut_head(path, directory.len + 1);
	}

	u64 hash = str8_hash(directory);
	FileWatchContext *fwctx = &os->file_watch_context;
	FileWatchDirectory *dir = lookup_file_watch_directory(fwctx, hash);
	if (!dir) {
		dir = da_push(a, fwctx);
		dir->hash   = hash;
		dir->name   = push_str8_zero(a, directory);
		dir->handle = CreateFileA((c8 *)dir->name.data, GENERIC_READ, FILE_SHARE_READ, 0,
		                          OPEN_EXISTING,
		                          FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED, 0);

		w32_context *ctx = (w32_context *)os->context;
		w32_io_completion_event *event = push_struct(a, typeof(*event));
		event->tag     = W32_IO_FILE_WATCH;
		event->context = (sptr)dir;
		CreateIoCompletionPort(dir->handle, ctx->io_completion_handle, (uptr)event, 0);

		dir->buffer.beg = arena_alloc(a, 4096 + sizeof(w32_overlapped), 64, 1);
		dir->buffer.end = dir->buffer.beg + 4096 + sizeof(w32_overlapped);
		w32_overlapped *overlapped = (w32_overlapped *)(dir->buffer.beg + 4096);
		zero_struct(overlapped);

		ReadDirectoryChangesW(dir->handle, dir->buffer.beg, 4096, 0,
		                      FILE_NOTIFY_CHANGE_LAST_WRITE, 0, overlapped, 0);
	}

	FileWatch *fw = da_push(a, dir);
	fw->user_data = user_data;
	fw->callback  = callback;
	fw->hash      = str8_hash(path);
}
