/**
 * Generate full builtin metadata as JSON from builtins.hpp.
 *
 * This is a richer companion to `generate-syntax-builtins.ts`: in addition to
 * names, it captures parameter signatures, default values, and descriptions
 * so editors (notably the VS Code extension) can show signature help and
 * autocomplete details without needing the WASM module.
 *
 * Output shape:
 *   {
 *     "functions": {
 *       "<name>": {
 *         "params": [{"name": "...", "required": true|false, "default"?: <number>}],
 *         "description": "...",
 *         "requires_state": true|false
 *       }
 *     },
 *     "aliases": {"<from>": "<to>"},
 *     "keywords": ["true", "false", "fn", "as", "match", "post"]
 *   }
 *
 * Usage: bun run scripts/generate-builtins-json.ts
 */
import { readFileSync, writeFileSync, mkdirSync, existsSync } from "fs";
import { resolve } from "path";

const ROOT_DIR = resolve(import.meta.dir, "../..");
const BUILTINS_HPP = resolve(ROOT_DIR, "akkado/include/akkado/builtins.hpp");
const OUTPUT_DIR = resolve(ROOT_DIR, "web/static/generated");
const OUTPUT_FILE = resolve(OUTPUT_DIR, "builtins.json");

// Hardcoded keyword set — mirrors web/src/lib/editor/akkado-language.ts:5.
// Kept in lock-step manually; small enough that codegen is overkill.
const KEYWORDS = ["true", "false", "fn", "as", "match", "post"];

interface BuiltinParam {
  name: string;
  required: boolean;
  default?: number;
}

interface BuiltinFunction {
  params: BuiltinParam[];
  description: string;
  requires_state: boolean;
}

/**
 * Locate the body of a top-level map literal in builtins.hpp.
 * Walks brace depth to find the matching `};` at depth 0.
 */
function extractMapBody(source: string, mapName: string): string {
  const opener = new RegExp(
    `${mapName}\\s*=\\s*\\{`
  ).exec(source);
  if (!opener) {
    throw new Error(`Could not find ${mapName} in builtins.hpp`);
  }
  const start = opener.index + opener[0].length;
  let depth = 1;
  for (let i = start; i < source.length; ++i) {
    const c = source[i];
    if (c === "{") depth++;
    else if (c === "}") {
      depth--;
      if (depth === 0) {
        return source.slice(start, i);
      }
    }
  }
  throw new Error(`Unterminated ${mapName} map`);
}

/**
 * Walk a map body and yield each top-level entry as the raw text between its
 * outermost `{...}`. Skips strings/chars to avoid counting braces inside them.
 */
function* iterEntries(body: string): Generator<{ raw: string; bodyStart: number }> {
  let i = 0;
  while (i < body.length) {
    // Find next entry opener: `{`
    while (i < body.length && body[i] !== "{") i++;
    if (i >= body.length) return;
    const start = i;
    let depth = 0;
    let inString = false;
    let inChar = false;
    for (; i < body.length; ++i) {
      const c = body[i];
      if (inString) {
        if (c === "\\" && i + 1 < body.length) { i++; continue; }
        if (c === '"') inString = false;
        continue;
      }
      if (inChar) {
        if (c === "\\" && i + 1 < body.length) { i++; continue; }
        if (c === "'") inChar = false;
        continue;
      }
      if (c === '"') { inString = true; continue; }
      if (c === "'") { inChar = true; continue; }
      if (c === "{") depth++;
      else if (c === "}") {
        depth--;
        if (depth === 0) {
          yield { raw: body.slice(start, i + 1), bodyStart: start };
          i++;
          break;
        }
      }
    }
  }
}

/**
 * Parse a single BUILTIN_FUNCTIONS entry of the form:
 *   {"name", {cedar::Opcode::FOO, INPUT, OPTIONAL, BOOL,
 *             {"p1", "p2", "", "", "", ""},
 *             {NAN, 0.5f, NAN, NAN, NAN},
 *             "Description"...}}
 *
 * Tolerates trailing fields (channel signatures, inst_rate, etc.) by reading
 * only the first 7 positional arguments.
 */
function parseBuiltinEntry(raw: string): { name: string; info: BuiltinFunction } | null {
  // Strip outermost `{` `}`
  const inner = raw.trim().replace(/^\{/, "").replace(/\}$/, "");

  // Name: leading "name"
  const nameMatch = inner.match(/^\s*"([^"]+)"\s*,\s*\{/);
  if (!nameMatch) return null;
  const name = nameMatch[1];

  // Now positioned right after `{` of the BuiltinInfo struct literal.
  const infoStart = nameMatch.index! + nameMatch[0].length;
  // Find matching `}` of the BuiltinInfo block
  let depth = 1;
  let infoEnd = -1;
  let inString = false;
  let inChar = false;
  for (let i = infoStart; i < inner.length; ++i) {
    const c = inner[i];
    if (inString) {
      if (c === "\\" && i + 1 < inner.length) { i++; continue; }
      if (c === '"') inString = false;
      continue;
    }
    if (inChar) {
      if (c === "\\" && i + 1 < inner.length) { i++; continue; }
      if (c === "'") inChar = false;
      continue;
    }
    if (c === '"') { inString = true; continue; }
    if (c === "'") { inChar = true; continue; }
    if (c === "{") depth++;
    else if (c === "}") {
      depth--;
      if (depth === 0) { infoEnd = i; break; }
    }
  }
  if (infoEnd === -1) return null;
  const infoBody = inner.slice(infoStart, infoEnd);

  // Walk top-level fields separated by commas at depth 0.
  const fields: string[] = [];
  let cur = "";
  let d = 0;
  let s = false;
  let ch = false;
  for (let i = 0; i < infoBody.length; ++i) {
    const c = infoBody[i];
    if (s) {
      cur += c;
      if (c === "\\" && i + 1 < infoBody.length) { cur += infoBody[++i]; continue; }
      if (c === '"') s = false;
      continue;
    }
    if (ch) {
      cur += c;
      if (c === "\\" && i + 1 < infoBody.length) { cur += infoBody[++i]; continue; }
      if (c === "'") ch = false;
      continue;
    }
    if (c === '"') { s = true; cur += c; continue; }
    if (c === "'") { ch = true; cur += c; continue; }
    if (c === "{") d++;
    else if (c === "}") d--;
    if (c === "," && d === 0) {
      fields.push(cur.trim());
      cur = "";
    } else {
      cur += c;
    }
  }
  if (cur.trim()) fields.push(cur.trim());

  // Expect at least: opcode, input_count, optional_count, requires_state,
  //                  param_names, defaults, description
  if (fields.length < 7) return null;

  const inputCount = parseInt(fields[1], 10);
  const optionalCount = parseInt(fields[2], 10);
  const requiresState = fields[3] === "true";
  if (Number.isNaN(inputCount) || Number.isNaN(optionalCount)) return null;

  const paramNames = parseStringArray(fields[4]);
  const defaults = parseFloatArray(fields[5]);
  const description = parseStringLiteral(fields[6]);

  // Build the params list: required up to inputCount, optional after.
  // defaults[i] applies to param at index inputCount + i.
  const totalParams = inputCount + optionalCount;
  const params: BuiltinParam[] = [];
  for (let i = 0; i < totalParams; ++i) {
    const pname = paramNames[i] ?? "";
    if (!pname) break;  // empty = end of declared params
    const param: BuiltinParam = {
      name: pname,
      required: i < inputCount,
    };
    if (i >= inputCount) {
      const dIdx = i - inputCount;
      const dv = defaults[dIdx];
      if (dv !== undefined && !Number.isNaN(dv)) {
        param.default = dv;
      }
    }
    params.push(param);
  }

  return {
    name,
    info: { params, description, requires_state: requiresState },
  };
}

function parseStringArray(text: string): string[] {
  // text looks like: {"a", "b", "", "", "", ""}
  const inner = text.trim().replace(/^\{/, "").replace(/\}$/, "");
  const out: string[] = [];
  const re = /"((?:[^"\\]|\\.)*)"/g;
  let m;
  while ((m = re.exec(inner)) !== null) {
    out.push(m[1]);
  }
  return out;
}

function parseFloatArray(text: string): number[] {
  // text looks like: {NAN, 0.5f, NAN, ...} or with cast: static_cast<float>(...)
  const inner = text.trim().replace(/^\{/, "").replace(/\}$/, "");
  return inner
    .split(",")
    .map((tok) => tok.trim())
    .filter((tok) => tok.length > 0)
    .map((tok) => {
      // Strip trailing 'f' suffix from floats
      const cleaned = tok.replace(/f$/, "");
      if (cleaned === "NAN" || cleaned.endsWith("NAN")) return NaN;
      const n = Number(cleaned);
      return Number.isNaN(n) ? NaN : n;
    });
}

function parseStringLiteral(text: string): string {
  const m = text.trim().match(/^"((?:[^"\\]|\\.)*)"/);
  return m ? m[1] : "";
}

function parseBuiltinFunctions(source: string): Record<string, BuiltinFunction> {
  const body = extractMapBody(source, "BUILTIN_FUNCTIONS");
  const out: Record<string, BuiltinFunction> = {};
  for (const { raw } of iterEntries(body)) {
    const parsed = parseBuiltinEntry(raw);
    if (parsed) {
      out[parsed.name] = parsed.info;
    }
  }
  return out;
}

function parseBuiltinAliases(source: string): Record<string, string> {
  const body = extractMapBody(source, "BUILTIN_ALIASES");
  const out: Record<string, string> = {};
  // Aliases are simpler: {"from", "to"}
  const re = /\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}/g;
  let m;
  while ((m = re.exec(body)) !== null) {
    out[m[1]] = m[2];
  }
  return out;
}

function main() {
  console.log(`Reading ${BUILTINS_HPP}...`);
  const source = readFileSync(BUILTINS_HPP, "utf-8");

  console.log("Parsing BUILTIN_FUNCTIONS...");
  const functions = parseBuiltinFunctions(source);
  const fnCount = Object.keys(functions).length;
  console.log(`  Parsed ${fnCount} functions`);

  console.log("Parsing BUILTIN_ALIASES...");
  const aliases = parseBuiltinAliases(source);
  const aliasCount = Object.keys(aliases).length;
  console.log(`  Parsed ${aliasCount} aliases`);

  if (!existsSync(OUTPUT_DIR)) {
    mkdirSync(OUTPUT_DIR, { recursive: true });
  }

  const output = {
    functions,
    aliases,
    keywords: KEYWORDS,
  };

  // Pretty-print so the file is readable in diffs.
  writeFileSync(OUTPUT_FILE, JSON.stringify(output, null, 2));
  console.log(`Wrote ${OUTPUT_FILE}`);
  console.log(`  ${fnCount} functions, ${aliasCount} aliases, ${KEYWORDS.length} keywords`);
}

main();
