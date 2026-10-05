/**
 * SPDX-License-Identifier: Apache-2.0
 * Adapted from Stack-chan by meganetaaan, https://github.com/stack-chan/stack-chan
 * Source commit: 2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d
 * Modified: Canvas projection, Nemossi state mapping and bounded seeded motion.
 * SimpleFace, Eye, Mouth, blink and breath retain the upstream shape equations.
 * Upstream random intervals are replaced by a repeating 32-slot seeded schedule.
 */
export const STACKCHAN_DESIGN = Object.freeze({
  source_commit: "2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d",
  source_width: 320, source_height: 240, target_width: 240, target_height: 240,
  scale_numerator: 3, scale_denominator: 4, offset_x: 0, offset_y: 30,
  left_eye_x: 90, left_eye_y: 93, right_eye_x: 230, right_eye_y: 96,
  eye_radius: 8, eyelid_width: 24, eyelid_height: 24, eye_open_steps: 12,
  mouth_x: 160, mouth_y: 148, mouth_min_width: 50, mouth_max_width: 90,
  mouth_min_height: 8, mouth_max_height: 58, background_rgb: 0, foreground_rgb: 16777215,
  blink_open_min_ms: 400, blink_open_max_ms: 5000,
  blink_transition_min_ms: 200, blink_transition_max_ms: 400, blink_min_open_milli: 200,
  breath_period_ms: 6000, breath_amplitude: 6, breath_steps: 8, motion_tick_ms: 33,
  gaze_interval_min_ms: 300, gaze_interval_max_ms: 2000, gaze_gain_milli: 200,
  animation_seed: 1313164623, animation_slots: 32,
});

export const FACE_STATES = Object.freeze({
  idle: { label: "기다리는 중", subtitle: "안녕하세요. 네모씨예요." },
  listening: { label: "듣는 중", subtitle: "네, 듣고 있어요." },
  thinking: { label: "생각하는 중", subtitle: "잠깐만 기다려 주세요." },
  speaking: { label: "소리 재생 중", subtitle: "데모 소리를 재생해요." },
  happy: { label: "기쁜 표정", subtitle: "반가워요. 또 이야기해요." },
  confused: { label: "갸우뚱", subtitle: "한 번 더 들려줄래요?" },
  error: { label: "잠시 멈춤", subtitle: "연결을 다시 확인해 주세요." },
  sleep: { label: "쉬는 중", subtitle: "잠깐 쉬고 있어요." },
});

/** Nemossi variations; the default retains the pinned upstream SimpleFace. */
export const FACE_EXPRESSIONS = Object.freeze(Object.fromEntries([
  { id: "default", label: "기본", emotion: "AUTO", left_open_cap: 12, right_open_cap: 12, iris_dx: 0, iris_dy: 0, mouth_rest_open_milli: 0, mouth_width_delta: 0, mouth_height_delta: 0, mouth_dx: 0, mouth_dy: 0 },
  { id: "joy", label: "기쁨", emotion: "HAPPY", left_open_cap: 12, right_open_cap: 12, iris_dx: 0, iris_dy: 0, mouth_rest_open_milli: 0, mouth_width_delta: 0, mouth_height_delta: 2, mouth_dx: 0, mouth_dy: 0 },
  { id: "curious", label: "궁금함", emotion: "NEUTRAL", left_open_cap: 12, right_open_cap: 9, iris_dx: 2, iris_dy: -1, mouth_rest_open_milli: 0, mouth_width_delta: -12, mouth_height_delta: 2, mouth_dx: 0, mouth_dy: 0 },
  { id: "pondering", label: "생각 중", emotion: "NEUTRAL", left_open_cap: 8, right_open_cap: 10, iris_dx: -2, iris_dy: -2, mouth_rest_open_milli: 0, mouth_width_delta: -18, mouth_height_delta: 0, mouth_dx: -4, mouth_dy: 0 },
  { id: "surprised", label: "놀람", emotion: "NEUTRAL", left_open_cap: 12, right_open_cap: 12, iris_dx: 0, iris_dy: -1, mouth_rest_open_milli: 280, mouth_width_delta: 0, mouth_height_delta: 0, mouth_dx: 0, mouth_dy: 0 },
  { id: "drowsy", label: "졸림", emotion: "SLEEPY", left_open_cap: 10, right_open_cap: 10, iris_dx: 0, iris_dy: 1, mouth_rest_open_milli: 0, mouth_width_delta: -16, mouth_height_delta: 0, mouth_dx: 0, mouth_dy: 1 },
  { id: "downcast", label: "시무룩함", emotion: "SAD", left_open_cap: 10, right_open_cap: 10, iris_dx: 0, iris_dy: 1, mouth_rest_open_milli: 0, mouth_width_delta: -12, mouth_height_delta: 0, mouth_dx: 0, mouth_dy: 3 },
].map(row => [row.id, Object.freeze(row)])));

const D = STACKCHAN_DESIGN;
const SCALE = D.scale_numerator / D.scale_denominator;
const clampUnit = value => Math.max(0, Math.min(1, Number.isFinite(value) ? value : 0));
const quantizeOpen = value => Math.round(clampUnit(value) * D.eye_open_steps);
const expressionRow = id => Object.hasOwn(FACE_EXPRESSIONS, id) ? FACE_EXPRESSIONS[id] : FACE_EXPRESSIONS.default;

export function hash32(value) {
  let x = value >>> 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d);
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b);
  return (x ^ (x >>> 16)) >>> 0;
}

function unitRandom(seed, slot, lane) {
  return hash32(seed ^ Math.imul(slot + 1, 0x9e3779b9) ^ Math.imul(lane + 1, 0x85ebca6b)) / 4294967296;
}

function normalRandom(seed, slot, lane) {
  const a = 1 - unitRandom(seed, slot, lane);
  const b = 1 - unitRandom(seed, slot, lane + 1);
  const angle = 2 * Math.PI * b;
  const wave = unitRandom(seed, slot, lane + 2) < 0.5 ? Math.sin(angle) : Math.cos(angle);
  return Math.sqrt(-2 * Math.log(a)) * wave * D.gaze_gain_milli / 1000;
}

function makeSchedule(seed) {
  return Array.from({ length: D.animation_slots }, (_, slot) => {
    const interval = (lane, min, max) => min + Math.floor(unitRandom(seed, slot, lane) * (max - min));
    return Object.freeze({
      open: interval(0, D.blink_open_min_ms, D.blink_open_max_ms),
      transition: interval(1, D.blink_transition_min_ms, D.blink_transition_max_ms),
      gazeInterval: interval(2, D.gaze_interval_min_ms, D.gaze_interval_max_ms),
      gaze: Object.freeze(slot === 0 ? { x: 0, y: 0 } : {
        x: normalRandom(seed, slot, 3), y: normalRandom(seed, slot, 6),
      }),
    });
  });
}

const SCHEDULE = Object.freeze(makeSchedule(D.animation_seed));

/** Upstream close for the first quarter, quadratic reopen for the rest. */
export function blinkEase(fraction) {
  const f = clampUnit(fraction);
  return f < 0.25 ? 1 - f * 4 : (f - 0.25) ** 2 * 16 / 9;
}

/** Sample at most 32 slots, independent of frame cadence and state changes. */
export function motionAt(elapsedMs, reducedMotion = false, seed = D.animation_seed) {
  const elapsed = Number.isFinite(elapsedMs) ? Math.max(0, elapsedMs) : 0;
  const timeMs = Math.floor(elapsed / D.motion_tick_ms) * D.motion_tick_ms;
  const schedule = seed === D.animation_seed ? SCHEDULE : makeSchedule(seed >>> 0);
  const blinkCycleMs = schedule.reduce((sum, slot) => sum + slot.open + slot.transition, 0);
  const gazeCycleMs = schedule.reduce((sum, slot) => sum + slot.gazeInterval, 0);
  let blinkTime = timeMs % blinkCycleMs, blinkSlot = 0;
  let gazeTime = timeMs % gazeCycleMs, gazeSlot = 0;
  while (blinkSlot < schedule.length - 1 && blinkTime >= schedule[blinkSlot].open + schedule[blinkSlot].transition) {
    blinkTime -= schedule[blinkSlot].open + schedule[blinkSlot].transition;
    blinkSlot++;
  }
  while (gazeSlot < schedule.length - 1 && gazeTime >= schedule[gazeSlot].gazeInterval) {
    gazeTime -= schedule[gazeSlot].gazeInterval;
    gazeSlot++;
  }
  const slot = schedule[blinkSlot];
  const minOpen = D.blink_min_open_milli / 1000;
  const eyeOpen = reducedMotion || blinkTime < slot.open ? 1 :
    minOpen + blinkEase((blinkTime - slot.open) / slot.transition) * (1 - minOpen);
  const breath = reducedMotion ? 0 : Math.round(Math.ceil(
    Math.sin(2 * Math.PI * (timeMs % D.breath_period_ms) / D.breath_period_ms) * D.breath_steps,
  ) / D.breath_steps * D.breath_amplitude);
  return {
    timeMs, eyeOpen, eyeOpenStep: quantizeOpen(eyeOpen), breath: breath || 0,
    gaze: reducedMotion ? { x: 0, y: 0 } : { ...schedule[gazeSlot].gaze },
    blinkSlot, gazeSlot, blinkOpenMs: slot.open, blinkTransitionMs: slot.transition,
    blinkCycleMs, gazeCycleMs,
  };
}

function projectRect(rect) {
  return {
    x: D.offset_x + rect.x * SCALE, y: D.offset_y + rect.y * SCALE,
    width: rect.width * SCALE, height: rect.height * SCALE,
  };
}

/** Native Port rounds local coordinates and dimensions before projection. */
export function mouthGeometry(open, breath = 0, expression = "default") {
  const row = expressionRow(expression);
  const rest = row.mouth_rest_open_milli / 1000;
  const value = rest + (1 - rest) * clampUnit(open);
  const width = Math.max(D.mouth_min_width, Math.min(D.mouth_max_width,
    D.mouth_min_width + (D.mouth_max_width - D.mouth_min_width) * (1 - value) + row.mouth_width_delta * (1 - value)));
  const height = Math.max(D.mouth_min_height, Math.min(D.mouth_max_height,
    D.mouth_min_height + (D.mouth_max_height - D.mouth_min_height) * value + row.mouth_height_delta * (1 - value)));
  return {
    x: D.mouth_x - D.mouth_max_width / 2 + Math.round((D.mouth_max_width - width) / 2) + row.mouth_dx,
    y: D.mouth_y - D.mouth_max_height / 2 + Math.round((D.mouth_max_height - height) / 2) + breath + row.mouth_dy,
    width: Math.round(width), height: Math.round(height),
  };
}

function eyeGeometry(cx, cy, side, emotion, step, gaze, breath, row) {
  const width = D.eyelid_width, height = D.eyelid_height;
  const x = cx - width / 2, y = cy - height / 2 + breath;
  const closedHeight = height * (1 - step / D.eye_open_steps);
  let masks;
  if (emotion === "SAD") {
    let leftHeight = (height + closedHeight) / 2, rightHeight = closedHeight;
    if (side === "left") [leftHeight, rightHeight] = [rightHeight, leftHeight];
    [leftHeight, rightHeight] = [rightHeight, leftHeight];
    masks = [{ kind: "polygon", points: [[x, y], [x, y + leftHeight], [x + width, y + rightHeight], [x + width, y]] }];
  } else if (emotion === "SLEEPY") {
    masks = [{ kind: "rect", x, y, width, height: height * 0.5 + closedHeight * 0.5 }];
  } else if (emotion === "HAPPY") {
    masks = [
      { kind: "rect", x, y, width, height: closedHeight * 0.6 },
      { kind: "rect", x, y: y + height * 0.6, width, height: height * 0.4 },
    ];
  } else {
    masks = [{ kind: "rect", x, y, width, height: closedHeight }];
  }
  return {
    side, viewport: { x, y, width, height },
    iris: { x: cx + gaze.x * 2 + row.iris_dx, y: cy + breath + gaze.y * 2 + row.iris_dy, radius: D.eye_radius }, masks,
  };
}

export function faceGeometry(state = "idle", mouthLevel = 0, motion = motionAt(0, true), expression = "default") {
  const row = expressionRow(expression);
  const stateEmotion = ({ happy: "HAPPY", error: "SAD", confused: "DOUBTFUL", sleep: "SLEEPY" })[state] || "NEUTRAL";
  const emotion = row.emotion === "AUTO" ? stateEmotion : row.emotion;
  const eyeOpenStep = state === "sleep" ? 0 : quantizeOpen(motion.eyeOpen);
  const native = {
    eyes: [
      eyeGeometry(D.left_eye_x, D.left_eye_y, "left", emotion, Math.round(eyeOpenStep * row.left_open_cap / D.eye_open_steps), motion.gaze, motion.breath, row),
      eyeGeometry(D.right_eye_x, D.right_eye_y, "right", emotion, Math.round(eyeOpenStep * row.right_open_cap / D.eye_open_steps), motion.gaze, motion.breath, row),
    ],
    mouth: mouthGeometry(state === "speaking" ? mouthLevel : 0, motion.breath, expression),
  };
  const projected = {
    eyes: native.eyes.map(eye => ({
      ...eye, viewport: projectRect(eye.viewport),
      iris: { x: D.offset_x + eye.iris.x * SCALE, y: D.offset_y + eye.iris.y * SCALE, radius: eye.iris.radius * SCALE },
      masks: eye.masks.map(mask => mask.kind === "rect" ? { kind: "rect", ...projectRect(mask) } : {
        kind: "polygon", points: mask.points.map(([x, y]) => [D.offset_x + x * SCALE, D.offset_y + y * SCALE]),
      }),
    })),
    mouth: projectRect(native.mouth),
  };
  return { emotion, eyeOpenStep, native, projected };
}

/** Render native geometry through the uniform 0.75 projection into 240x240. */
export function drawStackchanFace(ctx, { state = "idle", mouthLevel = 0, brightness = 1, motion = motionAt(0, true), expression = "default" } = {}) {
  const geometry = faceGeometry(state, mouthLevel, motion, expression);
  const level = Math.round(255 * Math.max(0.55, Math.min(1, Number.isFinite(brightness) ? brightness : 1)));
  const foreground = `rgb(${level},${level},${level})`;
  ctx.fillStyle = "#000000";
  ctx.fillRect(0, 0, D.target_width, D.target_height);
  ctx.save();
  ctx.translate(D.offset_x, D.offset_y);
  ctx.scale(SCALE, SCALE);
  for (const eye of geometry.native.eyes) {
    ctx.save();
    ctx.beginPath();
    const viewport = eye.viewport;
    ctx.rect(viewport.x, viewport.y, viewport.width, viewport.height);
    ctx.clip();
    ctx.fillStyle = foreground;
    ctx.beginPath();
    ctx.arc(eye.iris.x, eye.iris.y, eye.iris.radius, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = "#000000";
    for (const mask of eye.masks) {
      if (mask.kind === "rect") ctx.fillRect(mask.x, mask.y, mask.width, mask.height);
      else {
        ctx.beginPath();
        ctx.moveTo(...mask.points[0]);
        for (const point of mask.points.slice(1)) ctx.lineTo(...point);
        ctx.closePath();
        ctx.fill();
      }
    }
    ctx.restore();
  }
  ctx.fillStyle = foreground;
  const mouth = geometry.native.mouth;
  ctx.fillRect(mouth.x, mouth.y, mouth.width, mouth.height);
  ctx.restore();
  return geometry;
}

export class Face {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext("2d");
    this.state = "idle";
    this.expression = "default";
    this.brightness = 1;
    this.mouthLevel = 0;
    this.motionQuery = window.matchMedia("(prefers-reduced-motion: reduce)");
    this.motionOrigin = performance.now();
    this.running = true;
    this.dirty = true;
    this.lastMotionTime = -1;
    this.lastReduced = undefined;
    this.draw(this.motionOrigin);
    this.frame = requestAnimationFrame(time => this.tick(time));
  }

  setState(state) {
    if (!FACE_STATES[state]) return;
    this.state = state;
    this.dirty = true;
    this.updateLabel();
  }

  setExpression(expression) {
    if (!Object.hasOwn(FACE_EXPRESSIONS, expression)) return false;
    this.expression = expression;
    this.dirty = true;
    this.updateLabel();
    return true;
  }

  updateLabel() {
    const expression = this.expression === "default" ? "" : ` · ${FACE_EXPRESSIONS[this.expression].label}`;
    this.canvas.setAttribute("aria-label", `네모씨: ${FACE_STATES[this.state].label}${expression}`);
  }

  setBrightness(value) {
    this.brightness = Math.max(0.55, Math.min(1, Number.isFinite(value) ? value : 1));
    this.dirty = true;
  }

  setMouthLevel(value) {
    const next = clampUnit(value);
    if (next !== this.mouthLevel) this.dirty = true;
    this.mouthLevel = next;
  }

  tick(time) {
    if (!this.running) return;
    const reduced = this.motionQuery.matches;
    const motionTime = Math.floor(Math.max(0, time - this.motionOrigin) / D.motion_tick_ms) * D.motion_tick_ms;
    if (this.dirty || (!reduced && motionTime !== this.lastMotionTime) || reduced !== this.lastReduced) this.draw(time);
    this.frame = requestAnimationFrame(next => this.tick(next));
  }

  draw(time) {
    const reduced = this.motionQuery.matches;
    const motion = motionAt(time - this.motionOrigin, reduced);
    drawStackchanFace(this.ctx, { state: this.state, mouthLevel: this.mouthLevel, brightness: this.brightness, motion, expression: this.expression });
    this.lastMotionTime = motion.timeMs;
    this.lastReduced = reduced;
    this.dirty = false;
  }

  destroy() {
    this.running = false;
    cancelAnimationFrame(this.frame);
  }
}
