import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import {
  STACKCHAN_DESIGN, FACE_STATES, Face, hash32, blinkEase, motionAt,
  mouthGeometry, faceGeometry, drawStackchanFace,
} from '../web/face.js';

const fixture = JSON.parse(await readFile(new URL('./fixtures/stackchan-simple-face.json', import.meta.url), 'utf8'));
const baseline = motionAt(0, true);

function recordingContext() {
  const calls = [];
  const ctx = { calls, fillStyle: '', stack: [] };
  for (const method of ['beginPath', 'rect', 'clip', 'arc', 'moveTo', 'lineTo', 'closePath', 'translate', 'scale']) {
    ctx[method] = (...args) => calls.push({ method, args });
  }
  ctx.save = () => { ctx.stack.push(ctx.fillStyle); calls.push({ method: 'save', args: [] }); };
  ctx.restore = () => { ctx.fillStyle = ctx.stack.pop(); calls.push({ method: 'restore', args: [] }); };
  ctx.fillRect = (...args) => calls.push({ method: 'fillRect', args, color: ctx.fillStyle });
  ctx.fill = () => calls.push({ method: 'fill', args: [], color: ctx.fillStyle });
  return ctx;
}

function foregroundMouth(ctx) {
  return ctx.calls.filter(call => call.method === 'fillRect' && call.color !== '#000000').at(-1);
}

test('JS design and every C design macro match the official coordinate fixture', async () => {
  assert.deepEqual(STACKCHAN_DESIGN, fixture);
  const header = await readFile(new URL('../firmware/model/face_design.h', import.meta.url), 'utf8');
  for (const [key, value] of Object.entries(fixture)) {
    if (typeof value !== 'number') continue;
    const macro = `STACKCHAN_${key.toUpperCase()}`;
    const match = header.match(new RegExp(`^#define ${macro} ([0-9]+)u?$`, 'm'));
    assert.ok(match, `missing ${macro}`);
    assert.equal(Number(match[1]), value, macro);
  }
  const source = await readFile(new URL('../web/face.js', import.meta.url), 'utf8');
  assert.match(source, /SPDX-License-Identifier: Apache-2.0/);
  assert.match(source, /Adapted from Stack-chan by meganetaaan/);
  assert.match(source, /Modified:/);
  assert.match(source, new RegExp(fixture.source_commit));
});

test('original native eyes project as circles with a stationary 24px clipped viewport', () => {
  const layout = faceGeometry('idle', 0, baseline);
  assert.equal(layout.emotion, 'NEUTRAL');
  assert.equal(layout.eyeOpenStep, 12);
  assert.deepEqual(layout.native.eyes.map(eye => eye.iris), [
    { x: 90, y: 93, radius: 8 }, { x: 230, y: 96, radius: 8 },
  ]);
  assert.deepEqual(layout.projected.eyes.map(eye => eye.iris), [
    { x: 67.5, y: 99.75, radius: 6 }, { x: 172.5, y: 102, radius: 6 },
  ]);
  assert.deepEqual(layout.native.eyes.map(eye => eye.viewport), [
    { x: 78, y: 81, width: 24, height: 24 }, { x: 218, y: 84, width: 24, height: 24 },
  ]);
  const blink = faceGeometry('idle', 0, { ...baseline, eyeOpen: 0.5, gaze: { x: 0.5, y: -0.25 } });
  assert.equal(blink.eyeOpenStep, 6);
  assert.deepEqual(blink.native.eyes[0].viewport, layout.native.eyes[0].viewport);
  assert.deepEqual(blink.native.eyes[0].iris, { x: 91, y: 92.5, radius: 8 });
  assert.deepEqual(blink.native.eyes[0].masks, [{ kind: 'rect', x: 78, y: 81, width: 24, height: 12 }]);
  const ctx = recordingContext();
  drawStackchanFace(ctx, { motion: { ...baseline, eyeOpen: 0.5 } });
  assert.deepEqual(ctx.calls.filter(call => call.method === 'arc').map(call => call.args.slice(0, 3)), [[90, 93, 8], [230, 96, 8]]);
  assert.equal(ctx.calls.filter(call => call.method === 'clip').length, 2);
  assert.deepEqual(ctx.calls.find(call => call.method === 'scale').args, [0.75, 0.75]);
  assert.deepEqual(ctx.calls.find(call => call.method === 'translate').args, [0, 30]);
});

test('native mouth Port rounds before projection at silence, half and full level', () => {
  const native = [
    { x: 115, y: 144, width: 90, height: 8 },
    { x: 125, y: 132, width: 70, height: 33 },
    { x: 135, y: 119, width: 50, height: 58 },
  ];
  const projected = [
    { x: 86.25, y: 138, width: 67.5, height: 6 },
    { x: 93.75, y: 129, width: 52.5, height: 24.75 },
    { x: 101.25, y: 119.25, width: 37.5, height: 43.5 },
  ];
  for (const [index, open] of [0, 0.5, 1].entries()) {
    assert.deepEqual(mouthGeometry(open), native[index]);
    const layout = faceGeometry('speaking', open, baseline);
    assert.deepEqual(layout.native.mouth, native[index]);
    assert.deepEqual(layout.projected.mouth, projected[index]);
  }
  assert.deepEqual(mouthGeometry(0.01), { x: 115, y: 144, width: 90, height: 9 });
  assert.deepEqual(mouthGeometry(0.5, -4), { x: 125, y: 128, width: 70, height: 33 });
  assert.deepEqual(mouthGeometry(NaN), native[0]);
  assert.deepEqual(mouthGeometry(-1), native[0]);
  assert.deepEqual(mouthGeometry(2), native[2]);
});

test('state mapping retains upstream eyelid masks and neutral listening/thinking geometry', () => {
  const idle = faceGeometry('idle', 0, baseline);
  for (const state of ['listening', 'thinking']) assert.deepEqual(faceGeometry(state, 1, baseline), idle);
  const doubtful = faceGeometry('confused', 1, baseline);
  assert.equal(doubtful.emotion, 'DOUBTFUL');
  assert.deepEqual(doubtful.native, idle.native);
  const happy = faceGeometry('happy', 1, baseline);
  assert.equal(happy.emotion, 'HAPPY');
  assert.deepEqual(happy.native.eyes[0].masks, [
    { kind: 'rect', x: 78, y: 81, width: 24, height: 0 },
    { kind: 'rect', x: 78, y: 95.4, width: 24, height: 9.600000000000001 },
  ]);
  const sad = faceGeometry('error', 0, baseline);
  assert.equal(sad.emotion, 'SAD');
  assert.deepEqual(sad.native.eyes[0].masks[0].points, [[78, 81], [78, 93], [102, 81], [102, 81]]);
  assert.deepEqual(sad.native.eyes[1].masks[0].points, [[218, 84], [218, 84], [242, 96], [242, 84]]);
  const sleep = faceGeometry('sleep', 0, baseline);
  assert.equal(sleep.emotion, 'SLEEPY');
  assert.equal(sleep.eyeOpenStep, 0);
  assert.equal(sleep.native.eyes[0].masks[0].height, 24);
  for (const state of Object.keys(FACE_STATES)) {
    if (state !== 'speaking') assert.deepEqual(faceGeometry(state, 1, baseline).native.mouth, idle.native.mouth);
  }
});

test('blink uses the official linear close / quadratic reopen and 12-step masks', () => {
  assert.equal(blinkEase(0), 1);
  assert.equal(blinkEase(0.125), 0.5);
  assert.equal(blinkEase(0.25), 0);
  assert.equal(blinkEase(0.5), 1 / 9);
  assert.equal(blinkEase(1), 1);
  assert.equal(blinkEase(-1), 1);
  assert.equal(blinkEase(2), 1);
  assert.equal(faceGeometry('idle', 0, { ...baseline, eyeOpen: 0.2 }).eyeOpenStep, 2);
  assert.equal(faceGeometry('idle', 0, { ...baseline, eyeOpen: 0.375 }).eyeOpenStep, 5);
  assert.equal(faceGeometry('idle', 0, { ...baseline, eyeOpen: NaN }).eyeOpenStep, 0);
});

test('bounded seeded motion is stable, quantized and spans all 32 slots', () => {
  assert.equal(hash32(0), 0);
  assert.equal(hash32(1), 1753845952);
  const start = motionAt(0);
  assert.equal(start.eyeOpen, 1);
  assert.deepEqual(start.gaze, { x: 0, y: 0 });
  assert.equal(start.breath, 0);
  assert.deepEqual(motionAt(32), start);
  const blinks = new Set(), gazes = new Set();
  let minOpen = 1;
  for (let time = 0; time < Math.max(start.blinkCycleMs, start.gazeCycleMs); time += 33) {
    const motion = motionAt(time);
    blinks.add(motion.blinkSlot); gazes.add(motion.gazeSlot);
    assert.ok(motion.eyeOpen >= 0.2 && motion.eyeOpen <= 1);
    assert.ok(motion.eyeOpenStep >= 2 && motion.eyeOpenStep <= 12);
    assert.ok(motion.blinkOpenMs >= 400 && motion.blinkOpenMs < 5000);
    assert.ok(motion.blinkTransitionMs >= 200 && motion.blinkTransitionMs < 400);
    assert.ok(Math.abs(motion.breath) <= 6);
    assert.ok(Number.isFinite(motion.gaze.x) && Number.isFinite(motion.gaze.y));
    minOpen = Math.min(minOpen, motion.eyeOpen);
  }
  assert.equal(blinks.size, 32); assert.equal(gazes.size, 32);
  assert.ok(minOpen < 0.21, 'the schedule actually closes the top eyelid');
  const blinkRepeat = motionAt(start.blinkCycleMs * 33);
  assert.equal(blinkRepeat.blinkSlot, 0); assert.equal(blinkRepeat.eyeOpen, 1);
  assert.deepEqual(motionAt(start.gazeCycleMs * 33).gaze, { x: 0, y: 0 });
  assert.deepEqual(motionAt(1e12), motionAt(1e12));
  assert.notDeepEqual(motionAt(3300, false, 1), motionAt(3300, false, 2));
  assert.equal(motionAt(1485).breath, 6);
  assert.equal(motionAt(3927).breath, -4, 'negative half rounds toward positive infinity, as JS Math.round');
});

test('brightness dims only foreground; reduced motion preserves volume-driven speaking including silence', () => {
  const ctx = recordingContext();
  drawStackchanFace(ctx, { brightness: 0.55 });
  assert.deepEqual(ctx.calls[0], { method: 'fillRect', args: [0, 0, 240, 240], color: '#000000' });
  assert.equal(foregroundMouth(ctx).color, 'rgb(140,140,140)');
  assert.ok(ctx.calls.filter(call => call.method === 'fillRect' && call.color === '#000000').length >= 3);
  const reduced = motionAt(5000, true);
  assert.equal(reduced.eyeOpen, 1); assert.equal(reduced.breath, 0);
  assert.deepEqual(reduced.gaze, { x: 0, y: 0 });
  for (const [level, expected] of [[0, [115, 144, 90, 8]], [0.5, [125, 132, 70, 33]], [1, [135, 119, 50, 58]]]) {
    const recording = recordingContext();
    drawStackchanFace(recording, { state: 'speaking', mouthLevel: level, motion: reduced });
    assert.deepEqual(foregroundMouth(recording).args, expected);
    assert.equal(foregroundMouth(recording).color, 'rgb(255,255,255)');
  }
});

test('Face keeps motion origin across states and cancels its animation lifecycle', () => {
  const old = { window: globalThis.window, request: globalThis.requestAnimationFrame, cancel: globalThis.cancelAnimationFrame };
  const pending = new Map();
  let nextId = 0;
  const query = { matches: false };
  globalThis.window = { matchMedia: () => query };
  globalThis.requestAnimationFrame = callback => { pending.set(++nextId, callback); return nextId; };
  globalThis.cancelAnimationFrame = id => pending.delete(id);
  const ctx = recordingContext();
  const attributes = {};
  const canvas = { getContext: () => ctx, setAttribute: (name, value) => { attributes[name] = value; } };
  try {
    const face = new Face(canvas);
    const origin = face.motionOrigin;
    face.setState('listening'); face.tick(origin + 3300);
    assert.equal(face.motionOrigin, origin);
    face.setState('speaking'); face.setMouthLevel(0.5);
    query.matches = true; face.tick(origin + 4000);
    assert.deepEqual(foregroundMouth(ctx).args, [125, 132, 70, 33]);
    face.setMouthLevel(0); face.tick(origin + 4016);
    assert.deepEqual(foregroundMouth(ctx).args, [115, 144, 90, 8]);
    assert.equal(face.motionOrigin, origin);
    assert.equal(attributes['aria-label'], '네모씨: 소리 재생 중');
    face.setState('unknown'); assert.equal(face.state, 'speaking');
    face.setBrightness(NaN); assert.equal(face.brightness, 1);
    const queued = pending.get(face.frame);
    const lastFrame = face.frame;
    const count = ctx.calls.length;
    face.destroy();
    assert.equal(pending.has(lastFrame), false);
    queued(origin + 5000);
    assert.equal(face.frame, lastFrame);
    assert.equal(ctx.calls.length, count);
  } finally {
    globalThis.window = old.window;
    globalThis.requestAnimationFrame = old.request;
    globalThis.cancelAnimationFrame = old.cancel;
  }
});
