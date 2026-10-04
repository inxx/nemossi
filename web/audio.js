export const TARGET_SAMPLE_RATE = 16000;
export const MAX_SECONDS = 15;

/** Downsampling averages source samples; upsampling uses linear interpolation. */
export function resampleMono(samples, sourceRate, targetRate = TARGET_SAMPLE_RATE) {
  if (!Number.isFinite(sourceRate) || sourceRate <= 0 || !Number.isFinite(targetRate) || targetRate <= 0) throw new RangeError("Invalid sample rate");
  if (sourceRate === targetRate) return samples.slice();
  const ratio = sourceRate / targetRate;
  const length = Math.floor(samples.length / ratio);
  const output = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    const start = i * ratio;
    if (ratio < 1) {
      const index = Math.floor(start), weight = start - index;
      output[i] = samples[index] * (1 - weight) + (samples[Math.min(index + 1, samples.length - 1)] || 0) * weight;
    } else {
      const end = Math.min((i + 1) * ratio, samples.length);
      let sum = 0;
      for (let index = Math.floor(start); index < Math.ceil(end); index++) {
        const weight = Math.min(index + 1, end) - Math.max(index, start);
        sum += samples[index] * weight;
      }
      output[i] = sum / (end - start);
    }
  }
  return output;
}

export function encodeWav(samples, sampleRate = TARGET_SAMPLE_RATE) {
  const buffer = new ArrayBuffer(44 + samples.length * 2);
  const view = new DataView(buffer);
  const write = (offset, text) => { for (let i = 0; i < text.length; i++) view.setUint8(offset + i, text.charCodeAt(i)); };
  write(0, "RIFF"); view.setUint32(4, buffer.byteLength - 8, true); write(8, "WAVE");
  write(12, "fmt "); view.setUint32(16, 16, true); view.setUint16(20, 1, true);
  view.setUint16(22, 1, true); view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true);
  write(36, "data"); view.setUint32(40, samples.length * 2, true);
  for (let i = 0; i < samples.length; i++) {
    const value = Math.max(-1, Math.min(1, Number.isFinite(samples[i]) ? samples[i] : 0));
    view.setInt16(44 + i * 2, Math.round(value < 0 ? value * 32768 : value * 32767), true);
  }
  return new Blob([buffer], { type: "audio/wav" });
}

export function makeDemoWav() {
  const samples = new Float32Array(TARGET_SAMPLE_RATE * 0.75);
  for (let i = 0; i < samples.length; i++) {
    const time = i / TARGET_SAMPLE_RATE;
    const envelope = Math.min(1, time / 0.02, (0.75 - time) / 0.03);
    samples[i] = Math.sin(time * Math.PI * 2 * 260) * 0.08 * envelope;
  }
  return encodeWav(samples);
}

/** PCM16 WAV envelope for mouth animation, indexed by actual playback time. */
export function wavEnvelope(buffer, bucketsPerSecond = 30) {
  const view = new DataView(buffer);
  const tag = offset => String.fromCharCode(...new Uint8Array(buffer, offset, 4));
  if (view.byteLength < 44 || tag(0) !== "RIFF" || tag(8) !== "WAVE") throw new Error("Invalid WAV");
  let sampleRate, channels, bits, pcm, dataOffset, dataLength;
  for (let offset = 12; offset + 8 <= view.byteLength;) {
    const size = view.getUint32(offset + 4, true);
    if (offset + 8 + size > view.byteLength) throw new Error("Truncated WAV");
    if (tag(offset) === "fmt " && size >= 16) {
      pcm = view.getUint16(offset + 8, true); channels = view.getUint16(offset + 10, true);
      sampleRate = view.getUint32(offset + 12, true); bits = view.getUint16(offset + 22, true);
    }
    if (tag(offset) === "data") { dataOffset = offset + 8; dataLength = size; }
    offset += 8 + size + (size % 2);
  }
  if (pcm !== 1 || bits !== 16 || channels !== 1 || !sampleRate || dataOffset === undefined || dataLength % 2) throw new Error("Unsupported WAV");
  const samples = dataLength / 2;
  const samplesPerBucket = Math.max(1, Math.round(sampleRate / bucketsPerSecond));
  const levels = new Float32Array(Math.ceil(samples / samplesPerBucket));
  for (let bucket = 0; bucket < levels.length; bucket++) {
    const start = bucket * samplesPerBucket, end = Math.min(samples, start + samplesPerBucket);
    let energy = 0;
    for (let i = start; i < end; i++) { const value = view.getInt16(dataOffset + i * 2, true) / 32768; energy += value * value; }
    levels[bucket] = Math.min(1, Math.sqrt(energy / (end - start)) * 5);
  }
  return { levels, bucketsPerSecond: sampleRate / samplesPerBucket, duration: samples / sampleRate };
}

export function microphoneSupported() {
  return Boolean(window.isSecureContext && navigator.mediaDevices?.getUserMedia && window.AudioContext && window.AudioWorkletNode);
}

export async function captureMicrophone(signal) {
  let stream, context, source, processor, silentGain;
  let disposed = false;
  let chunks = [], sampleCount = 0;
  const cleanup = () => {
    if (disposed) return;
    disposed = true;
    try { source?.disconnect(); processor?.disconnect(); silentGain?.disconnect(); } catch { /* already disconnected */ }
    if (processor) { processor.port.onmessage = null; processor.port.close(); }
    stream?.getTracks().forEach(track => track.stop());
    if (context && context.state !== "closed") context.close().catch(() => {});
    signal.removeEventListener("abort", cleanup);
  };
  signal.addEventListener("abort", cleanup, { once: true });
  const check = () => { if (signal.aborted) throw new DOMException("Aborted", "AbortError"); };
  try {
    check();
    // Create/resume in the original pointer/key activation before permission awaits.
    context = new AudioContext({ latencyHint: "interactive" });
    const resumed = context.resume().then(() => null, error => error);
    stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true }, video: false });
    if (signal.aborted) { stream.getTracks().forEach(track => track.stop()); check(); }
    const resumeError = await resumed;
    if (resumeError) throw resumeError;
    await context.audioWorklet.addModule("/recorder-worklet.js");
    check();
    source = context.createMediaStreamSource(stream);
    processor = new AudioWorkletNode(context, "nemossi-recorder", { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1] });
    silentGain = context.createGain(); silentGain.gain.value = 0;
    const limit = Math.floor(context.sampleRate * MAX_SECONDS);
    processor.port.onmessage = event => {
      if (disposed || signal.aborted) return;
      const incoming = new Float32Array(event.data);
      const chunk = incoming.subarray(0, Math.max(0, limit - sampleCount));
      if (chunk.length) { chunks.push(chunk); sampleCount += chunk.length; }
    };
    source.connect(processor); processor.connect(silentGain); silentGain.connect(context.destination);
    await context.resume(); check();
    return {
      finish() {
        const rate = context.sampleRate;
        cleanup();
        const samples = new Float32Array(sampleCount);
        let offset = 0;
        for (const chunk of chunks) { samples.set(chunk, offset); offset += chunk.length; }
        chunks = [];
        if (samples.length < rate * 0.1) throw new Error("조금 더 길게 누른 채 말해 주세요.");
        return encodeWav(resampleMono(samples, rate));
      },
      cancel: cleanup,
    };
  } catch (error) {
    // getUserMedia may settle after an abort; stop its eventual stream too.
    stream?.getTracks().forEach(track => track.stop());
    if (context && context.state !== "closed") context.close().catch(() => {});
    cleanup();
    throw error;
  }
}
