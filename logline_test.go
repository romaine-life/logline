package logline

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

// golden is testdata/golden.json: the cases every implementation (this package, the
// Rust crate) writes and reads the same way.
type golden struct {
	Note      string      `json:"note"`
	Time      string      `json:"time"`
	PID       int         `json:"pid"`
	Level     Level       `json:"level"`
	MessageIn *string     `json:"message_in"`
	Message   string      `json:"message"`
	Fields    [][2]string `json:"fields"`
	Line      string      `json:"line"`
}

func cases(t *testing.T) []golden {
	t.Helper()
	b, err := os.ReadFile("testdata/golden.json")
	if err != nil {
		t.Fatal(err)
	}
	var cs []golden
	if err := json.Unmarshal(b, &cs); err != nil {
		t.Fatal(err)
	}
	return cs
}

func TestGoldenLinesFormatAndParse(t *testing.T) {
	for _, c := range cases(t) {
		tm, err := time.Parse(time.RFC3339Nano, c.Time)
		if err != nil {
			t.Fatalf("%s: %v", c.Note, err)
		}
		in := c.Message
		if c.MessageIn != nil {
			in = *c.MessageIn
		}
		var kv []any
		for _, f := range c.Fields {
			kv = append(kv, f[0], f[1])
		}
		if got := Format(tm, c.PID, c.Level, in, kv...); got != c.Line {
			t.Errorf("%s:\n got %s\nwant %s", c.Note, got, c.Line)
		}
		e, err := Parse(c.Line)
		if err != nil {
			t.Errorf("%s: parse: %v", c.Note, err)
			continue
		}
		var fields [][2]string
		for _, f := range e.Fields {
			fields = append(fields, [2]string{f.Key, f.Value})
		}
		if !e.Time.Equal(tm) || e.PID != c.PID || e.Level != c.Level || e.Message != c.Message || !slices.Equal(fields, c.Fields) {
			t.Errorf("%s: parsed %+v", c.Note, e)
		}
	}
}

func TestParseRefusesWhatIsntTheStandard(t *testing.T) {
	for _, line := range []string{
		"",
		"hello",
		"2026-11-25T10:30:43-08:00 pid=1 info no milliseconds",
		"2026-11-25 10:30:43.123 pid=1 info no zone",
		"2026/11/25 10:30:43 strip-owner's old format",
		"2026-11-25T10:30:43.123-08:00 info no pid",
		"2026-11-25T10:30:43.123-08:00 pid=0 info zero pid",
		"2026-11-25T10:30:43.123-08:00 pid=1 debug unknown level",
		"2026-11-25T10:30:43.123-08:00 pid=1 info",
		`2026-11-25T10:30:43.123-08:00 pid=1 info "unterminated`,
		`2026-11-25T10:30:43.123-08:00 pid=1 info msg k="unterminated`,
		"2026-11-25T10:30:43.123-08:00 pid=1 info msg k=",
		`2026-11-25T10:30:43.123-08:00 pid=1 info "quoted"junk`,
	} {
		if _, err := Parse(line); err == nil {
			t.Errorf("accepted %q", line)
		}
	}
}

func TestOddKeysAndMissingValuesStillMakeAStandardLine(t *testing.T) {
	line := Format(time.Now(), 3, Info, "x", "Bad Key", 1, "dangling")
	if !strings.HasSuffix(line, " info x bad_key=1 dangling=MISSING") {
		t.Fatal(line)
	}
	if _, err := Parse(line); err != nil {
		t.Fatal(err)
	}
}

func TestAFileLogAppendsClosesAndRotates(t *testing.T) {
	path := filepath.Join(t.TempDir(), "app", "app.log")
	l := OpenFile(path)
	l.now = func() time.Time { return time.Date(2026, 11, 25, 10, 0, 0, 0, time.FixedZone("", -8*3600)) }
	l.Info("one", "n", 1)
	l.Warn("two")
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	pid := os.Getpid()
	want := Format(l.now(), pid, Info, "one", "n", 1) + "\n" + Format(l.now(), pid, Warn, "two") + "\n"
	if string(b) != want {
		t.Fatalf("file %q", b)
	}
	// Past the limit, the next line starts a new file and the old one moves aside.
	if err := os.WriteFile(path, bytes.Repeat([]byte("x"), Limit+1), 0o600); err != nil {
		t.Fatal(err)
	}
	l.Error("three")
	if old, _ := os.Stat(path + ".old"); old == nil || old.Size() != Limit+1 {
		t.Fatal("the full file wasn't moved to .old")
	}
	if b, _ := os.ReadFile(path); string(b) != Format(l.now(), pid, Error, "three")+"\n" {
		t.Fatalf("new file %q", b)
	}
}

func TestOpenTakesTheOverridePath(t *testing.T) {
	path := filepath.Join(t.TempDir(), "x.log")
	t.Setenv("LOGLINE_TEST_LOG", path)
	if got := Open("someapp", "LOGLINE_TEST_LOG").Path(); got != path {
		t.Fatalf("path %q", got)
	}
	t.Setenv("LOGLINE_TEST_LOG", "")
	if got := Open("someapp", "LOGLINE_TEST_LOG").Path(); !strings.HasSuffix(got, filepath.Join("someapp", "someapp.log")) {
		t.Fatalf("default path %q", got)
	}
}

func TestAStreamLogWritesLines(t *testing.T) {
	var b bytes.Buffer
	l := New(&b)
	l.Info("served", "status", 200)
	if _, err := Parse(strings.TrimSuffix(b.String(), "\n")); err != nil || !strings.HasSuffix(b.String(), " info served status=200\n") {
		t.Fatalf("%q %v", b.String(), err)
	}
}

func TestPanicIsLoggedAndContinues(t *testing.T) {
	var b bytes.Buffer
	l := New(&b)
	defer func() {
		if r := recover(); r != "boom" {
			t.Fatalf("recovered %v", r)
		}
		e, err := Parse(strings.SplitN(b.String(), "\n", 2)[0])
		if err != nil || e.Level != Error || e.Message != "panic" || e.Fields[0].Value != "boom" {
			t.Fatalf("%q %v", b.String(), err)
		}
	}()
	defer l.Panic()
	panic("boom")
}

// Invalid UTF-8 in a value is written as U+FFFD, as every implementation's strings
// (which can't hold it) would have it.
func TestInvalidUTF8InAValueBecomesTheReplacementCharacter(t *testing.T) {
	line := Format(time.Date(2026, 1, 2, 3, 4, 5, 6e6, time.UTC), 9, Info, "x", "v", "a\xffb")
	if !strings.HasSuffix(line, " info x v=a�b") {
		t.Fatal(line)
	}
}
