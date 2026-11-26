//! The home estate's one log line (at-home-tools docs/app-logs.md, AHT-204), written and
//! read exactly as the Go package at the repository's root does; both run
//! `testdata/golden.json`:
//!
//! ```text
//! 2026-11-25T10:30:43.123-08:00 pid=47672 warn download stalled step=tools took=41.2s
//! ```
//!
//! The time (RFC 3339, milliseconds, the local zone's offset), the process id, the level
//! (info, warn or error), the message, then key=value fields. A message holding `=`,
//! starting with a quote, or empty is written quoted, so the first word with `=` always
//! starts the fields; a value holding a space, quote, `=` or control character is quoted,
//! with Go's escapes.
//!
//! A [`Logger`] appends lines to a file, opening and closing it for each line so log-ship
//! sees every line at once, and moves the file to `<name>.old` past 1 MiB. [`parse`] reads
//! a line back.

use chrono::{DateTime, FixedOffset, Local, SecondsFormat};
use std::fmt::Write as _;
use std::fs::{self, OpenOptions};
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// How much a line matters.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    Info,
    Warn,
    Error,
}

impl Level {
    pub fn as_str(self) -> &'static str {
        match self {
            Level::Info => "info",
            Level::Warn => "warn",
            Level::Error => "error",
        }
    }
}

impl std::str::FromStr for Level {
    type Err = String;

    fn from_str(s: &str) -> Result<Level, String> {
        match s {
            "info" => Ok(Level::Info),
            "warn" => Ok(Level::Warn),
            "error" => Ok(Level::Error),
            _ => Err(format!("level {s:?} isn't info, warn or error")),
        }
    }
}

/// The size past which a log file moves to `<name>.old` and starts again.
pub const LIMIT: u64 = 1 << 20;

/// A log file. Its methods never fail: a log that can't be written is skipped, since what
/// failed is still on the program's screen or exit code. Never log a secret: every home
/// PC's everyday NAS sign-in can read the pile.
pub struct Logger {
    path: Option<PathBuf>,
    lock: Mutex<()>,
}

impl Logger {
    /// The log of the program named `app`: `%LOCALAPPDATA%\<app>\<app>.log` on Windows (the
    /// user's cache directory elsewhere), or the path in the environment variable
    /// `override_var` when it is set, which tests use to keep their lines out of the pile.
    pub fn open(app: &str, override_var: &str) -> Logger {
        let path = match std::env::var_os(override_var) {
            Some(p) if !override_var.is_empty() && !p.is_empty() => Some(PathBuf::from(p)),
            _ => cache_dir().map(|d| d.join(app).join(format!("{app}.log"))),
        };
        Logger {
            path,
            lock: Mutex::new(()),
        }
    }

    /// A log at `path`.
    pub fn open_file(path: impl Into<PathBuf>) -> Logger {
        Logger {
            path: Some(path.into()),
            lock: Mutex::new(()),
        }
    }

    /// The file the logger appends to.
    pub fn path(&self) -> Option<&Path> {
        self.path.as_deref()
    }

    pub fn info(&self, msg: &str, fields: &[(&str, &dyn std::fmt::Display)]) {
        self.log(Level::Info, msg, fields)
    }

    pub fn warn(&self, msg: &str, fields: &[(&str, &dyn std::fmt::Display)]) {
        self.log(Level::Warn, msg, fields)
    }

    pub fn error(&self, msg: &str, fields: &[(&str, &dyn std::fmt::Display)]) {
        self.log(Level::Error, msg, fields)
    }

    /// Writes one line at `level`.
    pub fn log(&self, level: Level, msg: &str, fields: &[(&str, &dyn std::fmt::Display)]) {
        let Some(path) = &self.path else { return };
        let now = Local::now().fixed_offset();
        let line = format(now, std::process::id(), level, msg, fields) + "\n";
        let _guard = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        if fs::metadata(path).is_ok_and(|m| m.len() > LIMIT) {
            let mut old = path.as_os_str().to_owned();
            old.push(".old");
            let _ = fs::rename(path, old);
        }
        if let Some(dir) = path.parent() {
            let _ = fs::create_dir_all(dir);
        }
        if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) {
            let _ = file.write_all(line.as_bytes());
        }
    }
}

fn cache_dir() -> Option<PathBuf> {
    if cfg!(windows) {
        std::env::var_os("LOCALAPPDATA").map(PathBuf::from)
    } else if let Some(x) = std::env::var_os("XDG_CACHE_HOME").filter(|x| !x.is_empty()) {
        Some(PathBuf::from(x))
    } else {
        std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache"))
    }
}

/// One line, without its newline.
pub fn format(
    t: DateTime<FixedOffset>,
    pid: u32,
    level: Level,
    msg: &str,
    fields: &[(&str, &dyn std::fmt::Display)],
) -> String {
    let mut out = String::new();
    out.push_str(&t.to_rfc3339_opts(SecondsFormat::Millis, true));
    let _ = write!(out, " pid={pid} {} ", level.as_str());
    let msg = one_line(msg);
    if msg.is_empty() || msg.contains('=') || msg.starts_with('"') {
        out.push_str(&quote(&msg));
    } else {
        out.push_str(&msg);
    }
    for (key, value) in fields {
        let key = if valid_key(key) { key } else { "bad_key" };
        let value = value.to_string();
        let _ = write!(out, " {key}={}", format_value(&value));
    }
    out
}

/// A parsed line.
#[derive(Debug, PartialEq)]
pub struct Entry {
    pub time: DateTime<FixedOffset>,
    pub pid: u32,
    pub level: Level,
    pub message: String,
    pub fields: Vec<(String, String)>,
}

/// Reads a line written by [`format`] (without its newline), or says what doesn't fit the
/// standard.
pub fn parse(line: &str) -> Result<Entry, String> {
    let (stamp, rest) = line.split_once(' ').ok_or("no time")?;
    let time = parse_time(stamp)
        .ok_or_else(|| format!("time {stamp:?} isn't RFC 3339 with milliseconds and a zone"))?;
    let (pid, rest) = rest.split_once(' ').ok_or("no pid=<n> after the time")?;
    let pid: u32 = pid
        .strip_prefix("pid=")
        .and_then(|p| p.parse().ok())
        .filter(|&p| p > 0)
        .ok_or("no pid=<n> after the time")?;
    let (level, rest) = match rest.split_once(' ') {
        Some((l, r)) => (l, Some(r)),
        None => (rest, None),
    };
    let level: Level = level.parse()?;
    let mut rest = rest.ok_or("no message")?;
    let message;
    if rest.starts_with('"') {
        let (q, after) = unquote_prefix(rest).ok_or("unterminated quoted message")?;
        message = q;
        rest = after;
    } else {
        let end = first_field(rest).unwrap_or(rest.len());
        message = rest[..end].trim_end_matches(' ').to_string();
        rest = &rest[end..];
    }
    let mut fields = Vec::new();
    while !rest.is_empty() {
        rest = rest
            .strip_prefix(' ')
            .ok_or_else(|| format!("junk after the message: {rest:?}"))?;
        let (key, after) = rest
            .split_once('=')
            .filter(|(k, _)| valid_key(k))
            .ok_or_else(|| format!("field {rest:?} isn't key=value"))?;
        let value;
        if after.starts_with('"') {
            let (v, a) =
                unquote_prefix(after).ok_or_else(|| format!("field {key}: unterminated quote"))?;
            value = v;
            rest = a;
        } else {
            let end = after.find(' ').unwrap_or(after.len());
            if end == 0 {
                return Err(format!("field {key} has no value"));
            }
            value = after[..end].to_string();
            rest = &after[end..];
        }
        fields.push((key.to_string(), value));
    }
    Ok(Entry {
        time,
        pid,
        level,
        message,
        fields,
    })
}

/// The line's time: exactly three fractional digits and a zone (`Z` or `±hh:mm`).
fn parse_time(s: &str) -> Option<DateTime<FixedOffset>> {
    let b = s.as_bytes();
    if b.len() < 24 || b.get(19) != Some(&b'.') || !b[20..23].iter().all(u8::is_ascii_digit) {
        return None;
    }
    if !matches!(&s[23..], "Z") && !(s.len() == 29 && matches!(b[23], b'+' | b'-')) {
        return None;
    }
    DateTime::parse_from_rfc3339(s).ok()
}

/// Where the first ` key=` starts in an unquoted message's line.
fn first_field(s: &str) -> Option<usize> {
    s.match_indices(' ').map(|(i, _)| i).find(|&i| {
        let word = s[i + 1..].split(' ').next().unwrap_or("");
        word.split_once('=').is_some_and(|(k, _)| valid_key(k))
    })
}

fn valid_key(k: &str) -> bool {
    let mut chars = k.chars();
    matches!(chars.next(), Some('a'..='z'))
        && chars.all(|c| matches!(c, 'a'..='z' | '0'..='9' | '_' | '.' | '-'))
}

fn format_value(v: &str) -> String {
    if v.is_empty() || v.contains([' ', '"', '=']) || v.chars().any(char::is_control) {
        quote(v)
    } else {
        v.to_string()
    }
}

/// Go's strconv.Quote for valid UTF-8: `\"`, `\\`, `\n`, `\r`, `\t` and the rest of
/// Go's short escapes, other control characters as `\x..` or `\u....`.
fn quote(s: &str) -> String {
    let mut out = String::from('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\x07' => out.push_str("\\a"),
            '\x08' => out.push_str("\\b"),
            '\x0c' => out.push_str("\\f"),
            '\x0b' => out.push_str("\\v"),
            c if (c as u32) < 0x20 || c as u32 == 0x7f => {
                let _ = write!(out, "\\x{:02x}", c as u32);
            }
            c if c.is_control() => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Reads a Go-quoted string at the start of `s`: its value and what follows it.
fn unquote_prefix(s: &str) -> Option<(String, &str)> {
    let mut out = String::new();
    let mut chars = s.char_indices();
    chars.next()?; // the opening quote
    while let Some((i, c)) = chars.next() {
        match c {
            '"' => return Some((out, &s[i + 1..])),
            '\\' => {
                let (_, e) = chars.next()?;
                match e {
                    '"' => out.push('"'),
                    '\\' => out.push('\\'),
                    'n' => out.push('\n'),
                    'r' => out.push('\r'),
                    't' => out.push('\t'),
                    'a' => out.push('\x07'),
                    'b' => out.push('\x08'),
                    'f' => out.push('\x0c'),
                    'v' => out.push('\x0b'),
                    'x' | 'u' | 'U' => {
                        let n = match e {
                            'x' => 2,
                            'u' => 4,
                            _ => 8,
                        };
                        let hex: String = (0..n)
                            .filter_map(|_| chars.next().map(|(_, h)| h))
                            .collect();
                        out.push(char::from_u32(u32::from_str_radix(&hex, 16).ok()?)?);
                    }
                    _ => return None,
                }
            }
            c => out.push(c),
        }
    }
    None
}

/// Keeps a message on its line: line breaks become ` | `, tabs spaces, other control
/// characters go.
fn one_line(text: &str) -> String {
    let text = text.replace("\r\n", "\n").replace('\r', "\n");
    let text = text.trim_matches('\n').replace('\n', " | ");
    let text: String = text
        .chars()
        .filter_map(|c| match c {
            '\t' => Some(' '),
            c if c.is_control() => None,
            c => Some(c),
        })
        .collect();
    text.trim().to_string()
}

/// What [`redact`] writes in place of a credential.
pub const MARKER: &str = "***";

/// The estate's one list of credential shapes, the Go package's `secrets` exactly (both
/// run `testdata/redact.json`): each keeps the text that names the credential and
/// replaces the credential itself. Classes are spelled out in ASCII, and `\b` is the
/// ASCII word boundary `(?-u:\b)`, so Rust's and Go's regular expressions read them the
/// same way.
static SECRETS: std::sync::LazyLock<Vec<(regex::Regex, String)>> = std::sync::LazyLock::new(|| {
    let keep = format!("${{1}}{MARKER}");
    [
            // A URL's query parameter: ?code=, &sig=, &access_token= and the like.
            (
                r#"(?i)([?&](?:code|state|sig|signature|token|access_token|id_token|refresh_token|client_secret|password|passwd|secret|key|api_key|apikey|x-goog-signature|x-amz-signature)=)[^&\t\n\f\r "']+"#,
                keep.clone(),
            ),
            // An Authorization header, with or without its scheme.
            (
                r#"(?i)(authorization:[\t\n\f\r ]*(?:bearer|token|basic)?[\t\n\f\r ]*)[^\t\n\f\r '"]+"#,
                keep.clone(),
            ),
            (r#"(?i)(bearer[\t\n\f\r ]+)[^\t\n\f\r '"]+"#, keep.clone()),
            (
                r"(?i)(?-u:\b)(basic[\t\n\f\r ]+)[a-z0-9._~+/=-]{8,}",
                keep.clone(),
            ),
            // NAME=value and "name": "value", for the names credentials go by.
            (
                r#"(?i)(?-u:\b)((?:token|password|passwd|secret|api[_-]?key|access[_-]?key|client[_-]?secret|pat|bw_session)"?[\t\n\f\r ]*[=:][\t\n\f\r ]*"?)[^\t\n\f\r '"&]+"#,
                keep.clone(),
            ),
            // Any name ending in token or _secret, code_verifier and authorization, also in
            // single quotes, keeping an Authorization scheme; a value of digits alone stays.
            (
                r#"(?i)(?-u:\b)((?:[A-Za-z0-9_-]*(?:token|[_-]secret)|code[_-]?verifier|authorization)["']?[\t\n\f\r ]*[=:][\t\n\f\r ]*["']?(?:(?:bearer|token|basic)[\t\n\f\r ]+)?)[^\t\n\f\r '"&,;}]*[^\t\n\f\r '"&,;}0-9][^\t\n\f\r '"&,;}]*"#,
                keep.clone(),
            ),
            // A Cookie or Set-Cookie value, every cookie in it, to the end of the line or quote.
            (
                r#"(?i)(?-u:\b)((?:set-)?cookie["']?[\t\n\f\r ]*[=:][\t\n\f\r ]*["']?)[^\t\n\f\r '"][^\n\r'"]*"#,
                keep.clone(),
            ),
            // A command's flag: --token x, --password=x.
            (
                r#"(?i)(--(?:token|password|secret|api-key|client-secret)[= ])[^\t\n\f\r '"]+"#,
                keep.clone(),
            ),
            // The user and password in a URL.
            (r#"(://)[^/@\t\n\f\r '"]+@"#, format!("{keep}@")),
            // A JWT: three parts, or two long ones.
            (
                r"eyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+|eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}",
                MARKER.to_string(),
            ),
            // GitHub tokens, and OpenBao's (hvs., hvb., s., b.).
            (
                r"(?-u:\b)(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}",
                MARKER.to_string(),
            ),
            (
                r"(?-u:\b)(?:hvs|hvb|s|b)\.[A-Za-z0-9_-]{24,}(?-u:\b)",
                MARKER.to_string(),
            ),
        ]
        .into_iter()
        .map(|(pattern, with)| (regex::Regex::new(pattern).expect(pattern), with))
        .collect()
});

/// Replaces what looks like a credential in `text` with [`MARKER`], keeping the rest.
/// Pass it any field or message holding text from outside the program before logging
/// it: a command's output or arguments, another tool's error text, a URL, a header.
/// Every home PC's everyday NAS sign-in can read the pile.
pub fn redact(text: &str) -> String {
    let mut text = text.to_string();
    for (pattern, with) in SECRETS.iter() {
        text = pattern.replace_all(&text, with.as_str()).into_owned();
    }
    text
}
