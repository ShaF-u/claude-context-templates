#!/usr/bin/env node
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { McpClient } from './lib/mcp-client.mjs';
import { naiveBaseline } from './lib/naive-baseline.mjs';
import { estimateTokens } from './lib/tokenize.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..');
const cliPath = process.env.CORE_CLI_PATH ?? path.join(repoRoot, 'build', 'Core', 'Debug', 'aistudio_core_cli.exe');
const tasksPath = path.join(__dirname, 'tasks.json');

if (!existsSync(cliPath)) {
  console.error(`Core CLI not found at ${cliPath}.`);
  console.error('Build it first (see README.md "Core のビルド"), or set CORE_CLI_PATH to an existing binary.');
  process.exit(1);
}

const tasks = JSON.parse(readFileSync(tasksPath, 'utf8'));

const client = new McpClient(cliPath, repoRoot);
await client.start();

const results = [];
for (const task of tasks) {
  const baseline = naiveBaseline(repoRoot, task.keywords);

  const retrieveResult = await client.callTool('context_retrieve', { intent: task.intent });
  const coreTokens = estimateTokens(JSON.stringify(retrieveResult));

  const reductionPct = baseline.tokens > 0 ? (1 - coreTokens / baseline.tokens) * 100 : null;

  results.push({
    id: task.id,
    intent: task.intent,
    naiveTokens: baseline.tokens,
    naiveFileCount: baseline.fileCount,
    coreTokens,
    reductionPct,
  });
}

client.stop();

// ---- report ----
const header = ['task', 'naive(files/tokens)', 'core(tokens)', 'reduction'];
const rows = results.map((r) => [
  r.id,
  `${r.naiveFileCount}f / ${r.naiveTokens}t`,
  `${r.coreTokens}t`,
  r.reductionPct === null ? 'n/a' : `${r.reductionPct.toFixed(1)}%`,
]);
const widths = header.map((h, i) => Math.max(h.length, ...rows.map((r) => r[i].length)));
const fmt = (cols) => cols.map((c, i) => c.padEnd(widths[i])).join('  ');

console.log(fmt(header));
console.log(widths.map((w) => '-'.repeat(w)).join('  '));
for (const row of rows) console.log(fmt(row));

const validReductions = results.filter((r) => r.reductionPct !== null).map((r) => r.reductionPct).sort((a, b) => a - b);
const avg = validReductions.length ? validReductions.reduce((a, b) => a + b, 0) / validReductions.length : null;
const min = validReductions.length ? validReductions[0] : null;
const max = validReductions.length ? validReductions[validReductions.length - 1] : null;
const median = validReductions.length
  ? validReductions.length % 2 === 1
    ? validReductions[(validReductions.length - 1) / 2]
    : (validReductions[validReductions.length / 2 - 1] + validReductions[validReductions.length / 2]) / 2
  : null;

console.log('');
if (avg === null) {
  console.log('reduction: n/a (no task produced a naive baseline)');
} else {
  console.log(`reduction — avg: ${avg.toFixed(1)}%  median: ${median.toFixed(1)}%  min: ${min.toFixed(1)}%  max: ${max.toFixed(1)}%  (n=${validReductions.length})`);
}

// ---- persist ----
const outDir = path.join(__dirname, 'results');
mkdirSync(outDir, { recursive: true });
const outFile = path.join(outDir, `${new Date().toISOString().replace(/[:.]/g, '-')}.json`);
writeFileSync(
  outFile,
  JSON.stringify({ ranAt: new Date().toISOString(), results, stats: { avg, median, min, max, n: validReductions.length } }, null, 2)
);
console.log(`\nsaved: ${path.relative(repoRoot, outFile)}`);
