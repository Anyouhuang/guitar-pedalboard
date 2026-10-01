// Builds the web version into web/dist:
//   index.html   - complete page for hosting anywhere (GitHub Pages, Netlify, any HTTPS server)
//   worklet.js   - the audio processor (also embedded in the page; this copy is the fallback)
//   artifact.html - the same page without <html>/<head>/<body>, for publishing on claude.ai
//
// Usage: node web/build.mjs
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";

const read = (f) => readFileSync(new URL(`./src/${f}`, import.meta.url), "utf8");
const out = new URL("./dist/", import.meta.url);
mkdirSync(out, { recursive: true });

/** Bundles the DSP modules listed in src/dsp.js (`export * from "./dsp/x.js"`, in that order) into
    `const DSP = (() => { ...; return {every export}; })();`. Each file keeps its own scope, so their
    private helpers can't clash; only exported names are shared (and must be unique). */
function bundleDsp(name) {
  const files = [...read("dsp.js").matchAll(/^export \* from "\.\/(dsp\/[\w-]+\.js)";/gm)].map((m) => m[1]);
  if (!files.length) throw new Error("src/dsp.js lists no modules");
  const all = [];
  let code = "";
  for (const file of files) {
    const source = read(file).replace(/^import\s[\s\S]*?\sfrom\s+"[^"]+";[ \t]*\r?\n/gm, "");
    if (/^\s*import\s/m.test(source)) throw new Error(`${file}: unsupported import statement`);
    const exported = [...source.matchAll(/^export\s+(?:const|let|class|function)\s+([A-Za-z_$][\w$]*)/gm)].map((m) => m[1]);
    for (const e of exported) if (all.includes(e)) throw new Error(`${file}: "${e}" is already exported by another DSP module`);
    all.push(...exported);
    code += `// ---- ${file}\nconst { ${exported.join(", ")} } = (() => {\n${source.replace(/^export\s+/gm, "")}\nreturn { ${exported.join(", ")} };\n})();\n`;
  }
  return `const ${name} = (() => {\n${code}return { ${all.join(", ")} };\n})();\n`;
}

const dsp = bundleDsp("DSP");
const worklet = `${dsp}\n${read("worklet.js")}`;
const app = read("app.js").replace(/^"use strict";\s*/m, "");
if (/<\/script/i.test(worklet + app)) throw new Error("script text contains </script>");

const title = "<title>Guitar Pedalboard</title>";
const fonts = [
  '<link rel="preconnect" href="https://fonts.googleapis.com">',
  '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>',
  '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;600&family=Barlow+Condensed:wght@700;800&family=IBM+Plex+Mono&display=swap">',
].join("\n");
const icon = `<link rel="icon" href="data:image/svg+xml,${encodeURIComponent(
  '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect x="8" y="3" width="48" height="58" rx="9" fill="#e0702a"/>' +
  '<circle cx="22" cy="17" r="7" fill="#161618"/><circle cx="42" cy="17" r="7" fill="#161618"/><circle cx="32" cy="32" r="3" fill="#ff3b30"/>' +
  '<circle cx="32" cy="48" r="9" fill="#cfcfd4"/></svg>')}">`;
const style = `<style>\n${read("style.css")}</style>`;
const body = read("body.html");
const script = `<script>\n${dsp}\nconst WORKLET_SOURCE = ${JSON.stringify(worklet)};\n(() => {\n${app}\n})();\n</script>`;

writeFileSync(new URL("worklet.js", out), worklet);
writeFileSync(new URL("artifact.html", out), `${title}\n${fonts}\n${style}\n${body}\n${script}\n`);
writeFileSync(new URL("index.html", out), `<!doctype html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#131316">
${title}
${icon}
${fonts}
${style}
</head>
<body>
${body}
${script}
</body>
</html>
`);

const kb = (f) => (readFileSync(new URL(f, out)).length / 1024).toFixed(0);
console.log(`web/dist: index.html ${kb("index.html")} KB, worklet.js ${kb("worklet.js")} KB, artifact.html ${kb("artifact.html")} KB`);
