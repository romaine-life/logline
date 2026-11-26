package logline

import (
	"encoding/json"
	"os"
	"testing"
)

// TestRedactGolden runs testdata/redact.json, the cases the Rust crate runs too.
func TestRedactGolden(t *testing.T) {
	b, err := os.ReadFile("testdata/redact.json")
	if err != nil {
		t.Fatal(err)
	}
	var cases []struct{ Note, In, Out string }
	if err := json.Unmarshal(b, &cases); err != nil {
		t.Fatal(err)
	}
	for _, c := range cases {
		if got := Redact(c.In); got != c.Out {
			t.Errorf("%s:\n got %s\nwant %s", c.Note, got, c.Out)
		}
		if again := Redact(c.Out); again != c.Out {
			t.Errorf("%s: masking twice changed it: %s", c.Note, again)
		}
	}
}
