// Checks one effect engine's JavaScript port against its C++ original, using the reference data written
// by the standalone C++ test (Tests/standalone/<category>_test.cpp, see Harness.h).
//
//   node web/test/standalone.mjs <category> <ClassName> <MODELS_EXPORT> [module file, default <category>.js]
//   e.g.  node web/test/standalone.mjs volume VolumeFx VOLUME_MODELS
//
// For every case in test_output/standalone/<category>/manifest.json it replays the same input, knobs and
// knob changes through the JavaScript class and compares each sample; it also checks that the model list
// is the same as the C++ one, and that the engine copes with 128-sample blocks (the AudioWorklet's size).
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const [category, className, modelsName, moduleFile = `${category}.js`] = process.argv.slice(2);
if (!category || !className || !modelsName) {
  console.log("usage: node web/test/standalone.mjs <category> <ClassName> <MODELS_EXPORT> [module file]");
  process.exit(2);
}
const here = dirname(fileURLToPath(import.meta.url));
const refDir = join(here, "..", "..", "test_output", "standalone", category);
const module = await import(pathToFileURL(join(here, "..", "src", "dsp", moduleFile)).href);
const Fx = module[className], MODELS = module[modelsName];
if (!Fx || !MODELS) { console.log(`FAIL: ${moduleFile} must export ${className} and ${modelsName}`); process.exit(1); }

let failures = 0;
const check = (ok, what) => { if (!ok) { failures++; console.log(`  FAIL: ${what}`); } };
const readF32 = (file) => { const b = readFileSync(file); return new Float32Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.length)); };
const db = (x) => (x > 0 ? 20 * Math.log10(x) : -999);

const manifest = JSON.parse(readFileSync(join(refDir, "manifest.json"), "utf8"));
const input = readF32(join(refDir, "input.f32"));
const { sampleRate: fs, blockSize } = manifest;

// ---- the model list must be the same as the C++ one
{
  const near = (a, b) => Math.abs(a - b) <= 1e-6 * Math.max(1, Math.abs(a), Math.abs(b));
  let different = 0;
  manifest.models.forEach((want, i) => {
    const got = MODELS[i];
    const same = got && ["key", "name", "category", "engine", "variant", "basedOn", "timeKnob", "noteKnob", "timeKnob2", "noteKnob2", "stereo", "trails"].every((f) => got[f] === want[f])
      && got.knobs.length === want.knobs.length
      && want.knobs.every((k, j) => {
        const g = got.knobs[j];
        return g.name === k.name && g.unit === k.unit && ["min", "max", "def", "centre", "step"].every((f) => near(g[f], k[f]))
          && JSON.stringify(g.choices) === JSON.stringify(k.choices);
      });
    if (!same) { different++; console.log(`  model ${i} (${want.key}) differs from the C++ list`); }
  });
  console.log(`== ${category}: ${manifest.models.length} models in C++, ${MODELS.length} in JavaScript, ${different} different ==`);
  check(different === 0 && manifest.models.length === MODELS.length, "model list matches the C++ one");
}

function render(effect, c, block) {
  const n = input.length, out = new Float32Array(n * 2);
  const left = new Float32Array(block), right = new Float32Array(block);
  effect.setModel(c.variant);
  effect.setParameters(c.knobs);
  effect.reset();
  for (let pos = 0; pos < n; pos += block) {
    const len = Math.min(block, n - pos);
    let knobs = c.knobs;
    for (const s of c.schedule || []) if (pos >= s.atSample) knobs = s.knobs;
    effect.setParameters(knobs);
    left.set(input.subarray(pos, pos + len)); right.set(input.subarray(pos, pos + len));
    effect.process(left.subarray(0, len), right.subarray(0, len), len);
    for (let i = 0; i < len; ++i) { out[(pos + i) * 2] = left[i]; out[(pos + i) * 2 + 1] = right[i]; }
  }
  return out;
}

// ---- every case, sample by sample (one shared engine, like a slot that gets other models picked)
const shared = new Fx();
shared.prepare(fs, blockSize);
let worst = -999, slowest = Infinity;
for (const c of manifest.cases) {
  const expected = readF32(join(refDir, `${c.name}.f32`));
  const t0 = performance.now();
  const got = render(shared, c, blockSize);
  const speed = (input.length / fs) * 1000 / (performance.now() - t0);
  let sig = 0, err = 0, maxErr = 0, finite = true;
  for (let i = 0; i < got.length; ++i) {
    const e = got[i] - expected[i];
    sig += expected[i] * expected[i]; err += e * e; maxErr = Math.max(maxErr, Math.abs(e));
    finite = finite && Number.isFinite(got[i]);
  }
  const relDb = sig > 0 ? db(Math.sqrt(err / sig)) : (err > 0 ? 0 : -999);
  worst = Math.max(worst, relDb); slowest = Math.min(slowest, speed);
  console.log(`  ${c.name.padEnd(28)} difference ${relDb.toFixed(1).padStart(7)} dB   max ${maxErr.toExponential(1)}   (${speed.toFixed(0)}x realtime)`);
  check(finite, `${c.name} produced NaN/Inf`);
  check(relDb < -60, `${c.name} differs from the C++ version`);
  check(speed > 4, `${c.name} is too slow in JavaScript (${speed.toFixed(1)}x realtime)`);
  if (c.mix !== undefined) {
    check(Math.abs(shared.getMix() - c.mix) < 1e-5 && Math.abs(shared.getTailSeconds() - c.tailSeconds) < 1e-3,
      `${c.name}: getMix() / getTailSeconds() differ from C++ (${shared.getMix()} vs ${c.mix}, ${shared.getTailSeconds()} vs ${c.tailSeconds})`);
  }
}

// ---- 128-sample blocks (the AudioWorklet's block size) on an engine prepared for them
{
  const small = new Fx();
  small.prepare(fs, 128);
  let ok = true;
  for (const c of manifest.cases.filter((x) => !x.schedule)) {
    const out = render(small, c, 128);
    for (let i = 0; i < out.length; ++i) if (!Number.isFinite(out[i]) || Math.abs(out[i]) > 16) { ok = false; break; }
  }
  check(ok, "128-sample blocks produce NaN or runaway levels");
}

console.log(`  worst difference ${worst.toFixed(1)} dB, slowest ${slowest.toFixed(0)}x realtime`);
console.log(failures === 0 ? "ALL CHECKS PASSED" : `${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
