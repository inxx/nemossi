import test from 'node:test';
import assert from 'node:assert/strict';
import { DeviceClient, validateSnapshot } from '../web/device.js';

const OLD = 'a'.repeat(32), NEXT = 'b'.repeat(32), TURN = 'c'.repeat(32);
const snapshot = (changes = {}) => ({ connected: true, connection_id: OLD, revision: 1, state: 'idle', active_turn_id: null,
  last_event: 'connect', last_error: null, device_kind: 'nemossi_simulator', hardware_tested: false, dot_connected: false, ...changes });
const offline = changes => snapshot({ connected: false, connection_id: null, state: 'offline', ...changes });
const response = (body, status = 200) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };
const tick = () => new Promise(resolve => setImmediate(resolve));
const clientFor = (t, fetchImpl, options = {}) => {
  const client = new DeviceClient({ fetchImpl, ...options }); t.after(() => client.destroy()); return client;
};

test('connect → listen → WAV turn → actual playback ACKs preserve connection and turn identities', async t => {
  let state = offline({ revision: 0 }); const calls = [];
  const client = clientFor(t, async (path, options) => {
    calls.push({ path, options });
    if (path.endsWith('/connect')) { assert.deepEqual(JSON.parse(options.body), {}); state = snapshot(); }
    else if (path.endsWith('/event')) {
      const body = JSON.parse(options.body); assert.equal(body.connection_id, OLD);
      if (body.event === 'listen') state = snapshot({ revision: 2, state: 'listening', active_turn_id: TURN });
      if (body.event === 'playback_start') { assert.equal(body.turn_id, TURN); state = snapshot({ revision: 4, state: 'speaking', active_turn_id: TURN }); }
      if (body.event === 'playback_end') { assert.equal(body.turn_id, TURN); state = snapshot({ revision: 5 }); }
    } else if (path.endsWith('/turn')) {
      assert.equal(options.headers['X-Device-Connection-ID'], OLD);
      assert.equal(options.headers['X-Device-Turn-ID'], TURN);
      assert.equal(options.headers['Content-Type'], 'audio/wav');
      assert.equal(options.headers['X-Session-ID'], 'session-test');
      state = snapshot({ revision: 3, state: 'ready', active_turn_id: TURN });
      return response({ turn_id: TURN, text: 'demo', mode: 'mock', device: state });
    }
    return response(state);
  });
  await client.connect(); assert.equal(client.usable, true);
  const listening = await client.event('listen'); assert.equal(listening.active_turn_id, TURN);
  const result = await client.turn(new Blob(['wav']), { audio: true, sessionId: 'session-test', turnId: TURN });
  assert.equal(result.device.state, 'ready');
  await client.event('playback_start', { turnId: TURN }); assert.equal(client.snapshot.state, 'speaking');
  await client.event('playback_end', { turnId: TURN }); assert.equal(client.snapshot.state, 'idle');
  assert.equal(calls.length, 5);
});

test('mutations and polling are serialized rather than racing revisions', async t => {
  const listen = deferred(); const calls = [];
  const client = clientFor(t, async path => {
    calls.push(path);
    if (path.endsWith('/connect')) return response(snapshot());
    if (path.endsWith('/event')) return listen.promise;
    return response(snapshot({ revision: 3, state: 'listening', active_turn_id: TURN }));
  });
  await client.connect();
  const command = client.event('listen'); const polling = client.refresh();
  await tick(); assert.deepEqual(calls, ['/api/device/connect', '/api/device/event']);
  listen.resolve(response(snapshot({ revision: 2, state: 'listening', active_turn_id: TURN })));
  await command; await polling;
  assert.equal(client.snapshot.revision, 3); assert.equal(calls.at(-1), '/api/device');
});

test('disconnect aborts an in-flight turn and ignores its eventual successful response', async t => {
  const turn = deferred(); let turnSignal;
  const client = clientFor(t, async (path, options) => {
    if (path.endsWith('/connect')) return response(snapshot());
    if (path.endsWith('/turn')) { turnSignal = options.signal; return turn.promise; }
    assert.deepEqual(JSON.parse(options.body), { connection_id: OLD });
    return response(offline({ revision: 3, last_event: 'disconnect' }));
  });
  await client.connect();
  const request = client.turn('hello'); const rejected = assert.rejects(request, { name: 'AbortError' });
  await tick(); const disconnect = client.disconnect();
  assert.equal(turnSignal.aborted, true); assert.equal(client.usable, false);
  await rejected; await disconnect;
  turn.resolve(response({ turn_id: TURN, device: snapshot({ revision: 99, state: 'ready', active_turn_id: TURN }) }));
  await tick(); assert.equal(client.snapshot.connected, false); assert.equal(client.status, 'offline');
});

test('reconnect enables input only after confirmation and rejects a stale old poll', async t => {
  const oldPoll = deferred(), reconnect = deferred(); let connects = 0; const statuses = [], calls = [];
  const client = clientFor(t, async path => {
    calls.push(path);
    if (path.endsWith('/connect')) return ++connects === 1 ? response(snapshot({ revision: 20 })) : reconnect.promise;
    if (path.endsWith('/disconnect')) return response(offline({ revision: 21 }));
    return oldPoll.promise;
  }, { onStatus: status => statuses.push(status) });
  await client.connect();
  const poll = client.refresh(); const rejected = assert.rejects(poll, { name: 'AbortError' }); await tick();
  const connecting = client.connect({ reconnect: true });
  assert.equal(client.status, 'reconnecting'); assert.equal(client.usable, false);
  await rejected; await tick();
  reconnect.resolve(response(snapshot({ connection_id: NEXT, revision: 0 }))); await connecting;
  oldPoll.resolve(response(snapshot({ revision: 99 }))); await tick();
  assert.equal(client.usable, true); assert.equal(client.snapshot.connection_id, NEXT); assert.equal(client.snapshot.revision, 0);
  assert.equal(statuses.at(-1), 'connected');
  assert.deepEqual(calls, ['/api/device/connect', '/api/device', '/api/device/disconnect', '/api/device/connect']);
});

test('external abort releases the command queue even if fetch ignores its signal', async t => {
  const hanging = deferred(); let signal;
  const client = clientFor(t, async (path, options) => {
    if (path.endsWith('/turn')) { signal = options.signal; return hanging.promise; }
    return response(snapshot({ revision: 2 }));
  });
  await client.connect();
  const abort = new AbortController(); const request = client.turn('hello', { signal: abort.signal });
  const rejected = assert.rejects(request, { name: 'AbortError' }); await tick(); abort.abort(); await rejected;
  assert.equal(signal.aborted, true);
  await client.refresh(); assert.equal(client.usable, true);
  hanging.resolve(response({ device: snapshot({ revision: 99, state: 'ready', active_turn_id: TURN }), turn_id: TURN }));
  await tick(); assert.equal(client.snapshot.revision, 2);
});

test('bounded requests also time out a hanging response body, and a later refresh recovers', async t => {
  let hanging = false;
  const client = clientFor(t, async () => hanging ? { ok: true, status: 200, json: () => new Promise(() => {}) } : response(snapshot()), { requestTimeoutMs: 20 });
  await client.connect(); hanging = true;
  await assert.rejects(client.event('heartbeat'), error => error.code === 'timeout');
  assert.equal(client.usable, false); assert.equal(client.status, 'unavailable');
  hanging = false; await client.refresh(); assert.equal(client.usable, true);
});

test('cancel invalidates an active turn before queuing its server ACK', async t => {
  const hanging = deferred(); const events = [];
  const client = clientFor(t, async (path, options) => {
    if (path.endsWith('/turn')) return hanging.promise;
    if (path.endsWith('/event')) events.push(JSON.parse(options.body));
    return response(snapshot({ revision: events.length + 1 }));
  });
  await client.connect(); const turn = client.turn('hello'); const rejected = assert.rejects(turn, { name: 'AbortError' }); await tick();
  await client.event('cancel'); await rejected;
  assert.deepEqual(events, [{ connection_id: OLD, event: 'cancel' }]); assert.equal(client.snapshot.state, 'idle');
});

test('hub truth and device types are checked; lower same-connection revisions cannot roll back state', async t => {
  assert.throws(() => validateSnapshot(snapshot({ device_kind: 'glasses' })), error => error.code === 'invalid_snapshot');
  assert.throws(() => validateSnapshot(snapshot({ dot_connected: true })), error => error.code === 'invalid_snapshot');
  let revision = 4; let publications = 0;
  const client = clientFor(t, async path => {
    if (path === '/api/hub') return response({ hub: { role: 'mac_mini', mode: 'mock' }, dot_connected: false, device: snapshot({ revision }) });
    return response(snapshot({ revision }));
  }, { onSnapshot: () => publications++ });
  await client.refreshHub(); revision = 2; await client.refresh();
  assert.equal(client.snapshot.revision, 4); assert.equal(publications, 1);
});

test('fast suspend/start while the first poll awaits does not spawn a second polling loop', async t => {
  const initial = deferred(), firstPoll = deferred(); let hubs = 0, polls = 0;
  const client = clientFor(t, async () => {
    hubs++;
    if (hubs === 1) return initial.promise;
    return response({ hub: { role: 'mac_mini', mode: 'mock' }, dot_connected: false, device: offline({ revision: 0 }) });
  }, { pollIntervalMs: 40 });
  client.refresh = async () => { polls++; firstPoll.resolve(); return client.snapshot; };
  client.start(); await tick(); client.suspend(); client.start();
  initial.resolve(response({ hub: { role: 'mac_mini', mode: 'mock' }, dot_connected: false, device: offline({ revision: 0 }) }));
  await firstPoll.promise;
  await new Promise(resolve => setTimeout(resolve, 10)); client.suspend();
  assert.equal(hubs, 2); assert.equal(polls, 1);
});
