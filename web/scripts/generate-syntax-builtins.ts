/**
 * Generate syntax builtin names from builtins.hpp
 *
 * This script parses akkado/include/akkado/builtins.hpp to extract:
 * - All keys from BUILTIN_FUNCTIONS map
 * - All keys from BUILTIN_ALIASES map
 *
 * And generates:
 * - web/src/lib/editor/generated/syntax-builtins.ts
 *
 * Usage: bun run scripts/generate-syntax-builtins.ts
 */

import { readFileSync, writeFileSync, mkdirSync, existsSync } from "fs";
import { resolve } from "path";

const ROOT_DIR = resolve(import.meta.dir, "../..");
const BUILTINS_HPP = resolve(ROOT_DIR, "akkado/include/akkado/builtins.hpp");
const OUTPUT_DIR = resolve(ROOT_DIR, "web/src/lib/editor/generated");
const OUTPUT_FILE = resolve(OUTPUT_DIR, "syntax-builtins.ts");

function parseBuiltinFunctions(source: string): string[] {
  const names: string[] = [];

  const mapMatch = source.match(/BUILTIN_FUNCTIONS\s*=\s*\{([\s\S]*?)\n\};/);
  if (!mapMatch) {
    throw new Error("Could not find BUILTIN_FUNCTIONS in builtins.hpp");
  }

  const mapBody = mapMatch[1];
  const entryRegex = /\{"(\w+)",\s*\{cedar::Opcode/g;
  let match;

  while ((match = entryRegex.exec(mapBody)) !== null) {
    names.push(match[1]);
  }

  return names.sort();
}

function parseBuiltinAliases(source: string): string[] {
  const names: string[] = [];

  const mapMatch = source.match(/BUILTIN_ALIASES\s*=\s*\{([\s\S]*?)\n\};/);
  if (!mapMatch) {
    throw new Error("Could not find BUILTIN_ALIASES in builtins.hpp");
  }

  const mapBody = mapMatch[1];
  const entryRegex = /\{"(\w+)",\s*"/g;
  let match;

  while ((match = entryRegex.exec(mapBody)) !== null) {
    names.push(match[1]);
  }

  return names.sort();
}

function generate(builtins: string[], aliases: string[]): string {
  const builtinEntries = builtins.map((n) => `\t'${n}'`).join(",\n");
  const aliasEntries = aliases.map((n) => `\t'${n}'`).join(",\n");

  return `// AUTO-GENERATED — do not edit. Run: cd web && bun run build:syntax
export const BUILTIN_NAMES: ReadonlySet<string> = new Set([
${builtinEntries}
]);

export const ALIAS_NAMES: ReadonlySet<string> = new Set([
${aliasEntries}
]);
`;
}

function main() {
  console.log("Generating syntax builtins...");

  console.log(`Reading ${BUILTINS_HPP}...`);
  const source = readFileSync(BUILTINS_HPP, "utf-8");

  console.log("Parsing BUILTIN_FUNCTIONS...");
  const builtins = parseBuiltinFunctions(source);
  console.log(`  Found ${builtins.length} builtin function names`);

  console.log("Parsing BUILTIN_ALIASES...");
  const aliases = parseBuiltinAliases(source);
  console.log(`  Found ${aliases.length} alias names`);

  const output = generate(builtins, aliases);

  if (!existsSync(OUTPUT_DIR)) {
    console.log(`Creating directory ${OUTPUT_DIR}...`);
    mkdirSync(OUTPUT_DIR, { recursive: true });
  }

  console.log(`Writing ${OUTPUT_FILE}...`);
  writeFileSync(OUTPUT_FILE, output);

  console.log("Done!");
  console.log(`  ${builtins.length} builtins + ${aliases.length} aliases`);
}

main();
