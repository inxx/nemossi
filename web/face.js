/** Original primitive face rendering. No image or third-party face assets. */
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

export class Face {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext("2d");
    this.state = "idle";
    this.brightness = 1;
    this.mouthLevel = 0;
    this.motionQuery = window.matchMedia("(prefers-reduced-motion: reduce)");
    this.nextBlink = performance.now() + 2300;
    this.blinkStart = -1000;
    this.nextGaze = 0;
    this.gaze = { x: 0, y: 0 };
    this.targetGaze = { x: 0, y: 0 };
    this.running = true;
    this.frame = requestAnimationFrame(time => this.tick(time));
  }

  setState(state) {
    if (!FACE_STATES[state]) return;
    this.state = state;
    this.canvas.setAttribute("aria-label", `네모씨: ${FACE_STATES[state].label}`);
  }

  setBrightness(value) {
    this.brightness = Math.max(0.55, Math.min(1, value));
  }

  setMouthLevel(value) {
    this.mouthLevel = Math.max(0, Math.min(1, Number.isFinite(value) ? value : 0));
  }

  rect(x, y, width, height, radius = 3, rotation = 0) {
    const ctx = this.ctx;
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(rotation);
    ctx.beginPath();
    ctx.roundRect(-width / 2, -height / 2, width, height, Math.min(radius, height / 2, width / 2));
    ctx.fill();
    ctx.restore();
  }

  tick(time) {
    if (!this.running) return;
    const reduced = this.motionQuery.matches;
    if (time > this.nextGaze && !reduced) {
      this.targetGaze = { x: Math.random() * 8 - 4, y: Math.random() * 4 - 2 };
      this.nextGaze = time + 3000 + Math.random() * 3500;
    }
    if (time > this.nextBlink && !reduced) {
      this.blinkStart = time;
      this.nextBlink = time + 2800 + Math.random() * 3500;
    }
    const blinkAge = time - this.blinkStart;
    const blink = !reduced && blinkAge >= 0 && blinkAge < 160 ? 1 - Math.sin(blinkAge / 160 * Math.PI) * 0.95 : 1;
    const gazeX = this.state === "thinking" ? 8 : this.state === "sleep" || reduced ? 0 : this.targetGaze.x;
    const gazeY = this.state === "thinking" ? -4 : this.state === "sleep" || reduced ? 0 : this.targetGaze.y;
    this.gaze.x += (gazeX - this.gaze.x) * 0.035;
    this.gaze.y += (gazeY - this.gaze.y) * 0.035;
    this.draw(time, blink, reduced);
    this.frame = requestAnimationFrame(next => this.tick(next));
  }

  draw(time, blink, reduced) {
    const ctx = this.ctx;
    const level = Math.round(255 * this.brightness);
    ctx.fillStyle = `rgb(${level},${level},${level})`;
    ctx.fillRect(0, 0, 240, 240);
    ctx.fillStyle = "#20211e";
    const bob = reduced || this.state === "sleep" ? 0 : Math.sin(time / 1800) * 0.7;
    const x = this.gaze.x;
    const y = this.gaze.y + bob;
    let leftHeight = 32, rightHeight = 32, eyeWidth = 17, tilt = 0;
    if (this.state === "listening") leftHeight = rightHeight = 38;
    if (this.state === "happy") { leftHeight = rightHeight = 8; eyeWidth = 27; tilt = -0.13; }
    if (this.state === "confused") { leftHeight = 31; rightHeight = 17; tilt = -0.13; }
    if (this.state === "error") { leftHeight = rightHeight = 7; eyeWidth = 24; tilt = 0.21; }
    if (this.state === "sleep") { leftHeight = rightHeight = 4; eyeWidth = 25; }
    this.rect(81 + x, 105 + y, eyeWidth, Math.max(2, leftHeight * blink), 4, tilt);
    this.rect(159 + x, 105 + y, eyeWidth, Math.max(2, rightHeight * blink), 4, -tilt);
    let mouthWidth = 27, mouthHeight = 6, mouthTilt = 0;
    if (this.state === "listening") { mouthWidth = 10; mouthHeight = 10; }
    if (this.state === "thinking") { mouthWidth = 20; mouthHeight = 5; mouthTilt = -0.12; }
    if (this.state === "speaking") { mouthWidth = 25; mouthHeight = reduced ? 12 : 5 + this.mouthLevel * 24; }
    if (this.state === "happy") { mouthWidth = 27; mouthHeight = 12; }
    if (this.state === "confused") { mouthWidth = 20; mouthHeight = 5; mouthTilt = 0.13; }
    if (this.state === "error") { mouthWidth = 19; mouthHeight = 4; }
    if (this.state === "sleep") { mouthWidth = 9; mouthHeight = 4; }
    this.rect(120 + x * 0.7, 151 + y, mouthWidth, mouthHeight, 3, mouthTilt);
  }

  destroy() {
    this.running = false;
    cancelAnimationFrame(this.frame);
  }
}
