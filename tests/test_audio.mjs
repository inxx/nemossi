import test from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { encodeWav, makeDemoWav, resampleMono, wavEnvelope, TARGET_SAMPLE_RATE } from '../web/audio.js';

test('PCM16 WAV preserves signed endpoints, clamps overload and silences non-finite input', async () => {
  const blob = encodeWav(new Float32Array([-2, -1, -0.5, 0, 0.5, 1, 2, NaN, Infinity]));
  const bytes = Buffer.from(await blob.arrayBuffer());
  assert.equal(blob.type, 'audio/wav');
  assert.equal(bytes.toString('ascii', 0, 4), 'RIFF');
  assert.equal(bytes.toString('ascii', 8, 12), 'WAVE');
  assert.equal(bytes.readUInt32LE(4), bytes.length - 8);
  assert.equal(bytes.readUInt16LE(20), 1);
  assert.equal(bytes.readUInt16LE(22), 1);
  assert.equal(bytes.readUInt32LE(24), TARGET_SAMPLE_RATE);
  assert.equal(bytes.readUInt16LE(34), 16);
  assert.equal(bytes.readUInt32LE(40), 18);
  assert.deepEqual(Array.from({ length: 9 }, (_, i) => bytes.readInt16LE(44 + i * 2)),
    [-32768, -32768, -16384, 0, 16384, 32767, 32767, 0, 0]);
});

test('48k microphone conversion preserves duration and mean amplitude at 16k', () => {
  const input = new Float32Array(48000).fill(0.25);
  const output = resampleMono(input, 48000);
  assert.equal(output.length, 16000);
  assert.ok(output.every(value => value === 0.25));
  assert.equal(input.length, 48000);
});

test('44.1k conversion accepts non-integer ratios without duration drift or invalid samples', () => {
  const input = new Float32Array(44100);
  for (let i = 0; i < input.length; i++) input[i] = Math.sin(i * 2 * Math.PI * 200 / 44100) * 0.2;
  const output = resampleMono(input, 44100);
  assert.equal(output.length, 16000);
  assert.ok(output.every(value => Number.isFinite(value) && Math.abs(value) <= 0.2));
});

test('8k upsample, identity and empty input have deterministic ownership and bounds', () => {
  assert.deepEqual(Array.from(resampleMono(new Float32Array([0, 1]), 8000)), [0, 0.5, 1, 1]);
  const input = new Float32Array([0.2, -0.2]);
  const output = resampleMono(input, 16000);
  output[0] = 1;
  assert.notEqual(input[0], 1);
  assert.equal(resampleMono(new Float32Array(), 48000).length, 0);
  assert.throws(() => resampleMono(input, 0), RangeError);
  assert.throws(() => resampleMono(input, 48000, NaN), RangeError);
});

test('the actual Python bridge accepts the browser demo WAV and rejects overlong audio', async () => {
  const bytes = Buffer.from(await makeDemoWav().arrayBuffer());
  const script = 'import sys; from server.app import validate_wav; print(validate_wav(sys.stdin.buffer.read()))';
  const duration = execFileSync('python3', ['-c', script], { input: bytes, encoding: 'utf8' });
  assert.equal(Number(duration.trim()), 0.75);
  const overlong = Buffer.from(await encodeWav(new Float32Array(16000 * 16)).arrayBuffer());
  const invalid = 'import sys; from server.app import validate_wav, RequestError\ntry:\n validate_wav(sys.stdin.buffer.read())\nexcept RequestError as e:\n print(e.code)';
  const result = execFileSync('python3', ['-c', invalid], { input: overlong, encoding: 'utf8' });
  assert.equal(result.trim(), 'audio_too_long');
});

test('mouth envelope follows real PCM amplitude and stays closed for silence', async () => {
  const silence = wavEnvelope(await encodeWav(new Float32Array(1600)).arrayBuffer());
  assert.ok(silence.levels.every(value => value === 0));
  const audible = wavEnvelope(await encodeWav(new Float32Array(1600).fill(0.2)).arrayBuffer());
  assert.ok(audible.levels.every(value => Number.isFinite(value) && value > 0 && value <= 1));
  assert.equal(audible.duration, 0.1);
  assert.ok(Math.abs(audible.bucketsPerSecond - 30) < 0.1);
});
