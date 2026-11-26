//! The cases every implementation writes and reads the same way: the repository's
//! testdata/golden.json, which the Go package runs too.

use chrono::DateTime;
use logline::{Level, Logger, format, parse};
use serde::Deserialize;

#[derive(Deserialize)]
struct Golden {
    note: String,
    time: String,
    pid: u32,
    level: String,
    message_in: Option<String>,
    message: String,
    fields: Vec<(String, String)>,
    line: String,
}

fn cases() -> Vec<Golden> {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../testdata/golden.json");
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

#[test]
fn golden_lines_format_and_parse() {
    for c in cases() {
        let t = DateTime::parse_from_rfc3339(&c.time).unwrap();
        let level: Level = c.level.parse().unwrap();
        let input = c.message_in.as_deref().unwrap_or(&c.message);
        let fields: Vec<(&str, &dyn std::fmt::Display)> = c
            .fields
            .iter()
            .map(|(k, v)| (k.as_str(), v as &dyn std::fmt::Display))
            .collect();
        assert_eq!(
            format(t, c.pid, level, input, &fields),
            c.line,
            "{}",
            c.note
        );
        let e = parse(&c.line).unwrap_or_else(|err| panic!("{}: {err}", c.note));
        assert_eq!(
            (e.time, e.pid, e.level, &e.message, &e.fields),
            (t, c.pid, level, &c.message, &c.fields),
            "{}",
            c.note
        );
    }
}

#[test]
fn parse_refuses_what_isnt_the_standard() {
    for line in [
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
    ] {
        assert!(parse(line).is_err(), "accepted {line:?}");
    }
}

#[test]
fn a_file_log_appends_and_rotates() {
    let dir = std::env::temp_dir().join(format!("logline-test-{}", std::process::id()));
    let path = dir.join("app").join("app.log");
    let log = Logger::open_file(&path);
    log.info("one", &[("n", &1)]);
    log.warn("two", &[]);
    let text = std::fs::read_to_string(&path).unwrap();
    let lines: Vec<_> = text.lines().collect();
    assert_eq!(lines.len(), 2);
    assert!(
        lines[0].ends_with(" info one n=1") && parse(lines[0]).is_ok(),
        "{text}"
    );
    std::fs::write(&path, vec![b'x'; logline::LIMIT as usize + 1]).unwrap();
    log.error("three", &[]);
    let mut old = path.as_os_str().to_owned();
    old.push(".old");
    assert_eq!(std::fs::metadata(old).unwrap().len(), logline::LIMIT + 1);
    assert!(
        std::fs::read_to_string(&path)
            .unwrap()
            .ends_with(" error three\n")
    );
    let _ = std::fs::remove_dir_all(dir);
}
