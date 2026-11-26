/*
 * logline: the home estate's one log line (at-home-tools docs/app-logs.md, AHT-204),
 * written and read exactly as the Go package at the repository's root does; both run
 * testdata/golden.json and testdata/redact.json.
 *
 *	2026-11-25T10:30:43.123-08:00 pid=47672 warn download stalled step=tools took=41.2s
 *
 * The time (RFC 3339, milliseconds, the local zone's offset), the process id, the level
 * (info, warn or error), the message, then key=value fields. A message holding '=',
 * starting with a quote, or empty is written quoted, so the first word with '=' always
 * starts the fields; a value holding a space, quote, '=' or control character is quoted,
 * with Go's escapes.
 *
 * A logger appends lines to a file, opening and closing it for each line so log-ship sees
 * every line at once, and moves the file to <name>.old past 1 MiB. logline_parse reads a
 * line back. Strings are UTF-8. C99, the C library and the platform's own API only.
 */
#ifndef LOGLINE_H
#define LOGLINE_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How much a line matters. */
typedef enum logline_level {
	LOGLINE_INFO,
	LOGLINE_WARN,
	LOGLINE_ERROR
} logline_level;

/* "info", "warn" or "error". */
const char *logline_level_name(logline_level level);

/* The size past which a log file moves to <name>.old and starts again. */
#define LOGLINE_LIMIT (1L << 20)

/* What logline_redact writes in place of a credential. */
#define LOGLINE_MARKER "***"

/* A moment as the line writes it: the local date and time, and the zone's offset east of
 * UTC in minutes (0 is written Z). */
typedef struct logline_time {
	int year, month, day;
	int hour, minute, second, millisecond;
	int offset_minutes;
} logline_time;

/* Now, in the local zone. */
logline_time logline_now(void);

/* One key=value. Keys are lower-case words ([a-z][a-z0-9_.-]*); another is written
 * bad_key. A NULL value is written MISSING. */
typedef struct logline_field {
	const char *key;
	const char *value;
} logline_field;

/* One line, without its newline, allocated (free it); NULL when memory runs out. */
char *logline_format(logline_time t, long pid, logline_level level, const char *msg,
    const logline_field *fields, size_t nfields);

/* A parsed line; logline_entry_free releases its strings. */
typedef struct logline_entry {
	logline_time time;
	long pid;
	logline_level level;
	char *message;
	logline_field *fields; /* key and value allocated with each field */
	size_t nfields;
} logline_entry;

/* Reads a line written by logline_format (without its newline). 0 on success; -1 when it
 * doesn't fit the standard, with what doesn't fit in err (when errlen > 0). */
int logline_parse(const char *line, logline_entry *entry, char *err, size_t errlen);
void logline_entry_free(logline_entry *entry);

/* A copy of text with what looks like a credential replaced by LOGLINE_MARKER, keeping
 * the rest (free it; NULL when memory runs out). Pass it any field or message holding text
 * from outside the program before logging it: a command's output or arguments, another
 * tool's error text, a URL, a header. Every home PC's everyday NAS sign-in can read the
 * pile. */
char *logline_redact(const char *text);

/*
 * A log file or stream. Its functions never fail: a line that can't be written is
 * skipped, since what failed is still on the program's screen or exit code. Never log a
 * secret. A file line is one append, so lines from several threads or processes never
 * interleave; a logger keeps no lock, as two processes sharing a file (a tmux client and
 * its server) have none between them either.
 */
typedef struct logline_logger logline_logger;

/* The log of the program named app: %LOCALAPPDATA%\<app>\<app>.log on Windows (the user's
 * cache directory elsewhere, as Go's os.UserCacheDir), or the path in the environment
 * variable override_env when it is set, which tests and throwaway servers use to keep
 * their lines out of the NAS's pile. NULL only when memory runs out. */
logline_logger *logline_open(const char *app, const char *override_env);
/* A log at path. */
logline_logger *logline_open_file(const char *path);
/* A log written to a stream: a service's stdout, which its platform ships. */
logline_logger *logline_open_stream(FILE *stream);
/* Frees the logger (it holds no file open; a stream stays open). */
void logline_close(logline_logger *logger);
/* The file the logger appends to, or NULL when it writes to a stream or has no path. */
const char *logline_path(const logline_logger *logger);

/* Writes one line at level. */
void logline_log(logline_logger *logger, logline_level level, const char *msg,
    const logline_field *fields, size_t nfields);
/* Write one line; after the message come key, value string pairs, ended by a NULL key:
 *	logline_warn(log, "download stalled", "step", "tools", "took", took, (char *)NULL); */
void logline_info(logline_logger *logger, const char *msg, ...);
void logline_warn(logline_logger *logger, const char *msg, ...);
void logline_error(logline_logger *logger, const char *msg, ...);

#ifdef __cplusplus
}
#endif

#endif /* LOGLINE_H */
