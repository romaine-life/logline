// testdata/golden.json and testdata/redact.json, the cases every implementation runs, then
// the logger's own.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {Writable} from 'node:stream';
import test from 'node:test';
import {LIMIT, Logger, format, formatTime, parse, redact} from './logline.mjs';

const testdata = name => JSON.parse(fs.readFileSync(new URL(`../testdata/${name}`, import.meta.url), 'utf8'));

// A golden case's time: the moment and its zone's offset in minutes.
function moment(text) {
  const m = /(Z|([+-])(\d{2}):(\d{2}))$/.exec(text);
  const offset = m[1] === 'Z' ? 0 : (Number(m[3]) * 60 + Number(m[4])) * (m[2] === '-' ? -1 : 1);
  return {date: new Date(text), offset};
}

test('golden lines format and parse', () => {
  for (const c of testdata('golden.json')) {
    const {date, offset} = moment(c.time);
    assert.equal(format(date, c.pid, c.level, c.message_in ?? c.message, c.fields, offset), c.line, c.note);
    const e = parse(c.line);
    assert.equal(e.time.getTime(), date.getTime(), c.note);
    assert.equal(e.offset, offset, c.note);
    assert.deepEqual([e.pid, e.level, e.message, e.fields], [c.pid, c.level, c.message, c.fields], c.note);
  }
});

test('parse refuses what isn\'t the standard', () => {
  for (const line of [
    '',
    'hello',
    '2026-11-25T10:30:43-08:00 pid=1 info no milliseconds',
    '2026-11-25 10:30:43.123 pid=1 info no zone',
    '2026/11/25 10:30:43 strip-owner\'s old format',
    '2026-11-25T10:30:43.123-08:00 info no pid',
    '2026-11-25T10:30:43.123-08:00 pid=0 info zero pid',
    '2026-11-25T10:30:43.123-08:00 pid=1 debug unknown level',
    '2026-11-25T10:30:43.123-08:00 pid=1 info',
    '2026-11-25T10:30:43.123-08:00 pid=1 info "unterminated',
    '2026-11-25T10:30:43.123-08:00 pid=1 info msg k="unterminated',
    '2026-11-25T10:30:43.123-08:00 pid=1 info msg k=',
    '2026-11-25T10:30:43.123-08:00 pid=1 info "quoted"junk',
  ]) assert.throws(() => parse(line), undefined, line);
});

test('odd keys and missing values still make a standard line', () => {
  const line = format(new Date(), 3, 'info', 'x', [['Bad Key', 1], ['dangling']]);
  assert.ok(line.endsWith(' info x bad_key=1 dangling=MISSING'), line);
  parse(line);
});

test('fields can be an object', () => {
  assert.ok(format(new Date(), 3, 'warn', 'download stalled', {step: 'tools', took: '41.2s'})
    .endsWith(' warn download stalled step=tools took=41.2s'));
});

test('the local zone is the default', () => {
  const now = new Date();
  assert.equal(format(now, 1, 'info', 'x').split(' ')[0], formatTime(now, -now.getTimezoneOffset()));
  assert.equal(parse(format(now, 1, 'info', 'x')).time.getTime(), now.getTime());
});

test('redact golden', () => {
  for (const c of testdata('redact.json')) {
    assert.equal(redact(c.in), c.out, c.note);
    assert.equal(redact(c.out), c.out, `${c.note}: masking twice changed it`);
  }
});

test('a file log appends, closes and rotates', t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'logline-'));
  t.after(() => fs.rmSync(dir, {recursive: true, force: true}));
  const file = path.join(dir, 'app', 'app.log');
  const now = () => new Date('2026-11-25T10:00:00.000-08:00');
  const l = new Logger({path: file, now});
  l.info('one', {n: 1});
  l.warn('two');
  const at = date => format(date, process.pid, 'info', 'one', {n: 1});
  assert.equal(fs.readFileSync(file, 'utf8'), `${at(now())}\n${format(now(), process.pid, 'warn', 'two')}\n`);
  // Past the limit, the next line starts a new file and the old one moves aside.
  fs.writeFileSync(file, 'x'.repeat(LIMIT + 1));
  l.error('three');
  assert.equal(fs.statSync(`${file}.old`).size, LIMIT + 1);
  assert.equal(fs.readFileSync(file, 'utf8'), `${format(now(), process.pid, 'error', 'three')}\n`);
});

test('open takes the override path', t => {
  const saved = process.env.LOGLINE_TEST_LOG;
  t.after(() => { if (saved === undefined) delete process.env.LOGLINE_TEST_LOG; else process.env.LOGLINE_TEST_LOG = saved; });
  const file = path.join(os.tmpdir(), 'x.log');
  process.env.LOGLINE_TEST_LOG = file;
  assert.equal(Logger.open('someapp', 'LOGLINE_TEST_LOG').path, file);
  process.env.LOGLINE_TEST_LOG = '';
  assert.ok(Logger.open('someapp', 'LOGLINE_TEST_LOG').path.endsWith(path.join('someapp', 'someapp.log')));
});

test('a stream log writes lines', () => {
  let written = '';
  const l = Logger.stream(new Writable({write(chunk, _, done) { written += chunk; done(); }}));
  assert.equal(l.path, null);
  l.info('served', {status: 200});
  assert.ok(written.endsWith(' info served status=200\n'), written);
  parse(written.slice(0, -1));
});
