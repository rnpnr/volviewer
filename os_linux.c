/* See LICENSE for license details. */
#define OS_PATH_SEPARATOR_CHAR '/'
#define OS_PATH_SEPARATOR      "/"

#include "util.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

function OS_WRITE_FILE_FN(os_write_file)
{
	while (raw.len) {
		sz r = write(file, raw.data, raw.len);
		if (r < 0) return 0;
		raw = str8_cut_head(raw, r);
	}
	return 1;
}

function void __attribute__((noreturn))
os_exit(s32 code)
{
	_exit(code);
	unreachable();
}

function void __attribute__((noreturn))
os_fatal(str8 msg)
{
	os_write_file(STDERR_FILENO, msg);
	os_exit(1);
	unreachable();
}

function OS_ALLOC_ARENA_FN(os_alloc_arena)
{
	Arena result = {0};
	sz pagesize = sysconf(_SC_PAGESIZE);
	if (capacity % pagesize != 0)
		capacity += pagesize - capacity % pagesize;

	void *beg = mmap(0, capacity, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (beg != MAP_FAILED) {
		result.beg = beg;
		result.end = result.beg + capacity;
	}
	return result;
}

function OS_READ_WHOLE_FILE_FN(os_read_whole_file)
{
	str8 result = str8("");

	struct stat sb;
	s32 fd = open(file, O_RDONLY);
	if (fd >= 0 && fstat(fd, &sb) >= 0) {
		result = str8_alloc(arena, sb.st_size);
		sz rlen = read(fd, result.data, result.len);
		if (rlen != result.len)
			result = str8("");
	}
	if (fd >= 0) close(fd);

	return result;
}

function OS_WRITE_NEW_FILE_FN(os_write_new_file)
{
	b32 result = 0;
	sptr fd = open(fname, O_WRONLY|O_TRUNC|O_CREAT, 0600);
	if (fd != INVALID_FILE) {
		result = os_write_file(fd, raw);
		close(fd);
	}
	return result;
}

function OS_ADD_FILE_WATCH_FN(os_add_file_watch)
{
	str8 directory = path;
	directory.len  = str8_scan_backwards(path, '/');
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
		s32 mask    = IN_MOVED_TO|IN_CLOSE_WRITE;
		dir->handle = inotify_add_watch(fwctx->handle, (c8 *)dir->name.data, mask);
	}

	FileWatch *fw = da_push(a, dir);
	fw->user_data = user_data;
	fw->callback  = callback;
	fw->hash      = str8_hash(path);
}
