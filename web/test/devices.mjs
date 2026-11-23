// Checks the page's input / output device lists in a real browser: Edge or Chrome, headless and muted, with the
// browser's own fake audio devices. Nothing is recorded or played, no permission prompt appears, and the
// computer's real devices are not touched.
//
// Usage (after `node web/build.mjs`):  node web/test/devices.mjs
// Uses Edge or Chrome where they are usually installed, or the one named by the BROWSER environment variable;
// without one the test says so and is skipped.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, extname, join } from "node:path";
import { fileURLToPath } from "node:url";

const dist = join(dirname(fileURLToPath(import.meta.url)), "..", "dist");
const browser = [process.env.BROWSER,
                 "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe", "C:/Program Files/Microsoft/Edge/Application/msedge.exe",
                 "C:/Program Files/Google/Chrome/Application/chrome.exe", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                 "/usr/bin/google-chrome", "/usr/bin/chromium"].filter(Boolean).find((path) => existsSync(path));
if (!browser) {
  console.log("No Edge or Chrome found (set BROWSER to its path): device test skipped.");
  process.exit(0);
}

let failures = 0;
const check = (ok, what) => { console.log(`  ${ok ? "ok  " : "FAIL"}  ${what}`); if (!ok) failures++; };
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ---- the built page, served from this computer only
const types = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8" };
const server = createServer((request, response) => {
  const name = request.url.split("?")[0] === "/" ? "index.html" : request.url.split("?")[0].slice(1);
  const file = join(dist, name);
  if (!/^[\w.-]+$/.test(name) || !existsSync(file)) { response.writeHead(404); response.end(); return; }
  response.writeHead(200, { "Content-Type": types[extname(name)] || "application/octet-stream" });
  response.end(readFileSync(file));
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const origin = `http://localhost:${server.address().port}`; // "localhost": there the page offers its test hooks (window.__pedalboard)

// ---- the browser, with fake devices and a debugging port
const profile = mkdtempSync(join(tmpdir(), "pedalboard-devices-"));
const child = spawn(browser, ["--headless=new", "--disable-gpu", "--mute-audio", "--no-first-run", "--no-default-browser-check",
                              "--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream",
                              "--autoplay-policy=no-user-gesture-required", "--remote-debugging-port=0",
                              `--user-data-dir=${profile}`, `${origin}/`], { stdio: "ignore" });
// On Windows the process started here hands over to another one and exits, so the browser is neither watched nor
// stopped through `child`: its debugging port is read from the file it writes into its profile, and it is closed
// through that port (Browser.close), which ends every process it started.
let browserSocketUrl = "";
const cleanUp = async () => {
  if (browserSocketUrl) {
    await new Promise((resolve) => {
      const closer = new WebSocket(browserSocketUrl);
      const done = () => { try { closer.close(); } catch { /* already closed */ } resolve(); };
      closer.onopen = () => { closer.send(JSON.stringify({ id: 1, method: "Browser.close" })); setTimeout(done, 1500); };
      closer.onerror = done;
      closer.onclose = done;
    });
  }
  child.kill();
  server.close();
  for (let attempt = 0; attempt < 20; ++attempt) {
    try { rmSync(profile, { recursive: true, force: true }); break; } catch { await sleep(300); } // (the browser lets go of its files a moment later)
  }
};

try {
  let debugPort = 0;
  for (let attempt = 0; attempt < 100 && !debugPort; ++attempt) {
    const portFile = join(profile, "DevToolsActivePort");
    if (existsSync(portFile)) {
      const [port, path] = readFileSync(portFile, "utf8").split(/\r?\n/);
      if (Number(port) > 0 && path) { debugPort = Number(port); browserSocketUrl = `ws://127.0.0.1:${port}${path}`; }
    }
    if (!debugPort) await sleep(200);
  }
  if (!debugPort) throw new Error("the browser did not start");

  let target = null;
  for (let attempt = 0; attempt < 50 && !target; ++attempt) {
    const list = await fetch(`http://127.0.0.1:${debugPort}/json/list`).then((r) => r.json()).catch(() => []);
    target = list.find((t) => t.type === "page" && t.url.startsWith(origin)) || null;
    if (!target) await sleep(200);
  }
  if (!target) throw new Error("the page did not open");

  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = () => reject(new Error("no debugging connection")); });
  let lastId = 0;
  const waiting = new Map();
  socket.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.id && waiting.has(message.id)) { waiting.get(message.id)(message); waiting.delete(message.id); }
  };
  const send = (method, params = {}) => new Promise((resolve) => {
    const id = ++lastId;
    waiting.set(id, resolve);
    socket.send(JSON.stringify({ id, method, params }));
  });
  /** Runs `body` (the inside of an async function) in the page, as if started by a click. */
  const page = async (body) => {
    const reply = await send("Runtime.evaluate", { expression: `(async () => { const P = window.__pedalboard, engine = P && P.engine, values = P && P.values; ${body} })()`, awaitPromise: true, returnByValue: true, userGesture: true });
    if (reply.error || reply.result.exceptionDetails) throw new Error(`in the page: ${JSON.stringify(reply.error || reply.result.exceptionDetails.exception || reply.result.exceptionDetails.text)}`);
    return reply.result.result.value;
  };
  const until = async (condition, what, seconds = 10) => {
    for (let waited = 0; waited < seconds * 1000; waited += 100) {
      if (await page(`return !!(${condition});`)) return true;
      await sleep(100);
    }
    check(false, `timed out waiting for: ${what}`);
    return false;
  };
  const lists = () => page(`
    const texts = (id) => [...document.querySelector(id).options].map((o) => o.text);
    return { inputs: texts("#input-device"), outputs: texts("#output-device"),
             input: document.querySelector("#input-device").value, output: document.querySelector("#output-device").value,
             outputDisabled: document.querySelector("#output-device").disabled, scanHidden: document.querySelector("#device-scan").hidden,
             note: document.querySelector("#device-note").hidden ? "" : document.querySelector("#device-note").textContent,
             status: document.querySelector("#status-text").textContent };`);
  const reload = async () => {
    await send("Page.reload");
    await sleep(500);
    await until(`document.readyState === "complete" && window.__pedalboard && document.querySelector("#input-device")`, "the page to load again");
    await sleep(300);
  };
  const choose = (selectId, index) => page(`
    const select = document.querySelector("${selectId}");
    select.selectedIndex = ${index};
    select.dispatchEvent(new Event("change", { bubbles: true }));
    return select.value;`);

  await send("Page.enable");
  await until(`document.readyState === "complete" && window.__pedalboard`, "the page to load");
  await sleep(500);

  // ---------------------------------------------------------------------------
  console.log(`== Device lists (${browser.split(/[\\/]/).pop()}, fake devices) ==`);
  let now = await lists();
  console.log(`  before the page may use the microphone: inputs ${JSON.stringify(now.inputs)}, outputs ${JSON.stringify(now.outputs)}`);
  if (!now.scanHidden) {
    check(now.inputs.length === 1 && now.note.includes("麥克風權限"), "without the permission there is only the system default, and a note says why");
    await page(`document.querySelector("#device-scan").click();`);
    await until(`document.querySelector("#device-scan").hidden`, "the devices to be listed after the button");
    now = await lists();
  }
  console.log(`  listed: inputs ${JSON.stringify(now.inputs)}`);
  console.log(`          outputs ${JSON.stringify(now.outputs)}`);
  check(now.inputs.length >= 3 && now.inputs.slice(1).every((name) => name && !/^裝置 \d+$/.test(name)), "the inputs are listed by name, after the system default");
  check(now.outputs.length >= 2 && !now.outputDisabled, "the outputs are listed too, and this browser lets the page choose one");
  check(now.scanHidden && !now.note, "the button and the note are gone once the devices are listed");
  check(now.input === "" && now.output === "", "nothing chosen yet: both on the system default");
  const leftOpen = await page(`return !!engine.mic;`);
  check(!leftOpen, "listing the devices leaves no input open");

  // ---------------------------------------------------------------------------
  console.log("== Choosing devices ==");
  const lastInput = now.inputs.length - 1, lastOutput = now.outputs.length - 1;
  const inputChosen = await choose("#input-device", lastInput);
  check((await page(`return P.inputDeviceId;`)) === inputChosen && inputChosen !== "", `an input chosen before the sound is on is remembered (${now.inputs[lastInput]})`);

  await page(`if (document.querySelector("#mute").getAttribute("aria-pressed") !== "true") document.querySelector("#mute").click();
              document.querySelector("#power").click();`);
  await until(`engine.ctx && engine.ctx.state === "running"`, "the sound engine to run");
  await page(`document.querySelector('.source .chip[data-source="live"]').click();`);
  await until(`engine.mic && P.source === "live"`, "the live input to open");
  let mic = await page(`const s = engine.mic.track.getSettings(); return { id: s.deviceId, label: engine.mic.track.label, live: engine.mic.track.readyState };`);
  check(mic.id === inputChosen && mic.live === "live", `LIVE INPUT opens the chosen input: ${mic.label}`);

  await page(`window.__firstTrack = engine.mic.track;`);
  const otherInput = await choose("#input-device", 1);
  await until(`engine.mic && engine.mic.track !== window.__firstTrack`, "the input to be swapped");
  mic = await page(`const s = engine.mic.track.getSettings(); return { id: s.deviceId, label: engine.mic.track.label, old: window.__firstTrack.readyState, source: P.source, running: engine.ctx.state };`);
  check(mic.id === otherInput && otherInput !== inputChosen, `choosing another input while playing swaps it: ${mic.label}`);
  check(mic.old === "ended" && mic.source === "live" && mic.running === "running", "the first input is closed, the sound keeps running on LIVE INPUT");

  const outputChosen = await choose("#output-device", lastOutput);
  await until(`engine.ctx.sinkId === ${JSON.stringify(outputChosen)}`, "the output to be switched");
  let out = await page(`return { sink: engine.ctx.sinkId, chosen: P.outputDeviceId, running: engine.ctx.state };`);
  check(out.sink === outputChosen && out.chosen === outputChosen && out.running === "running", `choosing an output moves the sound there without stopping (${now.outputs[lastOutput]})`);

  await choose("#output-device", 0);
  await until(`engine.ctx.sinkId === ""`, "the output to go back to the default");
  out = await page(`return { sink: engine.ctx.sinkId, chosen: P.outputDeviceId };`);
  check(out.sink === "" && out.chosen === "", "and back to the system default");
  await choose("#output-device", lastOutput);
  await until(`engine.ctx.sinkId === ${JSON.stringify(outputChosen)}`, "the output to be switched again");

  // ---------------------------------------------------------------------------
  console.log("== Remembered after reloading ==");
  await page(`P.persist();`);
  await reload();
  now = await lists();
  check(now.input === otherInput && now.output === outputChosen, `the lists show the saved choices: ${now.inputs[1]} / ${now.outputs[lastOutput]}`);
  await page(`document.querySelector("#power").click();`);
  await until(`engine.ctx && engine.ctx.state === "running" && engine.mic`, "the engine and the live input to start again");
  const again = await page(`return { sink: engine.ctx.sinkId, mic: engine.mic.track.getSettings().deviceId, source: P.source, mute: values.mute };`);
  check(again.sink === outputChosen, "the engine starts on the saved output");
  check(again.mic === otherInput && again.source === "live", "and LIVE INPUT on the saved input");
  check(again.mute === true, "(still muted)");

  // ---------------------------------------------------------------------------
  console.log("== A saved device that is no longer there ==");
  await page(`P.inputDeviceId = "gone-input"; P.outputDeviceId = "gone-output"; P.persist();`);
  await reload();
  now = await lists();
  check(now.inputs.at(-1).includes("沒有連接") && now.outputs.at(-1).includes("沒有連接") && now.input === "gone-input",
        "the lists keep the missing choices, marked as not connected");
  await page(`document.querySelector("#power").click();`);
  await until(`engine.ctx && engine.ctx.state === "running" && engine.mic`, "the engine and the live input to start with the defaults");
  const fallback = await page(`return { input: P.inputDeviceId, output: P.outputDeviceId, sink: engine.ctx.sinkId, live: engine.mic.track.readyState, source: P.source,
                                        saved: JSON.parse(localStorage.getItem(P.storeKey)) };`);
  check(fallback.input === "" && fallback.live === "live" && fallback.source === "live", "the missing input: LIVE INPUT opens the system default instead");
  check(fallback.output === "" && fallback.sink === "", "the missing output: the sound goes to the system default instead");
  check(fallback.saved.inputDeviceId === "" && fallback.saved.outputDeviceId === "", "and the choices are saved as the system default again");
  now = await lists();
  check(now.input === "" && now.output === "" && !now.inputs.at(-1).includes("沒有連接"), "the lists are back on the system default");

  await page(`document.querySelector("#power").click();`);
  socket.close();
} catch (error) {
  check(false, `the test could not run: ${error.message}`);
} finally {
  await cleanUp();
}

console.log(failures === 0 ? "\nALL CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
