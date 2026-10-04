const STATES = new Set(["offline", "idle", "listening", "thinking", "ready", "speaking", "error"]);
const ID = /^[0-9a-f]{32}$/;
const abortError = () => new DOMException("이전 단말 요청을 취소했어요.", "AbortError");

export class DeviceError extends Error {
  constructor(message, code = "device_error", status = 0) {
    super(message); this.name = "DeviceError"; this.code = code; this.status = status;
  }
}

export function validateSnapshot(value) {
  if (!value || typeof value.connected !== "boolean" || !Number.isSafeInteger(value.revision) || value.revision < 0 || !STATES.has(value.state)
    || (value.connected ? !ID.test(value.connection_id || "") || value.state === "offline" : value.connection_id !== null || value.state !== "offline")
    || (value.active_turn_id !== null && !ID.test(value.active_turn_id || ""))
    || value.device_kind !== "nemossi_simulator" || value.hardware_tested !== false || value.dot_connected !== false) {
    throw new DeviceError("단말 서버의 상태 형식을 확인할 수 없어요.", "invalid_snapshot");
  }
  return Object.freeze({ ...value });
}

/** Shared simulator state lives on the hub; browser generations only guard stale I/O. */
export class DeviceClient {
  constructor({ fetchImpl = globalThis.fetch.bind(globalThis), onSnapshot = () => {}, onStatus = () => {}, onError = () => {},
    requestTimeoutMs = 4000, turnTimeoutMs = 19000, pollIntervalMs = 2000, heartbeatIntervalMs = 20000 } = {}) {
    this.fetchImpl = fetchImpl; this.onSnapshot = onSnapshot; this.onStatus = onStatus; this.onError = onError;
    this.requestTimeoutMs = requestTimeoutMs; this.turnTimeoutMs = turnTimeoutMs;
    this.pollIntervalMs = pollIntervalMs; this.heartbeatIntervalMs = heartbeatIntervalMs;
    this.snapshot = null; this.status = "checking"; this.generation = 0;
    this.controllers = new Set(); this.queue = Promise.resolve(); this.running = false; this.destroyed = false;
    this.lastHeartbeat = 0; this.pollTimer = null; this.pollGeneration = 0;
  }

  get usable() { return this.status === "connected" && this.snapshot?.connected === true; }
  _status(status) { this.status = status; this.onStatus(status); }
  _invalidate() { this.generation++; for (const controller of this.controllers) controller.abort(); }
  _assertEpoch(epoch) { if (this.destroyed || epoch !== this.generation) throw abortError(); }
  _enqueue(command) {
    const epoch = this.generation;
    const result = this.queue.then(() => { this._assertEpoch(epoch); return command(epoch); });
    this.queue = result.catch(() => {});
    return result;
  }
  _connection() {
    if (!this.usable) throw new DeviceError("네모씨 시뮬레이터를 먼저 연결해 주세요.", "not_connected");
    return this.snapshot.connection_id;
  }
  _apply(value, epoch, resetRevision = false) {
    this._assertEpoch(epoch);
    const snapshot = validateSnapshot(value);
    // Serial requests plus epochs prevent old connections from returning after reconnect.
    // A revision reset is valid after a hub restart / a new connection identity.
    if (!resetRevision && this.snapshot?.connection_id === snapshot.connection_id && snapshot.revision < this.snapshot.revision) return this.snapshot;
    this.snapshot = snapshot;
    this._status(snapshot.connected ? "connected" : "offline");
    this.onSnapshot(snapshot);
    return snapshot;
  }

  async _request(path, options, epoch, signal, timeoutMs = this.requestTimeoutMs) {
    this._assertEpoch(epoch);
    if (signal?.aborted) throw abortError();
    const controller = new AbortController(); this.controllers.add(controller);
    let timeout, expired = false, removeAbort;
    const externalAbort = () => controller.abort();
    signal?.addEventListener("abort", externalAbort, { once: true });
    const aborted = new Promise((_, reject) => {
      const rejectAbort = () => reject(expired ? new DeviceError("단말 서버의 응답 시간이 길어졌어요. 다시 연결해 주세요.", "timeout") : abortError());
      controller.signal.addEventListener("abort", rejectAbort, { once: true });
      removeAbort = () => controller.signal.removeEventListener("abort", rejectAbort);
      timeout = setTimeout(() => { expired = true; controller.abort(); }, timeoutMs);
    });
    const request = (async () => {
      const response = await this.fetchImpl(path, { ...options, signal: controller.signal, cache: "no-store" });
      let payload;
      try { payload = await response.json(); } catch (error) {
        if (controller.signal.aborted) throw abortError();
        throw new DeviceError("단말 서버의 응답 형식을 확인할 수 없어요.", "invalid_response", response.status);
      }
      if (!response.ok) {
        const detail = payload?.error;
        throw new DeviceError(typeof detail?.message === "string" ? detail.message : "단말 요청을 처리하지 못했어요.", detail?.code || "request_failed", response.status);
      }
      return payload;
    })();
    try {
      const payload = await Promise.race([request, aborted]);
      this._assertEpoch(epoch);
      if (signal?.aborted) throw abortError();
      return payload;
    } catch (error) {
      if (epoch === this.generation && !this.destroyed && (error instanceof TypeError || error.code === "timeout")) this._status("unavailable");
      throw error;
    } finally {
      clearTimeout(timeout); removeAbort(); signal?.removeEventListener("abort", externalAbort); this.controllers.delete(controller);
    }
  }
  _post(path, body, epoch, signal) {
    return this._request(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }, epoch, signal);
  }

  refreshHub() {
    return this._enqueue(async epoch => {
      try {
        const hub = await this._request("/api/hub", {}, epoch);
        if (hub?.hub?.role !== "mac_mini" || hub.hub.mode !== "mock" || hub.dot_connected !== false) throw new DeviceError("Mac mini mock 허브를 확인할 수 없어요.", "invalid_hub");
        this._apply(hub.device, epoch); return hub;
      } catch (error) { if (epoch === this.generation && error.name !== "AbortError" && !this.destroyed) this._status("unavailable"); throw error; }
    });
  }
  refresh() {
    return this._enqueue(async epoch => {
      try { return this._apply(await this._request("/api/device", {}, epoch), epoch); }
      catch (error) { if (epoch === this.generation && error.name !== "AbortError" && !this.destroyed) this._status("unavailable"); throw error; }
    });
  }
  connect({ reconnect = false } = {}) {
    const previousConnection = reconnect ? this.snapshot?.connection_id : null;
    this._invalidate(); this._status(reconnect ? "reconnecting" : "connecting");
    return this._enqueue(async epoch => {
      try {
        if (previousConnection) {
          try { this._apply(await this._post("/api/device/disconnect", { connection_id: previousConnection }, epoch), epoch); }
          catch (error) { if (error.status !== 409) throw error; }
          this._assertEpoch(epoch); this._status("reconnecting");
        }
        const snapshot = this._apply(await this._post("/api/device/connect", {}, epoch), epoch, true);
        this.lastHeartbeat = Date.now(); return snapshot;
      } catch (error) { if (epoch === this.generation && !this.destroyed) this._status("unavailable"); throw error; }
    });
  }
  disconnect() {
    const connectionId = this.snapshot?.connection_id;
    this._invalidate(); this._status("disconnecting");
    return this._enqueue(async epoch => {
      try {
        if (!connectionId) return this._apply(await this._request("/api/device", {}, epoch), epoch);
        return this._apply(await this._post("/api/device/disconnect", { connection_id: connectionId }, epoch), epoch);
      } catch (error) { if (epoch === this.generation && !this.destroyed) this._status("unavailable"); throw error; }
    });
  }
  event(event, { turnId, signal } = {}) {
    if (event === "cancel") this._invalidate();
    return this._enqueue(async epoch => {
      const body = { connection_id: this._connection(), event };
      if (turnId) body.turn_id = turnId;
      const snapshot = this._apply(await this._post("/api/device/event", body, epoch, signal), epoch);
      if (event === "heartbeat") this.lastHeartbeat = Date.now();
      return snapshot;
    });
  }
  turn(body, { audio = false, sessionId, turnId, signal } = {}) {
    return this._enqueue(async epoch => {
      const connectionId = this._connection();
      const headers = { "Content-Type": audio ? "audio/wav" : "application/json", "X-Device-Connection-ID": connectionId };
      if (turnId) headers["X-Device-Turn-ID"] = turnId;
      if (audio && sessionId) headers["X-Session-ID"] = sessionId;
      const result = await this._request("/api/device/turn", { method: "POST", headers, body: audio ? body : JSON.stringify({ text: body, session_id: sessionId }) }, epoch, signal, this.turnTimeoutMs);
      this._apply(result.device, epoch);
      if (!ID.test(result.turn_id || "") || result.turn_id !== this.snapshot.active_turn_id || this.snapshot.state !== "ready") throw new DeviceError("대화와 단말 상태가 일치하지 않아요.", "stale_turn");
      return result;
    });
  }

  start() {
    if (this.destroyed || this.running) return;
    this.running = true;
    const run = ++this.pollGeneration;
    const poll = async () => {
      if (!this.running || this.destroyed || run !== this.pollGeneration) return;
      try {
        if (!this.snapshot) await this.refreshHub(); else await this.refresh();
        if (this.running && run === this.pollGeneration && this.usable && Date.now() - this.lastHeartbeat >= this.heartbeatIntervalMs) await this.event("heartbeat");
      } catch (error) { if (error.name !== "AbortError" && this.running && run === this.pollGeneration) this.onError(error); }
      finally { if (this.running && !this.destroyed && run === this.pollGeneration) this.pollTimer = setTimeout(poll, this.pollIntervalMs); }
    };
    poll();
  }
  suspend() { this.running = false; this.pollGeneration++; clearTimeout(this.pollTimer); this.pollTimer = null; }
  destroy() { this.suspend(); this.destroyed = true; this._invalidate(); }
}
