# logline

The home estate's one log line, and the libraries every program writes it with
([AHT-204](https://openproject.home.romaine.life/wp/AHT-204)):

```text
2026-11-25T10:30:43.123-08:00 pid=47672 warn download stalled step=tools took=41.2s
```

The time (RFC 3339, milliseconds, the local zone's offset), the process id, the level
(`info`, `warn` or `error`), the message, then `key=value` fields. A message holding `=`,
starting with a quote, or empty is written quoted, so the first word with `=` always starts
the fields; a value holding a space, quote, `=` or control character is quoted, with Go's
escapes. A message's line breaks become ` | `. One line per event, the file opened and
closed for each line (so log-ship sees it at once), moved to `<name>.old` past 1 MiB.
Never log a secret: every home PC's everyday NAS sign-in can read the pile.

at-home-tools' [docs/app-logs.md](https://forgejo.romaine.life/romaine-life/at-home-tools/src/branch/main/docs/app-logs.md)
is where the standard is written down, with how logs are shipped and searched; this
repository is how a program follows it.

## Using it

**Go** (`github.com/romaine-life/logline`):

```go
log := logline.Open("home-sync", "HOME_SYNC_LOG") // %LOCALAPPDATA%\home-sync\home-sync.log, or $HOME_SYNC_LOG
log.Warn("download stalled", "step", "tools", "took", took)
defer log.Panic() // a crash is logged, then still ends the program

svc := logline.New(os.Stdout) // a service: its platform ships stdout
```

**Rust** (`logline`, by git tag):

```toml
logline = { git = "https://github.com/romaine-life/logline", tag = "v0.1.0" }
```

```rust
let log = logline::Logger::open("tuimail", "TUIMAIL_LOG");
log.warn("download stalled", &[("step", &"tools"), ("took", &took)]);
```

`Parse` (Go) and `parse` (Rust) read a line back and say what doesn't fit the standard;
log-ship uses it to check what it ships.

## How it fits together

| Path | Holds |
| --- | --- |
| `logline.go` | The Go package: `Format`, `Parse`, `Logger` |
| `rust/` | The Rust crate, the same API (a workspace member of the root `Cargo.toml`) |
| `testdata/golden.json` | The cases both implementations write and read identically; a third implementation joins by running it |

## Testing

```sh
go test ./...
cargo test && cargo clippy --all-targets -- -D warnings
```

## How it ships

pipeline-templates' `go`, `rust` and `release` suites, topic `release-module`: a merge
that changes the code takes the next tag and publishes it with its source; each fleet
repository whose `go.mod` requires the module gets a take-up PR. Rust users move their git
tag. The GitHub copy (a push mirror) is what Go and Cargo fetch.
