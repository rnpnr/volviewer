/* See LICENSE for license details. */
#define zero_struct(s) mem_clear(s, 0, sizeof(*s))
function void *
mem_clear(void *restrict p_, u8 c, uz size)
{
	u8 *p = p_;
	while (size) p[--size] = c;
	return p;
}

function void
mem_copy(void *restrict dest, void *restrict src, uz n)
{
	u8 *s = src, *d = dest;
	for (; n; n--) *d++ = *s++;
}

function u8 *
arena_commit(Arena *a, sz size)
{
	assert(a->end - a->beg >= size);
	u8 *result = a->beg;
	a->beg += size;
	return result;
}

function void
arena_pop(Arena *a, sz length)
{
	a->beg -= length;
}

#define push_array(a, t, n) (t *)arena_alloc(a, sizeof(t), alignof(t), n)
#define push_struct(a, t)   (t *)arena_alloc(a, sizeof(t), alignof(t), 1)
function void *
arena_alloc(Arena *a, sz len, sz align, sz count)
{
	/* NOTE: special case 0 arena */
	if (a->beg == 0)
		return 0;

	sz padding   = -(uintptr_t)a->beg & (align - 1);
	sz available = a->end - a->beg - padding;
	if (available < 0 || count > available / len)
		assert(0 && "arena OOM\n");
	void *p = a->beg + padding;
	a->beg += padding + count * len;
	/* TODO: Performance? */
	return mem_clear(p, 0, count * len);
}

enum { DA_INITIAL_CAP = 4 };
#define da_reserve(a, s, n) \
  (s)->data = da_reserve_((a), (s)->data, &(s)->capacity, (s)->count + n, \
                          _Alignof(typeof(*(s)->data)), sizeof(*(s)->data))

#define da_push(a, s) \
  ((s)->count == (s)->capacity  \
    ? da_reserve(a, s, 1),      \
      (s)->data + (s)->count++  \
    : (s)->data + (s)->count++)

function void *
da_reserve_(Arena *a, void *data, sz *capacity, sz needed, sz align, sz size)
{
	sz cap = *capacity;

	/* NOTE(rnp): handle both 0 initialized DAs and DAs that need to be moved (they started
	 * on the stack or someone allocated something in the middle of the arena during usage) */
	if (!data || a->beg != (u8 *)data + cap * size) {
		void *copy = arena_alloc(a, size, align, cap);
		if (data) mem_copy(copy, data, cap * size);
		data = copy;
	}

	if (!cap) cap = DA_INITIAL_CAP;
	while (cap < needed) cap *= 2;
	arena_alloc(a, size, align, cap - *capacity);
	*capacity = cap;
	return data;
}

function sv2
sv2_sub(sv2 a, sv2 b)
{
	sv2 result;
	result.x = a.x - b.x;
	result.y = a.y - b.y;
	return result;
}

function v3
v3_sub(v3 a, v3 b)
{
	v3 result;
	result.x = a.x - b.x;
	result.y = a.y - b.y;
	result.z = a.z - b.z;
	return result;
}

function f32
v3_dot(v3 a, v3 b)
{
	f32 result = a.x * b.x + a.y * b.y + a.z * b.z;
	return result;
}

function v3
v3_normalize(v3 a)
{
	f32 scale   = sqrt_f32(1 / v3_dot(a, a));
	v3  result  = a;
	result.x   *= scale;
	result.y   *= scale;
	result.z   *= scale;
	return result;
}

function v3
cross(v3 a, v3 b)
{
	v3 result;
	result.x = a.y * b.z - a.z * b.y;
	result.y = a.z * b.x - a.x * b.z;
	result.z = a.x * b.y - a.y * b.x;
	return result;
}

function f32
v4_dot(v4 a, v4 b)
{
	f32 result = 0;
	result += a.x * b.x;
	result += a.y * b.y;
	result += a.z * b.z;
	result += a.w * b.w;
	return result;
}

function v4
m4_row(m4 a, u32 row)
{
	v4 result;
	result.E[0] = a.c[0].E[row];
	result.E[1] = a.c[1].E[row];
	result.E[2] = a.c[2].E[row];
	result.E[3] = a.c[3].E[row];
	return result;
}

function v4
m4_column(m4 a, u32 column)
{
	v4 result = a.c[column];
	return result;
}

function m4
m4_mul(m4 a, m4 b)
{
	m4 result;
	for (u32 i = 0; i < countof(result.E); i++) {
		u32 base = i / 4;
		u32 sub  = i % 4;
		v4 v1 = m4_row(a, base);
		v4 v2 = m4_column(b, sub);
		result.E[i] = v4_dot(v1, v2);
	}
	return result;
}

function u32
utf8_encode(u8 *out, u32 cp)
{
	u32 result = 1;
	if (cp <= 0x7F) {
		out[0] = cp & 0x7F;
	} else if (cp <= 0x7FF) {
		result = 2;
		out[0] = ((cp >>  6) & 0x1F) | 0xC0;
		out[1] = ((cp >>  0) & 0x3F) | 0x80;
	} else if (cp <= 0xFFFF) {
		result = 3;
		out[0] = ((cp >> 12) & 0x0F) | 0xE0;
		out[1] = ((cp >>  6) & 0x3F) | 0x80;
		out[2] = ((cp >>  0) & 0x3F) | 0x80;
	} else if (cp <= 0x10FFFF) {
		result = 4;
		out[0] = ((cp >> 18) & 0x07) | 0xF0;
		out[1] = ((cp >> 12) & 0x3F) | 0x80;
		out[2] = ((cp >>  6) & 0x3F) | 0x80;
		out[3] = ((cp >>  0) & 0x3F) | 0x80;
	} else {
		out[0] = '?';
	}
	return result;
}

function UnicodeDecode
utf16_decode(u16 *data, sz length)
{
	UnicodeDecode result = {.cp = U32_MAX};
	if (length) {
		result.consumed = 1;
		result.cp = data[0];
		if (length > 1 && BETWEEN(data[0], 0xD800, 0xDBFF)
		               && BETWEEN(data[1], 0xDC00, 0xDFFF))
		{
			result.consumed = 2;
			result.cp = ((data[0] - 0xD800) << 10) | ((data[1] - 0xDC00) + 0x10000);
		}
	}
	return result;
}

function u32
utf16_encode(u16 *out, u32 cp)
{
	u32 result = 1;
	if (cp == U32_MAX) {
		out[0] = '?';
	} else if (cp < 0x10000) {
		out[0] = cp;
	} else {
		u32 value = cp - 0x10000;
		out[0] = 0xD800 + (value >> 10u);
		out[1] = 0xDC00 + (value & 0x3FFu);
		result = 2;
	}
	return result;
}

function Stream
stream_alloc(Arena *a, sz cap)
{
	Stream result = {.cap = cap};
	result.data = push_array(a, u8, cap);
	return result;
}

function str8
stream_to_str8(Stream *s)
{
	str8 result = str8("");
	if (!s->errors) result = (str8){.len = s->widx, .data = s->data};
	return result;
}

function void
stream_reset(Stream *s, sz index)
{
	s->errors = s->cap <= index;
	if (!s->errors)
		s->widx = index;
}

function void
stream_commit(Stream *s, sz count)
{
	s->errors |= !BETWEEN(s->widx + count, 0, s->cap);
	if (!s->errors)
		s->widx += count;
}

function void
stream_append(Stream *s, void *data, sz count)
{
	s->errors |= (s->cap - s->widx) < count;
	if (!s->errors) {
		mem_copy(s->data + s->widx, data, count);
		s->widx += count;
	}
}

function void
stream_append_byte(Stream *s, u8 b)
{
	stream_append(s, &b, 1);
}

function void
stream_append_str8(Stream *s, str8 str)
{
	stream_append(s, str.data, str.len);
}

#define stream_append_str8s(s, ...) stream_append_str8s_(s, (str8 []){__VA_ARGS__}, \
                                                         sizeof((str8 []){__VA_ARGS__}) / sizeof(str8))
function void
stream_append_str8s_(Stream *s, str8 *strs, sz count)
{
	for (sz i = 0; i < count; i++)
		stream_append(s, strs[i].data, strs[i].len);
}

function void
stream_append_u64(Stream *s, u64 n)
{
	u8 tmp[64];
	u8 *end = tmp + sizeof(tmp);
	u8 *beg = end;
	do { *--beg = '0' + (n % 10); } while (n /= 10);
	stream_append(s, beg, end - beg);
}

function void
stream_append_s64(Stream *s, s64 n)
{
	if (n < 0) {
		stream_append_byte(s, '-');
		n *= -1;
	}
	stream_append_u64(s, n);
}

function void
stream_append_f64(Stream *s, f64 f, s64 prec)
{
	if (f < 0) {
		stream_append_byte(s, '-');
		f *= -1;
	}

	/* NOTE: round last digit */
	f += 0.5f / prec;

	if (f >= (f64)(-1UL >> 1)) {
		stream_append_str8(s, str8("inf"));
	} else {
		u64 integral = f;
		u64 fraction = (f - integral) * prec;
		stream_append_u64(s, integral);
		stream_append_byte(s, '.');
		for (s64 i = prec / 10; i > 1; i /= 10) {
			if (i > fraction)
				stream_append_byte(s, '0');
		}
		stream_append_u64(s, fraction);
	}
}

function Stream
arena_stream(Arena a)
{
	Stream result = {0};
	result.data   = a.beg;
	result.cap    = a.end - a.beg;
	return result;
}

function str8
arena_stream_commit_zero(Arena *a, Stream *s)
{
	b32 error = s->errors || s->widx == s->cap;
	if (!error)
		s->data[s->widx] = 0;
	str8 result = stream_to_str8(s);
	arena_commit(a, result.len + 1);
	return result;
}


/* NOTE(rnp): FNV-1a hash */
function u64
str8_hash(str8 v)
{
	u64 h = 0x3243f6a8885a308d; /* digits of pi */
	for (; v.len; v.len--) {
		h ^= v.data[v.len - 1] & 0xFF;
		h *= 1111111111111111111; /* random prime */
	}
	return h;
}

function str8
c_str_to_str8(char *cstr)
{
	str8 result = {.data = (u8 *)cstr};
	if (cstr) while (*cstr) { cstr++; }
	result.len = (u8 *)cstr - result.data;
	return result;
}

/* NOTE(rnp): returns < 0 if byte is not found */
function sz
str8_scan_backwards(str8 s, u8 byte)
{
	sz result = s.len;
	while (result && s.data[result - 1] != byte) result--;
	result--;
	return result;
}

function str8
str8_cut_head(str8 s, sz cut)
{
	str8 result = s;
	if (cut > 0) {
		result.data += cut;
		result.len  -= cut;
	}
	return result;
}

function str8
str8_alloc(Arena *a, sz len)
{
	str8 result = {.data = push_array(a, u8, len), .len = len};
	return result;
}

function str8
str16_to_str8(Arena *a, str16 in)
{
	str8 result = str8("");
	if (in.len) {
		sz commit = in.len * 4;
		sz length = 0;
		u8 *data = arena_commit(a, commit + 1);
		u16 *beg = in.data;
		u16 *end = in.data + in.len;
		while (beg < end) {
			UnicodeDecode decode = utf16_decode(beg, end - beg);
			length += utf8_encode(data + length, decode.cp);
			beg    += decode.consumed;
		}
		data[length] = 0;
		result = (str8){.len = length, .data = data};
		arena_pop(a, commit - length);
	}
	return result;
}

function str16
str8_to_str16(Arena *a, str8 in)
{
	str16 result = {0};
	if (in.len) {
		sz required = 2 * in.len + 1;
		u16 *data   = push_array(a, u16, required);
		sz length   = 0;
		/* TODO(rnp): utf8_decode */
		for (sz i = 0; i < in.len; i++) {
			u32 cp  = in.data[i];
			length += utf16_encode(data + length, cp);
		}
		result = (str16){.len = length, .data = data};
		arena_pop(a, required - length);
	}
	return result;
}

function str8
push_str8(Arena *a, str8 str)
{
	str8 result = str8_alloc(a, str.len);
	mem_copy(result.data, str.data, result.len);
	return result;
}

function str8
push_str8_zero(Arena *a, str8 str)
{
	str8 result  = str8_alloc(a, str.len + 1);
	result.len  -= 1;
	mem_copy(result.data, str.data, result.len);
	return result;
}

function f64
parse_f64(str8 s)
{
	f64 integral = 0, fractional = 0, sign = 1;

	if (s.len && *s.data == '-') {
		sign = -1;
		s.data++;
		s.len--;
	}

	while (s.len && *s.data != '.') {
		integral *= 10;
		integral += *s.data - '0';
		s.data++;
		s.len--;
	}

	if (*s.data == '.') { s.data++; s.len--; }

	while (s.len) {
		assert(s.data[s.len - 1] != '.');
		fractional /= 10;
		fractional += (f64)(s.data[--s.len] - '0') / 10.0;
	}
	f64 result = sign * (integral + fractional);
	return result;
}

function FileWatchDirectory *
lookup_file_watch_directory(FileWatchContext *ctx, u64 hash)
{
	FileWatchDirectory *result = 0;
	for (u32 i = 0; i < ctx->count; i++) {
		FileWatchDirectory *test = ctx->data + i;
		if (test->hash == hash) {
			result = test;
			break;
		}
	}
	return result;
}
