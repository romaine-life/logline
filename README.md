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
logline = { git = "https://forgejo.romaine.life/romaine-life/logline", tag = "v0.1.0" }
```

```rust
let log = logline::Logger::open("tuimail", "TUIMAIL_LOG");
log.warn("download stalled", &[("step", &"tools"), ("took", &took)]);
```

**C** (`c/logline.h`, `c/logline.c`; C99, the C library and the platform's own API, nothing
else). A CMake build takes it as a package, `find_package(logline CONFIG REQUIRED)` and
`target_link_libraries(... logline::logline)`, which a vcpkg overlay port builds from this
repository at a pinned commit (`vcpkg_from_git`, then `vcpkg_cmake_configure` with
`SOURCE_PATH "${SOURCE_PATH}/c"` and `-DLOGLINE_TESTS=OFF`), the way at-tmux takes
at-protocol; a build without CMake (at-tmux's configure) fetches the same commit and compiles
`c/logline.c` with `c/` on its include path.

```c
logline_logger *log = logline_open("tmux", "TMUX_LOG"); /* %LOCALAPPDATA%\tmux\tmux.log, or $TMUX_LOG */
logline_warn(log, "download stalled", "step", "tools", "took", took, (char *)NULL);

char *safe = logline_redact(output); /* allocated, as logline_format's and logline_parse's strings are */
logline_error(log, "command failed", "output", safe, (char *)NULL);
free(safe);
```

`logline_log` takes the fields as an array of `logline_field`, and `logline_open_stream(stdout)`
writes to a stream. Values are strings; the caller formats numbers. A logger keeps no lock: a
file line is one append, which never interleaves with another thread's or process's.

**C#** (`dotnet/Logline`, namespace `RomaineLife.Logline`; .NET 9, no packages). The estate
has no NuGet feed, so a program builds the source in: its project compiles
`dotnet/Logline/*.cs` from a copy of this repository pinned at a release tag
(`<Compile Include="...\dotnet\Logline\*.cs" />`), or references `dotnet/Logline/Logline.csproj`.
Compiling it in keeps a mod such as spirelens one assembly, as its hot-reloaded Core is.

```csharp
var log = Logger.Open("spirelens", "SPIRELENS_LOG");
log.Warn("download stalled", ("step", "tools"), ("took", took));
log.Error("ingest failed", ("error", LogLine.Redact(e.Message)));

var svc = new Logger(Console.Out); // a service: its platform ships stdout
```

**JavaScript** (`@romaine-life/logline`, `js/logline.mjs`; an ES module for Node 20 and later,
no dependencies). Each release publishes it to Forgejo's npm registry at the release's
version (pipeline-templates' `packages.mjs`); a service takes it with the registry lines in
an `.npmrc` beside its `package.json` and an exact pin, and every later release proposes
itself to that service as a take-up PR:

```text
@romaine-life:registry=https://forgejo.romaine.life/api/packages/romaine-life/npm/
//forgejo.romaine.life/api/packages/romaine-life/npm/:_authToken=${FORGEJO_PACKAGES_TOKEN}
```

```js
import {Logger, redact} from '@romaine-life/logline';

const log = Logger.stream(process.stdout); // a service: its platform ships stdout
log.warn('download stalled', {step: 'tools', took: '41.2s'});
log.error('upstream failed', {error: redact(err.message)});

const file = Logger.open('pokosmari', 'POKOSMARI_LOG'); // a file, as a program on a PC writes
```

Fields are an object or an array of `[key, value]` pairs; a value is written with `String()`.

`Parse` (Go, C#), `parse` (Rust, JavaScript) and `logline_parse` (C) read a line back and say
what doesn't fit the standard; log-ship uses Go's to check what it ships.

## Masking credentials

`Redact` (Go), `redact` (Rust, JavaScript), `LogLine.Redact` (C#) and `logline_redact` (C)
replace what looks like a credential with `***`, keeping the rest of the text. Pass any field
or message through it that holds text from outside the program: a command's output or the
arguments it was run with, another tool's error text, a URL, a header, a container's log line.
Text the program writes itself (its own messages, counts, paths it chose) needs none.

```go
log.Error("command failed", "name", name, "error", logline.Redact(err.Error()))
```

It holds the estate's one list of credential shapes; a program never keeps its own:

| Shape | Example, masked |
| --- | --- |
| A URL's credential parameter (`code`, `state`, `sig`, `token`, `access_token`, `password`, `key`, cloud signatures...) | `/callback?code=***&state=***` |
| An `Authorization` header, a bearer token, basic credentials | `Authorization: Bearer ***` |
| A name credentials go by, as `NAME=value` or a JSON field (`token`, `password`, `secret`, `api_key`, `client_secret`, `pat`, `BW_SESSION`...) | `PASSWORD=***`, `"password": "***"` |
| Any name ending in `token` or `_secret` (`refresh_token`, `id_token`, `session_token`, `accessToken`, `X-Auth-Token`...), `code_verifier` and `authorization`, as `NAME=value`, `name: value`, a JSON or single-quoted field, or a URL parameter; a value of digits alone (`"page_token": 2`) is kept, and so are `tokens=3`, `max_tokens`, `token_type` | `refresh_token=***`, `"id_token": "***"` |
| A `Cookie` or `Set-Cookie` value, every cookie in it, to the end of the line or quote | `Cookie: ***` |
| A flag (`--token`, `--password`, `--secret`, `--api-key`, `--client-secret`) | `--client-secret ***` |
| The user and password in a URL | `https://***@forgejo.romaine.life/x` |
| A JWT, a GitHub token, an OpenBao token | `***` |

A new shape goes into every implementation and `testdata/redact.json`, which they all run.

## How it fits together

| Path | Holds |
| --- | --- |
| `logline.go` | The Go package: `Format`, `Parse`, `Logger` |
| `rust/` | The Rust crate, the same API (a workspace member of the root `Cargo.toml`) |
| `c/` | The C library, its CMake package and its test (the root `CMakeLists.txt` and `CMakePresets.json` build it for CI) |
| `dotnet/` | The C# library and its xunit tests (the root `logline.sln` holds both) |
| `js/`, `package.json` | The JavaScript module and its `node --test` tests; the root `package.json` is the npm package |
| `testdata/golden.json` | The cases every implementation writes and reads identically; a new implementation joins by running it |
| `redact.go`, `testdata/redact.json` | `Redact` and the cases every implementation masks identically |

## Testing

```sh
go test ./...
cargo test && cargo clippy --all-targets -- -D warnings
cmake --preset ci -B out && cmake --build out && ctest --test-dir out
dotnet test logline.sln
npm test
```

## How it ships

pipeline-templates' `go`, `rust`, `cmake` (Linux and Windows), `dotnet` (Windows), `web-app`
and `release` suites, topic `release-module`: every pull request runs each language's tests,
and a merge that changes the code takes the next tag and publishes it with its source. Each
fleet repository whose `go.mod` requires the module gets a take-up PR, and the release
publishes `@romaine-life/logline` to Forgejo's npm registry, which proposes it to every
repository whose `package.json` uses it. Rust users move their git tag; C and C# programs
move the commit or tag they pin. Go fetches the module by its GitHub path (a push mirror of
this repository), which the go suite maps to Forgejo; Cargo fetches from Forgejo directly, as
pipeline-templates' check-fetches requires of git dependencies. The repository is public on
both, so a docker build reads it with no credentials.
