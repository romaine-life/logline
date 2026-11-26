// logline: the home estate's one log line (at-home-tools docs/app-logs.md, AHT-204),
// written and read exactly as the Go package at the repository's root does; both run
// testdata/golden.json and testdata/redact.json.
//
//   2026-11-25T10:30:43.123-08:00 pid=47672 warn download stalled step=tools took=41.2s
//
// The time (RFC 3339, milliseconds, the local zone's offset), the process id, the level
// (info, warn or error), the message, then key=value fields. A message holding "=",
// starting with a quote, or empty is written quoted, so the first word with "=" always
// starts the fields; a value holding a space, quote, "=" or control character is quoted,
// with Go's escapes.
//
// A Logger appends lines to a file, opening and closing it for each line so log-ship sees
// every line at once, and moves the file to <name>.old past 1 MiB; or writes them to a
// stream, a service's stdout. parse reads a line back. No dependencies.

import fs from 'node:fs';
import path from 'node:path';

/** The levels a line takes. */
export const levels = Object.freeze(['info', 'warn', 'error']);

/** The size past which a log file moves to <name>.old and starts again. */
export const LIMIT = 1 << 20;

/** What redact writes in place of a credential. */
export const MARKER = '***';

const pad = (n, width = 2) => String(n).padStart(width, '0');

/** The line's time: RFC 3339 with milliseconds and the zone's offset in minutes east of
 * UTC (the local zone's at that moment by default), 0 written Z. */
export function formatTime(date, offset = -date.getTimezoneOffset()) {
  const local = new Date(date.getTime() + offset * 60000);
  const stamp = `${pad(local.getUTCFullYear(), 4)}-${pad(local.getUTCMonth() + 1)}-${pad(local.getUTCDate())}`
    + `T${pad(local.getUTCHours())}:${pad(local.getUTCMinutes())}:${pad(local.getUTCSeconds())}`
    + `.${pad(local.getUTCMilliseconds(), 3)}`;
  if (offset === 0) return `${stamp}Z`;
  const abs = Math.abs(offset);
  return `${stamp}${offset < 0 ? '-' : '+'}${pad(Math.floor(abs / 60))}:${pad(abs % 60)}`;
}

const isControl = code => code < 0x20 || (code >= 0x7f && code <= 0x9f);
// Go's unicode.IsSpace.
const isSpace = code => (code >= 0x09 && code <= 0x0d) || code === 0x20 || code === 0x85 || code === 0xa0
  || code === 0x1680 || (code >= 0x2000 && code <= 0x200a) || code === 0x2028 || code === 0x2029
  || code === 0x202f || code === 0x205f || code === 0x3000;
// A lone surrogate is JavaScript's text that isn't UTF-8; Go writes U+FFFD for it.
const wellFormed = text => text.toWellFormed();

function validKey(key) {
  return /^[a-z][a-z0-9_.-]*$/.test(key);
}

// Go's strconv.Quote for the text a line holds: \" \\ and Go's short escapes, other C0
// characters and DEL as \x.., C1 as \u.....
const shortEscapes = {'"': '\\"', '\\': '\\\\', '\n': '\\n', '\r': '\\r', '\t': '\\t', '\x07': '\\a', '\b': '\\b', '\f': '\\f', '\v': '\\v'};
function quote(text) {
  let out = '"';
  for (const c of text) {
    const code = c.codePointAt(0);
    if (shortEscapes[c]) out += shortEscapes[c];
    else if (code < 0x20 || code === 0x7f) out += `\\x${code.toString(16).padStart(2, '0')}`;
    else if (isControl(code)) out += `\\u${code.toString(16).padStart(4, '0')}`;
    else out += c;
  }
  return `${out}"`;
}

function formatValue(value) {
  if (value === '' || /[ "=]/.test(value) || [...value].some(c => isControl(c.codePointAt(0)))) return quote(value);
  return value;
}

// Keeps a message on its line: line breaks become " | ", tabs spaces, other control
// characters go.
function oneLine(text) {
  text = wellFormed(text).replaceAll('\r\n', '\n').replaceAll('\r', '\n');
  text = text.replace(/^\n+|\n+$/g, '').replaceAll('\n', ' | ');
  let out = '';
  for (const c of text) {
    const code = c.codePointAt(0);
    if (c === '\t') out += ' ';
    else if (!isControl(code)) out += c;
  }
  // Go's strings.TrimSpace (String.prototype.trim also trims U+FEFF, which Go keeps).
  const chars = [...out];
  let start = 0, end = chars.length;
  while (start < end && isSpace(chars[start].codePointAt(0))) start++;
  while (end > start && isSpace(chars[end - 1].codePointAt(0))) end--;
  return chars.slice(start, end).join('');
}

function stringify(value) {
  return wellFormed(String(value));
}

/** Fields as [key, value] pairs, from an object or an array of pairs. */
function pairs(fields) {
  if (fields == null) return [];
  if (Array.isArray(fields)) return fields;
  return Object.entries(fields);
}

/**
 * One line, without its newline. fields is an object ({step: 'tools'}) or an array of
 * [key, value] pairs; a value is written with String(). offset is the zone's minutes east
 * of UTC, the local zone's by default.
 */
export function format(time, pid, level, message, fields = [], offset = -time.getTimezoneOffset()) {
  const msg = oneLine(String(message ?? ''));
  let line = `${formatTime(time, offset)} pid=${pid} ${level} `;
  line += msg === '' || msg.includes('=') || msg.startsWith('"') ? quote(msg) : msg;
  for (const pair of pairs(fields)) {
    const key = validKey(String(pair[0])) ? String(pair[0]) : 'bad_key';
    const value = pair.length < 2 ? 'MISSING' : stringify(pair[1]);
    line += ` ${key}=${formatValue(value)}`;
  }
  return line;
}

const daysIn = (year, month) => new Date(Date.UTC(year, month, 0)).getUTCDate();

// The line's time: exactly three fractional digits and a zone, Z or +hh:mm.
function parseTime(stamp) {
  const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})\.(\d{3})(?:(Z)|([+-])(\d{2}):(\d{2}))$/.exec(stamp);
  if (!m) return null;
  const [year, month, day, hour, minute, second, ms] = m.slice(1, 8).map(Number);
  if (month < 1 || month > 12 || day < 1 || day > daysIn(year, month) || hour > 23 || minute > 59 || second > 59) return null;
  let offset = 0;
  if (!m[8]) {
    const oh = Number(m[10]), om = Number(m[11]);
    if (oh > 23 || om > 59) return null;
    offset = (oh * 60 + om) * (m[9] === '-' ? -1 : 1);
  }
  const date = new Date(Date.UTC(year, month - 1, day, hour, minute, second, ms) - offset * 60000);
  return {date, offset};
}

// Reads a Go-quoted string at the start of s, as strconv.QuotedPrefix and Unquote do: its
// value and how many characters the quoted string took, or null.
function unquotePrefix(s) {
  const simple = {'"': '"', '\\': '\\', n: '\n', r: '\r', t: '\t', a: '\x07', b: '\b', f: '\f', v: '\v'};
  let out = '', i = 1;
  while (i < s.length) {
    const c = s[i];
    if (c === '"') return {value: out, used: i + 1};
    if (c === '\n') return null;
    if (c !== '\\') { out += c; i++; continue; }
    const e = s[i + 1];
    if (e === undefined) return null;
    i += 2;
    if (e in simple) { out += simple[e]; continue; }
    if (e === 'x' || e === 'u' || e === 'U') {
      const len = e === 'x' ? 2 : e === 'u' ? 4 : 8, hex = s.slice(i, i + len);
      if (!new RegExp(`^[0-9a-fA-F]{${len}}$`).test(hex)) return null;
      const code = parseInt(hex, 16);
      if (e !== 'x' && (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))) return null;
      out += String.fromCodePoint(code);
      i += len;
      continue;
    }
    if (/[0-7]/.test(e)) {
      const oct = s.slice(i - 1, i + 2);
      if (!/^[0-7]{3}$/.test(oct) || parseInt(oct, 8) > 255) return null;
      out += String.fromCharCode(parseInt(oct, 8));
      i += 2;
      continue;
    }
    return null;
  }
  return null;
}

// Where the first " key=" starts in an unquoted message's line, or -1.
function firstField(s) {
  for (let i = 0; i < s.length; i++) {
    if (s[i] !== ' ') continue;
    let word = s.slice(i + 1);
    const space = word.indexOf(' ');
    if (space >= 0) word = word.slice(0, space);
    const eq = word.indexOf('=');
    if (eq >= 0 && validKey(word.slice(0, eq))) return i;
  }
  return -1;
}

// Go's strings.Cut.
function cut(s, sep) {
  const i = s.indexOf(sep);
  return i < 0 ? [s, '', false] : [s.slice(0, i), s.slice(i + sep.length), true];
}

/**
 * Reads a line written by format (without its newline): {time (a Date), offset (minutes
 * east of UTC), pid, level, message, fields ([key, value] pairs)}. Throws an Error saying
 * what doesn't fit the standard.
 */
export function parse(line) {
  let [stamp, rest, ok] = cut(line, ' ');
  if (!ok) throw new Error('no time');
  const time = parseTime(stamp);
  if (!time) throw new Error(`time ${JSON.stringify(stamp)} isn't RFC 3339 with milliseconds and a zone`);
  let pid;
  [pid, rest, ok] = cut(rest, ' ');
  const n = /^pid=[+-]?\d+$/.test(pid) ? Number(pid.slice(4)) : NaN;
  if (!ok || !(n > 0) || !Number.isSafeInteger(n)) throw new Error('no pid=<n> after the time');
  let level;
  [level, rest, ok] = cut(rest, ' ');
  if (!levels.includes(level)) throw new Error(`level ${JSON.stringify(level)} isn't info, warn or error`);
  if (!ok) throw new Error('no message');
  let message;
  if (rest.startsWith('"')) {
    const q = unquotePrefix(rest);
    if (!q) throw new Error('unterminated quoted message');
    message = q.value;
    rest = rest.slice(q.used);
  } else {
    const i = firstField(rest), end = i >= 0 ? i : rest.length;
    message = rest.slice(0, end);
    if (message.endsWith(' ')) message = message.slice(0, -1);
    rest = rest.slice(end);
  }
  const fields = [];
  while (rest !== '') {
    if (!rest.startsWith(' ')) throw new Error(`junk after the message: ${JSON.stringify(rest)}`);
    rest = rest.slice(1);
    const [key, after, found] = cut(rest, '=');
    if (!found || !validKey(key)) throw new Error(`field ${JSON.stringify(rest)} isn't key=value`);
    let value;
    if (after.startsWith('"')) {
      const q = unquotePrefix(after);
      if (!q) throw new Error(`field ${key}: unterminated quote`);
      value = q.value;
      rest = after.slice(q.used);
    } else {
      [value, rest] = cut(after, ' ');
      if (rest !== '') rest = ` ${rest}`;
      if (value === '') throw new Error(`field ${key} has no value`);
    }
    fields.push([key, value]);
  }
  return {time: time.date, offset: time.offset, pid: n, level, message, fields};
}

// The estate's one list of credential shapes, the Go package's secrets exactly (redact.go;
// both run testdata/redact.json): each keeps the text that names the credential and
// replaces the credential itself. JavaScript's \b is ASCII's without the u flag, as Go's is.
const keep = `$1${MARKER}`;
const secrets = [
  // A URL's query parameter: ?code=, &sig=, &access_token= and the like.
  [/([?&](?:code|state|sig|signature|token|access_token|id_token|refresh_token|client_secret|password|passwd|secret|key|api_key|apikey|x-goog-signature|x-amz-signature)=)[^&\t\n\f\r "']+/gi, keep],
  // An Authorization header, with or without its scheme.
  [/(authorization:[\t\n\f\r ]*(?:bearer|token|basic)?[\t\n\f\r ]*)[^\t\n\f\r '"]+/gi, keep],
  [/(bearer[\t\n\f\r ]+)[^\t\n\f\r '"]+/gi, keep],
  [/\b(basic[\t\n\f\r ]+)[a-z0-9._~+/=-]{8,}/gi, keep],
  // NAME=value and "name": "value", for the names credentials go by.
  [/\b((?:token|password|passwd|secret|api[_-]?key|access[_-]?key|client[_-]?secret|pat|bw_session)"?[\t\n\f\r ]*[=:][\t\n\f\r ]*"?)[^\t\n\f\r '"&]+/gi, keep],
  // Any name ending in token or _secret, code_verifier and authorization, also in single
  // quotes, keeping an Authorization scheme; a value of digits alone stays.
  [/\b((?:[A-Za-z0-9_-]*(?:token|[_-]secret)|code[_-]?verifier|authorization)["']?[\t\n\f\r ]*[=:][\t\n\f\r ]*["']?(?:(?:bearer|token|basic)[\t\n\f\r ]+)?)[^\t\n\f\r '"&,;}]*[^\t\n\f\r '"&,;}0-9][^\t\n\f\r '"&,;}]*/gi, keep],
  // A Cookie or Set-Cookie value, every cookie in it, to the end of the line or quote.
  [/\b((?:set-)?cookie["']?[\t\n\f\r ]*[=:][\t\n\f\r ]*["']?)[^\t\n\f\r '"][^\n\r'"]*/gi, keep],
  // A command's flag: --token x, --password=x.
  [/(--(?:token|password|secret|api-key|client-secret)[= ])[^\t\n\f\r '"]+/gi, keep],
  // The user and password in a URL.
  [/(:\/\/)[^/@\t\n\f\r '"]+@/g, `$1${MARKER}@`],
  // A JWT: three parts, or two long ones.
  [/eyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+|eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}/g, MARKER],
  // GitHub tokens, and OpenBao's (hvs., hvb., s., b.).
  [/\b(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}/g, MARKER],
  [/\b(?:hvs|hvb|s|b)\.[A-Za-z0-9_-]{24,}\b/g, MARKER],
];

/**
 * text with what looks like a credential replaced by MARKER, keeping the rest. Pass it any
 * field or message holding text from outside the program before logging it: a command's
 * output or arguments, another tool's error text, a URL, a header. Every home PC's
 * everyday NAS sign-in can read the pile.
 */
export function redact(text) {
  let out = String(text);
  for (const [pattern, replacement] of secrets) out = out.replace(pattern, replacement);
  return out;
}

// Go's os.UserCacheDir: %LocalAppData%, ~/Library/Caches, or $XDG_CACHE_HOME (when
// absolute) else ~/.cache; null when it can't be found.
function cacheDir(env = process.env) {
  if (process.platform === 'win32') return env.LOCALAPPDATA || null;
  if (process.platform === 'darwin') return env.HOME ? path.join(env.HOME, 'Library', 'Caches') : null;
  if (env.XDG_CACHE_HOME) return path.isAbsolute(env.XDG_CACHE_HOME) ? env.XDG_CACHE_HOME : null;
  return env.HOME ? path.join(env.HOME, '.cache') : null;
}

/**
 * A log file or stream. Its methods never throw: a line that can't be written is
 * skipped, since what failed is still on the program's screen or exit code. Never log a
 * secret: every home PC's everyday NAS sign-in can read the pile.
 */
export class Logger {
  #path;
  #stream;
  #now;

  /** {path} for a file, {stream} for a stream; now is the clock, for tests. */
  constructor({path: file = null, stream = null, now = () => new Date()} = {}) {
    this.#path = file;
    this.#stream = stream;
    this.#now = now;
  }

  /**
   * The log of the program named app: %LOCALAPPDATA%\<app>\<app>.log on Windows (the
   * user's cache directory elsewhere), or the path in the environment variable
   * overrideVar when it is set, which tests and throwaway servers use to keep their lines
   * out of the NAS's pile.
   */
  static open(app, overrideVar) {
    let file = overrideVar ? process.env[overrideVar] || null : null;
    if (!file) {
      const dir = cacheDir();
      file = dir ? path.join(dir, app, `${app}.log`) : null;
    }
    return new Logger({path: file});
  }

  /** A log at file. */
  static file(file) {
    return new Logger({path: file});
  }

  /** A log written to a stream (a service's stdout, which its platform ships). */
  static stream(stream = process.stdout) {
    return new Logger({stream});
  }

  /** The file the logger appends to, or null when it writes to a stream. */
  get path() {
    return this.#path;
  }

  info(message, fields) { this.log('info', message, fields); }
  warn(message, fields) { this.log('warn', message, fields); }
  error(message, fields) { this.log('error', message, fields); }

  /** Writes one line at level. */
  log(level, message, fields) {
    try {
      const line = `${format(this.#now(), process.pid, level, message, fields)}\n`;
      if (this.#stream) { this.#stream.write(line); return; }
      if (!this.#path) return;
      try {
        if (fs.statSync(this.#path).size > LIMIT) fs.renameSync(this.#path, `${this.#path}.old`);
      } catch {}
      fs.mkdirSync(path.dirname(this.#path), {recursive: true, mode: 0o700});
      fs.appendFileSync(this.#path, line, {mode: 0o600});
    } catch {}
  }
}
