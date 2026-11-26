//! testdata/redact.json, which the Go package runs too.

use serde::Deserialize;

#[derive(Deserialize)]
struct Case {
    note: String,
    #[serde(rename = "in")]
    input: String,
    out: String,
}

#[test]
fn redact_golden() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../testdata/redact.json");
    let cases: Vec<Case> = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    for c in cases {
        assert_eq!(logline::redact(&c.input), c.out, "{}", c.note);
        assert_eq!(
            logline::redact(&c.out),
            c.out,
            "{}: masking twice changed it",
            c.note
        );
    }
}
