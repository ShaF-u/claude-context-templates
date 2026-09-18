#!/usr/bin/env node
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runHeadless } from './lib/claude-runner.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..');
const cliPath = process.env.CORE_CLI_PATH ?? path.join(repoRoot, 'build', 'Core', 'Debug', 'aistudio_core_cli.exe');
const tasksPath = path.join(__dirname, 'tasks.json');

// Every run below spends real usage against whatever account `claude` is
// logged into (subscription quota, or metered API cost) — this is real
// model traffic, not a simulation. Cap how many tasks run per invocation so
// iterating on the harness doesn't burn the whole suite by accident.
const sample = Number(process.env.BENCH_SAMPLE) || Infinity;
const model = process.env.BENCH_MODEL || undefined; // e.g. "sonnet" — pin for comparable runs across time

if (!existsSync(cliPath)) {
  console.error(`Core CLI not found at ${cliPath}.`);
  console.error('Build it first (see README.md "Core のビルド"), or set CORE_CLI_PATH to an existing binary.');
  process.exit(1);
}

const allTasks = JSON.parse(readFileSync(tasksPath, 'utf8'));
const tasks = allTasks.slice(0, sample);

const NAIVE_TOOLS = 'Read,Grep,Glob';
// "" disables every built-in tool, so the core run can only use what the
// MCP server offers — otherwise the model just reaches for Grep/Read and
// never touches context_retrieve (see claude-runner.mjs).
const CORE_TOOLS = '';

const naiveMcpConfig = { mcpServers: {} };
const coreMcpConfig = {
  mcpServers: {
    'context-reduction-core': { type: 'stdio', command: cliPath, args: ['--mcp'], env: {} },
  },
};

function promptFor(intent) {
  return `Without asking me anything, explain ${intent} in this codebase. ` +
    'Answer in 3-6 sentences and cite the specific files/functions involved. ' +
    'Do not write or run any code, just investigate and answer.';
}

console.log(`Running ${tasks.length}/${allTasks.length} task(s) x 2 real Claude runs each. This costs real usage.\n`);

const results = [];
for (const task of tasks) {
  const prompt = promptFor(task.intent);
  process.stdout.write(`[${task.id}] naive... `);
  const naive = runHeadless({ cwd: repoRoot, prompt, mcpConfig: naiveMcpConfig, tools: NAIVE_TOOLS, model });
  console.log(`${naive.contextTokens}t ($${naive.costUsd.toFixed(3)})`);

  process.stdout.write(`[${task.id}] core...  `);
  const core = runHeadless({ cwd: repoRoot, prompt, mcpConfig: coreMcpConfig, tools: CORE_TOOLS, model });
  console.log(`${core.contextTokens}t ($${core.costUsd.toFixed(3)})`);

  // Three views of the same pair of runs:
  //   cumulative — every turn's context summed (what the API billed).
  //     Grows with the number of turns, so it is noisy run to run.
  //   final — the last turn's context (what the statusline shows).
  //   exploration — final minus the first turn's base (system prompt +
  //     tool schemas). The base is fixed per configuration and can't be
  //     retrieved away, so this is the part retrieval can actually shrink
  //     and the one the 80% target is judged on.
  const pct = (a, b) => (a > 0 && b !== null ? (1 - b / a) * 100 : null);
  const explore = (r) => (r.finalContextTokens !== null && r.baseContextTokens !== null ? r.finalContextTokens - r.baseContextTokens : null);
  results.push({
    id: task.id,
    intent: task.intent,
    naive,
    core,
    reductionPct: pct(naive.contextTokens, core.contextTokens),
    finalReductionPct: pct(naive.finalContextTokens, core.finalContextTokens),
    explorationReductionPct: pct(explore(naive), explore(core)),
  });
}

// ---- report ----
const fmtPct = (v) => (v === null ? 'n/a' : `${v.toFixed(1)}%`);
const header = ['task', 'naive cum/final/calls', 'core cum/final/calls', 'cum', 'final', 'explore'];
const rows = results.map((r) => [
  r.id,
  `${r.naive.contextTokens}/${r.naive.finalContextTokens}/${r.naive.toolCalls}`,
  `${r.core.contextTokens}/${r.core.finalContextTokens}/${r.core.toolCalls}`,
  fmtPct(r.reductionPct),
  fmtPct(r.finalReductionPct),
  fmtPct(r.explorationReductionPct),
]);
const widths = header.map((h, i) => Math.max(h.length, ...rows.map((r) => r[i].length)));
const fmt = (cols) => cols.map((c, i) => c.padEnd(widths[i])).join('  ');

console.log('');
console.log(fmt(header));
console.log(widths.map((w) => '-'.repeat(w)).join('  '));
for (const row of rows) console.log(fmt(row));

function stats(values) {
  const v = values.filter((x) => x !== null).sort((a, b) => a - b);
  if (!v.length) return null;
  const avg = v.reduce((a, b) => a + b, 0) / v.length;
  const median = v.length % 2 === 1 ? v[(v.length - 1) / 2] : (v[v.length / 2 - 1] + v[v.length / 2]) / 2;
  return { avg, median, min: v[0], max: v[v.length - 1], n: v.length };
}
const describe = (label, s) =>
  s === null
    ? `${label}: n/a`
    : `${label} — avg: ${s.avg.toFixed(1)}%  median: ${s.median.toFixed(1)}%  min: ${s.min.toFixed(1)}%  max: ${s.max.toFixed(1)}%  (n=${s.n})`;

const cumulative = stats(results.map((r) => r.reductionPct));
const final = stats(results.map((r) => r.finalReductionPct));
const exploration = stats(results.map((r) => r.explorationReductionPct));
const totalCostUsd = results.reduce((sum, r) => sum + r.naive.costUsd + r.core.costUsd, 0);

console.log('');
console.log(describe('cumulative reduction ', cumulative));
console.log(describe('final-context reduction', final));
console.log(describe('exploration reduction ', exploration));
if (exploration !== null) {
  console.log(`80% target (exploration, avg): ${exploration.avg >= 80 ? 'MET' : 'not met'} (${exploration.avg.toFixed(1)}%)`);
}
console.log(`plan usage this run (list-price equivalent): $${totalCostUsd.toFixed(3)}`);

// ---- persist ----
const outDir = path.join(__dirname, 'results');
mkdirSync(outDir, { recursive: true });
const outFile = path.join(outDir, `${new Date().toISOString().replace(/[:.]/g, '-')}.json`);
writeFileSync(
  outFile,
  JSON.stringify({ ranAt: new Date().toISOString(), model: model ?? 'default', results, stats: { cumulative, final, exploration, totalCostUsd } }, null, 2)
);
console.log(`\nsaved: ${path.relative(repoRoot, outFile)}`);
