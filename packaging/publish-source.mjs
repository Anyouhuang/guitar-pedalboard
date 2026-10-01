// Publishes the source code to the GitHub repository (the same one GitHub Pages serves the web version from),
// as one commit made through the GitHub API: no git setup or git settings are needed or touched.
// The built page at the repository's root (index.html, worklet.js, README.md, images/) is left alone;
// web\deploy-github.ps1 takes care of that.
//
// Usage:  node packaging/publish-source.mjs ["commit message"]
// Needs the GitHub CLI logged in: tools\gh\bin\gh.exe (or gh on PATH), see web\deploy-github.ps1.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("..", import.meta.url));
const repoName = "guitar-pedalboard";
const message = process.argv[2] || "Update source code";

const localGh = join(root, "tools", "gh", "bin", "gh.exe");
const ghPath = existsSync(localGh) ? localGh : "gh";
const temp = mkdtempSync(join(tmpdir(), "publish-source-"));

/** Calls the GitHub API; a body is handed over as a file (large JSON doesn't fit on a command line). */
function gh(method, path, body) {
  const args = ["api", "-X", method, path];
  if (body !== undefined) {
    const file = join(temp, "body.json");
    writeFileSync(file, JSON.stringify(body));
    args.push("--input", file);
  }
  const out = execFileSync(ghPath, args, { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  return out.trim() ? JSON.parse(out) : null;
}

// ---- what gets published: repository path -> file contents
const sourceRoots = ["Source", "Tests", "Resources", "docs", "packaging", "web"];
const sourceFiles = ["CMakeLists.txt", "LICENSE", ".gitignore"];
const skip = (path) => path.startsWith("web/dist/") || path.endsWith(".lnk");

const files = new Map();
const walk = (dir) => {
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) walk(full);
    else {
      const path = relative(root, full).split(sep).join("/");
      if (!skip(path)) files.set(path, readFileSync(full));
    }
  }
};
for (const dir of sourceRoots) walk(join(root, dir));
for (const name of sourceFiles) files.set(name, readFileSync(join(root, name)));
// the full guide (this project's README) goes next to the repository's own front-page README, minus the lines that
// point at the author's private claude.ai copy
const guide = readFileSync(join(root, "README.md"), "utf8").split("\n").filter((line) => !line.includes("claude.ai/artifact")).join("\n");
files.set("GUIDE.md", Buffer.from(guide, "utf8"));

/** Files this script owns in the repository: anything else (the built page, README.md, images/) is not touched. */
const managed = (path) => sourceRoots.some((dir) => path.startsWith(`${dir}/`)) || sourceFiles.includes(path) || path === "GUIDE.md";
const blobSha = (bytes) => createHash("sha1").update(`blob ${bytes.length}\0`).update(bytes).digest("hex");

try {
  const owner = gh("GET", "user").login;
  const full = `${owner}/${repoName}`;
  const branch = gh("GET", `repos/${full}`).default_branch;
  const head = gh("GET", `repos/${full}/git/ref/heads/${branch}`).object.sha;
  const headTree = gh("GET", `repos/${full}/git/commits/${head}`).tree.sha;
  const remote = new Map(gh("GET", `repos/${full}/git/trees/${headTree}?recursive=1`).tree
    .filter((e) => e.type === "blob").map((e) => [e.path, e.sha]));

  const changes = [];
  for (const [path, bytes] of [...files].sort(([a], [b]) => a.localeCompare(b))) {
    if (remote.get(path) === blobSha(bytes)) continue; // unchanged
    const blob = gh("POST", `repos/${full}/git/blobs`, { content: bytes.toString("base64"), encoding: "base64" });
    changes.push({ path, mode: "100644", type: "blob", sha: blob.sha });
    console.log(`  ${remote.has(path) ? "update" : "add   "} ${path}`);
  }
  for (const path of remote.keys()) {
    if (managed(path) && !files.has(path)) {
      changes.push({ path, mode: "100644", type: "blob", sha: null });
      console.log(`  delete ${path}`);
    }
  }

  if (changes.length === 0) {
    console.log(`Nothing to publish: ${full} already has this source code.`);
  } else {
    const tree = gh("POST", `repos/${full}/git/trees`, { base_tree: headTree, tree: changes });
    const commit = gh("POST", `repos/${full}/git/commits`, { message, tree: tree.sha, parents: [head] });
    gh("PATCH", `repos/${full}/git/refs/heads/${branch}`, { sha: commit.sha });
    console.log(`Published ${changes.length} change(s) of ${files.size} source files: https://github.com/${full}/commit/${commit.sha}`);
  }
} finally {
  rmSync(temp, { recursive: true, force: true });
}
