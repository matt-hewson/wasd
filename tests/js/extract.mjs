// Pulls named functions and constants out of a page template's <script>
// so the tests run the page's real code without a browser. live.html wraps
// its script in a closure, so it can't simply be imported; instead each
// definition is cut out by matching brackets (skipping strings, template
// literals, regex literals and comments) and the pieces are evaluated
// together, in the order given, in a fresh function scope.
//
//   const { derive, vagueLine } = load("templates/live.html", ["esc", ..., "derive"]);
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

const REGEX_BEFORE = new Set(["(", ",", "=", ":", "[", "!", "&", "|", "?", "{", "}", ";", "+", "-", "*", "%", "<", ">", "~", "^", "\n"]);

// Index just past the end of the definition starting at `from`: a
// function's closing brace, or a const's terminating semicolon.
function definitionEnd(src, from, isFunction) {
  const stack = [];
  let i = from, lastSignificant = "\n", seenBody = false;
  while (i < src.length) {
    const c = src[i], n = src[i + 1];
    if (c === "/" && n === "/") { i = src.indexOf("\n", i); if (i < 0) return src.length; continue; }
    if (c === "/" && n === "*") { i = src.indexOf("*/", i + 2) + 2; continue; }
    if (c === '"' || c === "'") {
      for (i++; src[i] !== c; i++) if (src[i] === "\\") i++;
      i++; lastSignificant = c; continue;
    }
    if (c === "`") { i = templateEnd(src, i + 1); lastSignificant = "`"; continue; }
    if (c === "/" && (REGEX_BEFORE.has(lastSignificant) || /\b(return|typeof|case)$/.test(src.slice(Math.max(0, i - 7), i).trimEnd()))) {
      let inClass = false;
      for (i++; ; i++) {
        if (src[i] === "\\") { i++; continue; }
        if (src[i] === "[") inClass = true;
        else if (src[i] === "]") inClass = false;
        else if (src[i] === "/" && !inClass) break;
      }
      i++;
      while (/[a-z]/i.test(src[i])) i++;  // flags
      lastSignificant = "/"; continue;
    }
    if (c === "(" || c === "[" || c === "{") { stack.push(c); if (c === "{") seenBody = true; }
    else if (c === ")" || c === "]" || c === "}") {
      stack.pop();
      if (isFunction && c === "}" && seenBody && stack.length === 0) return i + 1;
    } else if (c === ";" && !isFunction && stack.length === 0) return i + 1;
    if (!/\s/.test(c)) lastSignificant = c;
    i++;
  }
  throw new Error("unterminated definition at " + from);
}

function templateEnd(src, i) {
  for (; i < src.length; i++) {
    if (src[i] === "\\") { i++; continue; }
    if (src[i] === "`") return i + 1;
    if (src[i] === "$" && src[i + 1] === "{") {
      // Skip the ${...} expression, which may itself hold strings and templates.
      let depth = 1;
      for (i += 2; depth; i++) {
        const c = src[i];
        if (c === "`") { i = templateEnd(src, i + 1) - 1; continue; }
        if (c === '"' || c === "'") { for (i++; src[i] !== c; i++) if (src[i] === "\\") i++; continue; }
        if (c === "{") depth++;
        else if (c === "}") depth--;
      }
      i--;
    }
  }
  throw new Error("unterminated template literal");
}

export function definition(src, name) {
  const re = new RegExp(`(?:^|[^\\w.$])(function\\s+${name}\\s*\\(|(?:const|let)\\s+${name}\\s*=)`, "m");
  const m = re.exec(src);
  if (!m) throw new Error(`no definition of ${name}`);
  const start = m.index + m[0].indexOf(m[1]);
  return src.slice(start, definitionEnd(src, start, m[1].startsWith("function")));
}

export function scriptOf(relPath) {
  const html = readFileSync(resolve(ROOT, relPath), "utf8");
  const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]);
  if (!scripts.length) throw new Error(`no <script> in ${relPath}`);
  return scripts.join("\n");
}

// Evaluates the named definitions (dependencies first) and returns them.
export function load(relPath, names, extraGlobals = {}) {
  const src = scriptOf(relPath);
  const body = names.map(n => definition(src, n)).join("\n");
  const keys = Object.keys(extraGlobals);
  return new Function(...keys, `"use strict";\n${body}\nreturn { ${names.join(", ")} };`)(...keys.map(k => extraGlobals[k]));
}
