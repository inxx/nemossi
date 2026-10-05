import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { FACE_EXPRESSIONS, FACE_STATES, Face, motionAt, faceGeometry, mouthGeometry, drawStackchanFace } from '../web/face.js';

const fixture = JSON.parse(await readFile(new URL('./fixtures/nemossi-expressions.json', import.meta.url), 'utf8'));
const baseline = motionAt(0, true);
const fields = ['left_open_cap', 'right_open_cap', 'iris_dx', 'iris_dy', 'mouth_rest_open_milli', 'mouth_width_delta', 'mouth_height_delta', 'mouth_dx', 'mouth_dy'];

function context() {
  const calls = [];
  const ctx = { calls, fillStyle: '', styles: [] };
  for (const method of ['beginPath', 'rect', 'clip', 'arc', 'moveTo', 'lineTo', 'closePath', 'translate', 'scale']) ctx[method] = (...args) => calls.push({ method, args });
  ctx.save = () => ctx.styles.push(ctx.fillStyle);
  ctx.restore = () => { ctx.fillStyle = ctx.styles.pop(); };
  ctx.fill = () => calls.push({ method: 'fill', color: ctx.fillStyle });
  ctx.fillRect = (...args) => calls.push({ method: 'fillRect', args, color: ctx.fillStyle });
  return ctx;
}

test('seven JS expression rows, C table and Korean selector exactly match the shared fixture', async () => {
  assert.deepEqual(Object.values(FACE_EXPRESSIONS), fixture.expressions);
  assert.equal(fixture.expressions.length, 7);
  assert.ok(Object.isFrozen(FACE_EXPRESSIONS));
  const source = await readFile(new URL('../firmware/model/face.c', import.meta.url), 'utf8');
  const table = source.match(/FACE_EXPRESSION_DESIGNS\[FACE_EXPRESSION_COUNT\] = \{([\s\S]*?)\n\};/)[1];
  const cRows = [...table.matchAll(/\{"([a-z]+)",\s*FACE_EXPRESSION_EMOTION_([A-Z]+),([\s\d,-]+)\}/g)];
  assert.equal(cRows.length, 7);
  for (const [index, row] of fixture.expressions.entries()) {
    assert.equal(cRows[index][1], row.id);
    assert.equal(cRows[index][2], row.emotion);
    assert.deepEqual(cRows[index][3].split(',').map(value => Number(value.trim())), fields.map(field => row[field]));
    assert.ok(Object.isFrozen(FACE_EXPRESSIONS[row.id]));
  }
  const html = await readFile(new URL('../web/index.html', import.meta.url), 'utf8');
  const selector = html.match(/<select id="expressionSelect">([\s\S]*?)<\/select>/)[1];
  const options = [...selector.matchAll(/<option value="([a-z]+)">([^<]+)<\/option>/g)].map(match => ({ id: match[1], label: match[2] }));
  assert.deepEqual(options, fixture.expressions.map(({id, label}) => ({id, label})));
  assert.match(html, /label for="stateSelect">동작 확인/);
});

test('default stays compatible across all lifecycle states and original mouth levels', () => {
  for (const state of Object.keys(FACE_STATES)) {
    for (const time of [0, 1485, 3927, 6006, 99990]) {
      const motion = motionAt(time);
      for (const level of [0, 0.01, 0.5, 1]) assert.deepEqual(faceGeometry(state, level, motion, 'default'), faceGeometry(state, level, motion));
    }
  }
  assert.deepEqual(mouthGeometry(0), { x: 115, y: 144, width: 90, height: 8 });
  assert.deepEqual(mouthGeometry(0.5), { x: 125, y: 132, width: 70, height: 33 });
  assert.deepEqual(mouthGeometry(1), { x: 135, y: 119, width: 50, height: 58 });
  assert.deepEqual(mouthGeometry(0.6249999999999998), { x: 127, y: 128, width: 65, height: 39 });
  assert.deepEqual(mouthGeometry(0.4875000000000002), { x: 125, y: 132, width: 70, height: 32 });
  assert.equal(faceGeometry('error', 0, baseline).emotion, 'SAD');
  assert.equal(faceGeometry('happy', 0, baseline).emotion, 'HAPPY');
  assert.equal(faceGeometry('confused', 0, baseline).emotion, 'DOUBTFUL');
});

test('expressions mask fixed circles, preserve viewports and quantize base step before eye caps', () => {
  const curious = faceGeometry('idle', 0, baseline, 'curious');
  assert.deepEqual(curious.native.eyes[0].iris, { x: 92, y: 92, radius: 8 });
  assert.deepEqual(curious.native.eyes[1].iris, { x: 232, y: 95, radius: 8 });
  assert.equal(curious.native.eyes[1].masks[0].height, 6);
  const pondering = faceGeometry('idle', 0, baseline, 'pondering');
  assert.ok(Math.abs(pondering.native.eyes[0].masks[0].height - 8) < 1e-12);
  assert.deepEqual(pondering.native.eyes[0].iris, { x: 88, y: 91, radius: 8 });
  const partial = faceGeometry('idle', 0, { ...baseline, eyeOpen: 0.375 }, 'curious');
  // base round(0.375*12)=5; right round(5*9/12)=4, rather than direct round(0.375*9)=3.
  assert.ok(Math.abs(partial.native.eyes[1].masks[0].height - 16) < 1e-12);
  for (const row of fixture.expressions) {
    const layout = faceGeometry('idle', 0, baseline, row.id);
    for (const [index, eye] of layout.native.eyes.entries()) {
      assert.equal(eye.iris.radius, 8);
      assert.deepEqual(eye.viewport, faceGeometry().native.eyes[index].viewport);
    }
    const sleep = faceGeometry('sleep', 0, baseline, row.id);
    assert.equal(sleep.eyeOpenStep, 0);
  }
});

test('rest mouths are distinct and speaking reaches the original fully open port without decorations', () => {
  const expected = {
    default: { x: 115, y: 144, width: 90, height: 8 },
    joy: { x: 115, y: 143, width: 90, height: 10 },
    curious: { x: 121, y: 143, width: 78, height: 10 },
    pondering: { x: 120, y: 144, width: 72, height: 8 },
    surprised: { x: 121, y: 137, width: 79, height: 22 },
    drowsy: { x: 123, y: 145, width: 74, height: 8 },
    downcast: { x: 121, y: 147, width: 78, height: 8 },
  };
  for (const row of fixture.expressions) {
    assert.deepEqual(mouthGeometry(0, 0, row.id), expected[row.id]);
    assert.deepEqual(faceGeometry('idle', 1, baseline, row.id).native.mouth, expected[row.id]);
    const full = mouthGeometry(1, 0, row.id);
    assert.deepEqual(full, { x: 135 + row.mouth_dx, y: 119 + row.mouth_dy, width: 50, height: 58 });
    const silence = faceGeometry('speaking', 0, baseline, row.id);
    assert.deepEqual(silence.native.mouth, expected[row.id]);
    const half = faceGeometry('speaking', 0.5, baseline, row.id);
    assert.ok(half.native.mouth.height > expected[row.id].height);
    assert.ok(half.native.mouth.width < expected[row.id].width);
    assert.deepEqual(mouthGeometry(NaN, 0, row.id), expected[row.id]);
    assert.deepEqual(mouthGeometry(2, 0, row.id), full);
    assert.deepEqual(mouthGeometry(-1, 0, row.id), expected[row.id]);
  }
});

test('all expressions retain black background, white circle foreground and volume changes under reduced motion', () => {
  for (const row of fixture.expressions) {
    const ctx = context();
    const layout = drawStackchanFace(ctx, { expression: row.id, state: 'speaking', mouthLevel: 0.5, motion: motionAt(3927, true) });
    assert.deepEqual(ctx.calls[0], { method: 'fillRect', args: [0, 0, 240, 240], color: '#000000' });
    assert.equal(ctx.calls.filter(call => call.method === 'arc').length, 2);
    assert.ok(ctx.calls.filter(call => call.method === 'arc').every(call => call.args[2] === 8));
    assert.ok(ctx.calls.filter(call => call.color).every(call => ['#000000', 'rgb(255,255,255)'].includes(call.color)));
    assert.deepEqual(layout.projected.mouth.width, layout.native.mouth.width * 0.75);
    assert.deepEqual(layout.projected.mouth.y, 30 + layout.native.mouth.y * 0.75);
  }
});

test('expression selection during speaking preserves state, mouth, motion clock and animation resource', () => {
  const old = { window: globalThis.window, request: globalThis.requestAnimationFrame, cancel: globalThis.cancelAnimationFrame };
  const pending = new Map(); let nextId = 0;
  globalThis.window = { matchMedia: () => ({ matches: true }) };
  globalThis.requestAnimationFrame = callback => { pending.set(++nextId, callback); return nextId; };
  globalThis.cancelAnimationFrame = id => pending.delete(id);
  const attributes = {};
  const canvas = { getContext: () => context(), setAttribute: (key, value) => { attributes[key] = value; } };
  try {
    const face = new Face(canvas);
    face.setState('speaking'); face.setMouthLevel(0.5);
    const unchanged = { origin: face.motionOrigin, frame: face.frame, mouth: face.mouthLevel, state: face.state };
    for (const expression of Object.keys(FACE_EXPRESSIONS)) {
      assert.equal(face.setExpression(expression), true);
      assert.equal(face.expression, expression);
      assert.deepEqual({ origin: face.motionOrigin, frame: face.frame, mouth: face.mouthLevel, state: face.state }, unchanged);
      assert.match(attributes['aria-label'], /소리 재생 중/);
    }
    assert.equal(face.setExpression('__proto__'), false);
    assert.equal(face.setExpression('unknown'), false);
    assert.equal(face.expression, 'downcast');
    face.destroy(); assert.equal(pending.has(unchanged.frame), false);
  } finally {
    globalThis.window = old.window; globalThis.requestAnimationFrame = old.request; globalThis.cancelAnimationFrame = old.cancel;
  }
  for (const invalid of ['unknown', '__proto__', 'constructor', null]) assert.deepEqual(faceGeometry('idle', 0, baseline, invalid), faceGeometry());
});
