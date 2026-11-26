// Package logline writes and reads the home estate's one log line (at-home-tools
// docs/app-logs.md, AHT-204):
//
//	2026-11-25T10:30:43.123-08:00 pid=47672 warn download stalled step=tools took=41.2s
//
// That is the time (RFC 3339, milliseconds, the local zone's offset), the process id, the
// level (info, warn or error), the message, then key=value fields. A message holding "=",
// starting with a quote, or empty is written quoted, so the first word with "=" in it
// always starts the fields; a value holding a space, quote, "=" or control character is
// quoted. Quoting is Go's: "\"", "\\", "\n" and the like.
//
// A Logger appends lines to a file, opening and closing it for each line so log-ship
// sees every line at once, and moves the file to <name>.old past 1 MiB. Parse reads a
// line back, which is how log-ship checks what it ships.
package logline

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime/debug"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
)

// Level is how much a line matters.
type Level string

const (
	Info  Level = "info"
	Warn  Level = "warn"
	Error Level = "error"
)

// TimeLayout is the line's time: RFC 3339 with milliseconds and the zone's offset.
const TimeLayout = "2006-01-02T15:04:05.000Z07:00"

// Limit is the size past which a log file moves to <name>.old and starts again.
const Limit = 1 << 20

// Logger writes lines to a file or a stream. Its methods never fail: a log that can't be
// written is skipped, since what failed is still on the program's screen or exit code.
// Never log a secret: every home PC's everyday NAS sign-in can read the pile.
type Logger struct {
	path string    // the file, opened per line; "" when writing to w
	w    io.Writer // a stream (a service's stdout) when there is no file
	mu   sync.Mutex
	pid  int
	now  func() time.Time
}

// Open is the log of the program named app: %LOCALAPPDATA%\<app>\<app>.log on Windows
// (the user's cache directory elsewhere), or the path in the environment variable
// override when it is set, which tests and throwaway servers use to keep their lines out
// of the NAS's pile.
func Open(app, override string) *Logger {
	path := os.Getenv(override)
	if override == "" || path == "" {
		dir, err := os.UserCacheDir()
		if err == nil {
			path = filepath.Join(dir, app, app+".log")
		}
	}
	return &Logger{path: path, pid: os.Getpid(), now: time.Now}
}

// OpenFile is a log at path.
func OpenFile(path string) *Logger {
	return &Logger{path: path, pid: os.Getpid(), now: time.Now}
}

// New is a log written to w: a service's stdout, which its platform ships.
func New(w io.Writer) *Logger {
	return &Logger{w: w, pid: os.Getpid(), now: time.Now}
}

// Path is the file the logger appends to, or "" when it writes to a stream.
func (l *Logger) Path() string { return l.path }

// Info, Warn and Error write one line. kv is key, value pairs: keys are lower-case words
// ([a-z][a-z0-9_.-]*); a value is written with fmt's %v.
func (l *Logger) Info(msg string, kv ...any)  { l.Log(Info, msg, kv...) }
func (l *Logger) Warn(msg string, kv ...any)  { l.Log(Warn, msg, kv...) }
func (l *Logger) Error(msg string, kv ...any) { l.Log(Error, msg, kv...) }

// Log writes one line at level.
func (l *Logger) Log(level Level, msg string, kv ...any) {
	line := Format(l.now(), l.pid, level, msg, kv...) + "\n"
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.path == "" {
		if l.w != nil {
			_, _ = io.WriteString(l.w, line)
		}
		return
	}
	if info, err := os.Stat(l.path); err == nil && info.Size() > Limit {
		_ = os.Rename(l.path, l.path+".old")
	}
	if err := os.MkdirAll(filepath.Dir(l.path), 0o700); err != nil {
		return
	}
	file, err := os.OpenFile(l.path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		return
	}
	defer file.Close()
	_, _ = io.WriteString(file, line)
}

// Panic, deferred, records a crash and lets it continue, so it still ends the program.
func (l *Logger) Panic() {
	if r := recover(); r != nil {
		l.Error("panic", "value", fmt.Sprint(r), "stack", string(debug.Stack()))
		panic(r)
	}
}

// Format is one line, without its newline.
func Format(t time.Time, pid int, level Level, msg string, kv ...any) string {
	var b strings.Builder
	b.WriteString(t.Format(TimeLayout))
	b.WriteString(" pid=")
	b.WriteString(strconv.Itoa(pid))
	b.WriteByte(' ')
	b.WriteString(string(level))
	b.WriteByte(' ')
	msg = oneLine(msg)
	if msg == "" || strings.ContainsRune(msg, '=') || strings.HasPrefix(msg, `"`) {
		b.WriteString(strconv.Quote(msg))
	} else {
		b.WriteString(msg)
	}
	for i := 0; i < len(kv); i += 2 {
		key := fmt.Sprint(kv[i])
		if !validKey(key) {
			key = "bad_key"
		}
		value := "MISSING"
		if i+1 < len(kv) {
			value = fmt.Sprint(kv[i+1])
		}
		b.WriteByte(' ')
		b.WriteString(key)
		b.WriteByte('=')
		b.WriteString(formatValue(value))
	}
	return b.String()
}

// Entry is a parsed line.
type Entry struct {
	Time    time.Time
	PID     int
	Level   Level
	Message string
	Fields  []Field
}

// Field is one key=value.
type Field struct{ Key, Value string }

// Parse reads a line written by Format (without its newline) and says what doesn't fit
// the standard.
func Parse(line string) (Entry, error) {
	var e Entry
	stamp, rest, ok := strings.Cut(line, " ")
	if !ok {
		return e, errors.New("no time")
	}
	t, err := time.Parse(TimeLayout, stamp)
	if err != nil {
		return e, fmt.Errorf("time %q isn't RFC 3339 with milliseconds and a zone", stamp)
	}
	e.Time = t
	pid, rest, ok := strings.Cut(rest, " ")
	n, err := strconv.Atoi(strings.TrimPrefix(pid, "pid="))
	if !ok || !strings.HasPrefix(pid, "pid=") || err != nil || n <= 0 {
		return e, errors.New("no pid=<n> after the time")
	}
	e.PID = n
	level, rest, ok := strings.Cut(rest, " ")
	switch Level(level) {
	case Info, Warn, Error:
		e.Level = Level(level)
	default:
		return e, fmt.Errorf("level %q isn't info, warn or error", level)
	}
	if !ok {
		return e, errors.New("no message")
	}
	if strings.HasPrefix(rest, `"`) {
		q, err := strconv.QuotedPrefix(rest)
		if err != nil {
			return e, errors.New("unterminated quoted message")
		}
		e.Message, _ = strconv.Unquote(q)
		rest = rest[len(q):]
	} else {
		end := len(rest)
		if i := firstField(rest); i >= 0 {
			end = i
		}
		e.Message = strings.TrimSuffix(rest[:end], " ")
		rest = rest[end:]
	}
	for rest != "" {
		if !strings.HasPrefix(rest, " ") {
			return e, fmt.Errorf("junk after the message: %q", rest)
		}
		rest = rest[1:]
		key, after, ok := strings.Cut(rest, "=")
		if !ok || !validKey(key) {
			return e, fmt.Errorf("field %q isn't key=value", rest)
		}
		var value string
		if strings.HasPrefix(after, `"`) {
			q, err := strconv.QuotedPrefix(after)
			if err != nil {
				return e, fmt.Errorf("field %s: unterminated quote", key)
			}
			value, _ = strconv.Unquote(q)
			rest = after[len(q):]
		} else {
			value, rest, _ = strings.Cut(after, " ")
			if rest != "" {
				rest = " " + rest
			}
			if value == "" {
				return e, fmt.Errorf("field %s has no value", key)
			}
		}
		e.Fields = append(e.Fields, Field{key, value})
	}
	return e, nil
}

// firstField is where the first " key=" starts in an unquoted message's line, or -1.
func firstField(s string) int {
	for i := 0; i < len(s); i++ {
		if s[i] != ' ' {
			continue
		}
		word := s[i+1:]
		if j := strings.IndexByte(word, ' '); j >= 0 {
			word = word[:j]
		}
		if key, _, ok := strings.Cut(word, "="); ok && validKey(key) {
			return i
		}
	}
	return -1
}

func validKey(k string) bool {
	if k == "" || k[0] < 'a' || k[0] > 'z' {
		return false
	}
	for _, r := range k {
		if !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '_' || r == '.' || r == '-') {
			return false
		}
	}
	return true
}

func formatValue(v string) string {
	if v == "" || strings.ContainsAny(v, ` "=`) || strings.IndexFunc(v, unicode.IsControl) >= 0 || !utf8Valid(v) {
		return strconv.Quote(v)
	}
	return v
}

func utf8Valid(s string) bool { return strings.ToValidUTF8(s, "�") == s }

// oneLine keeps a message on its line: line breaks become " | ", tabs spaces, other
// control characters go.
func oneLine(text string) string {
	text = strings.ReplaceAll(strings.ReplaceAll(text, "\r\n", "\n"), "\r", "\n")
	text = strings.ReplaceAll(strings.Trim(text, "\n"), "\n", " | ")
	text = strings.Map(func(r rune) rune {
		if r == '\t' {
			return ' '
		}
		if unicode.IsControl(r) {
			return -1
		}
		return r
	}, text)
	return strings.TrimSpace(strings.ToValidUTF8(text, "�"))
}
