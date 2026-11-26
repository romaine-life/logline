/*
 * The C library's tests: testdata/golden.json and testdata/redact.json, the cases every
 * implementation runs, read here with a small JSON reader, then the logger's own.
 *
 *	logline-test <testdata dir> <scratch dir>
 */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <process.h>
#include <stdlib.h>
#define getpid _getpid
#else
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logline.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ------------------------------------------------------------------ JSON */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct json {
	int type;
	double num;
	char *str;
	size_t n;
	struct json **items;
	char **keys;
};

static const char *jp;

static void
ws(void)
{
	while (*jp == ' ' || *jp == '\t' || *jp == '\n' || *jp == '\r')
		jp++;
}

static void
die(const char *what)
{
	printf("bad JSON: %s at %.20s\n", what, jp);
	exit(2);
}

static void *
xrealloc(void *p, size_t n)
{
	if ((p = realloc(p, n)) == NULL)
		die("out of memory");
	return p;
}

static void
utf8(char **s, size_t *n, unsigned long r)
{
	*s = xrealloc(*s, *n + 5);
	if (r < 0x80)
		(*s)[(*n)++] = (char)r;
	else if (r < 0x800) {
		(*s)[(*n)++] = (char)(0xc0 | (r >> 6));
		(*s)[(*n)++] = (char)(0x80 | (r & 0x3f));
	} else if (r < 0x10000) {
		(*s)[(*n)++] = (char)(0xe0 | (r >> 12));
		(*s)[(*n)++] = (char)(0x80 | ((r >> 6) & 0x3f));
		(*s)[(*n)++] = (char)(0x80 | (r & 0x3f));
	} else {
		(*s)[(*n)++] = (char)(0xf0 | (r >> 18));
		(*s)[(*n)++] = (char)(0x80 | ((r >> 12) & 0x3f));
		(*s)[(*n)++] = (char)(0x80 | ((r >> 6) & 0x3f));
		(*s)[(*n)++] = (char)(0x80 | (r & 0x3f));
	}
}

static unsigned long
hex4(void)
{
	unsigned long v = 0;
	int k;

	for (k = 0; k < 4; k++) {
		char c = *jp++;
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned long)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned long)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned long)(c - 'A' + 10);
		else
			die("bad \\u");
	}
	return v;
}

static char *
jstring(void)
{
	char *s = NULL;
	size_t n = 0;
	unsigned long r;

	if (*jp++ != '"')
		die("expected a string");
	while (*jp != '"') {
		if (*jp == '\0')
			die("unterminated string");
		if (*jp != '\\') {
			s = xrealloc(s, n + 1);
			s[n++] = *jp++;
			continue;
		}
		jp++;
		switch (*jp++) {
		case '"': utf8(&s, &n, '"'); break;
		case '\\': utf8(&s, &n, '\\'); break;
		case '/': utf8(&s, &n, '/'); break;
		case 'b': utf8(&s, &n, '\b'); break;
		case 'f': utf8(&s, &n, '\f'); break;
		case 'n': utf8(&s, &n, '\n'); break;
		case 'r': utf8(&s, &n, '\r'); break;
		case 't': utf8(&s, &n, '\t'); break;
		case 'u':
			r = hex4();
			if (r >= 0xd800 && r <= 0xdbff && jp[0] == '\\' && jp[1] == 'u') {
				jp += 2;
				r = 0x10000 + ((r - 0xd800) << 10) + (hex4() - 0xdc00);
			}
			utf8(&s, &n, r);
			break;
		default:
			die("bad escape");
		}
	}
	jp++;
	s = xrealloc(s, n + 1);
	s[n] = '\0';
	return s;
}

static struct json *
jvalue(void)
{
	struct json *v = calloc(1, sizeof *v);

	if (v == NULL)
		die("out of memory");
	ws();
	if (*jp == '"') {
		v->type = J_STR;
		v->str = jstring();
	} else if (*jp == '[' || *jp == '{') {
		int obj = *jp++ == '{';
		v->type = obj ? J_OBJ : J_ARR;
		ws();
		if (*jp == (obj ? '}' : ']')) {
			jp++;
			return v;
		}
		for (;;) {
			v->items = xrealloc(v->items, (v->n + 1) * sizeof *v->items);
			if (obj) {
				ws();
				v->keys = xrealloc(v->keys, (v->n + 1) * sizeof *v->keys);
				v->keys[v->n] = jstring();
				ws();
				if (*jp++ != ':')
					die("expected :");
			}
			v->items[v->n++] = jvalue();
			ws();
			if (*jp == ',') {
				jp++;
				continue;
			}
			if (*jp++ != (obj ? '}' : ']'))
				die("expected , or the end");
			break;
		}
	} else if (strncmp(jp, "true", 4) == 0 || strncmp(jp, "false", 5) == 0) {
		v->type = J_BOOL;
		v->num = *jp == 't';
		jp += *jp == 't' ? 4 : 5;
	} else if (strncmp(jp, "null", 4) == 0) {
		jp += 4;
	} else {
		char *end;
		v->type = J_NUM;
		v->num = strtod(jp, &end);
		if (end == jp)
			die("expected a value");
		jp = end;
	}
	return v;
}

static struct json *
get(const struct json *obj, const char *key)
{
	size_t i;

	for (i = 0; i < obj->n; i++)
		if (strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	return NULL;
}

static const char *
str(const struct json *obj, const char *key)
{
	struct json *v = get(obj, key);

	if (v == NULL || v->type != J_STR) {
		printf("bad case: no string %s\n", key);
		exit(2);
	}
	return v->str;
}

static char *
read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	char *s = NULL;
	size_t n = 0, got;

	if (f == NULL)
		return NULL;
	for (;;) {
		s = xrealloc(s, n + 65536 + 1);
		got = fread(s + n, 1, 65536, f);
		n += got;
		if (got == 0)
			break;
	}
	fclose(f);
	s[n] = '\0';
	if (size != NULL)
		*size = n;
	return s;
}

static struct json *
load(const char *dir, const char *name)
{
	char path[4096];
	char *text;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	if ((text = read_file(path, NULL)) == NULL) {
		printf("can't read %s\n", path);
		exit(2);
	}
	jp = text;
	return jvalue();
}

/* ------------------------------------------------------------------ cases */

static logline_time
time_of(const char *s)
{
	logline_time t;
	char zone[8] = "";
	int oh = 0, om = 0;

	memset(&t, 0, sizeof t);
	if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d.%3d%7s", &t.year, &t.month, &t.day,
	    &t.hour, &t.minute, &t.second, &t.millisecond, zone) != 8) {
		printf("bad case time %s\n", s);
		exit(2);
	}
	if (zone[0] != 'Z' && sscanf(zone + 1, "%2d:%2d", &oh, &om) == 2)
		t.offset_minutes = (oh * 60 + om) * (zone[0] == '-' ? -1 : 1);
	return t;
}

static logline_level
level_of(const char *s)
{
	return strcmp(s, "warn") == 0 ? LOGLINE_WARN :
	    strcmp(s, "error") == 0 ? LOGLINE_ERROR : LOGLINE_INFO;
}

static int
same_time(logline_time a, logline_time b)
{
	return a.year == b.year && a.month == b.month && a.day == b.day &&
	    a.hour == b.hour && a.minute == b.minute && a.second == b.second &&
	    a.millisecond == b.millisecond && a.offset_minutes == b.offset_minutes;
}

static void
golden_lines_format_and_parse(const char *testdata)
{
	struct json *cases = load(testdata, "golden.json");
	size_t i, k;

	for (i = 0; i < cases->n; i++) {
		struct json *c = cases->items[i], *fj = get(c, "fields");
		const char *note = str(c, "note"), *line = str(c, "line");
		const char *in = get(c, "message_in") ? str(c, "message_in") : str(c, "message");
		logline_time t = time_of(str(c, "time"));
		logline_field *fields = calloc(fj->n + 1, sizeof *fields);
		logline_entry e;
		char err[256], *got;
		long pid = (long)get(c, "pid")->num;

		for (k = 0; k < fj->n; k++) {
			fields[k].key = fj->items[k]->items[0]->str;
			fields[k].value = fj->items[k]->items[1]->str;
		}
		got = logline_format(t, pid, level_of(str(c, "level")), in, fields, fj->n);
		CHECK(got != NULL && strcmp(got, line) == 0, "%s:\n got %s\nwant %s", note,
		    got ? got : "(null)", line);
		free(got);

		if (logline_parse(line, &e, err, sizeof err) != 0) {
			CHECK(0, "%s: parse: %s", note, err);
		} else {
			int ok = same_time(e.time, t) && e.pid == pid &&
			    e.level == level_of(str(c, "level")) &&
			    strcmp(e.message, str(c, "message")) == 0 && e.nfields == fj->n;
			for (k = 0; ok && k < fj->n; k++)
				ok = strcmp(e.fields[k].key, fields[k].key) == 0 &&
				    strcmp(e.fields[k].value, fields[k].value) == 0;
			CHECK(ok, "%s: parsed message %s, %lu fields", note, e.message,
			    (unsigned long)e.nfields);
			logline_entry_free(&e);
		}
		free(fields);
	}
}

static void
parse_refuses_what_isnt_the_standard(void)
{
	static const char *lines[] = {
		"",
		"hello",
		"2026-11-25T10:30:43-08:00 pid=1 info no milliseconds",
		"2026-11-25 10:30:43.123 pid=1 info no zone",
		"2026/11/25 10:30:43 strip-owner's old format",
		"2026-11-25T10:30:43.123-08:00 info no pid",
		"2026-11-25T10:30:43.123-08:00 pid=0 info zero pid",
		"2026-11-25T10:30:43.123-08:00 pid=1 debug unknown level",
		"2026-11-25T10:30:43.123-08:00 pid=1 info",
		"2026-11-25T10:30:43.123-08:00 pid=1 info \"unterminated",
		"2026-11-25T10:30:43.123-08:00 pid=1 info msg k=\"unterminated",
		"2026-11-25T10:30:43.123-08:00 pid=1 info msg k=",
		"2026-11-25T10:30:43.123-08:00 pid=1 info \"quoted\"junk",
		NULL,
	};
	logline_entry e;
	char err[256];
	size_t i;

	for (i = 0; lines[i] != NULL; i++) {
		int r = logline_parse(lines[i], &e, err, sizeof err);
		CHECK(r != 0, "accepted \"%s\"", lines[i]);
		if (r == 0)
			logline_entry_free(&e);
	}
}

static void
odd_keys_and_missing_values_still_make_a_standard_line(void)
{
	logline_field fields[] = {{"Bad Key", "1"}, {"dangling", NULL}};
	const char *want = " info x bad_key=1 dangling=MISSING";
	char *line = logline_format(logline_now(), 3, LOGLINE_INFO, "x", fields, 2);
	logline_entry e;
	char err[256];

	CHECK(line != NULL && strlen(line) > strlen(want) &&
	    strcmp(line + strlen(line) - strlen(want), want) == 0, "%s", line);
	CHECK(logline_parse(line, &e, err, sizeof err) == 0, "%s: %s", line, err);
	logline_entry_free(&e);
	free(line);
}

static void
redact_golden(const char *testdata)
{
	struct json *cases = load(testdata, "redact.json");
	size_t i;

	for (i = 0; i < cases->n; i++) {
		struct json *c = cases->items[i];
		const char *out = str(c, "out");
		char *got = logline_redact(str(c, "in")), *again = logline_redact(out);

		CHECK(got != NULL && strcmp(got, out) == 0, "%s:\n got %s\nwant %s",
		    str(c, "note"), got ? got : "(null)", out);
		CHECK(again != NULL && strcmp(again, out) == 0,
		    "%s: masking twice changed it: %s", str(c, "note"), again ? again : "(null)");
		free(got);
		free(again);
	}
}

/* ------------------------------------------------------------------ logger */

static long
size_of(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (f == NULL)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

/* The file's lines, each parsed: how many, with the entries in out. */
static size_t
parse_file(const char *path, logline_entry *out, size_t max)
{
	char *text = read_file(path, NULL), *line, *nl, err[256];
	size_t n = 0;

	if (text == NULL)
		return 0;
	for (line = text; *line != '\0' && n < max; line = nl + 1) {
		if ((nl = strchr(line, '\n')) == NULL) {
			CHECK(0, "%s: a line without its newline", path);
			break;
		}
		*nl = '\0';
		if (logline_parse(line, &out[n], err, sizeof err) != 0) {
			CHECK(0, "%s: %s: %s", path, line, err);
			break;
		}
		n++;
	}
	free(text);
	return n;
}

static void
make_dir(const char *path)
{
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, 0700);
#endif
}

static void
a_file_log_appends_closes_and_rotates(const char *work)
{
	char path[4096], old[4200];
	logline_logger *l;
	logline_entry e[4];
	size_t n, i;
	FILE *f;
	char *fill;

	snprintf(path, sizeof path, "%s/app/app.log", work);
	snprintf(old, sizeof old, "%s.old", path);
	remove(path);
	remove(old);
	l = logline_open_file(path);
	CHECK(strcmp(logline_path(l), path) == 0, "path %s", logline_path(l));
	logline_info(l, "one", "n", "1", (char *)NULL);
	logline_warn(l, "two", (char *)NULL);
	n = parse_file(path, e, 4);
	CHECK(n == 2, "%lu lines", (unsigned long)n);
	if (n == 2) {
		CHECK(e[0].pid == (long)getpid() && e[0].level == LOGLINE_INFO &&
		    strcmp(e[0].message, "one") == 0 && e[0].nfields == 1 &&
		    strcmp(e[0].fields[0].key, "n") == 0 &&
		    strcmp(e[0].fields[0].value, "1") == 0, "first line %s", e[0].message);
		CHECK(e[1].level == LOGLINE_WARN && strcmp(e[1].message, "two") == 0 &&
		    e[1].nfields == 0, "second line %s", e[1].message);
	}
	for (i = 0; i < n; i++)
		logline_entry_free(&e[i]);

	/* Past the limit, the next line starts a new file and the old one moves aside. */
	fill = malloc(LOGLINE_LIMIT + 1);
	memset(fill, 'x', LOGLINE_LIMIT + 1);
	f = fopen(path, "wb");
	fwrite(fill, 1, LOGLINE_LIMIT + 1, f);
	fclose(f);
	free(fill);
	logline_error(l, "three", (char *)NULL);
	CHECK(size_of(old) == LOGLINE_LIMIT + 1, "the full file wasn't moved to .old");
	n = parse_file(path, e, 4);
	CHECK(n == 1 && e[0].level == LOGLINE_ERROR && strcmp(e[0].message, "three") == 0,
	    "new file has %lu lines", (unsigned long)n);
	for (i = 0; i < n; i++)
		logline_entry_free(&e[i]);
	logline_close(l);
	remove(path);
	remove(old);
}

static void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	wchar_t wn[64], wv[4096];
	MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
	MultiByteToWideChar(CP_UTF8, 0, value, -1, wv, 4096);
	_wputenv_s(wn, wv);
#else
	if (value[0] == '\0')
		unsetenv(name);
	else
		setenv(name, value, 1);
#endif
}

static void
open_takes_the_override_path(const char *work)
{
	char path[4096], want[256];
	logline_logger *l;
	const char *got;

	snprintf(path, sizeof path, "%s/x.log", work);
	set_env("LOGLINE_TEST_LOG", path);
	l = logline_open("someapp", "LOGLINE_TEST_LOG");
	CHECK(logline_path(l) != NULL && strcmp(logline_path(l), path) == 0, "path %s",
	    logline_path(l) ? logline_path(l) : "(null)");
	logline_close(l);

	set_env("LOGLINE_TEST_LOG", "");
	l = logline_open("someapp", "LOGLINE_TEST_LOG");
	got = logline_path(l);
#ifdef _WIN32
	snprintf(want, sizeof want, "\\someapp\\someapp.log");
#else
	snprintf(want, sizeof want, "/someapp/someapp.log");
#endif
	CHECK(got != NULL && strlen(got) > strlen(want) &&
	    strcmp(got + strlen(got) - strlen(want), want) == 0, "default path %s",
	    got ? got : "(null)");
	logline_close(l);
}

static void
a_stream_log_writes_lines(const char *work)
{
	char path[4096], *text, err[256];
	const char *want = " info served status=200\n";
	logline_logger *l;
	logline_entry e;
	FILE *f;
	size_t n;

	snprintf(path, sizeof path, "%s/stream.txt", work);
	if ((f = fopen(path, "wb")) == NULL) {
		CHECK(0, "can't write %s", path);
		return;
	}
	l = logline_open_stream(f);
	CHECK(logline_path(l) == NULL, "a stream has a path");
	logline_info(l, "served", "status", "200", (char *)NULL);
	logline_close(l);
	fclose(f);
	text = read_file(path, &n);
	CHECK(text != NULL && n > strlen(want) && strcmp(text + n - strlen(want), want) == 0,
	    "%s", text ? text : "(null)");
	if (text != NULL && n > 0) {
		text[n - 1] = '\0';
		CHECK(logline_parse(text, &e, err, sizeof err) == 0, "%s: %s", text, err);
		logline_entry_free(&e);
	}
	free(text);
	remove(path);
}

static void
now_is_a_standard_time(void)
{
	char *line = logline_format(logline_now(), 1, LOGLINE_INFO, "now", NULL, 0);
	logline_entry e;
	char err[256];

	CHECK(line != NULL && logline_parse(line, &e, err, sizeof err) == 0, "%s: %s",
	    line ? line : "(null)", err);
	logline_entry_free(&e);
	free(line);
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		printf("usage: logline-test <testdata dir> <scratch dir>\n");
		return 2;
	}
	make_dir(argv[2]);
	golden_lines_format_and_parse(argv[1]);
	parse_refuses_what_isnt_the_standard();
	odd_keys_and_missing_values_still_make_a_standard_line();
	redact_golden(argv[1]);
	a_file_log_appends_closes_and_rotates(argv[2]);
	open_takes_the_override_path(argv[2]);
	a_stream_log_writes_lines(argv[2]);
	now_is_a_standard_time();
	if (failures > 0) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
