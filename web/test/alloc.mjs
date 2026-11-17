// Does the audio path allocate memory while it runs? Garbage made on the audio thread has to be collected
// on the audio thread, and that costs time exactly where there is none to spare when the buffers are small.
// This runs the models in a child process with V8's --trace-gc and adds up what the garbage collector had
// to throw away while each one played: the real figure, with the JIT working as it does in a browser.
// (A heap profiler would name the functions, but attaching one changes what the JIT inlines, and with it
// most of the numbers; `sites` below uses it anyway, as a hint of where to look.)
//
// One number boxed per sample is 16 bytes x 128 = 2048 bytes per block, so a model under the mark (1024)
// allocates nothing per sample; a little per-block garbage (parameter updates) is normal. What is left above
// the mark are numbers V8 boxes when it does not inline a small per-sample helper into a large loop. Which
// calls those are depends on the V8 version (and Firefox and Safari do not box numbers at all), so this is a
// report to compare before and after a change, not a pass / fail test.
//
// Usage: node web/test/alloc.mjs [case ...]
//          the whole chain, as the AudioWorklet runs it: every model in slot 1, every amp, some boards.
//          Cases: a model key, "default", "empty", "noise", "heavy", "amp:<key>".
//        node web/test/alloc.mjs engine <file.js> <ClassName> <MODELS_EXPORT> [model key ...]
//          one engine on its own (loads only that file), e.g.  engine delayfx.js DelayFx DELAY_MODELS
//        node web/test/alloc.mjs sites <case ...>     functions that allocate (profiler attached, see above)
import { spawnSync } from "node:child_process";
import { writeSync } from "node:fs";
import inspector from "node:inspector";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const fs = 48000, block = 128, warmup = 1500, blocks = 6000, mark = 1024;
const here = dirname(fileURLToPath(import.meta.url)), self = fileURLToPath(import.meta.url);
let args = process.argv.slice(2);
const child = args[0] === "--child";
if (child) args = args.slice(1);
const sites = args[0] === "sites";
if (sites) args = args.slice(1);
const engineMode = args[0] === "engine";
const [moduleFile, className, modelsName] = engineMode ? args.slice(1, 4) : [];
const wanted = engineMode ? args.slice(4) : args;

// ---------------------------------------------------------------------------------------------------------
// Parent: run the cases in a child with --trace-gc (a small young generation, so that even little garbage
// shows) and add up what each scavenge reclaimed between the child's markers.
if (!child && !sites) {
  const run = spawnSync(process.execPath, ["--trace-gc", "--max-semi-space-size=1", "--min-semi-space-size=1", self, "--child", ...process.argv.slice(2)],
    { encoding: "utf8", maxBuffer: 256 * 1024 * 1024 });
  if (run.status !== 0 && !run.stdout.includes("CASE-END")) { console.log(run.stdout.slice(-2000), run.stderr.slice(-2000)); process.exit(2); }
  const results = [];
  let current = null, reclaimed = 0;
  for (const line of run.stdout.split(/\r?\n/)) {
    const start = line.match(/CASE-START (\S+)/), end = line.match(/CASE-END (\S+) (\S+)/);
    const gc = line.match(/(?:Scavenge|Mark-Compact)[^0-9]*([\d.]+) \([\d.]+\) -> ([\d.]+) \([\d.]+\) MB/);
    if (start) { current = start[1]; reclaimed = 0; }
    else if (gc && current) reclaimed += Math.max(0, Number(gc[1]) - Number(gc[2]));
    else if (end && current) { results.push({ what: current, perBlock: (reclaimed * 1024 * 1024) / blocks, speed: Number(end[2]) }); current = null; }
  }
  results.sort((a, b) => b.perBlock - a.perBlock);
  const over = results.filter((r) => r.perBlock > mark);
  const total = results.reduce((sum, r) => sum + r.perBlock, 0);
  console.log(`== Garbage made on the audio path, bytes per ${block}-sample block (${results.length} cases, ${(blocks * block / fs).toFixed(0)} s of audio each) ==`);
  for (const r of results.slice(0, wanted.length || engineMode ? results.length : 15))
    console.log(`  ${r.what.padEnd(34)} ${r.perBlock.toFixed(0).padStart(7)}   (${r.speed.toFixed(0)}x realtime)`);
  if (results.length) console.log(`  worst ${results[0].perBlock.toFixed(0)}, mean ${(total / results.length).toFixed(0)}, ${over.length} of ${results.length} above ${mark} B/block`);
  process.exit(0);
}

// ---------------------------------------------------------------------------------------------------------
// Child (or `sites`): build each case's step function, then measure it.
const input = new Float32Array(block * 64);
for (let i = 0; i < input.length; ++i) input[i] = 0.3 * Math.sin((2 * Math.PI * 110 * i) / fs) * Math.exp(-((i % 4096) / 2000));
const inputBlocks = Array.from({ length: 64 }, (_, b) => input.subarray(b * block, (b + 1) * block)); // made once, not while measuring
const left = new Float32Array(block), right = new Float32Array(block), mono = new Float32Array(block);
const cases = []; // [name, step(blockIndex)]

if (engineMode) {
  // one engine, driven the way a slot drives it: model and knobs set on every block, then process
  const module = await import(pathToFileURL(join(here, "..", "src", "dsp", moduleFile)).href);
  const Fx = module[className], MODELS = module[modelsName];
  if (!Fx || !MODELS) { console.log(`${moduleFile} must export ${className} and ${modelsName}`); process.exit(2); }
  const dryLeft = new Float32Array(block), dryRight = new Float32Array(block);
  for (const model of MODELS.filter((m) => !wanted.length || wanted.includes(m.key))) {
    const fx = new Fx();
    fx.prepare(fs, block);
    const knobs = new Float32Array(12);
    model.knobs.forEach((k, i) => { knobs[i] = k.def; });
    fx.setModel(model.variant); fx.setParameters(knobs); fx.reset();
    cases.push([model.key, (b) => {
      const x = inputBlocks[b & 63];
      left.set(x); right.set(x);
      fx.setModel(model.variant); fx.setParameters(knobs);
      if (fx.processDry) { dryLeft.set(x); dryRight.set(x); fx.processDry(dryLeft, dryRight, block); }
      fx.process(left, right, block);
    }]);
  }
} else {
  const DSP = await import("../src/dsp.js");
  const emptyBoard = (edit) => {
    const p = DSP.defaultBoard();
    p.slots = p.slots.map(() => DSP.makeSlot("empty", false));
    p.amp = DSP.makeAmp("", "", false);
    p.eqOn = false;
    edit(p);
    return p;
  };
  const boardFor = (what) => {
    if (what === "default") return DSP.defaultBoard();
    if (what === "empty") return emptyBoard(() => {});
    if (what === "noise") return emptyBoard((p) => { p.eqOn = true; p.humMode = 2; p.denoise = 50; });
    if (what === "heavy") { // eight heavy effects and a high-gain amp
      const p = DSP.defaultBoard();
      p.slots = ["smart_harmony", "bass_octaver", "growler", "line6_drive", "tri_chorus", "octo", "cave", "particle_verb"].map((k) => DSP.makeSlot(k, true));
      p.amp = DSP.makeAmp("line6_insane", "cab_412_uber", true);
      p.humMode = 2; p.denoise = 50;
      return p;
    }
    if (what.startsWith("amp:")) return emptyBoard((p) => { p.amp = DSP.makeAmp(what.slice(4), DSP.defaultCabFor(what.slice(4)), true); });
    return emptyBoard((p) => { p.slots[0] = DSP.makeSlot(what, true); });
  };
  const names = wanted.length ? wanted
    : ["empty", "default", "noise", "heavy", ...DSP.MODELS.slice(1).map((m) => m.key), ...DSP.AMPS.map((a) => `amp:${a.key}`)];
  for (const what of names) {
    const params = boardFor(what);
    for (const s of params.slots) DSP.resolveTempo(s, 120);
    const chain = new DSP.FxChain();
    chain.prepare(fs, block);
    chain.setParameters(params);
    chain.reset();
    cases.push([what, (b) => {
      mono.set(inputBlocks[b & 63]);
      chain.setParameters(params); // as the worklet does on every block
      chain.process(mono, left, right, block);
    }]);
  }
}

if (child) {
  for (const [what, step] of cases) {
    for (let b = 0; b < warmup; ++b) step(b); // lets the JIT settle and lazily made engines appear
    writeSync(1, `\nCASE-START ${what}\n`);
    const t0 = performance.now();
    for (let b = 0; b < blocks; ++b) step(b);
    const speed = ((blocks * block) / fs) * 1000 / (performance.now() - t0);
    writeSync(1, `\nCASE-END ${what} ${speed.toFixed(1)}\n`);
  }
  process.exit(0);
}

// `sites`: the sampling heap profiler, to see which functions allocate
const session = new inspector.Session();
session.connect();
const post = (method, params) => new Promise((resolve, reject) => session.post(method, params, (err, r) => (err ? reject(err) : resolve(r))));
const allSites = new Map();
for (const [what, step] of cases) {
  for (let b = 0; b < warmup; ++b) step(b);
  await post("HeapProfiler.startSampling", { samplingInterval: 256, includeObjectsCollectedByMajorGC: true, includeObjectsCollectedByMinorGC: true });
  for (let b = 0; b < 2500; ++b) step(b);
  const { profile } = await post("HeapProfiler.stopSampling");
  const walk = (node) => {
    if (node.selfSize > 0 && !node.callFrame.url.startsWith("node:")) {
      const f = node.callFrame, key = `${f.functionName || "(anonymous)"}  ${f.url.split("/").pop()}:${f.lineNumber + 1}`;
      const site = allSites.get(key) || { worst: 0, worstCase: "" };
      if (node.selfSize > site.worst) { site.worst = node.selfSize; site.worstCase = what; }
      allSites.set(key, site);
    }
    node.children.forEach(walk);
  };
  walk(profile.head);
}
session.disconnect();
console.log("== Allocation sites with a heap profiler attached (bytes per block, in the case where each is worst) ==");
for (const [key, site] of [...allSites].sort((a, b) => b[1].worst - a[1].worst).slice(0, 50))
  console.log(`  ${(site.worst / 2500).toFixed(0).padStart(7)}  ${key.padEnd(46)} ${site.worstCase}`);
