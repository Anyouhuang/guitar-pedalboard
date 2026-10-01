// Guitar Pedalboard - page logic. The build provides `DSP` (dsp.js) and WORKLET_SOURCE.
"use strict";

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, attrs = {}) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  return e;
};

// ============================================================================
// Parameters: same ids, ranges, skews and text as the plugin (PluginProcessor.cpp)
const fmt = {
  db: (v) => `${v.toFixed(1)} dB`,
  signedDb: (v) => `${v > 0 ? "+" : ""}${v.toFixed(1)} dB`,
  pct: (v) => `${Math.round(v)} %`,
  ms: (v) => `${Math.round(v)} ms`,
  hz: (v) => `${v.toFixed(2)} Hz`,
  knob: (v) => v.toFixed(1),
  q: (v) => v.toFixed(2),
  freq: (v) => (v >= 1000 ? `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz` : `${Math.round(v)} Hz`),
};

const PARAMS = {};
const float = (id, name, min, max, dflt, format, { centre = 0, step = 0 } = {}) => {
  const skew = centre ? Math.log(0.5) / Math.log((centre - min) / (max - min)) : 1;
  PARAMS[id] = { id, name, kind: "float", min, max, dflt, format, skew, step };
};
const bool = (id, name, dflt) => (PARAMS[id] = { id, name, kind: "bool", dflt });
const choice = (id, name, choices, dflt = 0) => (PARAMS[id] = { id, name, kind: "choice", choices, dflt });

float("inGain", "Input", -24, 24, 0, fmt.db, { step: 0.1 });
float("master", "Master", -40, 12, 0, fmt.db, { step: 0.1 });
bool("mute", "Mute", false);
bool("cab", "Cab Sim", true);
float("tempo", "Tempo", 30, 300, 120, (v) => `${v.toFixed(1)} BPM`, { step: 0.1 });
bool("eqOn", "EQ", true);
float("eqLcFreq", "Low cut", 20, 600, 20, fmt.freq, { centre: 100 });
float("eqBassFreq", "Bass freq", 40, 500, 100, fmt.freq, { centre: 120 });
float("eqBassGain", "Bass gain", -15, 15, 0, fmt.signedDb, { step: 0.1 });
float("eqLmFreq", "Low mid freq", 100, 2000, 400, fmt.freq, { centre: 450 });
float("eqLmGain", "Low mid gain", -15, 15, 0, fmt.signedDb, { step: 0.1 });
float("eqLmQ", "Low mid Q", 0.3, 6, 1, fmt.q, { centre: 1 });
float("eqHmFreq", "High mid freq", 500, 8000, 2000, fmt.freq, { centre: 2000 });
float("eqHmGain", "High mid gain", -15, 15, 0, fmt.signedDb, { step: 0.1 });
float("eqHmQ", "High mid Q", 0.3, 6, 1, fmt.q, { centre: 1 });
float("eqTrebFreq", "Treble freq", 1500, 15000, 5000, fmt.freq, { centre: 5000 });
float("eqTrebGain", "Treble gain", -15, 15, 0, fmt.signedDb, { step: 0.1 });
float("eqHcFreq", "High cut", 1000, 20000, 20000, fmt.freq, { centre: 6000 });
choice("nrHum", "Hum filter", DSP.HUM_MODES, 0);
float("nrAmount", "Denoise", 0, 100, 0, (v) => (v < 0.5 ? "off" : `${Math.round(v)} %`), { step: 1 });
bool("tunerOn", "Tuner", true);

// juce::NormalisableRange conversions
function toNorm(p, v) {
  const prop = clamp((v - p.min) / (p.max - p.min), 0, 1);
  return p.skew === 1 ? prop : Math.pow(prop, p.skew);
}
function fromNorm(p, n) {
  let prop = clamp(n, 0, 1);
  if (p.skew !== 1 && prop > 0) prop = Math.exp(Math.log(prop) / p.skew);
  let v = p.min + (p.max - p.min) * prop;
  if (p.step > 0) v = p.min + p.step * Math.round((v - p.min) / p.step);
  return clamp(v, p.min, p.max);
}

// ============================================================================
// Signal chain: eight slots (model, on/off, knobs in plain units) and where the amp/cab block sits.
// Each slot's knobs get PARAMS entries shaped by its current model (same knobs as the plugin, see DSP.MODELS).
const NUM_SLOTS = DSP.NUM_SLOTS, NUM_BLOCKS = NUM_SLOTS + 1;
const onId = (s) => `s${s}on`, modelId = (s) => `s${s}model`, knobId = (s, k) => `s${s}k${k}`;
let ampPosition = 2;
for (let s = 0; s < NUM_SLOTS; ++s) bool(onId(s), `Slot ${s + 1}`, false);

/** The time knob `k` of slot `s` is at when it follows the tempo, or null when it is set by hand. */
function syncedTime(s, k) {
  const info = DSP.modelInfo(values[modelId(s)]);
  const note = k === info.timeKnob ? info.noteKnob : k === info.timeKnob2 ? info.noteKnob2 : -1;
  if (note < 0 || Math.round(values[knobId(s, note)]) <= 0) return null;
  return DSP.resolveTempo(readSlot(s), values.tempo).knobs[k];
}
/** PARAMS entry for one knob of a model (a slot's, the amp's or the cab's). `text` may override the read-out. */
function knobParam(id, spec, text) {
  return spec.unit === DSP.UNIT.choice
    ? { id, name: spec.name, kind: "choice", choices: spec.choices, dflt: spec.def }
    : { id, name: spec.name, kind: "float", min: spec.min, max: spec.max, dflt: spec.def, skew: DSP.knobSkew(spec), step: spec.step,
        format: text || ((v) => DSP.knobText(spec, v)) };
}
function registerSlotParams(s) {
  const info = DSP.modelInfo(values[modelId(s)]);
  for (let k = 0; k < DSP.MAX_KNOBS; ++k) {
    const id = knobId(s, k), spec = info.knobs[k];
    if (!spec) { delete PARAMS[id]; continue; }
    // a time that follows the tempo shows that time
    PARAMS[id] = knobParam(id, spec, (v) => { const t = syncedTime(s, k); return t === null ? DSP.knobText(spec, v) : `♩ ${DSP.knobText(spec, t)}`; });
  }
}
const clampKnob = (spec, v) =>
  spec.unit === DSP.UNIT.choice ? clamp(Math.round(v), 0, spec.choices.length - 1) : clamp(v, spec.min, spec.max);

// The amp block: amp model and cabinet ("" = none), each with its own knobs
const ampKnobId = (k) => `ampk${k}`, cabKnobId = (k) => `cabk${k}`;
const ampInfo = () => DSP.AMPS[DSP.ampIndex(values.ampModel)] || null;
const cabInfo = () => DSP.CABS[DSP.cabIndex(values.cabModel)] || null;
function registerAmpParams() {
  const amp = ampInfo(), cab = cabInfo();
  for (let k = 0; k < DSP.MAX_AMP_KNOBS; ++k) { const spec = amp && amp.knobs[k]; if (spec) PARAMS[ampKnobId(k)] = knobParam(ampKnobId(k), spec); else delete PARAMS[ampKnobId(k)]; }
  for (let k = 0; k < DSP.MAX_CAB_KNOBS; ++k) { const spec = cab && cab.knobs[k]; if (spec) PARAMS[cabKnobId(k)] = knobParam(cabKnobId(k), spec); else delete PARAMS[cabKnobId(k)]; }
}
function readAmp() {
  const amp = ampInfo(), cab = cabInfo();
  const ampKnobs = new Array(DSP.MAX_AMP_KNOBS).fill(0), cabKnobs = new Array(DSP.MAX_CAB_KNOBS).fill(0);
  if (amp) amp.knobs.forEach((_, k) => (ampKnobs[k] = values[ampKnobId(k)]));
  if (cab) cab.knobs.forEach((_, k) => (cabKnobs[k] = values[cabKnobId(k)]));
  return { on: values.cab, amp: amp ? amp.key : "", cab: cab ? cab.key : "", ampKnobs, cabKnobs };
}
/** Sets the amp block from saved / default data; unknown keys become "none", missing knobs their defaults. */
function loadAmp(block) {
  values.ampModel = DSP.ampIndex(block.amp) >= 0 ? block.amp : "";
  values.cabModel = DSP.cabIndex(block.cab) >= 0 ? block.cab : "";
  registerAmpParams();
  const knob = (list, k, spec) => clampKnob(spec, Array.isArray(list) && Number.isFinite(list[k]) ? list[k] : spec.def);
  const amp = ampInfo(), cab = cabInfo();
  if (amp) amp.knobs.forEach((spec, k) => (values[ampKnobId(k)] = knob(block.ampKnobs, k, spec)));
  if (cab) cab.knobs.forEach((spec, k) => (values[cabKnobId(k)] = knob(block.cabKnobs, k, spec)));
}
function readSlot(s) {
  const key = values[modelId(s)], knobs = new Array(DSP.MAX_KNOBS).fill(0);
  DSP.modelInfo(key).knobs.forEach((_, k) => (knobs[k] = values[knobId(s, k)]));
  return { on: values[onId(s)], model: key, knobs };
}
function loadSlot(s, slot) {
  const known = DSP.MODELS.some((m) => m.key === slot.model);
  const info = DSP.modelInfo(known ? slot.model : "empty");
  values[modelId(s)] = info.key;
  values[onId(s)] = info.key !== "empty" && !!slot.on;
  registerSlotParams(s);
  info.knobs.forEach((spec, k) => {
    values[knobId(s, k)] = clampKnob(spec, Array.isArray(slot.knobs) && Number.isFinite(slot.knobs[k]) ? slot.knobs[k] : spec.def);
  });
}

// ============================================================================
// State (remembered per browser)
const STORE_KEY = "guitar-pedalboard.web.v2"; // v2: the slot chain
const values = {};
for (const p of Object.values(PARAMS)) values[p.id] = p.dflt;
DSP.defaultBoard().slots.forEach((slot, s) => loadSlot(s, slot));
loadAmp(DSP.defaultBoard().amp);
let source = "demo";
let playing = true; // demo riff / file transport (STOP / PLAY)
let chordPattern = DSP.CHORD_PATTERNS.arpeggio, chordTempo = 90, loopingChord = -1;
try {
  const saved = JSON.parse(localStorage.getItem(STORE_KEY) || "null");
  if (saved && saved.values) {
    for (const [id, v] of Object.entries(saved.values)) {
      const p = PARAMS[id];
      if (!p || /^(s\d|amp|cab)k/.test(id)) continue;
      if (p.kind === "float" && Number.isFinite(v)) values[id] = clamp(v, p.min, p.max);
      if (p.kind === "bool" && typeof v === "boolean") values[id] = v;
      if (p.kind === "choice" && Number.isInteger(v) && v >= 0 && v < p.choices.length) values[id] = v;
    }
    if (Array.isArray(saved.slots)) saved.slots.slice(0, NUM_SLOTS).forEach((slot, s) => slot && loadSlot(s, slot));
    if (Number.isInteger(saved.ampPosition)) ampPosition = clamp(saved.ampPosition, 0, NUM_SLOTS);
    if (saved.amp && typeof saved.amp === "object") loadAmp({ ...saved.amp, on: values.cab });
    if (saved.source === "demo" || saved.source === "live") source = saved.source;
    if (Number.isInteger(saved.chordPattern) && saved.chordPattern >= 0 && saved.chordPattern <= 2) chordPattern = saved.chordPattern;
    if (Number.isFinite(saved.chordTempo)) chordTempo = clamp(Math.round(saved.chordTempo), 40, 240);
  }
} catch { /* storage unavailable: defaults */ }

const listeners = {};
const onChange = (id, fn) => { (listeners[id] ||= []).push(fn); fn(values[id]); };
let saveTimer = 0;
function setValue(id, v) {
  const p = PARAMS[id];
  if (p.kind === "float") v = clamp(v, p.min, p.max);
  if (values[id] === v) return;
  values[id] = v;
  notify(id);
  sendParams();
  schedulePersist();
}
/** Re-runs the listeners of `id` (e.g. a synced time's text after the tempo changed). */
function notify(id) { for (const fn of listeners[id] || []) fn(values[id]); }
function schedulePersist() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(persist, 400);
}
function persist() {
  const globals = {};
  for (const [id, v] of Object.entries(values)) if (!/^(s\d|ampk|cabk|ampModel|cabModel)/.test(id)) globals[id] = v;
  const slots = Array.from({ length: NUM_SLOTS }, (_, s) => readSlot(s));
  try { localStorage.setItem(STORE_KEY, JSON.stringify({ values: globals, slots, amp: readAmp(), ampPosition, source, chordPattern, chordTempo })); } catch { /* ignore */ }
}

function fxParams() {
  const v = values;
  return {
    inputGainDb: v.inGain, outputGainDb: v.master, mute: v.mute, amp: readAmp(), ampPosition,
    eqOn: v.eqOn,
    eq: { lowCutHz: v.eqLcFreq, bassHz: v.eqBassFreq, bassDb: v.eqBassGain, lowMidHz: v.eqLmFreq, lowMidDb: v.eqLmGain,
          lowMidQ: v.eqLmQ, highMidHz: v.eqHmFreq, highMidDb: v.eqHmGain, highMidQ: v.eqHmQ,
          trebleHz: v.eqTrebFreq, trebleDb: v.eqTrebGain, highCutHz: v.eqHcFreq },
    humMode: v.nrHum, denoise: v.nrAmount,
    slots: Array.from({ length: NUM_SLOTS }, (_, s) => DSP.resolveTempo(readSlot(s), v.tempo)),
  };
}

// ============================================================================
// Audio engine
const engine = { ctx: null, node: null, kind: "", mic: null, starting: null, wantOn: false, silentAudio: null,
                 streamOut: null, outputElement: null };
const tunerDetector = new DSP.PitchDetector();
const analyzer = new DSP.SpectrumAnalyzer();
tunerDetector.prepare(48000);
const levels = { in: 0, out: 0, progress: 0 };
let fileData = null; // { samples, rate, name }
const isIOS = /iP(hone|ad|od)/.test(navigator.userAgent) || (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
/** The copy on its own HTTPS address, where the microphone / audio interface can be used. */
const SELF_HOSTED_URL = "https://anyouhuang.github.io/guitar-pedalboard/";
// On iPhone/iPad, Web Audio obeys the ring/silent switch, and a page embedded in another page (like the
// claude.ai viewer) can't switch its audio session to "playback". There, the sound is sent through an
// <audio> element instead, which plays like a video does. On a page of its own, the output stays direct.
const useMediaElementOutput = isIOS && window.top !== window.self
  || (location.hostname === "localhost" && location.search.includes("mediaout")); // testing only

function post(msg, transfer) { if (engine.node) engine.node.port.postMessage(msg, transfer || []); }
let paramsQueued = false;
function sendParams() {
  if (paramsQueued || !engine.node) return;
  paramsQueued = true;
  queueMicrotask(() => { paramsQueued = false; post({ type: "params", params: fxParams() }); });
}

/** Tells the audio thread what the page is showing: with the tuner and the analyser off it sends no audio back at all. */
function sendMonitor() { post({ type: "monitor", tuner: values.tunerOn, analyzer: showAnalyzer }); }

/** A tiny silent WAV, looped by an <audio> element on older iPhones (see unlockAudio). */
function silentWavUrl() {
  const n = 800, bytes = new Uint8Array(44 + n), v = new DataView(bytes.buffer);
  const text = (o, t) => [...t].forEach((c, i) => (bytes[o + i] = c.charCodeAt(0)));
  text(0, "RIFF"); v.setUint32(4, 36 + n, true); text(8, "WAVEfmt "); v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); v.setUint16(22, 1, true); v.setUint32(24, 8000, true); v.setUint32(28, 8000, true);
  v.setUint16(32, 1, true); v.setUint16(34, 8, true); text(36, "data"); v.setUint32(40, n, true);
  bytes.fill(128, 44);
  return URL.createObjectURL(new Blob([bytes], { type: "audio/wav" }));
}

/** Sends the board's output through an <audio> element (see useMediaElementOutput). */
function startMediaElementOutput(ctx) {
  if (!engine.streamOut) {
    engine.streamOut = ctx.createMediaStreamDestination();
    const a = new Audio();
    a.setAttribute("playsinline", "");
    a.srcObject = engine.streamOut.stream;
    engine.outputElement = a;
  }
  engine.outputElement.play().catch((err) => setStatus("iPhone 還沒允許播放，請再按一次 POWER（" + err.name + "）"));
}

/** Must run synchronously inside the tap/click, or iOS keeps the sound off. */
function unlockAudio(ctx) {
  if (useMediaElementOutput) startMediaElementOutput(ctx);
  // iPhone's ring/silent switch mutes web audio unless the page asks for "playback" (Safari 16.4+)
  try { if (navigator.audioSession) navigator.audioSession.type = "playback"; } catch { /* not supported */ }
  ctx.resume().catch(() => {});
  try {
    const src = ctx.createBufferSource();
    src.buffer = ctx.createBuffer(1, 1, ctx.sampleRate);
    src.connect(ctx.destination);
    src.start(0);
  } catch { /* ignore */ }
  // older iPhones: a looping silent <audio> moves the page to the playback audio session as well
  if (isIOS && !navigator.audioSession && !engine.silentAudio) {
    try {
      const a = new Audio(silentWavUrl());
      a.loop = true;
      a.setAttribute("playsinline", "");
      a.play().catch(() => {});
      engine.silentAudio = a;
    } catch { /* ignore */ }
  }
}

async function loadWorklet(ctx) {
  if (typeof WORKLET_SOURCE === "string") {
    const url = URL.createObjectURL(new Blob([WORKLET_SOURCE], { type: "text/javascript" }));
    try { await ctx.audioWorklet.addModule(url); return; } catch { /* blob blocked: try the file next to the page */ }
  }
  await ctx.audioWorklet.addModule("worklet.js");
}

/** Fallback when AudioWorklet is missing or can't be loaded: the same DSP on the page's thread
    (ScriptProcessorNode). Works everywhere, with a little more latency. */
function createScriptProcessorEngine(ctx, init) {
  const block = 1024;
  const node = ctx.createScriptProcessor(block, 1, 2);
  const chain = new DSP.FxChain();
  chain.prepare(ctx.sampleRate, block);
  let params = init.params;
  chain.setParameters(params);
  chain.reset();
  const player = new DSP.TestSignalPlayer();
  player.prepare(ctx.sampleRate);
  player.setSource(init.source);
  player.setChordPattern(init.chordPattern);
  player.setChordTempo(init.chordTempo);
  const mono = new Float32Array(block), postEq = new Float32Array(block);
  let postLength = 0, wantTuner = init.tuner !== false, wantAnalyzer = init.analyzer !== false;
  const tap = (data, n) => { postEq.set(data.subarray(0, n), postLength); postLength += n; };
  chain.analyzerTap = wantAnalyzer ? tap : null;
  node.onaudioprocess = (e) => {
    const input = e.inputBuffer.getChannelData(0), left = e.outputBuffer.getChannelData(0), right = e.outputBuffer.getChannelData(1);
    const n = left.length;
    if (player.source === "live") mono.set(input.subarray(0, n)); else mono.fill(0);
    player.process(mono, n);
    postLength = 0;
    chain.setParameters(params);
    chain.process(mono.subarray(0, n), left, right, n);
    onAudioData({ tuner: wantTuner ? mono.slice(0, n) : null, analyzer: wantAnalyzer ? postEq.slice(0, n) : null,
                  inPeak: chain.inputPeak, outPeak: chain.outputPeak, progress: player.progress, chord: player.loopingChord() });
  };
  node.port = {
    postMessage(m) {
      if (m.type === "params") params = m.params;
      else if (m.type === "source") player.setSource(m.source);
      else if (m.type === "pluck") player.pluck(m.index);
      else if (m.type === "strum") player.strum(m.index);
      else if (m.type === "chord") player.playChord(m.index);
      else if (m.type === "chordStop") player.stopChord();
      else if (m.type === "chordSettings") { player.setChordPattern(m.pattern); player.setChordTempo(m.bpm); }
      else if (m.type === "file") player.setFile(m.samples, m.rate);
      else if (m.type === "playing") player.setPlaying(m.playing);
      else if (m.type === "monitor") { wantTuner = m.tuner; wantAnalyzer = m.analyzer; chain.analyzerTap = wantAnalyzer ? tap : null; }
    },
  };
  return node;
}

/** Call directly from a tap/click handler (the first lines must run inside the gesture). */
async function startAudio() {
  engine.wantOn = true;
  if (engine.ctx) {
    unlockAudio(engine.ctx);
    await engine.ctx.resume().catch(() => {});
    setPower(engine.ctx.state === "running");
    return engine.ctx.state === "running";
  }
  if (engine.starting) return engine.starting;

  const Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) {
    setStatus("這個瀏覽器不支援 Web Audio，請改用新版 Safari、Chrome、Edge 或 Firefox。");
    return false;
  }
  const ctx = new Ctx({ latencyHint: "interactive" });
  unlockAudio(ctx);

  engine.starting = (async () => {
    // The processor starts with the board's real settings, so even its first few milliseconds
    // respect MUTE, the pedal switches and the chosen source.
    const startSource = source === "file" && !fileData ? "demo" : source;
    const init = { params: fxParams(), source: startSource, playing, chordPattern, chordTempo, tuner: values.tunerOn, analyzer: showAnalyzer };
    let node = null;
    const forceFallback = location.hostname === "localhost" && location.search.includes("fallback"); // testing only
    if (ctx.audioWorklet && !forceFallback) {
      try {
        await loadWorklet(ctx);
        node = new AudioWorkletNode(ctx, "pedalboard", {
          numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], processorOptions: init,
        });
        node.port.onmessage = (e) => onAudioData(e.data);
        engine.kind = "AudioWorklet";
      } catch (err) {
        console.warn("AudioWorklet unavailable, using ScriptProcessor fallback:", err);
        node = null;
      }
    }
    if (!node) {
      node = createScriptProcessorEngine(ctx, init);
      engine.kind = "相容模式";
    }
    node.connect(useMediaElementOutput ? engine.streamOut : ctx.destination);
    engine.ctx = ctx;
    engine.node = node;
    ctx.onstatechange = () => {
      const running = ctx.state === "running";
      setPower(running);
      if (!running && engine.wantOn) setStatus("音訊被系統暫停（例如來電或切換 App），請再按一次 POWER。");
    };
    tunerDetector.prepare(ctx.sampleRate);
    analyzer.setSampleRate(ctx.sampleRate);
    if (fileData) post({ type: "file", samples: fileData.samples.slice(), rate: fileData.rate });
    await applySource(startSource);
    await ctx.resume().catch(() => {});
    setPower(ctx.state === "running");
    if (ctx.state !== "running") setStatus("瀏覽器還沒允許播放聲音，請再按一次 POWER。");
    return ctx.state === "running";
  })();
  const ok = await engine.starting;
  engine.starting = null;
  return ok;
}

async function stopAudio() {
  engine.wantOn = false;
  if (engine.outputElement) engine.outputElement.pause();
  if (engine.ctx) await engine.ctx.suspend();
  setPower(false);
}

function onAudioData(m) {
  if (m.tuner && values.tunerOn) tunerDetector.pushSamples(m.tuner);
  if (m.analyzer) analyzer.push(m.analyzer);
  levels.in = Math.max(levels.in, m.inPeak);
  levels.out = Math.max(levels.out, m.outPeak);
  levels.progress = m.progress;
  if (m.chord !== undefined && m.chord !== loopingChord) { loopingChord = m.chord; renderChordButtons(); renderSource(); }
}

async function enableMic() {
  if (engine.mic) return true;
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    setStatus("這個頁面無法使用音訊輸入（瀏覽器封鎖或非 HTTPS）。可以用 DEMO RIFF 或載入音檔測試。");
    return false;
  }
  // iPhone: recording needs the "play-and-record" audio session (plain "playback" can't capture)
  try { if (navigator.audioSession) navigator.audioSession.type = "play-and-record"; } catch { /* not supported */ }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      // no voice processing, and ask for the smallest input buffer the device allows
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1, latency: 0 },
    });
    const src = engine.ctx.createMediaStreamSource(stream);
    src.connect(engine.node);
    engine.mic = { stream, src, track: stream.getAudioTracks()[0] };
    return true;
  } catch (err) {
    const denied = err && (err.name === "NotAllowedError" || err.name === "SecurityError");
    let message;
    if (!denied) message = "找不到音訊輸入裝置。請接上錄音介面後再試，或改用 DEMO RIFF。";
    else if (window.top !== window.self) message = `這個頁面被嵌在其他網站裡（例如 claude.ai），不能使用麥克風。要接吉他請開 ${SELF_HOSTED_URL}`;
    else if (isIOS) message = "麥克風權限被拒絕。請點網址列左邊的「大小」圖示 →「網站設定」→「麥克風」改成「允許」，重新整理後再選 LIVE INPUT。";
    else message = "麥克風權限被拒絕。請點網址列左邊的鎖頭／設定圖示，把「麥克風」改成「允許」，重新整理後再選 LIVE INPUT。";
    setStatus(message, 20000);
    return false;
  }
}

async function applySource(next) {
  if (next === "live" && !(await enableMic())) next = source === "live" ? "demo" : source;
  if (next === "file" && !fileData) next = "demo";
  source = next;
  if (source !== "live") playing = true; // choosing the riff or a file starts it
  post({ type: "source", source });
  persist();
  renderSource();
}

// ============================================================================
// Widgets
const START_ANGLE = -144, END_ANGLE = 144; // JUCE's default rotary range, degrees from 12 o'clock
const polar = (r, deg) => [50 + r * Math.sin((deg * Math.PI) / 180), 50 - r * Math.cos((deg * Math.PI) / 180)];
const arcPath = (r, a0, a1) => {
  const [x0, y0] = polar(r, a0), [x1, y1] = polar(r, a1);
  return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
};
let svgIds = 0;

/** Rotary knob bound to a float parameter (rebindable, for the EQ band knobs). */
function makeKnob(label, paramId) {
  const root = el("div", "knob");
  const lab = el("label");
  const dial = el("div", "dial", { role: "slider", tabindex: "0" });
  const out = el("output");
  const id = `k${++svgIds}`;
  lab.id = `${id}-label`;
  dial.setAttribute("aria-labelledby", lab.id);
  lab.textContent = label;
  dial.innerHTML = `<svg viewBox="0 0 100 100" aria-hidden="true">
    <defs>
      <linearGradient id="${id}-body" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3c3c42"/><stop offset="1" stop-color="#0c0c0e"/></linearGradient>
      <linearGradient id="${id}-cap" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#4a4a52"/><stop offset="1" stop-color="#1a1a1e"/></linearGradient>
    </defs>
    <path d="${arcPath(45, START_ANGLE, END_ANGLE)}" fill="none" stroke="rgba(0,0,0,.35)" stroke-width="6" stroke-linecap="round"/>
    <path class="value" fill="none" stroke="var(--arc, #ffb020)" stroke-width="6" stroke-linecap="round"/>
    <circle cx="50" cy="53" r="35" fill="rgba(0,0,0,.45)"/>
    <circle cx="50" cy="50" r="34" fill="url(#${id}-body)"/>
    <circle cx="50" cy="50" r="25" fill="url(#${id}-cap)" stroke="rgba(255,255,255,.12)"/>
    <line class="pointer" x1="50" y1="40" x2="50" y2="18" stroke="#fff" stroke-width="4.5" stroke-linecap="round"/>
  </svg>`;
  root.append(lab, dial, out);
  const valuePath = dial.querySelector(".value"), pointer = dial.querySelector(".pointer");
  let param = null, unsubscribeToken = 0;

  const render = (v) => {
    const n = toNorm(param, v), angle = START_ANGLE + n * (END_ANGLE - START_ANGLE);
    valuePath.setAttribute("d", n > 0.001 ? arcPath(45, START_ANGLE, angle) : "");
    pointer.setAttribute("transform", `rotate(${angle.toFixed(1)} 50 50)`);
    out.textContent = param.format(v);
    dial.setAttribute("aria-valuenow", v.toFixed(2));
    dial.setAttribute("aria-valuetext", param.format(v));
  };
  const bind = (pid) => {
    param = PARAMS[pid];
    const token = ++unsubscribeToken;
    dial.setAttribute("aria-valuemin", param.min);
    dial.setAttribute("aria-valuemax", param.max);
    onChange(pid, (v) => { if (token === unsubscribeToken) render(v); });
  };
  bind(paramId);

  const setNorm = (n) => setValue(param.id, fromNorm(param, n));
  let drag = null;
  dial.addEventListener("pointerdown", (e) => {
    dial.focus({ preventScroll: true });
    dial.setPointerCapture(e.pointerId);
    drag = { y: e.clientY, start: toNorm(param, values[param.id]) };
    e.preventDefault();
  });
  dial.addEventListener("pointermove", (e) => {
    if (!drag) return;
    const range = e.shiftKey ? 800 : 180;
    setNorm(drag.start + (drag.y - e.clientY) / range);
  });
  const end = () => { drag = null; };
  dial.addEventListener("pointerup", end);
  dial.addEventListener("pointercancel", end);
  dial.addEventListener("dblclick", () => setValue(param.id, param.dflt));
  // the wheel turns a knob only once it has been clicked (focused), so scrolling the page never does
  dial.addEventListener("wheel", (e) => {
    if (document.activeElement !== dial) return;
    e.preventDefault();
    setNorm(toNorm(param, values[param.id]) - Math.sign(e.deltaY) * 0.02);
  }, { passive: false });
  dial.addEventListener("keydown", (e) => {
    const n = toNorm(param, values[param.id]);
    const steps = { ArrowUp: 0.01, ArrowRight: 0.01, ArrowDown: -0.01, ArrowLeft: -0.01, PageUp: 0.1, PageDown: -0.1 };
    if (e.key in steps) setNorm(n + steps[e.key]);
    else if (e.key === "Home") setNorm(0);
    else if (e.key === "End") setNorm(1);
    else return;
    e.preventDefault();
  });
  return { root, bind, setLabel: (t) => { if (lab.textContent !== t) lab.textContent = t; } };
}

function makeMeter() {
  const root = el("div", "meter", { "aria-hidden": "true" });
  root.innerHTML = '<div class="fill"></div><div class="hold"></div><div class="clip"></div>';
  const fill = root.querySelector(".fill"), hold = root.querySelector(".hold"), clip = root.querySelector(".clip");
  const state = { level: 0, hold: 0, holdFrames: 0, clipFrames: 0 };
  const toPct = (g) => clamp((DSP.gainToDb(Math.max(g, 1e-6)) + 60) / 60, 0, 1) * 100;
  return {
    root,
    update(peak) {
      state.level = Math.max(peak, state.level * 0.82);
      if (peak >= state.hold) { state.hold = peak; state.holdFrames = 45; } else if (--state.holdFrames < 0) state.hold *= 0.9;
      state.clipFrames = peak >= 1 ? 60 : Math.max(0, state.clipFrames - 1);
      fill.style.height = `${toPct(state.level)}%`;
      hold.style.bottom = `${toPct(state.hold)}%`;
      hold.style.opacity = state.hold > 0.001 ? 1 : 0;
      clip.classList.toggle("on", state.clipFrames > 0);
    },
  };
}

function bindToggle(button, id) {
  button.addEventListener("click", () => setValue(id, !values[id]));
  onChange(id, (v) => button.setAttribute("aria-pressed", String(v)));
}

// ============================================================================
// Header
const inMeter = makeMeter(), outMeter = makeMeter();
$("#io-in").append(makeKnob("Input", "inGain").root, inMeter.root);
$("#io-out").append(makeKnob("Master", "master").root, outMeter.root);
bindToggle($("#cab"), "cab");
bindToggle($("#mute"), "mute");

const powerButton = $("#power");
function setPower(on) { powerButton.setAttribute("aria-pressed", String(on)); renderSource(); }
powerButton.addEventListener("click", () => {
  if (powerButton.getAttribute("aria-pressed") === "true") stopAudio(); else startAudio();
});

// ============================================================================
// Signal chain, HD500X style: IN > eight FX slots with the amp/cab block among them > OUT.
// Click a block to edit it below, drag it (mouse) or use MOVE to reorder, double-click to switch it on / off.
const CATEGORY_COLOURS = ["var(--empty)", "var(--gate)", "var(--drive)", "var(--mod)", "var(--delay)", "var(--reverb)",
                          "var(--filter)", "var(--pitch)", "var(--eq)", "var(--wah)", "var(--volume)", "var(--amp)", "var(--amp)"];
const chainRow = $("#chain-row");
let selectedPos = 1;

function blockAt(pos) {
  return pos === ampPosition ? { amp: true, slot: -1 } : { amp: false, slot: pos < ampPosition ? pos : pos - 1 };
}
function blockInfo(b) {
  const key = b.amp ? "" : values[modelId(b.slot)];
  const info = b.amp ? null : DSP.modelInfo(key);
  const empty = !b.amp && key === "empty";
  const on = b.amp ? values.cab : !empty && values[onId(b.slot)];
  const colour = b.amp ? "var(--amp)" : CATEGORY_COLOURS[info.category];
  return { key, info, empty, on, colour };
}

/** The amp block's tile: the amp's name (or the cab's when there is no amp), and the cab's on a small second line. */
function ampTileText() {
  const amp = ampInfo(), cab = cabInfo();
  if (!amp) return [cab ? cab.name : "No Amp", ""];
  return [amp.name, cab ? cab.name : "No Cab"];
}

function renderChain() {
  chainRow.replaceChildren(...Array.from({ length: NUM_BLOCKS }, (_, pos) => {
    const b = blockAt(pos), { info, empty, on, colour } = blockInfo(b);
    const btn = el("button", "block", { type: "button", "data-pos": String(pos), "aria-pressed": String(pos === selectedPos) });
    btn.classList.toggle("empty", empty);
    btn.classList.toggle("off", !on && !empty);
    btn.style.setProperty("--cat", colour);
    const cat = el("span", "cat"), name = el("span", "name"), foot = el("span", "foot"), num = el("span", "num");
    const [title, sub] = b.amp ? ampTileText() : [empty ? "+" : info.name, ""];
    cat.textContent = b.amp ? "Amp" : empty ? "Empty" : DSP.CATEGORY_NAMES[info.category];
    name.textContent = title;
    num.textContent = b.amp ? sub : String(b.slot + 1);
    num.classList.toggle("sub", b.amp);
    foot.append(el("i", on ? "led on" : "led"), num);
    btn.append(cat, name, foot);
    btn.title = empty ? "空的格子：點一下選效果" : "點一下編輯，雙擊開關，拖曳換位置";
    btn.setAttribute("aria-label", `${b.amp ? "Amp / cab" : `Slot ${b.slot + 1}`}: ${title}${sub ? `, ${sub}` : ""}${empty ? "" : on ? ", on" : ", off"}`);
    const li = el("li");
    li.append(btn);
    return li;
  }));
}

function chainChanged() {
  sendParams();
  schedulePersist();
  renderChain();
  renderEditor();
}

function selectBlock(pos) {
  selectedPos = clamp(pos, 0, NUM_BLOCKS - 1);
  // the tiles stay the same elements (only the highlight moves), so a click's second half still lands on them
  for (const btn of chainRow.querySelectorAll(".block")) btn.setAttribute("aria-pressed", String(Number(btn.dataset.pos) === selectedPos));
  renderEditor();
}

/** Moves the block at chain position `from` to `to`, shifting the others (slot contents move with it). */
function moveBlock(from, to) {
  if (from === to) return;
  const order = Array.from({ length: NUM_BLOCKS }, (_, i) => blockAt(i));
  const saved = Array.from({ length: NUM_SLOTS }, (_, s) => readSlot(s));
  const [moved] = order.splice(from, 1);
  order.splice(to, 0, moved);
  let s = 0;
  order.forEach((b, i) => { if (b.amp) ampPosition = i; else loadSlot(s++, saved[b.slot]); });
  selectedPos = to;
  chainChanged();
}

function setSlotModel(s, key) {
  const wasEmpty = values[modelId(s)] === "empty";
  const info = DSP.modelInfo(key);
  const knobs = info.knobs.map((k) => k.def);
  loadSlot(s, { model: info.key, on: key === "empty" ? false : wasEmpty ? true : values[onId(s)], knobs });
  chainChanged();
}

/** Picks the amp (knobs to their defaults; like on the HD500X it brings the cab it is usually played through) or the cab. */
function setAmpModel(key) {
  const current = readAmp();
  const amp = DSP.makeAmp(key, current.cab, current.on);
  const usual = DSP.defaultCabFor(key);
  if (key && usual && DSP.cabIndex(usual) >= 0) { loadAmp({ ...DSP.makeAmp(key, usual, current.on) }); }
  else loadAmp({ ...current, amp: amp.amp, ampKnobs: amp.ampKnobs });
  chainChanged();
}
function setCabModel(key) {
  const current = readAmp();
  loadAmp({ ...current, cab: key, cabKnobs: DSP.makeAmp("", key).cabKnobs });
  chainChanged();
}

function toggleBlock(pos) {
  const b = blockAt(pos);
  if (b.amp) { setValue("cab", !values.cab); return; }
  if (values[modelId(b.slot)] === "empty") return;
  setValue(onId(b.slot), !values[onId(b.slot)]);
}

// click / double-click / drag (mouse and pen; on touch screens the MOVE buttons reorder, so the page still scrolls)
let chainDrag = null, suppressClick = false, lastPointerType = "mouse", lastTap = { pos: -1, time: 0 };
// Double-click is detected here rather than with "dblclick": switching a pedal re-renders the tiles, and a
// "dblclick" aimed at a tile that was replaced in between never arrives. A mouse click carries the system's
// click count (so its own double-click speed applies); taps on a touch screen are timed here.
chainRow.addEventListener("click", (e) => {
  const btn = e.target.closest(".block");
  if (!btn || suppressClick) return;
  const pos = Number(btn.dataset.pos), now = performance.now();
  const mouse = (e.pointerType || lastPointerType) === "mouse";
  const double = e.detail === 2 || (!mouse && e.detail > 0 && lastTap.pos === pos && now - lastTap.time < 400);
  lastTap = double ? { pos: -1, time: 0 } : { pos, time: now };
  if (pos !== selectedPos) selectBlock(pos);
  if (double) toggleBlock(pos);
});
chainRow.addEventListener("pointerdown", (e) => {
  lastPointerType = e.pointerType;
  const btn = e.target.closest(".block");
  if (!btn || e.pointerType === "touch" || e.button !== 0) return;
  chainDrag = { from: Number(btn.dataset.pos), x: e.clientX, y: e.clientY, active: false, btn, target: -1, id: e.pointerId };
});
window.addEventListener("pointermove", (e) => {
  const d = chainDrag;
  if (!d || e.pointerId !== d.id) return;
  const dx = e.clientX - d.x, dy = e.clientY - d.y;
  if (!d.active && Math.hypot(dx, dy) > 6) { d.active = true; d.btn.classList.add("dragging"); }
  if (!d.active) return;
  d.btn.style.transform = `translate(${dx}px, ${dy}px)`;
  let best = d.from, bestDistance = Infinity;
  for (const b of chainRow.querySelectorAll(".block")) {
    if (b === d.btn) continue;
    const r = b.getBoundingClientRect(), distance = Math.hypot(e.clientX - (r.left + r.width / 2), e.clientY - (r.top + r.height / 2));
    if (distance < bestDistance) { bestDistance = distance; best = Number(b.dataset.pos); }
  }
  d.target = best;
  for (const b of chainRow.querySelectorAll(".block")) {
    const p = Number(b.dataset.pos);
    b.classList.toggle("drop-before", p === best && best < d.from);
    b.classList.toggle("drop-after", p === best && best > d.from);
  }
});
const endChainDrag = (e) => {
  const d = chainDrag;
  if (!d || e.pointerId !== d.id) return;
  chainDrag = null;
  if (!d.active) return;
  suppressClick = true;
  setTimeout(() => { suppressClick = false; }, 0);
  if (e.type === "pointerup" && d.target >= 0 && d.target !== d.from) moveBlock(d.from, d.target);
  else renderChain();
};
window.addEventListener("pointerup", endChainDrag);
window.addEventListener("pointercancel", endChainDrag);

// ---------- global tempo (TAP), like the HD500X's TAP switch ----------
const tapButton = $("#tap"), tempoInput = $("#tempo"), tapTempo = new DSP.TapTempo();
const tapLabel = tapButton.querySelector("span");
onChange("tempo", (v) => {
  if (document.activeElement !== tempoInput) tempoInput.value = String(Math.round(v * 10) / 10);
  for (let s = 0; s < NUM_SLOTS; ++s) {
    const info = DSP.modelInfo(values[modelId(s)]);
    for (const k of [info.timeKnob, info.timeKnob2]) if (k >= 0) notify(knobId(s, k));
  }
});
tempoInput.addEventListener("input", () => {
  const bpm = Number(tempoInput.value);
  if (Number.isFinite(bpm) && bpm >= 30 && bpm <= 300) setValue("tempo", bpm);
});
tempoInput.addEventListener("change", () => { tempoInput.value = String(Math.round(values.tempo * 10) / 10); });
tapButton.addEventListener("pointerdown", (e) => {
  e.preventDefault();
  const beat = tapTempo.tap(performance.now());
  if (beat > 0) setValue("tempo", clamp(Math.round((60000 / beat) * 10) / 10, 30, 300));
});
tapButton.addEventListener("keydown", (e) => {
  if (e.key === " " || e.key === "Enter") { e.preventDefault(); tapButton.dispatchEvent(new PointerEvent("pointerdown")); }
});
function tickTempo() {
  const beat = 60000 / values.tempo, since = performance.now() - tapTempo.getLastTapMs();
  tapButton.classList.toggle("beat", since % beat < 90);
  const text = `TAP ${Math.round(values.tempo)}`;
  if (tapLabel.textContent !== text) tapLabel.textContent = text;
}

// ---------- the selected block's editor ----------
const editor = $("#editor"), edTitle = $("#ed-title"), edSlot = $("#ed-slot"), edCategory = $("#ed-category"), edModel = $("#ed-model");
const edBased = $("#ed-based"), edKnobs = $("#ed-knobs"), edLed = $("#ed-led"), edStomp = $("#ed-stomp");
const edLeft = $("#ed-left"), edRight = $("#ed-right"), edClear = $("#ed-clear");
let editorIds = [];

function renderEditorSwitch() {
  const b = blockAt(selectedPos), { empty, on } = blockInfo(b);
  edStomp.hidden = edLed.hidden = empty;
  edLed.classList.toggle("on", on);
  edStomp.setAttribute("aria-pressed", String(on));
}

/** One control of the editor: a rotary knob, or a menu for switches and note values. */
function makeControl(id, spec, extraClass = "") {
  editorIds.push(id);
  if (spec.unit !== DSP.UNIT.choice) {
    const knob = makeKnob(spec.name, id);
    if (extraClass) knob.root.classList.add(extraClass);
    return { root: knob.root, select: null };
  }
  const root = el("div", `knob choice ${extraClass}`.trim()), lab = el("label"), select = el("select", "", { "aria-label": spec.name });
  lab.textContent = spec.name;
  spec.choices.forEach((c, i) => select.append(new Option(c, String(i))));
  select.addEventListener("change", () => setValue(id, Number(select.value)));
  onChange(id, (v) => { select.value = String(v); });
  root.append(lab, select);
  return { root, select };
}

function renderEditor() {
  const b = blockAt(selectedPos), { key, info, empty, colour } = blockInfo(b);
  editor.style.setProperty("--body", colour);
  editor.classList.toggle("empty", empty);
  edTitle.textContent = b.amp ? "Amp / Cab" : empty ? "Empty slot" : info.name;
  edSlot.textContent = b.amp ? "Amp block" : `Slot ${b.slot + 1}`;
  edModel.hidden = !b.amp && empty;
  edClear.hidden = b.amp || empty;

  // the two menus: effect type + model, or (amp block) amp + cabinet
  const amp = ampInfo(), cab = cabInfo();
  if (b.amp) {
    edCategory.replaceChildren(new Option("No Amp", ""), ...DSP.AMPS.map((m) => new Option(m.name, m.key)));
    edCategory.value = amp ? amp.key : "";
    edCategory.setAttribute("aria-label", "Amp model");
    edModel.replaceChildren(new Option("No Cab", ""), ...DSP.CABS.map((m) => new Option(m.name, m.key)));
    edModel.value = cab ? cab.key : "";
    edModel.setAttribute("aria-label", "Speaker cabinet");
  } else {
    edCategory.replaceChildren(...DSP.CATEGORY_ORDER.map((c) => new Option(DSP.CATEGORY_NAMES[c], String(c)))); // the HD500X's order
    edCategory.value = String(info.category);
    edCategory.setAttribute("aria-label", "Effect type");
    edModel.replaceChildren(...DSP.MODELS.filter((m) => m.category === info.category && m.key !== "empty").map((m) => new Option(m.name, m.key)));
    edModel.value = key;
    edModel.setAttribute("aria-label", "Model");
  }
  edBased.textContent = b.amp
    ? [amp && `Amp: ${amp.basedOn}`, cab && `Cab: ${cab.basedOn}`].filter(Boolean).join(" · ")
      || "沒有選音箱和箱體：訊號直接通過。用耳機或監聽喇叭時建議至少選一個箱體（Cab）。"
    : empty ? "空的格子：訊號直接通過。上面選一個效果類型。" : `Based on: ${info.basedOn}`;
  edLeft.disabled = selectedPos === 0;
  edRight.disabled = selectedPos === NUM_BLOCKS - 1;
  renderEditorSwitch();

  // controls, relabelled for this model; switches and note values are menus
  for (const id of editorIds) delete listeners[id];
  editorIds = [];
  edKnobs.replaceChildren();

  if (b.amp) {
    if (amp) amp.knobs.forEach((spec, k) => edKnobs.append(makeControl(ampKnobId(k), spec).root));
    if (cab) cab.knobs.forEach((spec, k) => edKnobs.append(makeControl(cabKnobId(k), spec, "cab").root));
  } else if (!empty) {
    const s = b.slot, roots = [];
    info.knobs.forEach((spec, k) => { const c = makeControl(knobId(s, k), spec); roots.push(c); edKnobs.append(c.root); });
    // a time knob can't be turned while it follows the tempo
    for (const [timeKnob, noteKnob] of [[info.timeKnob, info.noteKnob], [info.timeKnob2, info.noteKnob2]]) {
      if (timeKnob < 0 || noteKnob < 0) continue;
      roots[noteKnob].select.title = "ms = 用旋鈕設定時間；選音符值 = 跟著 TAP 的速度";
      onChange(knobId(s, noteKnob), () => {
        roots[timeKnob].root.classList.toggle("synced", syncedTime(s, timeKnob) !== null);
        notify(knobId(s, timeKnob));
      });
    }
  }
  if (!edKnobs.children.length) {
    const hint = el("p", "ed-hint");
    hint.textContent = b.amp ? "左邊選音箱（Amp）和箱體（Cab）" : "選好效果類型和型號後，旋鈕會出現在這裡";
    edKnobs.append(hint);
  }
}

edCategory.addEventListener("change", () => {
  const b = blockAt(selectedPos);
  if (b.amp) { setAmpModel(edCategory.value); return; }
  const category = Number(edCategory.value);
  const first = DSP.MODELS.find((m) => m.category === category);
  if (DSP.modelInfo(values[modelId(b.slot)]).category !== category) setSlotModel(b.slot, first ? first.key : "empty");
});
edModel.addEventListener("change", () => {
  const b = blockAt(selectedPos);
  if (b.amp) setCabModel(edModel.value);
  else if (edModel.value !== values[modelId(b.slot)]) setSlotModel(b.slot, edModel.value);
});
edStomp.addEventListener("click", () => toggleBlock(selectedPos));
edLeft.addEventListener("click", () => moveBlock(selectedPos, selectedPos - 1));
edRight.addEventListener("click", () => moveBlock(selectedPos, selectedPos + 1));
edClear.addEventListener("click", () => { const b = blockAt(selectedPos); if (!b.amp) setSlotModel(b.slot, "empty"); });

onChange("cab", () => { renderChain(); renderEditorSwitch(); });
for (let s = 0; s < NUM_SLOTS; ++s) onChange(onId(s), () => { renderChain(); renderEditorSwitch(); });
renderChain();
renderEditor();

// ============================================================================
// Input source strip
const statusText = $("#status-text"), progressBar = $("#progress-bar");
let statusOverride = "", statusTimer = 0;
function setStatus(text, ms = 9000) {
  statusOverride = text;
  clearTimeout(statusTimer);
  statusTimer = setTimeout(() => { statusOverride = ""; renderSource(); }, ms);
  renderSource();
}
function setPlaying(on) {
  playing = on;
  post({ type: "playing", playing: on });
  renderSource();
}

const transport = $("#transport");
transport.addEventListener("click", async () => {
  if (playing && engine.ctx && engine.ctx.state === "running") { setPlaying(false); return; }
  if (!(await startAudio())) return;
  setPlaying(true);
});

function renderSource() {
  for (const chip of document.querySelectorAll(".source .chip")) chip.setAttribute("aria-checked", String(chip.dataset.source === source));
  const stopped = source !== "live" && !playing;
  transport.hidden = source === "live";
  transport.dataset.state = stopped ? "stopped" : "playing";
  transport.querySelector(".label").textContent = stopped ? "Play" : "Stop";
  const powered = powerButton.getAttribute("aria-pressed") === "true";
  let text;
  if (statusOverride) text = statusOverride;
  else if (!powered) text = source === "live" ? "按 POWER 開始，接著允許使用音訊輸入" : "按 POWER 開始播放";
  else if (source === "live") text = "吉他輸入（請戴耳機，避免回授）";
  else if (loopingChord >= 0) text = `和弦循環：${DSP.CHORDS[loopingChord].name} · ${DSP.CHORD_PATTERN_NAMES[chordPattern]} · ${chordTempo} BPM`;
  else if (!playing) text = "已停止：可以按 PLUCK 撥單弦、CHORD 刷和弦，或按 PLAY 從頭播放";
  else if (source === "demo") text = "內建樂句，100 BPM，循環播放";
  else text = fileData ? fileData.name : "尚未載入音檔";
  if (powered && isIOS && !useMediaElementOutput && !statusOverride) text += "（沒聲音？請確認音量已調高、不是靜音模式）";
  statusText.textContent = text;
}
renderSource();

for (const chip of document.querySelectorAll(".source .chip")) {
  chip.addEventListener("click", async () => {
    const want = chip.dataset.source;
    if (want === "file" && !fileData) { $("#file").click(); return; }
    if (!(await startAudio())) return;
    await applySource(want);
  });
}

$("#file").addEventListener("change", async (e) => {
  const file = e.target.files && e.target.files[0];
  e.target.value = "";
  if (!file) return;
  if (!(await startAudio())) return;
  try {
    setStatus(`讀取 ${file.name}…`);
    const buffer = await engine.ctx.decodeAudioData(await file.arrayBuffer());
    const samples = new Float32Array(buffer.length);
    for (let ch = 0; ch < buffer.numberOfChannels; ++ch) {
      const data = buffer.getChannelData(ch);
      for (let i = 0; i < data.length; ++i) samples[i] += data[i] / buffer.numberOfChannels;
    }
    fileData = { samples, rate: buffer.sampleRate, name: file.name };
    post({ type: "file", samples: samples.slice(), rate: buffer.sampleRate });
    statusOverride = "";
    await applySource("file");
  } catch {
    setStatus(`無法讀取 ${file.name}。支援 WAV、MP3、FLAC、OGG、M4A（視瀏覽器而定）。`);
  }
});

for (const b of document.querySelectorAll(".pluck button")) {
  // "click", not "pointerdown": on iPhone only a completed tap may start audio
  b.addEventListener("click", async () => {
    if (!(await startAudio())) return;
    // plucking means "let me hear this string": stop the looping demo / file first
    if (source !== "live" && playing) setPlaying(false);
    post({ type: "pluck", index: Number(b.dataset.string) });
  });
}

const chordRow = $("#chord-buttons");
const chordButtons = DSP.CHORDS.map((chord, i) => {
  const b = el("button", "", { type: "button", title: `Play ${chord.name} (${chord.shape})`, "aria-pressed": "false" });
  b.textContent = chord.name;
  b.addEventListener("click", async () => {
    if (!(await startAudio())) return;
    if (source !== "live" && playing) setPlaying(false); // hear the chord on its own
    post({ type: "chord", index: i });
  });
  chordRow.append(b);
  return b;
});
function renderChordButtons() {
  chordButtons.forEach((b, i) => b.setAttribute("aria-pressed", String(i === loopingChord)));
}

const patternSelect = $("#chord-pattern"), bpmInput = $("#chord-bpm");
DSP.CHORD_PATTERN_NAMES.forEach((name, i) => patternSelect.append(new Option(name, String(i))));
patternSelect.value = String(chordPattern);
bpmInput.value = String(chordTempo);
const sendChordSettings = () => { post({ type: "chordSettings", pattern: chordPattern, bpm: chordTempo }); persist(); renderSource(); };
patternSelect.addEventListener("change", () => { chordPattern = Number(patternSelect.value); sendChordSettings(); });
bpmInput.addEventListener("input", () => {
  const bpm = Number(bpmInput.value);
  if (Number.isFinite(bpm) && bpm >= 40 && bpm <= 240) { chordTempo = Math.round(bpm); sendChordSettings(); }
});
bpmInput.addEventListener("change", () => { bpmInput.value = String(chordTempo); }); // snap back an out-of-range entry
$("#chord-stop").addEventListener("click", () => post({ type: "chordStop" }));

// ============================================================================
// EQ panel
const EQ_BANDS = [
  { name: "Low cut", freq: "eqLcFreq", colour: "#a8b2bd" },
  { name: "Bass", freq: "eqBassFreq", gain: "eqBassGain", colour: "#ff6b5e" },
  { name: "Lo mid", freq: "eqLmFreq", gain: "eqLmGain", q: "eqLmQ", colour: "#ffb020" },
  { name: "Hi mid", freq: "eqHmFreq", gain: "eqHmGain", q: "eqHmQ", colour: "#35e07a" },
  { name: "Treble", freq: "eqTrebFreq", gain: "eqTrebGain", colour: "#4fb3ff" },
  { name: "High cut", freq: "eqHcFreq", colour: "#c58bff" },
];
let selectedBand = 2, hoveredBand = -1, showAnalyzer = true;

bindToggle($("#eq-on"), "eqOn");
$("#analyzer").addEventListener("click", () => {
  showAnalyzer = !showAnalyzer;
  $("#analyzer").setAttribute("aria-pressed", String(showAnalyzer));
  analyzer.clear();
  sendMonitor();
});
$("#eq-flat").addEventListener("click", () => {
  for (const b of EQ_BANDS) for (const id of [b.freq, b.gain, b.q]) if (id) setValue(id, PARAMS[id].dflt);
});

// noise section: mains-hum filter (on the input) and hiss reduction (on the output)
const humSelect = $("#nr-hum"), denoiseRange = $("#nr-amount"), denoiseText = $("#nr-amount-text");
DSP.HUM_MODES.forEach((name, i) => humSelect.append(new Option(i === 0 ? "Hum: off" : `Hum: ${name}`, String(i))));
humSelect.addEventListener("change", () => setValue("nrHum", Number(humSelect.value)));
onChange("nrHum", (v) => { humSelect.value = String(v); });
denoiseRange.addEventListener("input", () => setValue("nrAmount", Number(denoiseRange.value)));
onChange("nrAmount", (v) => { denoiseRange.value = String(v); denoiseText.textContent = `Denoise: ${PARAMS.nrAmount.format(v)}`; });

const bandChips = $("#band-chips"), bandKnobsBox = $("#band-knobs");
const bandKnobs = { freq: makeKnob("Freq", "eqLmFreq"), gain: makeKnob("Gain", "eqLmGain"), q: makeKnob("Q", "eqLmQ") };
bandKnobsBox.append(bandKnobs.freq.root, bandKnobs.gain.root, bandKnobs.q.root);
EQ_BANDS.forEach((b, i) => {
  const chip = el("button", "chip", { type: "button", role: "radio" });
  chip.style.setProperty("--chip", b.colour);
  chip.textContent = b.name;
  chip.addEventListener("click", () => selectBand(i));
  bandChips.append(chip);
});
function selectBand(i) {
  selectedBand = i;
  const b = EQ_BANDS[i];
  [...bandChips.children].forEach((c, j) => c.setAttribute("aria-checked", String(j === i)));
  bandKnobsBox.style.setProperty("--band", b.colour);
  for (const key of ["freq", "gain", "q"]) {
    bandKnobs[key].root.hidden = !b[key];
    if (b[key]) bandKnobs[key].bind(b[key]);
  }
}
selectBand(2);

const eqCanvas = $("#eq-canvas");
const MIN_HZ = 20, MAX_HZ = 20000, RANGE_DB = 18, SPEC_RANGE = 84;
const eqPlot = { x: 0, y: 0, w: 1, h: 1 };
const xForFreq = (hz) => eqPlot.x + (eqPlot.w * Math.log(hz / MIN_HZ)) / Math.log(MAX_HZ / MIN_HZ);
const freqForX = (x) => MIN_HZ * Math.pow(MAX_HZ / MIN_HZ, clamp((x - eqPlot.x) / eqPlot.w, 0, 1));
const yForDb = (db) => eqPlot.y + eqPlot.h / 2 - (db / RANGE_DB) * (eqPlot.h / 2);
const dbForY = (y) => ((eqPlot.y + eqPlot.h / 2 - y) / (eqPlot.h / 2)) * RANGE_DB;
const eqSettings = () => fxParams().eq;
const eqFilters = Array.from({ length: 6 }, () => new DSP.Biquad());

function nodePos(i) {
  const b = EQ_BANDS[i];
  return [xForFreq(values[b.freq]), yForDb(b.gain ? values[b.gain] : -3)];
}
function bandAt(x, y, radius) {
  let best = -1, bestD = radius;
  for (let i = 0; i < EQ_BANDS.length; ++i) {
    const [nx, ny] = nodePos(i), d = Math.hypot(nx - x, ny - y);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}
function canvasPoint(e) {
  const r = eqCanvas.getBoundingClientRect();
  return [e.clientX - r.left, e.clientY - r.top];
}
let eqDrag = -1;
eqCanvas.addEventListener("pointerdown", (e) => {
  const [x, y] = canvasPoint(e);
  const band = bandAt(x, y, e.pointerType === "touch" ? 30 : 16);
  if (band < 0) return;
  eqDrag = band;
  selectBand(band);
  eqCanvas.setPointerCapture(e.pointerId);
  e.preventDefault();
});
eqCanvas.addEventListener("pointermove", (e) => {
  const [x, y] = canvasPoint(e);
  if (eqDrag >= 0) {
    const b = EQ_BANDS[eqDrag];
    setValue(b.freq, freqForX(x));
    if (b.gain) setValue(b.gain, dbForY(y));
  } else {
    hoveredBand = bandAt(x, y, 16);
    eqCanvas.style.cursor = hoveredBand >= 0 ? "grab" : "default";
  }
});
const endEqDrag = () => { eqDrag = -1; };
eqCanvas.addEventListener("pointerup", endEqDrag);
eqCanvas.addEventListener("pointercancel", endEqDrag);
eqCanvas.addEventListener("pointerleave", () => { hoveredBand = -1; });
eqCanvas.addEventListener("dblclick", (e) => {
  const [x, y] = canvasPoint(e);
  const band = bandAt(x, y, 16);
  if (band >= 0) for (const id of [EQ_BANDS[band].freq, EQ_BANDS[band].gain, EQ_BANDS[band].q]) if (id) setValue(id, PARAMS[id].dflt);
});
eqCanvas.addEventListener("wheel", (e) => {
  const [x, y] = canvasPoint(e);
  const band = bandAt(x, y, 16), qId = band >= 0 ? EQ_BANDS[band].q : null;
  if (!qId) return; // not on a point: let the page scroll
  e.preventDefault();
  selectBand(band);
  setValue(qId, values[qId] * Math.pow(2, -Math.sign(e.deltaY) * 0.15));
}, { passive: false });

function roundRect(g, x, y, w, h, r) {
  g.beginPath();
  if (g.roundRect) g.roundRect(x, y, w, h, r); else g.rect(x, y, w, h);
}

function fitCanvas(canvas) {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const g = canvas.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  return [g, w, h];
}

function drawEq() {
  const [g, w, h] = fitCanvas(eqCanvas);
  Object.assign(eqPlot, { x: 36, y: 22, w: w - 44, h: h - 44 });
  const fs = engine.ctx ? engine.ctx.sampleRate : 48000;
  const active = values.eqOn;
  g.clearRect(0, 0, w, h);

  // grid
  g.font = "10.5px 'IBM Plex Mono', ui-monospace, monospace";
  g.textAlign = "center";
  g.textBaseline = "top";
  for (const hz of [30, 40, 60, 70, 80, 90, 300, 400, 600, 700, 800, 900, 3000, 4000, 6000, 7000, 8000, 9000]) {
    g.fillStyle = "rgba(255,255,255,0.035)";
    g.fillRect(Math.round(xForFreq(hz)), eqPlot.y, 1, eqPlot.h);
  }
  for (const [hz, text] of [[50, "50"], [100, "100"], [200, "200"], [500, "500"], [1000, "1k"], [2000, "2k"], [5000, "5k"], [10000, "10k"]]) {
    const x = Math.round(xForFreq(hz));
    g.fillStyle = "rgba(255,255,255,0.08)";
    g.fillRect(x, eqPlot.y, 1, eqPlot.h);
    g.fillStyle = "#8f8f98";
    g.fillText(text, x, eqPlot.y + eqPlot.h + 5);
  }
  g.textAlign = "right";
  g.textBaseline = "middle";
  for (let db = -12; db <= 12; db += 6) {
    const y = Math.round(yForDb(db));
    g.fillStyle = db === 0 ? "rgba(255,255,255,0.16)" : "rgba(255,255,255,0.07)";
    g.fillRect(eqPlot.x, y, eqPlot.w, 1);
    g.fillStyle = "#8f8f98";
    g.fillText((db > 0 ? "+" : "") + db, eqPlot.x - 6, y);
  }

  g.save();
  g.beginPath();
  g.rect(eqPlot.x, eqPlot.y, eqPlot.w, eqPlot.h);
  g.clip();

  // live spectrum of the post-EQ signal
  if (showAnalyzer) {
    const cols = analyzer.columns, n = cols.length;
    g.beginPath();
    g.moveTo(eqPlot.x, eqPlot.y + eqPlot.h);
    for (let c = 0; c < n; ++c) {
      const y = eqPlot.y + eqPlot.h - eqPlot.h * clamp((cols[c] + SPEC_RANGE) / SPEC_RANGE, 0, 1);
      g.lineTo(eqPlot.x + (eqPlot.w * c) / (n - 1), y);
    }
    g.lineTo(eqPlot.x + eqPlot.w, eqPlot.y + eqPlot.h);
    g.closePath();
    const grad = g.createLinearGradient(0, eqPlot.y, 0, eqPlot.y + eqPlot.h);
    grad.addColorStop(0, "rgba(52,195,255,0.33)");
    grad.addColorStop(1, "rgba(52,195,255,0.03)");
    g.fillStyle = grad;
    g.fill();
    g.strokeStyle = "rgba(52,195,255,0.55)";
    g.lineWidth = 1;
    g.stroke();
  }

  // response: the selected band shaded, the total as a white line
  DSP.Equalizer.design(eqSettings(), fs, eqFilters);
  const points = Math.max(2, Math.floor(eqPlot.w / 2)), zero = yForDb(0);
  const total = new Path2D(), sel = new Path2D();
  sel.moveTo(eqPlot.x, zero);
  for (let i = 0; i < points; ++i) {
    const x = eqPlot.x + (eqPlot.w * i) / (points - 1), hz = freqForX(x);
    let gain = 1;
    for (const f of eqFilters) gain *= f.getMagnitude(hz, fs);
    const yt = yForDb(DSP.gainToDb(gain)), ys = yForDb(DSP.gainToDb(eqFilters[selectedBand].getMagnitude(hz, fs)));
    if (i === 0) total.moveTo(x, yt); else total.lineTo(x, yt);
    sel.lineTo(x, ys);
  }
  sel.lineTo(eqPlot.x + eqPlot.w, zero);
  sel.closePath();
  g.globalAlpha = active ? 0.2 : 0.08;
  g.fillStyle = EQ_BANDS[selectedBand].colour;
  g.fill(sel);
  g.globalAlpha = 1;
  g.strokeStyle = active ? "rgba(255,255,255,0.95)" : "rgba(255,255,255,0.3)";
  g.lineWidth = 2.2;
  g.lineJoin = "round";
  g.stroke(total);
  g.restore();

  // readout for the selected band
  const b = EQ_BANDS[selectedBand];
  let text = b.name.toUpperCase();
  for (const id of [b.freq, b.gain, b.q]) if (id) text += `   ${id === b.q ? "Q " : ""}${PARAMS[id].format(values[id])}`;
  g.font = "600 12px 'Barlow', system-ui, sans-serif";
  g.textAlign = "left";
  g.textBaseline = "middle";
  g.fillStyle = b.colour;
  g.fillText(text, eqPlot.x + 6, 11);
  g.textAlign = "right";
  g.fillStyle = "#8f8f98";
  g.font = "11px 'Barlow', system-ui, sans-serif";
  g.fillText(active ? (w > 520 ? "drag points  |  wheel: Q  |  double-click: reset" : "drag the points") : "EQ BYPASSED", eqPlot.x + eqPlot.w, 11);

  // band points
  EQ_BANDS.forEach((band, i) => {
    const [x, y] = nodePos(i), r = i === selectedBand ? 7.5 : 6;
    g.globalAlpha = active ? 1 : 0.45;
    if (i === selectedBand || i === hoveredBand || i === eqDrag) {
      g.fillStyle = band.colour + "40";
      g.beginPath(); g.arc(x, y, r * 1.8, 0, Math.PI * 2); g.fill();
    }
    g.fillStyle = band.colour;
    g.beginPath(); g.arc(x, y, r, 0, Math.PI * 2); g.fill();
    g.strokeStyle = i === selectedBand ? "#fff" : "rgba(0,0,0,0.6)";
    g.lineWidth = i === selectedBand ? 2 : 1;
    g.stroke();
    g.globalAlpha = 1;
  });
}

// ============================================================================
// Tuner
const tunerCanvas = $("#tuner");
const tunerState = { hz: 0, pending: 0, silent: 0 };
tunerCanvas.addEventListener("click", () => setValue("tunerOn", !values.tunerOn));
tunerCanvas.addEventListener("keydown", (e) => { if (e.key === " " || e.key === "Enter") { e.preventDefault(); setValue("tunerOn", !values.tunerOn); } });
onChange("tunerOn", (on) => {
  tunerCanvas.setAttribute("aria-pressed", String(on));
  tunerState.hz = 0; tunerState.pending = 0; tunerState.silent = 0;
  sendMonitor();
});
function updateTuner() {
  if (!engine.ctx || !values.tunerOn) return;
  const hz = tunerDetector.detect();
  if (hz > 0) {
    tunerState.silent = 0;
    const jump = tunerState.hz <= 0 || Math.abs(1200 * Math.log2(hz / tunerState.hz)) > 40;
    if (!jump) { tunerState.hz += 0.3 * (hz - tunerState.hz); tunerState.pending = 0; }
    else if (tunerState.hz <= 0 || ++tunerState.pending >= 2) { tunerState.hz = hz; tunerState.pending = 0; }
  } else if (++tunerState.silent > 18) tunerState.hz = 0;
}
function drawTuner() {
  const [g, w, h] = fitCanvas(tunerCanvas);
  g.clearRect(0, 0, w, h);
  g.fillStyle = "#0a1310";
  roundRect(g, 3, 3, w - 6, h - 6, 6); g.fill();
  if (!values.tunerOn) {
    g.textAlign = "center"; g.textBaseline = "middle";
    g.fillStyle = "rgba(125,255,176,0.35)";
    g.font = "700 22px 'Barlow Condensed', 'Arial Narrow', sans-serif";
    g.fillText("TUNER OFF", w / 2, h / 2 - 8);
    g.font = "12px 'IBM Plex Mono', monospace";
    g.fillText("點一下開啟", w / 2, h / 2 + 16);
    return;
  }
  const has = tunerState.hz > 0;
  const note = has ? DSP.frequencyToNote(tunerState.hz) : null;
  const cents = has ? note.cents : 0, abs = Math.abs(cents);
  const colour = !has ? "rgba(125,255,176,0.18)" : abs < 3 ? "#35e07a" : abs < 15 ? "#ffb020" : "#ff3b30";
  const noteW = Math.min(84, w * 0.24);

  g.textAlign = "center";
  g.textBaseline = "middle";
  g.fillStyle = has ? colour : "rgba(125,255,176,0.25)";
  g.font = "700 44px 'Barlow Condensed', 'Arial Narrow', sans-serif";
  g.fillText(has ? note.name : "–", 12 + noteW / 2, h / 2 - 7);
  g.font = "12px 'IBM Plex Mono', monospace";
  g.fillStyle = has ? "rgba(125,255,176,0.7)" : "rgba(125,255,176,0.3)";
  g.fillText(has ? `octave ${note.octave}` : "TUNER", 12 + noteW / 2, h - 16);

  const sx = 12 + noteW + 12, sw = w - sx - 16, sy = 14, sh = h - 44;
  const xFor = (c) => sx + (sw * (c + 50)) / 100;
  g.fillStyle = "rgba(53,224,122,0.12)";
  g.fillRect(xFor(-3), sy, xFor(3) - xFor(-3), sh);
  for (let c = -50; c <= 50; c += 5) {
    const tall = c === 0 ? sh : c % 25 === 0 ? sh * 0.55 : sh * 0.3;
    g.fillStyle = c === 0 ? "rgba(125,255,176,0.6)" : "rgba(125,255,176,0.25)";
    g.fillRect(xFor(c) - 0.75, sy + (sh - tall) / 2, 1.5, tall);
  }
  if (has) {
    const x = xFor(clamp(cents, -50, 50));
    g.fillStyle = colour + "4d";
    roundRect(g, x - 6, sy - 2, 12, sh + 4, 4); g.fill();
    g.fillStyle = colour;
    roundRect(g, x - 2, sy - 2, 4, sh + 4, 2); g.fill();
  }
  g.font = "12px 'IBM Plex Mono', monospace";
  g.fillStyle = has ? "rgba(125,255,176,0.85)" : "rgba(125,255,176,0.35)";
  const ry = h - 16;
  if (has) {
    g.textAlign = "left"; g.fillText(`${cents >= 0 ? "+" : ""}${cents.toFixed(1)} cents`, sx, ry);
    g.textAlign = "right"; g.fillText(`${tunerState.hz.toFixed(2)} Hz`, sx + sw, ry);
    if (abs < 3 && sw > 260) { g.textAlign = "center"; g.fillText("IN TUNE", sx + sw / 2, ry); }
  } else {
    g.textAlign = "center";
    g.fillText(engine.ctx ? "play a single string" : "press POWER", sx + sw / 2, ry);
  }
}

// ============================================================================
// Audio status line (helps when there's no sound on a particular device)
const engineInfo = $("#engine-info");
const STATE_NAMES = { running: "運作中", suspended: "暫停", interrupted: "被系統中斷", closed: "已關閉" };
let lastInfo = 0;
function renderEngineInfo(t) {
  if (t - lastInfo < 500) return;
  lastInfo = t;
  const ctx = engine.ctx;
  if (!ctx) return;
  const latency = Math.round(((ctx.baseLatency || 0) + (ctx.outputLatency || 0)) * 1000);
  const route = useMediaElementOutput
    ? `輸出：媒體通道（${engine.outputElement && !engine.outputElement.paused ? "播放中" : "未播放"}）`
    : "輸出：直接";
  const parts = [`音訊狀態：${STATE_NAMES[ctx.state] || ctx.state}`, engine.kind, route, `${(ctx.sampleRate / 1000).toFixed(1)} kHz`];
  if (latency > 0) parts.push(`輸出延遲約 ${latency} ms`);
  // input side, when the browser reports it (Chrome / Edge do; Safari may not)
  const inputLatency = source === "live" && engine.mic && engine.mic.track.getSettings ? engine.mic.track.getSettings().latency : 0;
  if (inputLatency > 0) {
    parts.push(`輸入延遲約 ${Math.round(inputLatency * 1000)} ms`);
    if (latency > 0) parts.push(`合計約 ${latency + Math.round(inputLatency * 1000)} ms`);
  }
  const text = parts.filter(Boolean).join(" · ");
  if (engineInfo.textContent !== text) engineInfo.textContent = text;
}

// ============================================================================
// Frame loop: meters every frame, analysis + canvases ~30 fps
let lastHeavy = 0;
function frame(t) {
  requestAnimationFrame(frame);
  inMeter.update(levels.in);
  outMeter.update(levels.out);
  levels.in = levels.out = 0;
  progressBar.style.width = source === "live" || !engine.ctx || !playing ? "0%" : `${(levels.progress * 100).toFixed(1)}%`;
  tickTempo();
  renderEngineInfo(t);
  if (t - lastHeavy < 32) return;
  lastHeavy = t;
  updateTuner();
  if (showAnalyzer) analyzer.process();
  drawTuner();
  drawEq();
}
requestAnimationFrame(frame);
document.fonts && document.fonts.ready.then(() => { drawTuner(); drawEq(); });

// debugging handle for local testing only
if (location.hostname === "localhost") window.__pedalboard = { engine, values, levels, fxParams, tunerState, analyzer, frame: () => frame(performance.now() + 1000) };
