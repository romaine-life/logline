/*
 * logline: the home estate's one log line in C (logline.h says what it is). The Go
 * package at the repository's root is the reference; this file follows it rule for rule,
 * and both run testdata/golden.json and testdata/redact.json.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#else
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logline.h"

/* A growing string. A failed allocation marks it bad, and every later write is dropped. */
struct buf {
	char *p;
	size_t n, cap;
	int bad;
};

static void
put(struct buf *b, const char *s, size_t n)
{
	if (b->bad)
		return;
	if (b->n + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 128;
		char *p;
		while (cap < b->n + n + 1)
			cap *= 2;
		if ((p = realloc(b->p, cap)) == NULL) {
			b->bad = 1;
			return;
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = '\0';
}

static void
puts_(struct buf *b, const char *s)
{
	put(b, s, strlen(s));
}

static void
putch(struct buf *b, char c)
{
	put(b, &c, 1);
}

/* The string, or NULL (and nothing to free) when memory ran out. */
static char *
done(struct buf *b)
{
	if (b->bad) {
		free(b->p);
		return NULL;
	}
	if (b->p == NULL)
		return calloc(1, 1);
	return b->p;
}

static const char hex[] = "0123456789abcdef";

/* One rune of UTF-8 at s, as Go's utf8.DecodeRune reads it: the rune and its width, or -1
 * and width 1 for a byte that doesn't start a valid encoding. */
static size_t
decode(const unsigned char *s, size_t n, long *r)
{
	unsigned char c = s[0];
	size_t w, i;
	unsigned char lo = 0x80, hi = 0xbf;
	long v;

	*r = -1;
	if (c < 0x80) {
		*r = c;
		return 1;
	}
	if (c >= 0xc2 && c <= 0xdf) {
		w = 2;
		v = c & 0x1f;
	} else if (c >= 0xe0 && c <= 0xef) {
		w = 3;
		v = c & 0x0f;
		if (c == 0xe0)
			lo = 0xa0;
		else if (c == 0xed)
			hi = 0x9f;
	} else if (c >= 0xf0 && c <= 0xf4) {
		w = 4;
		v = c & 0x07;
		if (c == 0xf0)
			lo = 0x90;
		else if (c == 0xf4)
			hi = 0x8f;
	} else
		return 1;
	if (n < w)
		return 1;
	for (i = 1; i < w; i++) {
		unsigned char d = s[i];
		if (d < (i == 1 ? lo : 0x80) || d > (i == 1 ? hi : 0xbf))
			return 1;
		v = (v << 6) | (d & 0x3f);
	}
	*r = v;
	return w;
}

static void
put_rune(struct buf *b, long r)
{
	char s[4];

	if (r < 0x80) {
		s[0] = (char)r;
		put(b, s, 1);
	} else if (r < 0x800) {
		s[0] = (char)(0xc0 | (r >> 6));
		s[1] = (char)(0x80 | (r & 0x3f));
		put(b, s, 2);
	} else if (r < 0x10000) {
		s[0] = (char)(0xe0 | (r >> 12));
		s[1] = (char)(0x80 | ((r >> 6) & 0x3f));
		s[2] = (char)(0x80 | (r & 0x3f));
		put(b, s, 3);
	} else {
		s[0] = (char)(0xf0 | (r >> 18));
		s[1] = (char)(0x80 | ((r >> 12) & 0x3f));
		s[2] = (char)(0x80 | ((r >> 6) & 0x3f));
		s[3] = (char)(0x80 | (r & 0x3f));
		put(b, s, 4);
	}
}

/* Go's unicode.IsControl: C0, DEL and C1. */
static int
is_control(long r)
{
	return (r >= 0 && r < 0x20) || (r >= 0x7f && r <= 0x9f);
}

/* Go's unicode.IsSpace. */
static int
is_space(long r)
{
	return (r >= 0x09 && r <= 0x0d) || r == ' ' || r == 0x85 || r == 0xa0 ||
	    r == 0x1680 || (r >= 0x2000 && r <= 0x200a) || r == 0x2028 ||
	    r == 0x2029 || r == 0x202f || r == 0x205f || r == 0x3000;
}

const char *
logline_level_name(logline_level level)
{
	switch (level) {
	case LOGLINE_WARN:
		return "warn";
	case LOGLINE_ERROR:
		return "error";
	default:
		return "info";
	}
}

static int
valid_key(const char *k, size_t n)
{
	size_t i;

	if (n == 0 || k[0] < 'a' || k[0] > 'z')
		return 0;
	for (i = 1; i < n; i++) {
		char c = k[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
		    c == '.' || c == '-'))
			return 0;
	}
	return 1;
}

/* Go's strconv.Quote for the text a line holds: \" \\ and Go's short escapes, other C0
 * characters and DEL as \x.., C1 as \u...., a byte that isn't UTF-8 as \x... */
static void
quote(struct buf *b, const char *s, size_t n)
{
	const unsigned char *u = (const unsigned char *)s;
	size_t i = 0, w;
	long r;
	char e[7];

	putch(b, '"');
	while (i < n) {
		w = decode(u + i, n - i, &r);
		if (r < 0) {
			e[0] = '\\';
			e[1] = 'x';
			e[2] = hex[u[i] >> 4];
			e[3] = hex[u[i] & 0xf];
			put(b, e, 4);
		} else if (r == '"')
			puts_(b, "\\\"");
		else if (r == '\\')
			puts_(b, "\\\\");
		else if (r == '\n')
			puts_(b, "\\n");
		else if (r == '\r')
			puts_(b, "\\r");
		else if (r == '\t')
			puts_(b, "\\t");
		else if (r == 0x07)
			puts_(b, "\\a");
		else if (r == 0x08)
			puts_(b, "\\b");
		else if (r == 0x0c)
			puts_(b, "\\f");
		else if (r == 0x0b)
			puts_(b, "\\v");
		else if (r < 0x20 || r == 0x7f) {
			e[0] = '\\';
			e[1] = 'x';
			e[2] = hex[r >> 4];
			e[3] = hex[r & 0xf];
			put(b, e, 4);
		} else if (is_control(r)) {
			e[0] = '\\';
			e[1] = 'u';
			e[2] = '0';
			e[3] = '0';
			e[4] = hex[r >> 4];
			e[5] = hex[r & 0xf];
			put(b, e, 6);
		} else
			put(b, s + i, w);
		i += w;
	}
	putch(b, '"');
}

static void
format_value(struct buf *b, const char *v)
{
	const unsigned char *u = (const unsigned char *)v;
	size_t n = strlen(v), i = 0, w;
	long r;
	int q = n == 0;

	while (!q && i < n) {
		w = decode(u + i, n - i, &r);
		if (r < 0 || r == ' ' || r == '"' || r == '=' || is_control(r))
			q = 1;
		i += w;
	}
	if (q)
		quote(b, v, n);
	else
		put(b, v, n);
}

/* Keeps a message on its line: line breaks become " | ", tabs spaces, other control
 * characters go, and a byte that isn't UTF-8 becomes U+FFFD. */
static char *
one_line(const char *text)
{
	struct buf lf = {0}, out = {0};
	const unsigned char *u;
	size_t i, n, start, end, w, last;
	long r;
	char *s;

	/* \r\n and \r become \n, then the ends' \n go and the rest become " | ". */
	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] == '\r') {
			putch(&lf, '\n');
			if (text[i + 1] == '\n')
				i++;
		} else
			putch(&lf, text[i]);
	}
	if ((s = done(&lf)) == NULL)
		return NULL;
	n = strlen(s);
	start = 0;
	end = n;
	while (start < end && s[start] == '\n')
		start++;
	while (end > start && s[end - 1] == '\n')
		end--;
	u = (const unsigned char *)s;
	for (i = start; i < end; i += w) {
		w = decode(u + i, end - i, &r);
		if (r == '\n')
			puts_(&out, " | ");
		else if (r == '\t')
			putch(&out, ' ');
		else if (r < 0)
			put_rune(&out, 0xfffd);
		else if (!is_control(r))
			put(&out, s + i, w);
	}
	free(s);
	if ((s = done(&out)) == NULL)
		return NULL;
	/* Go's strings.TrimSpace. */
	n = strlen(s);
	u = (const unsigned char *)s;
	start = 0;
	while (start < n) {
		w = decode(u + start, n - start, &r);
		if (!is_space(r))
			break;
		start += w;
	}
	last = start;
	for (i = start; i < n; i += w) {
		w = decode(u + i, n - i, &r);
		if (!is_space(r))
			last = i + w;
	}
	memmove(s, s + start, last - start);
	s[last - start] = '\0';
	return s;
}

static void
format_time(struct buf *b, logline_time t)
{
	char s[40];
	int off = t.offset_minutes, sign = '+';

	snprintf(s, sizeof s, "%04d-%02d-%02dT%02d:%02d:%02d.%03d", t.year, t.month,
	    t.day, t.hour, t.minute, t.second, t.millisecond);
	puts_(b, s);
	if (off == 0) {
		putch(b, 'Z');
		return;
	}
	if (off < 0) {
		sign = '-';
		off = -off;
	}
	snprintf(s, sizeof s, "%c%02d:%02d", sign, off / 60, off % 60);
	puts_(b, s);
}

char *
logline_format(logline_time t, long pid, logline_level level, const char *msg,
    const logline_field *fields, size_t nfields)
{
	struct buf b = {0};
	char pidtext[32], *m;
	size_t i;

	format_time(&b, t);
	snprintf(pidtext, sizeof pidtext, " pid=%ld ", pid);
	puts_(&b, pidtext);
	puts_(&b, logline_level_name(level));
	putch(&b, ' ');
	if ((m = one_line(msg != NULL ? msg : "")) == NULL)
		b.bad = 1;
	else {
		if (m[0] == '\0' || m[0] == '"' || strchr(m, '=') != NULL)
			quote(&b, m, strlen(m));
		else
			puts_(&b, m);
		free(m);
	}
	for (i = 0; i < nfields; i++) {
		const char *key = fields[i].key != NULL ? fields[i].key : "";
		putch(&b, ' ');
		puts_(&b, valid_key(key, strlen(key)) ? key : "bad_key");
		putch(&b, '=');
		format_value(&b, fields[i].value != NULL ? fields[i].value : "MISSING");
	}
	return done(&b);
}

/* ------------------------------------------------------------------ parse */

static int
fail(char *err, size_t errlen, const char *what, const char *text, size_t n)
{
	if (err != NULL && errlen > 0) {
		if (text != NULL)
			snprintf(err, errlen, what, (int)(n > 200 ? 200 : n), text);
		else
			snprintf(err, errlen, "%s", what);
	}
	return -1;
}

static int
digits(const char *s, size_t n, int *v)
{
	size_t i;

	*v = 0;
	for (i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		*v = *v * 10 + (s[i] - '0');
	}
	return 1;
}

static int
days_in(int year, int month)
{
	static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	return month == 2 && leap ? 29 : days[month - 1];
}

/* The line's time: RFC 3339 with exactly three fractional digits and a zone, Z or
 * +hh:mm, as Go's time.Parse reads the layout 2006-01-02T15:04:05.000Z07:00. */
static int
parse_time(const char *s, size_t n, logline_time *t)
{
	int oh, om;

	if (n != 24 && n != 29)
		return 0;
	if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
	    s[16] != ':' || s[19] != '.')
		return 0;
	if (!digits(s, 4, &t->year) || !digits(s + 5, 2, &t->month) ||
	    !digits(s + 8, 2, &t->day) || !digits(s + 11, 2, &t->hour) ||
	    !digits(s + 14, 2, &t->minute) || !digits(s + 17, 2, &t->second) ||
	    !digits(s + 20, 3, &t->millisecond))
		return 0;
	if (t->month < 1 || t->month > 12 || t->day < 1 ||
	    t->day > days_in(t->year, t->month) || t->hour > 23 || t->minute > 59 ||
	    t->second > 59)
		return 0;
	if (n == 24) {
		t->offset_minutes = 0;
		return s[23] == 'Z';
	}
	if ((s[23] != '+' && s[23] != '-') || s[26] != ':' ||
	    !digits(s + 24, 2, &oh) || !digits(s + 27, 2, &om) || oh > 23 || om > 59)
		return 0;
	t->offset_minutes = (oh * 60 + om) * (s[23] == '-' ? -1 : 1);
	return 1;
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Reads a Go-quoted string at the start of s, as strconv.QuotedPrefix and Unquote do:
 * its value (allocated) and how many bytes the quoted string took, or NULL. */
static char *
unquote_prefix(const char *s, size_t *used)
{
	const unsigned char *u = (const unsigned char *)s;
	struct buf b = {0};
	size_t i = 1, n = strlen(s), w, k, len;
	long r, v;
	int d;

	if (s[0] != '"')
		return NULL;
	while (i < n) {
		char c = s[i];
		if (c == '"') {
			*used = i + 1;
			return done(&b);
		}
		if (c == '\n')
			break;
		if (c != '\\') {
			w = decode(u + i, n - i, &r);
			if (r < 0)
				put_rune(&b, 0xfffd);
			else
				put(&b, s + i, w);
			i += w;
			continue;
		}
		if (++i >= n)
			break;
		c = s[i++];
		switch (c) {
		case '"': putch(&b, '"'); continue;
		case '\\': putch(&b, '\\'); continue;
		case 'n': putch(&b, '\n'); continue;
		case 'r': putch(&b, '\r'); continue;
		case 't': putch(&b, '\t'); continue;
		case 'a': putch(&b, 0x07); continue;
		case 'b': putch(&b, 0x08); continue;
		case 'f': putch(&b, 0x0c); continue;
		case 'v': putch(&b, 0x0b); continue;
		case 'x': case 'u': case 'U':
			len = c == 'x' ? 2 : c == 'u' ? 4 : 8;
			v = 0;
			for (k = 0; k < len; k++) {
				if (i + k >= n || (d = hexval(s[i + k])) < 0)
					goto bad;
				v = v * 16 + d;
			}
			i += len;
			if (c == 'x')
				putch(&b, (char)v);
			else if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
				goto bad;
			else
				put_rune(&b, v);
			continue;
		default:
			if (c >= '0' && c <= '7') {
				v = c - '0';
				for (k = 0; k < 2; k++) {
					if (i + k >= n || s[i + k] < '0' || s[i + k] > '7')
						goto bad;
					v = v * 8 + (s[i + k] - '0');
				}
				if (v > 255)
					goto bad;
				i += 2;
				putch(&b, (char)v);
				continue;
			}
			goto bad;
		}
	}
bad:
	free(b.p);
	return NULL;
}

/* Where the first " key=" starts in an unquoted message's line, or -1. */
static long
first_field(const char *s, size_t n)
{
	size_t i, j, k;

	for (i = 0; i < n; i++) {
		if (s[i] != ' ')
			continue;
		for (j = i + 1; j < n && s[j] != ' '; j++)
			;
		for (k = i + 1; k < j && s[k] != '='; k++)
			;
		if (k < j && valid_key(s + i + 1, k - i - 1))
			return (long)i;
	}
	return -1;
}

static char *
copy(const char *s, size_t n)
{
	char *p = malloc(n + 1);

	if (p != NULL) {
		memcpy(p, s, n);
		p[n] = '\0';
	}
	return p;
}

static int
add_field(logline_entry *e, char *key, char *value)
{
	logline_field *f;

	if (key == NULL || value == NULL ||
	    (f = realloc(e->fields, (e->nfields + 1) * sizeof *f)) == NULL) {
		free(key);
		free(value);
		return 0;
	}
	e->fields = f;
	f[e->nfields].key = key;
	f[e->nfields].value = value;
	e->nfields++;
	return 1;
}

void
logline_entry_free(logline_entry *e)
{
	size_t i;

	if (e == NULL)
		return;
	for (i = 0; i < e->nfields; i++) {
		free((char *)e->fields[i].key);
		free((char *)e->fields[i].value);
	}
	free(e->fields);
	free(e->message);
	memset(e, 0, sizeof *e);
}

int
logline_parse(const char *line, logline_entry *e, char *err, size_t errlen)
{
	const char *rest, *sp, *p;
	size_t n, used;
	long pid = 0;
	long end;

	memset(e, 0, sizeof *e);
	if ((sp = strchr(line, ' ')) == NULL)
		return fail(err, errlen, "no time", NULL, 0);
	if (!parse_time(line, (size_t)(sp - line), &e->time))
		return fail(err, errlen, "time \"%.*s\" isn't RFC 3339 with milliseconds and a zone",
		    line, (size_t)(sp - line));
	rest = sp + 1;

	/* pid=<n>, as Go's strconv.Atoi reads n, above 0. */
	sp = strchr(rest, ' ');
	p = rest + 4;
	if (sp == NULL || strncmp(rest, "pid=", 4) != 0)
		return fail(err, errlen, "no pid=<n> after the time", NULL, 0);
	{
		int neg = 0, any = 0;
		if (*p == '+' || *p == '-')
			neg = *p++ == '-';
		for (; p < sp; p++) {
			if (*p < '0' || *p > '9' || pid > (LONG_MAX - 9) / 10)
				return fail(err, errlen, "no pid=<n> after the time", NULL, 0);
			pid = pid * 10 + (*p - '0');
			any = 1;
		}
		if (!any || neg || pid <= 0)
			return fail(err, errlen, "no pid=<n> after the time", NULL, 0);
	}
	e->pid = pid;
	rest = sp + 1;

	sp = strchr(rest, ' ');
	n = sp != NULL ? (size_t)(sp - rest) : strlen(rest);
	if (n == 4 && strncmp(rest, "info", 4) == 0)
		e->level = LOGLINE_INFO;
	else if (n == 4 && strncmp(rest, "warn", 4) == 0)
		e->level = LOGLINE_WARN;
	else if (n == 5 && strncmp(rest, "error", 5) == 0)
		e->level = LOGLINE_ERROR;
	else
		return fail(err, errlen, "level \"%.*s\" isn't info, warn or error", rest, n);
	if (sp == NULL)
		return fail(err, errlen, "no message", NULL, 0);
	rest = sp + 1;

	if (rest[0] == '"') {
		if ((e->message = unquote_prefix(rest, &used)) == NULL)
			return fail(err, errlen, "unterminated quoted message", NULL, 0);
		rest += used;
	} else {
		n = strlen(rest);
		end = first_field(rest, n);
		if (end >= 0)
			n = (size_t)end;
		used = n;
		if (n > 0 && rest[n - 1] == ' ')
			n--;
		if ((e->message = copy(rest, n)) == NULL)
			goto nomem;
		rest += used;
	}

	while (*rest != '\0') {
		const char *key, *eq, *after;
		char *value;

		if (*rest != ' ') {
			fail(err, errlen, "junk after the message: \"%.*s\"", rest, strlen(rest));
			goto out;
		}
		key = rest + 1;
		eq = strchr(key, '=');
		if (eq == NULL || !valid_key(key, (size_t)(eq - key))) {
			fail(err, errlen, "field \"%.*s\" isn't key=value", key, strlen(key));
			goto out;
		}
		after = eq + 1;
		if (*after == '"') {
			if ((value = unquote_prefix(after, &used)) == NULL) {
				fail(err, errlen, "field %.*s: unterminated quote", key, (size_t)(eq - key));
				goto out;
			}
			rest = after + used;
		} else {
			sp = strchr(after, ' ');
			n = sp != NULL ? (size_t)(sp - after) : strlen(after);
			if (n == 0) {
				fail(err, errlen, "field %.*s has no value", key, (size_t)(eq - key));
				goto out;
			}
			if ((value = copy(after, n)) == NULL)
				goto nomem;
			/* Go's Cut: a space with nothing after it ends the line. */
			rest = sp != NULL && sp[1] != '\0' ? sp : after + strlen(after);
		}
		if (!add_field(e, copy(key, (size_t)(eq - key)), value))
			goto nomem;
	}
	return 0;
nomem:
	fail(err, errlen, "out of memory", NULL, 0);
out:
	logline_entry_free(e);
	return -1;
}

/* ------------------------------------------------------------------ redact */

/*
 * The estate's one list of credential shapes, the Go package's secrets exactly (redact.go;
 * both run testdata/redact.json): each keeps the text that names the credential and
 * replaces the credential itself. C has no regular expressions of its own, so each shape
 * is a matcher that finds what Go's leftmost-first match finds at a position. A match
 * keeps its first keep bytes, then the marker, then suffix. Letters match without case
 * where Go's pattern says (?i), in ASCII.
 */
struct match {
	size_t end, keep;
	const char *suffix;
};

typedef int (*matcher)(const char *s, size_t n, size_t i, struct match *m);

static int
lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Whether s at i starts with word, without case. */
static int
iprefix(const char *s, size_t n, size_t i, const char *word)
{
	size_t k;

	for (k = 0; word[k] != '\0'; k++)
		if (i + k >= n || lower((unsigned char)s[i + k]) != word[k])
			return 0;
	return 1;
}

static int
is_word(const char *s, size_t n, size_t i)
{
	int c = i < n ? (unsigned char)s[i] : 0;

	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	    (c >= '0' && c <= '9') || c == '_';
}

/* \b before a word character at i. */
static int
starts_word(const char *s, size_t n, size_t i)
{
	return i == 0 || !is_word(s, n, i - 1);
}

static int
is_ws(int c)
{
	return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

/* How many bytes from i are none of stop (and not the end). */
static size_t
run_not(const char *s, size_t n, size_t i, const char *stop)
{
	size_t k = i;

	while (k < n && strchr(stop, s[k]) == NULL)
		k++;
	return k - i;
}

static size_t
run_ws(const char *s, size_t n, size_t i)
{
	size_t k = i;

	while (k < n && is_ws((unsigned char)s[k]))
		k++;
	return k - i;
}

/* [A-Za-z0-9_-], or [A-Za-z0-9_] without dash. */
static size_t
run_token(const char *s, size_t n, size_t i, int dash)
{
	size_t k = i;

	while (k < n && (is_word(s, n, k) || (dash && s[k] == '-')))
		k++;
	return k - i;
}

static const char ws_quote[] = "\t\n\f\r '\"";

/* A URL's query parameter: ?code=, &sig=, &access_token= and the like. */
static int
m_query(const char *s, size_t n, size_t i, struct match *m)
{
	static const char *names[] = {"code", "state", "sig", "signature", "token",
	    "access_token", "id_token", "refresh_token", "client_secret", "password",
	    "passwd", "secret", "key", "api_key", "apikey", "x-goog-signature",
	    "x-amz-signature", NULL};
	size_t k, p, v;

	if (s[i] != '?' && s[i] != '&')
		return 0;
	for (k = 0; names[k] != NULL; k++) {
		p = i + 1 + strlen(names[k]);
		if (!iprefix(s, n, i + 1, names[k]) || p >= n || s[p] != '=')
			continue;
		p++;
		if ((v = run_not(s, n, p, "&\t\n\f\r \"'")) == 0)
			return 0;
		m->keep = p - i;
		m->end = p + v;
		return 1;
	}
	return 0;
}

/* An Authorization header, with or without its scheme. */
static int
m_authorization(const char *s, size_t n, size_t i, struct match *m)
{
	static const char *schemes[] = {"bearer", "token", "basic", "", NULL};
	size_t k, p, q, v;

	if (!iprefix(s, n, i, "authorization:"))
		return 0;
	p = i + 14;
	p += run_ws(s, n, p);
	for (k = 0; schemes[k] != NULL; k++) {
		if (!iprefix(s, n, p, schemes[k]))
			continue;
		q = p + strlen(schemes[k]);
		q += run_ws(s, n, q);
		if ((v = run_not(s, n, q, ws_quote)) > 0) {
			m->keep = q - i;
			m->end = q + v;
			return 1;
		}
	}
	return 0;
}

static int
m_bearer(const char *s, size_t n, size_t i, struct match *m)
{
	size_t p, w, v;

	if (!iprefix(s, n, i, "bearer"))
		return 0;
	p = i + 6;
	if ((w = run_ws(s, n, p)) == 0)
		return 0;
	p += w;
	if ((v = run_not(s, n, p, ws_quote)) == 0)
		return 0;
	m->keep = p - i;
	m->end = p + v;
	return 1;
}

static int
m_basic(const char *s, size_t n, size_t i, struct match *m)
{
	size_t p, w, k;

	if (!starts_word(s, n, i) || !iprefix(s, n, i, "basic"))
		return 0;
	p = i + 5;
	if ((w = run_ws(s, n, p)) == 0)
		return 0;
	p += w;
	for (k = p; k < n && (is_word(s, n, k) || strchr("._~+/=-", s[k]) != NULL) &&
	    s[k] != '\0'; k++)
		;
	if (k - p < 8)
		return 0;
	m->keep = p - i;
	m->end = k;
	return 1;
}

/* NAME=value and "name": "value", for the names credentials go by. */
static int
m_name(const char *s, size_t n, size_t i, struct match *m)
{
	static const char *names[] = {"token", "password", "passwd", "secret",
	    "api_key", "api-key", "apikey", "access_key", "access-key", "accesskey",
	    "client_secret", "client-secret", "clientsecret", "pat", "bw_session", NULL};
	size_t k, p, v;

	if (!starts_word(s, n, i))
		return 0;
	for (k = 0; names[k] != NULL; k++) {
		if (!iprefix(s, n, i, names[k]))
			continue;
		p = i + strlen(names[k]);
		if (p < n && s[p] == '"')
			p++;
		p += run_ws(s, n, p);
		if (p >= n || (s[p] != '=' && s[p] != ':'))
			continue;
		p++;
		p += run_ws(s, n, p);
		if (p < n && s[p] == '"')
			p++;
		if ((v = run_not(s, n, p, "\t\n\f\r '\"&")) == 0)
			continue;
		m->keep = p - i;
		m->end = p + v;
		return 1;
	}
	return 0;
}

/* A command's flag: --token x, --password=x. */
static int
m_flag(const char *s, size_t n, size_t i, struct match *m)
{
	static const char *names[] = {"token", "password", "secret", "api-key",
	    "client-secret", NULL};
	size_t k, p, v;

	if (!iprefix(s, n, i, "--"))
		return 0;
	for (k = 0; names[k] != NULL; k++) {
		p = i + 2 + strlen(names[k]);
		if (!iprefix(s, n, i + 2, names[k]) || p >= n || (s[p] != '=' && s[p] != ' '))
			continue;
		p++;
		if ((v = run_not(s, n, p, ws_quote)) == 0)
			continue;
		m->keep = p - i;
		m->end = p + v;
		return 1;
	}
	return 0;
}

/* The user and password in a URL. */
static int
m_userinfo(const char *s, size_t n, size_t i, struct match *m)
{
	size_t v;

	if (!iprefix(s, n, i, "://"))
		return 0;
	v = run_not(s, n, i + 3, "/@\t\n\f\r '\"");
	if (v == 0 || i + 3 + v >= n || s[i + 3 + v] != '@')
		return 0;
	m->keep = 3;
	m->end = i + 3 + v + 1;
	m->suffix = "@";
	return 1;
}

/* A JWT: three parts, or two long ones. */
static int
m_jwt(const char *s, size_t n, size_t i, struct match *m)
{
	size_t p, a, b, c;

	if (i + 3 > n || strncmp(s + i, "eyJ", 3) != 0)
		return 0;
	p = i + 3;
	a = run_token(s, n, p, 1);
	if (p + a < n && s[p + a] == '.') {
		b = run_token(s, n, p + a + 1, 1);
		if (b > 0 && p + a + 1 + b < n && s[p + a + 1 + b] == '.') {
			c = run_token(s, n, p + a + 2 + b, 1);
			if (c > 0) {
				m->keep = 0;
				m->end = p + a + 2 + b + c;
				return 1;
			}
		}
		if (a >= 10 && b >= 10) {
			m->keep = 0;
			m->end = p + a + 1 + b;
			return 1;
		}
	}
	return 0;
}

/* GitHub tokens. */
static int
m_github(const char *s, size_t n, size_t i, struct match *m)
{
	size_t p, v;

	if (!starts_word(s, n, i))
		return 0;
	if (i + 4 <= n && s[i] == 'g' && s[i + 1] == 'h' &&
	    strchr("pousr", s[i + 2]) != NULL && s[i + 2] != '\0' && s[i + 3] == '_')
		p = i + 4;
	else if (i + 11 <= n && strncmp(s + i, "github_pat_", 11) == 0)
		p = i + 11;
	else
		return 0;
	if ((v = run_token(s, n, p, 0)) < 20)
		return 0;
	m->keep = 0;
	m->end = p + v;
	return 1;
}

/* OpenBao's tokens: hvs., hvb., s., b., then at least 24 characters ending at a word
 * boundary. */
static int
m_openbao(const char *s, size_t n, size_t i, struct match *m)
{
	static const char *prefixes[] = {"hvs.", "hvb.", "s.", "b.", NULL};
	size_t k, p, v;

	if (!starts_word(s, n, i))
		return 0;
	for (k = 0; prefixes[k] != NULL; k++) {
		size_t len = strlen(prefixes[k]);
		if (i + len > n || strncmp(s + i, prefixes[k], len) != 0)
			continue;
		p = i + len;
		for (v = run_token(s, n, p, 1); v >= 24; v--) {
			if (is_word(s, n, p + v - 1) != is_word(s, n, p + v)) {
				m->keep = 0;
				m->end = p + v;
				return 1;
			}
		}
	}
	return 0;
}

static const matcher secrets[] = {m_query, m_authorization, m_bearer, m_basic, m_name,
    m_flag, m_userinfo, m_jwt, m_github, m_openbao};

char *
logline_redact(const char *text)
{
	char *cur, *next;
	size_t k, i, n;
	struct match m;

	if ((cur = copy(text, strlen(text))) == NULL)
		return NULL;
	for (k = 0; k < sizeof secrets / sizeof secrets[0]; k++) {
		struct buf b = {0};
		n = strlen(cur);
		for (i = 0; i < n;) {
			m.suffix = "";
			if (secrets[k](cur, n, i, &m)) {
				put(&b, cur + i, m.keep);
				puts_(&b, LOGLINE_MARKER);
				puts_(&b, m.suffix);
				i = m.end;
			} else
				putch(&b, cur[i++]);
		}
		free(cur);
		if ((next = done(&b)) == NULL)
			return NULL;
		cur = next;
	}
	return cur;
}

/* ------------------------------------------------------------------ logger */

struct logline_logger {
	char *path;
	FILE *stream;
};

logline_time
logline_now(void)
{
	logline_time t;
#ifdef _WIN32
	SYSTEMTIME utc, local;
	FILETIME ft, uft, lft;
	ULARGE_INTEGER u, l;
	LONGLONG diff;

	GetSystemTimeAsFileTime(&ft);
	FileTimeToSystemTime(&ft, &utc);
	if (!SystemTimeToTzSpecificLocalTime(NULL, &utc, &local))
		local = utc;
	SystemTimeToFileTime(&utc, &uft);
	SystemTimeToFileTime(&local, &lft);
	u.LowPart = uft.dwLowDateTime;
	u.HighPart = uft.dwHighDateTime;
	l.LowPart = lft.dwLowDateTime;
	l.HighPart = lft.dwHighDateTime;
	diff = (LONGLONG)l.QuadPart - (LONGLONG)u.QuadPart; /* 100 ns */
	t.year = local.wYear;
	t.month = local.wMonth;
	t.day = local.wDay;
	t.hour = local.wHour;
	t.minute = local.wMinute;
	t.second = local.wSecond;
	t.millisecond = local.wMilliseconds;
	t.offset_minutes = (int)(diff / 600000000);
#else
	struct timespec ts;
	struct tm lt, gt;
	time_t s;
	int days;

	clock_gettime(CLOCK_REALTIME, &ts);
	s = ts.tv_sec;
	localtime_r(&s, &lt);
	gmtime_r(&s, &gt);
	if (lt.tm_year != gt.tm_year)
		days = lt.tm_year > gt.tm_year ? 1 : -1;
	else
		days = lt.tm_yday - gt.tm_yday;
	t.year = lt.tm_year + 1900;
	t.month = lt.tm_mon + 1;
	t.day = lt.tm_mday;
	t.hour = lt.tm_hour;
	t.minute = lt.tm_min;
	t.second = lt.tm_sec > 59 ? 59 : lt.tm_sec;
	t.millisecond = (int)(ts.tv_nsec / 1000000);
	t.offset_minutes = days * 1440 + (lt.tm_hour - gt.tm_hour) * 60 +
	    (lt.tm_min - gt.tm_min);
#endif
	return t;
}

static long
current_pid(void)
{
#ifdef _WIN32
	return (long)GetCurrentProcessId();
#else
	return (long)getpid();
#endif
}

#ifdef _WIN32
#define SEP '\\'

static wchar_t *
widen(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w;

	if (n <= 0 || (w = malloc((size_t)n * sizeof *w)) == NULL)
		return NULL;
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

/* An environment variable as UTF-8 (allocated), or NULL when unset or empty. */
static char *
env(const char *name)
{
	wchar_t *wname = widen(name), *v;
	char *s = NULL;
	int n;

	if (wname == NULL)
		return NULL;
	v = _wgetenv(wname);
	free(wname);
	if (v == NULL || v[0] == L'\0')
		return NULL;
	n = WideCharToMultiByte(CP_UTF8, 0, v, -1, NULL, 0, NULL, NULL);
	if (n > 0 && (s = malloc((size_t)n)) != NULL)
		WideCharToMultiByte(CP_UTF8, 0, v, -1, s, n, NULL, NULL);
	return s;
}
#else
#define SEP '/'

static char *
env(const char *name)
{
	const char *v = getenv(name);

	return v != NULL && v[0] != '\0' ? copy(v, strlen(v)) : NULL;
}
#endif

/* Go's os.UserCacheDir: %LocalAppData%, ~/Library/Caches, or $XDG_CACHE_HOME (when
 * absolute) else ~/.cache. */
static char *
cache_dir(void)
{
#if defined(_WIN32)
	return env("LOCALAPPDATA");
#else
	char *home, *dir;
	struct buf b = {0};
#if !defined(__APPLE__)
	if ((dir = env("XDG_CACHE_HOME")) != NULL) {
		if (dir[0] == '/')
			return dir;
		free(dir);
		return NULL;
	}
#endif
	if ((home = env("HOME")) == NULL)
		return NULL;
	puts_(&b, home);
#if defined(__APPLE__)
	puts_(&b, "/Library/Caches");
#else
	puts_(&b, "/.cache");
#endif
	free(home);
	return done(&b);
#endif
}

static logline_logger *
new_logger(char *path, FILE *stream)
{
	logline_logger *l = calloc(1, sizeof *l);

	if (l == NULL) {
		free(path);
		return NULL;
	}
	l->path = path;
	l->stream = stream;
	return l;
}

logline_logger *
logline_open(const char *app, const char *override_env)
{
	char *path = NULL, *dir;
	struct buf b = {0};

	if (override_env != NULL && override_env[0] != '\0')
		path = env(override_env);
	if (path == NULL && (dir = cache_dir()) != NULL) {
		puts_(&b, dir);
		putch(&b, SEP);
		puts_(&b, app);
		putch(&b, SEP);
		puts_(&b, app);
		puts_(&b, ".log");
		free(dir);
		path = done(&b);
	}
	return new_logger(path, NULL);
}

logline_logger *
logline_open_file(const char *path)
{
	char *p = copy(path, strlen(path));

	return p == NULL ? NULL : new_logger(p, NULL);
}

logline_logger *
logline_open_stream(FILE *stream)
{
	return new_logger(NULL, stream);
}

void
logline_close(logline_logger *l)
{
	if (l == NULL)
		return;
	free(l->path);
	free(l);
}

const char *
logline_path(const logline_logger *l)
{
	return l != NULL ? l->path : NULL;
}

static int
is_sep(char c)
{
#ifdef _WIN32
	return c == '\\' || c == '/';
#else
	return c == '/';
#endif
}

/* Makes every directory above the file at path, as Go's os.MkdirAll does for its
 * directory; a part that exists or can't be made (a drive, a share) is passed by. */
static void
make_parents(const char *path)
{
	char *p = copy(path, strlen(path));
	size_t i, n;

	if (p == NULL)
		return;
	n = strlen(p);
	while (n > 0 && !is_sep(p[n - 1]))
		n--;
	for (i = 1; i < n; i++) {
		char c = p[i];
		if (!is_sep(c) || is_sep(p[i - 1]))
			continue;
		p[i] = '\0';
#ifdef _WIN32
		{
			wchar_t *w = widen(p);
			if (w != NULL)
				CreateDirectoryW(w, NULL);
			free(w);
		}
#else
		mkdir(p, 0700);
#endif
		p[i] = c;
	}
	free(p);
}

/* Moves the file to <path>.old once it's past the limit, then appends line in one write. */
static void
append(const char *path, const char *line)
{
	struct buf old = {0};
	char *oldpath;
	size_t n = strlen(line);

	puts_(&old, path);
	puts_(&old, ".old");
	if ((oldpath = done(&old)) == NULL)
		return;
#ifdef _WIN32
	{
		wchar_t *w = widen(path), *wold = widen(oldpath);
		WIN32_FILE_ATTRIBUTE_DATA a;
		HANDLE h;
		DWORD wrote;

		if (w != NULL && wold != NULL) {
			if (GetFileAttributesExW(w, GetFileExInfoStandard, &a) &&
			    (((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow) >
			    (ULONGLONG)LOGLINE_LIMIT)
				MoveFileExW(w, wold, MOVEFILE_REPLACE_EXISTING);
			make_parents(path);
			h = CreateFileW(w, FILE_APPEND_DATA,
			    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (h != INVALID_HANDLE_VALUE) {
				WriteFile(h, line, (DWORD)n, &wrote, NULL);
				CloseHandle(h);
			}
		}
		free(w);
		free(wold);
	}
#else
	{
		struct stat st;
		int fd, flags = O_WRONLY | O_APPEND | O_CREAT;
		ssize_t w;
		size_t off = 0;

#ifdef O_CLOEXEC
		flags |= O_CLOEXEC;
#endif
		if (stat(path, &st) == 0 && st.st_size > LOGLINE_LIMIT)
			rename(path, oldpath);
		make_parents(path);
		if ((fd = open(path, flags, 0600)) >= 0) {
			while (off < n) {
				w = write(fd, line + off, n - off);
				if (w < 0 && errno == EINTR)
					continue;
				if (w <= 0)
					break;
				off += (size_t)w;
			}
			close(fd);
		}
	}
#endif
	free(oldpath);
}

void
logline_log(logline_logger *l, logline_level level, const char *msg,
    const logline_field *fields, size_t nfields)
{
	char *line;
	struct buf b = {0};

	if (l == NULL || (l->path == NULL && l->stream == NULL))
		return;
	if ((line = logline_format(logline_now(), current_pid(), level, msg, fields,
	    nfields)) == NULL)
		return;
	puts_(&b, line);
	putch(&b, '\n');
	free(line);
	if ((line = done(&b)) == NULL)
		return;
	if (l->stream != NULL) {
		fputs(line, l->stream);
		fflush(l->stream);
	} else
		append(l->path, line);
	free(line);
}

static void
log_pairs(logline_logger *l, logline_level level, const char *msg, va_list ap)
{
	logline_field *fields = NULL, *f;
	size_t n = 0;
	const char *key, *value;

	while ((key = va_arg(ap, const char *)) != NULL) {
		value = va_arg(ap, const char *);
		if ((f = realloc(fields, (n + 1) * sizeof *f)) == NULL) {
			free(fields);
			return;
		}
		fields = f;
		fields[n].key = key;
		fields[n].value = value;
		n++;
		if (value == NULL)
			break;
	}
	logline_log(l, level, msg, fields, n);
	free(fields);
}

void
logline_info(logline_logger *l, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	log_pairs(l, LOGLINE_INFO, msg, ap);
	va_end(ap);
}

void
logline_warn(logline_logger *l, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	log_pairs(l, LOGLINE_WARN, msg, ap);
	va_end(ap);
}

void
logline_error(logline_logger *l, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	log_pairs(l, LOGLINE_ERROR, msg, ap);
	va_end(ap);
}
