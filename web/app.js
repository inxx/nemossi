import { Face, FACE_STATES } from "./face.js";
import { makeDemoWav, wavEnvelope, microphoneSupported, captureMicrophone, MAX_SECONDS } from "./audio.js";
import { DeviceClient } from "./device.js";

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
let keyboardHeld = false;
let pointerHeld = false;
let hadConnection = false;
let connectionAction = false;
const device = new DeviceClient({ onSnapshot: applyDeviceSnapshot, onStatus: applyConnectionStatus, onError: () => {} });

const DEVICE_STATE_LABELS = { offline: "offline", idle: "대기", listening: "입력 중", thinking: "처리 중", ready: "재생 준비", speaking: "재생 중", error: "오류" };
function applyConnectionStatus(status) {
  const connected = status === "connected";
  const transient = ["checking", "connecting", "disconnecting", "reconnecting"].includes(status);
  const labels = { checking: "상태 확인 중", connecting: "연결하는 중", connected: "시뮬레이터 연결됨", disconnecting: "연결 해제 중", reconnecting: "다시 연결하는 중", offline: "시뮬레이터 연결 끊김", unavailable: "허브 연결 끊김" };
  $("deviceConnectionLabel").textContent = labels[status] || "연결 확인 필요";
  $("settingsConnectionLabel").textContent = `현재 단말: ${labels[status] || "연결 확인 필요"}`;
  $("deviceConnectionLabel").closest(".connection-card").className = `connection-card ${status}`;
  $("deviceStatusText").textContent = connected ? "서버와 연결 상태 · 대화 · 재생 완료를 주고받아요." : transient ? "서버가 연결을 확인하면 입력할 수 있어요." : "연결 후 단말 데모를 시작할 수 있어요. 실제 dot 통화는 offline이에요.";
  $("connectDeviceButton").hidden = connected || hadConnection;
  $("reconnectDeviceButton").hidden = !hadConnection;
  $("disconnectDeviceButton").hidden = !connected;
  for (const id of ["connectDeviceButton", "reconnectDeviceButton", "disconnectDeviceButton"]) $(id).disabled = transient || connectionAction;
  if (status === "unavailable") {
    $("connectionStatus").className = "server-status offline"; $("connectionLabel").textContent = "허브 연결 확인 필요";
    if (operation) cancelOperation({ silent: true, notifyServer: false });
    showNotice("Mac mini 허브와 연결이 끊겼어요. 다시 연결해 주세요.", { error: true, connection: true });
  }
  updateControls();
}
function applyDeviceSnapshot(snapshot) {
  if (snapshot.connected) hadConnection = true;
  $("reconnectDeviceButton").hidden = !hadConnection;
  $("connectionStatus").className = "server-status ready"; $("connectionLabel").textContent = "Mac mini mock 허브 준비";
  $("deviceStateLabel").textContent = `서버 상태 · ${DEVICE_STATE_LABELS[snapshot.state]}`;
  $("recoverDeviceButton").hidden = snapshot.state !== "error" || !device.usable;
  $("recoverDeviceButton").disabled = connectionAction || Boolean(operation);
  if (operation && (snapshot.connection_id !== operation.connectionId || !snapshot.connected
    || (operation.serverTurnId && operation.phase !== "settling" && snapshot.active_turn_id !== operation.serverTurnId))) {
    cancelOperation({ silent: true, notifyServer: false });
    showNotice("서버에서 단말 연결이나 대화가 변경됐어요. 현재 상태에서 다시 시작해 주세요.");
  }
  if (!operation && manualState === "auto" && !(happyTimer && snapshot.state === "idle")) {
    const phase = { offline: "sleep", idle: "idle", listening: "listening", thinking: "thinking", ready: "thinking", speaking: "speaking", error: "error" }[snapshot.state];
    setPhase(phase, snapshot.state === "offline" ? "연결하면 다시 만나요." : snapshot.state === "ready" ? "단말에서 재생을 기다려요." : snapshot.state === "speaking" ? "단말이 재생 상태를 보고했어요." : undefined);
  }
  updateControls();
}

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
  const idle = device.usable && device.snapshot.state === "idle" && !connectionAction;
  $("talkButton").disabled = !listening && (!idle || Boolean(operation));
  $("cancelButton").disabled = !operation;
  $("sendButton").disabled = Boolean(operation) || !idle;
  $("messageInput").disabled = Boolean(operation) || !idle;
  $("microphoneToggle").disabled = Boolean(operation) || !idle || !microphoneSupported();
  $("talkLabel").textContent = listening ? "놓으면 보내요" : microphoneEnabled ? "누른 채 말하기" : "대화하기";
  $("talkHint").textContent = !device.usable ? "네모씨 시뮬레이터를 연결한 뒤 시작해요." : microphoneEnabled ? "버튼 또는 Space 키를 누른 채 말해요. 최대 15초." : "마이크 없이 데모를 시작해요. Space 키로도 가능해요.";
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
  if (operation || !device.usable || device.snapshot.state !== "idle" || connectionAction) return null;
  clearTimeout(happyTimer); resetManualState(); clearNotice();
  const op = { id: ++generation, controller: new AbortController(), phase, connectionId: device.snapshot.connection_id, timers: new Set(), capture: null, audio: null, releasing: false };
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
  if (state === "happy") happyTimer = setTimeout(() => { happyTimer = null; if (!operation && manualState === "auto") setPhase("idle"); }, 1800);
}
function cancelOperation({ silent = false, notifyServer = true } = {}) {
  clearTimeout(happyTimer); happyTimer = null; pointerHeld = keyboardHeld = false;
  const hadOperation = Boolean(operation);
  if (operation) { const op = operation; operation = null; generation++; op.controller.abort(); cleanOperation(op); }
  if (hadOperation && notifyServer && device.usable) device.event("cancel").catch(() => device.refresh().catch(() => {}));
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
  if (device.usable && device.snapshot.state !== "error") device.event("cancel").catch(() => device.refresh().catch(() => {}));
}

async function requestTurn(op, body, audioInput = false) {
  if (!current(op)) return;
  op.phase = "thinking"; setPhase("thinking"); updateControls();
  try {
    const result = await device.turn(body, { audio: audioInput, sessionId, turnId: op.serverTurnId, signal: op.controller.signal });
    if (!current(op)) return;
    op.serverTurnId = result.turn_id; op.phase = "ready";
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
    } else { throw new Error("데모 소리가 없어 재생을 확인할 수 없어요."); }
  } catch (error) {
    if (error.name === "AbortError" || !current(op)) return;
    if (error instanceof TypeError) { error.message = "로컬 서버에 연결할 수 없어요. 실행 상태를 확인해 주세요."; error.connection = true; }
    operationError(op, error);
  }
}

async function playAudio(op, url) {
  if (!current(op)) return;
  op.phase = "ready"; setPhase("thinking", "데모 소리를 준비하고 있어요."); updateControls();
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
  const done = async () => {
    op.audioEnded = true;
    if (!current(op) || !op.playbackStarted || op.phase === "settling") return;
    op.phase = "settling"; setPhase("thinking", "재생 완료를 확인하고 있어요.");
    try {
      await device.event("playback_end", { turnId: op.serverTurnId, signal: op.controller.signal });
      if (current(op)) finishOperation(op);
    } catch (error) { if (error.name !== "AbortError") operationError(op, error); }
  };
  const failed = async () => {
    if (!current(op) || op.phase === "settling") return;
    op.phase = "settling"; audio.pause(); face.setMouthLevel(0);
    try {
      await device.event("playback_error", { turnId: op.serverTurnId, signal: op.controller.signal });
      if (!current(op)) return;
      finishOperation(op, "error", "소리를 재생하지 못했어요.");
      showNotice("브라우저가 데모 소리를 재생하지 못했어요. 단말 오류를 복구한 뒤 새 대화를 시작해 주세요.", { error: true, audio: true });
    } catch (error) { if (error.name !== "AbortError") operationError(op, error); }
  };
  audio.addEventListener("ended", () => { done(); }, { once: true });
  audio.addEventListener("error", failed, { once: true });
  const timeout = setTimeout(() => { if (current(op)) failed(); }, 18000); op.timers.add(timeout);
  try {
    await audio.play();
    if (!current(op)) { audio.pause(); return; }
    if (op.phase === "settling") return;
    await device.event("playback_start", { turnId: op.serverTurnId, signal: op.controller.signal });
    if (!current(op) || op.phase === "settling") return;
    op.playbackStarted = true; op.phase = "speaking"; setPhase("speaking"); updateControls();
    if (op.audioEnded || audio.ended) done();
  } catch {
    if (current(op)) failed();
  }
}

async function beginListening() {
  const op = beginOperation("listening"); if (!op) return;
  op.startedAt = performance.now(); op.isMicrophone = microphoneEnabled;
  try {
    const listening = await device.event("listen", { signal: op.controller.signal });
    if (!current(op)) return;
    op.serverTurnId = listening.active_turn_id;
    const deadline = setTimeout(() => {
      if (current(op) && op.phase === "listening") operationError(op, new Error("입력 준비 시간이 길어졌어요. 다시 시작해 주세요."));
    }, 19000); op.timers.add(deadline);
    if (!op.isMicrophone) {
      const maximum = setTimeout(() => endListening(), MAX_SECONDS * 1000); op.timers.add(maximum);
      if (op.releaseRequested) endListening();
      return;
    }
    setPhase("listening", "마이크를 준비하고 있어요.");
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
  if (!op.serverTurnId || (op.isMicrophone && !op.capture)) { op.releaseRequested = true; return; }
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
document.addEventListener("visibilitychange", () => {
  if (document.hidden) { if (operation) cancelOperation({ silent: true }); device.suspend(); }
  else device.start();
});

$("stateSelect").addEventListener("change", event => {
  cancelOperation({ silent: true }); manualState = event.target.value;
  setPhase(manualState === "auto" ? "idle" : manualState);
  if (manualState === "speaking") face.setMouthLevel(0.45);
});
$("expressionSelect").addEventListener("change", () => {
  face.setExpression($("expressionSelect").value);
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
async function connectionCommand(action) {
  if (connectionAction) return;
  cancelOperation({ silent: true, notifyServer: false }); resetManualState();
  connectionAction = true; updateControls();
  for (const id of ["connectDeviceButton", "disconnectDeviceButton", "reconnectDeviceButton", "recoverDeviceButton"]) $(id).disabled = true;
  try {
    if (action === "disconnect") await device.disconnect();
    else if (action === "recover") await device.event("recover");
    else await device.connect({ reconnect: action === "reconnect" });
    clearNotice();
  } catch (error) {
    if (error.name !== "AbortError") showNotice(error.message || "단말 연결을 완료하지 못했어요.", { error: true, connection: true });
  } finally {
    connectionAction = false; applyConnectionStatus(device.status);
    if (device.snapshot && device.status !== "unavailable") applyDeviceSnapshot(device.snapshot);
    updateControls();
  }
}
$("connectDeviceButton").addEventListener("click", () => connectionCommand("connect"));
$("disconnectDeviceButton").addEventListener("click", () => connectionCommand("disconnect"));
$("reconnectDeviceButton").addEventListener("click", () => connectionCommand("reconnect"));
$("recoverDeviceButton").addEventListener("click", () => connectionCommand("recover"));
$("retryAudioButton").addEventListener("click", () => { if (device.snapshot?.state === "error") connectionCommand("recover"); });
$("retryConnectionButton").addEventListener("click", () => device.refreshHub().catch(error => showNotice(error.message, { error: true, connection: true })));
window.addEventListener("pagehide", event => {
  permissionGeneration++; microphoneEnabled = false; $("microphoneToggle").checked = false;
  cancelOperation({ silent: true }); device.suspend();
  if (!event.persisted) { device.destroy(); face.destroy(); }
});
window.addEventListener("pageshow", event => { if (event.persisted) device.start(); });

if (!microphoneSupported()) $("microphoneHint").textContent = "이 브라우저에서는 마이크 녹음을 지원하지 않아요. localhost의 최신 브라우저에서 사용할 수 있어요.";
updateSettings(); updateControls(); setPhase("sleep", "연결하면 다시 만나요."); device.start();
