import { Face, FACE_STATES } from "./face.js";
import { makeDemoWav, wavEnvelope, microphoneSupported, captureMicrophone, MAX_SECONDS } from "./audio.js";

const $ = id => document.getElementById(id);
const face = new Face($("faceCanvas"));
const settingsDialog = $("settingsDialog");
const dotDialog = $("dotDialog");
let operation = null;
let generation = 0;
let permissionGeneration = 0;
let microphoneEnabled = false;
let manualState = "auto";
let happyTimer;
let retryAudioUrl = null;
let healthController;
let keyboardHeld = false;
let pointerHeld = false;

function stored(key, fallback) {
  try { const value = Number(localStorage.getItem(key)); return localStorage.getItem(key) === null || !Number.isFinite(value) ? fallback : value; } catch { return fallback; }
}
function save(key, value) { try { localStorage.setItem(key, String(value)); } catch { /* private browsing may prevent storage */ } }
let volume = Math.max(0, Math.min(1, stored("nemossi.volume", 0.35)));
let brightness = Math.max(0.55, Math.min(1, stored("nemossi.brightness", 1)));
let sessionId;
try {
  sessionId = sessionStorage.getItem("nemossi.session");
  if (!sessionId || !/^[a-zA-Z0-9_-]{1,64}$/.test(sessionId)) {
    sessionId = crypto.randomUUID?.() || `session-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;
    sessionStorage.setItem("nemossi.session", sessionId);
  }
} catch { sessionId = `session-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`; }

function setPhase(state, subtitle) {
  face.setState(state);
  $("stateLabel").textContent = FACE_STATES[state].label;
  $("faceSubtitle").textContent = subtitle || FACE_STATES[state].subtitle;
  $("talkButton").classList.toggle("recording", state === "listening");
}
function resetManualState() { manualState = "auto"; $("stateSelect").value = "auto"; }
function updateControls() {
  const listening = operation?.phase === "listening";
  $("talkButton").disabled = Boolean(operation && !listening);
  $("cancelButton").disabled = !operation;
  $("sendButton").disabled = Boolean(operation);
  $("messageInput").disabled = Boolean(operation);
  $("microphoneToggle").disabled = Boolean(operation) || !microphoneSupported();
  $("talkLabel").textContent = listening ? "놓으면 보내요" : microphoneEnabled ? "누른 채 말하기" : "대화하기";
  $("talkHint").textContent = microphoneEnabled ? "버튼 또는 Space 키를 누른 채 말해요. 최대 15초." : "마이크 없이 데모를 시작해요. Space 키로도 가능해요.";
}
function showNotice(message, { error = false, audio = false, connection = false } = {}) {
  $("notice").hidden = false;
  $("notice").classList.toggle("error", error);
  $("noticeText").textContent = message;
  $("retryAudioButton").hidden = !audio;
  $("retryConnectionButton").hidden = !connection;
}
function clearNotice() {
  $("notice").hidden = true;
  $("retryAudioButton").hidden = true;
  $("retryConnectionButton").hidden = true;
  retryAudioUrl = null;
}
function addMessage(speaker, text) {
  $("welcomeMessage")?.remove();
  const message = document.createElement("div");
  message.className = `message ${speaker}`;
  const label = document.createElement("span"); label.className = "speaker";
  label.textContent = speaker === "user" ? "나" : "네모씨 · mock";
  const body = document.createElement("p"); body.textContent = text;
  message.append(label, body); $("conversationLog").append(message);
  // Bound DOM growth on long preview sessions, without retaining recordings.
  while ($("conversationLog").children.length > 40) $("conversationLog").firstElementChild.remove();
  $("conversationLog").scrollTop = $("conversationLog").scrollHeight;
}
function current(op) { return operation === op && op.id === generation && !op.controller.signal.aborted; }
function beginOperation(phase) {
  if (operation) return null;
  clearTimeout(happyTimer); resetManualState(); clearNotice();
  const op = { id: ++generation, controller: new AbortController(), phase, timers: new Set(), capture: null, audio: null, releasing: false };
  operation = op; setPhase(phase); updateControls(); return op;
}
function delay(op, milliseconds) {
  return new Promise(resolve => {
    const finish = () => { clearTimeout(timer); op.timers.delete(timer); op.controller.signal.removeEventListener("abort", finish); resolve(); };
    const timer = setTimeout(finish, milliseconds); op.timers.add(timer);
    op.controller.signal.addEventListener("abort", finish, { once: true });
  });
}
function cleanOperation(op) {
  op.capture?.cancel(); op.capture = null;
  for (const timer of op.timers) clearTimeout(timer);
  op.timers.clear();
  if (op.audioFrame) cancelAnimationFrame(op.audioFrame);
  face.setMouthLevel(0);
  if (op.audio) { op.audio.pause(); op.audio.removeAttribute("src"); op.audio.load(); op.audio = null; }
}
function finishOperation(op, state = "happy", subtitle) {
  if (!current(op)) return;
  op.controller.abort(); cleanOperation(op); operation = null; pointerHeld = keyboardHeld = false;
  setPhase(state, subtitle); updateControls();
  if (state === "happy") happyTimer = setTimeout(() => { if (!operation && manualState === "auto") setPhase("idle"); }, 1800);
}
function cancelOperation({ silent = false } = {}) {
  clearTimeout(happyTimer); pointerHeld = keyboardHeld = false;
  if (operation) { const op = operation; operation = null; generation++; op.controller.abort(); cleanOperation(op); }
  if (!silent) clearNotice();
  setPhase("idle", silent ? undefined : "취소했어요. 다시 시작할 수 있어요.");
  updateControls();
}
function operationError(op, error) {
  if (!current(op)) return;
  const message = error.name === "NotAllowedError" ? "마이크 권한을 허용하지 않았어요. 설정에서 다시 선택하거나 데모로 진행해 주세요." : error.name === "NotFoundError" ? "사용할 수 있는 마이크가 없어요. 마이크 없이 데모를 사용할 수 있어요." : error.message || "요청을 완료하지 못했어요. 다시 시도해 주세요.";
  showNotice(message, { error: true, connection: error.connection === true });
  if (error.name === "NotAllowedError" || error.name === "NotFoundError") { microphoneEnabled = false; $("microphoneToggle").checked = false; }
  finishOperation(op, "error");
}

async function checkHealth() {
  healthController?.abort(); const controller = new AbortController(); healthController = controller;
  const timeout = setTimeout(() => controller.abort(), 4000);
  $("connectionStatus").className = "server-status"; $("connectionLabel").textContent = "서버 확인 중";
  try {
    const response = await fetch("/api/health", { signal: controller.signal, cache: "no-store" });
    if (!response.ok) throw new Error("Server unavailable");
    const health = await response.json();
    if (healthController !== controller) return;
    if (health.mode !== "mock") throw new Error("Unexpected server mode");
    $("connectionStatus").className = "server-status ready"; $("connectionLabel").textContent = "데모 서버 준비";
    // This preview never claims dot connectivity, even if a different server reports it.
    if (!operation) clearNotice();
  } catch {
    if (healthController !== controller) return;
    $("connectionStatus").className = "server-status offline"; $("connectionLabel").textContent = "서버 연결 확인 필요";
    if (!operation) showNotice("로컬 서버에 연결할 수 없어요. 실행 상태를 확인해 주세요.", { error: true, connection: true });
  } finally { clearTimeout(timeout); }
}

async function requestTurn(op, body, audioInput = false) {
  if (!current(op)) return;
  op.phase = "thinking"; setPhase("thinking"); updateControls();
  const timeout = setTimeout(() => {
    if (!current(op)) return;
    showNotice("서버 응답을 기다리는 시간이 길어졌어요. 다시 시도해 주세요.", { error: true, connection: true });
    op.controller.abort(); cleanOperation(op); operation = null; generation++;
    setPhase("error"); updateControls();
  }, 20000); op.timers.add(timeout);
  try {
    const response = await fetch("/api/turn", {
      method: "POST", signal: op.controller.signal,
      headers: audioInput ? { "Content-Type": "audio/wav", "X-Session-ID": sessionId } : { "Content-Type": "application/json" },
      body: audioInput ? body : JSON.stringify({ text: body, session_id: sessionId }),
    });
    if (!current(op)) return;
    if (!response.ok) {
      let detail;
      try { detail = await response.json(); } catch { /* non-JSON error response */ }
      const error = new Error(detail?.error?.message || (typeof detail?.error === "string" ? detail.error : `서버가 요청을 처리하지 못했어요 (${response.status}).`));
      error.connection = response.status >= 500; throw error;
    }
    const result = await response.json();
    clearTimeout(timeout); op.timers.delete(timeout);
    if (!current(op)) return;
    if (result.mode !== "mock" || typeof result.text !== "string") throw new Error("데모 서버의 응답 형식을 확인할 수 없어요.");
    if (audioInput) addMessage("user", typeof result.transcript === "string" ? result.transcript : "오디오 데모 입력");
    addMessage("nemossi", result.text);
    // Brief pause makes the visible state transition legible, and remains cancellable.
    await delay(op, 350);
    if (!current(op)) return;
    if (result.audio?.url) {
      const url = new URL(result.audio.url, window.location.href);
      if (url.origin !== window.location.origin || !/^\/api\/audio\/[a-zA-Z0-9_-]+\.wav$/.test(url.pathname)) throw new Error("데모 소리 주소를 확인할 수 없어요.");
      await playAudio(op, url.href);
    } else { finishOperation(op); }
  } catch (error) {
    if (error.name === "AbortError" || !current(op)) return;
    if (error instanceof TypeError) { error.message = "로컬 서버에 연결할 수 없어요. 실행 상태를 확인해 주세요."; error.connection = true; }
    operationError(op, error);
  } finally { clearTimeout(timeout); op.timers.delete(timeout); }
}

async function playAudio(op, url) {
  if (!current(op)) return;
  op.phase = "speaking"; setPhase("speaking"); updateControls();
  const audio = new Audio(url); op.audio = audio; audio.volume = volume;
  let envelope;
  // Never route browser playback through a new AudioContext. PCM levels only animate the mouth.
  fetch(url, { signal: op.controller.signal }).then(async response => {
    if (!response.ok) return;
    const buffer = await response.arrayBuffer();
    if (current(op) && buffer.byteLength <= 1024 * 1024) envelope = wavEnvelope(buffer);
  }).catch(() => {});
  const animateMouth = () => {
    if (!current(op)) return;
    const level = envelope?.levels[Math.floor(audio.currentTime * envelope.bucketsPerSecond)] || 0;
    face.setMouthLevel(audio.paused ? 0 : level);
    op.audioFrame = requestAnimationFrame(animateMouth);
  };
  animateMouth();
  const done = () => { if (current(op)) finishOperation(op); };
  const failed = () => {
    if (!current(op)) return;
    retryAudioUrl = url;
    showNotice("데모 소리를 재생하지 못했어요. 버튼을 눌러 다시 들어볼 수 있어요.", { audio: true });
    finishOperation(op, "happy", "화면과 응답을 확인했어요.");
  };
  audio.addEventListener("ended", done, { once: true });
  audio.addEventListener("error", failed, { once: true });
  const timeout = setTimeout(() => { if (current(op)) failed(); }, 20000); op.timers.add(timeout);
  try {
    await audio.play();
    if (!current(op)) { audio.pause(); return; }
  } catch {
    if (!current(op)) return;
    retryAudioUrl = url;
    showNotice("브라우저가 데모 소리 재생을 멈췄어요. 버튼을 눌러 재생해 주세요.", { audio: true });
    finishOperation(op, "happy", "응답을 확인했어요. 소리도 들어보세요.");
  }
}

async function beginListening() {
  const op = beginOperation("listening"); if (!op) return;
  op.startedAt = performance.now(); op.isMicrophone = microphoneEnabled;
  if (!op.isMicrophone) return;
  setPhase("listening", "마이크를 준비하고 있어요.");
  try {
    op.capture = await captureMicrophone(op.controller.signal);
    if (!current(op)) { op.capture.cancel(); return; }
    setPhase("listening");
    const maximum = setTimeout(() => endListening(), MAX_SECONDS * 1000); op.timers.add(maximum);
    if (op.releaseRequested) endListening();
  } catch (error) { if (error.name !== "AbortError") operationError(op, error); }
}
async function endListening() {
  const op = operation;
  if (!op || op.phase !== "listening" || op.releasing) return;
  if (op.isMicrophone && !op.capture) { op.releaseRequested = true; return; }
  op.releasing = true;
  for (const timer of op.timers) clearTimeout(timer); op.timers.clear();
  try {
    const body = op.isMicrophone ? op.capture.finish() : makeDemoWav();
    op.capture = null;
    if (!op.isMicrophone) await delay(op, Math.max(0, 420 - (performance.now() - op.startedAt)));
    if (current(op)) await requestTurn(op, body, true);
  } catch (error) { operationError(op, error); }
}

$("talkButton").addEventListener("pointerdown", event => {
  if (event.button !== 0 || operation) return;
  event.preventDefault(); pointerHeld = true;
  $("talkButton").setPointerCapture(event.pointerId); beginListening();
});
$("talkButton").addEventListener("pointerup", event => {
  if (!pointerHeld) return;
  pointerHeld = false; event.preventDefault(); endListening();
});
$("talkButton").addEventListener("pointercancel", () => { if (pointerHeld) cancelOperation(); });
$("talkButton").addEventListener("lostpointercapture", () => { if (pointerHeld) cancelOperation(); });
$("talkButton").addEventListener("click", async event => {
  // Pointer gestures are handled above. Enter / assistive activation uses one demo turn.
  if (event.detail !== 0 || operation) return;
  if (microphoneEnabled) { showNotice("마이크 입력은 버튼 또는 Space 키를 누른 채 말한 뒤 놓아 주세요."); return; }
  await beginListening(); if (operation) { const op = operation; await delay(op, 420); if (current(op)) endListening(); }
});
$("cancelButton").addEventListener("click", () => cancelOperation());
$("textForm").addEventListener("submit", event => {
  event.preventDefault(); const text = $("messageInput").value.trim();
  if (!text || operation) return;
  const op = beginOperation("thinking"); if (!op) return;
  $("messageInput").value = ""; addMessage("user", text); requestTurn(op, text);
});
const isTextTarget = target => target instanceof Element && (target.closest("input, textarea, select, [contenteditable='true']") || target.closest("dialog"));
document.addEventListener("keydown", event => {
  if (event.code !== "Space" || event.repeat || isTextTarget(event.target) || settingsDialog.open || dotDialog.open) return;
  // Respect native Space activation for other controls.
  if (event.target.closest?.("button") && event.target !== $("talkButton")) return;
  event.preventDefault(); if (operation || keyboardHeld) return;
  keyboardHeld = true; beginListening();
});
document.addEventListener("keyup", event => {
  if (event.code !== "Space" || !keyboardHeld) return;
  event.preventDefault(); keyboardHeld = false; endListening();
});
window.addEventListener("blur", () => { if (operation?.phase === "listening") cancelOperation(); });
document.addEventListener("visibilitychange", () => { if (document.hidden && operation) cancelOperation({ silent: true }); });

$("stateSelect").addEventListener("change", event => {
  cancelOperation({ silent: true }); manualState = event.target.value;
  setPhase(manualState === "auto" ? "idle" : manualState);
  if (manualState === "speaking") face.setMouthLevel(0.45);
});
$("settingsButton").addEventListener("click", () => settingsDialog.showModal());
for (const id of ["closeSettingsButton", "doneSettingsButton"]) $(id).addEventListener("click", () => settingsDialog.close());
$("dotInfoButton").addEventListener("click", () => dotDialog.showModal());
for (const id of ["closeDotButton", "doneDotButton"]) $(id).addEventListener("click", () => dotDialog.close());
for (const dialog of [settingsDialog, dotDialog]) dialog.addEventListener("click", event => { if (event.target === dialog) { const rect = dialog.getBoundingClientRect(); if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) dialog.close(); } });
function updateSettings() {
  $("volumeInput").value = Math.round(volume * 100); $("volumeOutput").textContent = `${Math.round(volume * 100)}%`;
  $("brightnessInput").value = Math.round(brightness * 100); $("brightnessOutput").textContent = `${Math.round(brightness * 100)}%`;
  face.setBrightness(brightness);
}
$("volumeInput").addEventListener("input", event => { volume = Number(event.target.value) / 100; save("nemossi.volume", volume); if (operation?.audio) operation.audio.volume = volume; updateSettings(); });
$("brightnessInput").addEventListener("input", event => { brightness = Number(event.target.value) / 100; save("nemossi.brightness", brightness); updateSettings(); });
$("microphoneToggle").addEventListener("change", async event => {
  const enabled = event.target.checked;
  const attempt = ++permissionGeneration;
  microphoneEnabled = false; updateControls();
  if (!enabled) { $("microphoneHint").textContent = "선택하면 브라우저에 권한을 요청해요. 누른 채 말하면 로컬 mock 서버로 보내요."; return; }
  if (!microphoneSupported()) { event.target.checked = false; return; }
  event.target.disabled = true;
  $("microphoneHint").textContent = "브라우저의 마이크 권한을 확인하고 있어요.";
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: false });
    if (attempt !== permissionGeneration) return;
    microphoneEnabled = true; event.target.checked = true;
    $("microphoneHint").textContent = "준비됐어요. 버튼 또는 Space 키를 누른 채 말해요. 오디오는 로컬 mock 서버로 보내요.";
  } catch {
    if (attempt !== permissionGeneration) return;
    event.target.checked = false;
    $("microphoneHint").textContent = "마이크 권한을 받지 못했어요. 다시 선택하거나 마이크 없이 데모를 사용해요.";
  } finally {
    stream?.getTracks().forEach(track => track.stop());
    if (attempt === permissionGeneration) updateControls();
  }
});
$("retryAudioButton").addEventListener("click", () => { if (!retryAudioUrl || operation) return; const url = retryAudioUrl; const op = beginOperation("speaking"); if (op) playAudio(op, url); });
$("retryConnectionButton").addEventListener("click", () => checkHealth());
window.addEventListener("pagehide", event => { permissionGeneration++; microphoneEnabled = false; $("microphoneToggle").checked = false; healthController?.abort(); healthController = null; cancelOperation({ silent: true }); if (!event.persisted) face.destroy(); });

if (!microphoneSupported()) $("microphoneHint").textContent = "이 브라우저에서는 마이크 녹음을 지원하지 않아요. localhost의 최신 브라우저에서 사용할 수 있어요.";
updateSettings(); updateControls(); setPhase("idle"); checkHealth();
