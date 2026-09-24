#!/usr/bin/env node
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runHeadless } from './lib/claude-runner.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
// The repository under test. Defaults to this one, but Core is built for
// large projects (README.md), and the reduction ceiling is
// 1 - (core turns / naive turns) -- which rises with how many turns the
// grep-and-read baseline needs. Measuring only here understates the tool
// in the regime it targets.
const repoRoot = path.resolve(process.env.BENCH_REPO ?? path.resolve(__dirname, '..'));
const cliPath = process.env.CORE_CLI_PATH ?? path.join(repoRoot, 'build', 'Core', 'Debug', 'aistudio_core_cli.exe');
const tasksPath = process.env.BENCH_TASKS_FILE ?? path.join(__dirname, 'tasks.json');

// Every run below spends real usage against whatever account `claude` is
// logged into (subscription quota, or metered API cost) — this is real
// model traffic, not a simulation. Cap how many tasks run per invocation so
// iterating on the harness doesn't burn the whole suite by accident.
const sample = Number(process.env.BENCH_SAMPLE) || Infinity;
const onlyIds = process.env.BENCH_TASKS ? process.env.BENCH_TASKS.split(',').map((s) => s.trim()).filter(Boolean) : null;
const model = process.env.BENCH_MODEL || undefined; // e.g. "sonnet" — pin for comparable runs across time
const repeat = Math.max(1, Number(process.env.BENCH_REPEAT) || 1); // runs per side; the median is compared

if (!existsSync(cliPath)) {
  console.error(`Core CLI not found at ${cliPath}.`);
  console.error('Build it first (see README.md "Core のビルド"), or set CORE_CLI_PATH to an existing binary.');
  process.exit(1);
}

const allTasks = JSON.parse(readFileSync(tasksPath, 'utf8'));
const tasks = (onlyIds ? allTasks.filter((t) => onlyIds.includes(t.id)) : allTasks).slice(0, sample);

// A run that dies halfway (the usual cause: the account's usage limit,
// which makes `claude` exit 1) keeps every task it finished. Each pair is
// appended to this file as soon as it completes, and the summary at the
// end is written from the same file, so a partial run still leaves data.
const outDir = path.join(__dirname, 'results');
mkdirSync(outDir, { recursive: true });
const runStamp = new Date().toISOString().replace(/[:.]/g, '-');
const outFile = path.join(outDir, `${runStamp}.json`);
const persist = (results, complete) =>
  writeFileSync(
    outFile,
    JSON.stringify({ ranAt: new Date().toISOString(), model: model ?? 'default', complete, results, stats: summarize(results) }, null, 2)
  );

const NAIVE_TOOLS = 'Read,Grep,Glob';
// "" disables every built-in tool, so the core run can only use what the
// MCP server offers — otherwise the model just reaches for Grep/Read and
// never touches context_retrieve (see claude-runner.mjs).
const CORE_TOOLS = '';

const naiveMcpConfig = { mcpServers: {} };
const coreMcpConfig = {
  mcpServers: {
    'context-reduction-core': {
      type: 'stdio',
      command: cliPath,
      // --config lets the server run against a repository without adding
      // a config file to it (BENCH_CORE_CONFIG); without it Core falls
      // back to that repository's own aistudio.config, and a missing one
      // means an unbudgeted response.
      args: process.env.BENCH_CORE_CONFIG ? ['--mcp', '--config', path.resolve(process.env.BENCH_CORE_CONFIG)] : ['--mcp'],
      env: {},
    },
  },
};

function promptFor(intent) {
  return `Without asking me anything, explain ${intent} in this codebase. ` +
    'Answer in 3-6 sentences and cite the specific files/functions involved. ' +
    'Do not write or run any code, just investigate and answer.';
}

console.log(`Running ${tasks.length}/${allTasks.length} task(s) x ${2 * repeat} real Claude runs each. This costs real usage.\n`);

// The same task run twice differs by up to 3.9x on the naive side and
// 6.5x on the core side (measured over this repository's own result
// history, 2026-09-22): the model's exploration path is not
// deterministic, so a single pair per task measures the run, not the
// change. Each side is run `repeat` times and the MEDIAN run is what
// gets compared -- median rather than mean because the long tail is one
// run where the model kept going, not a shift in the distribution.
function medianRun(runs) {
  const sorted = [...runs].sort((a, b) => a.contextTokens - b.contextTokens);
  return sorted[Math.floor((sorted.length - 1) / 2)];
}

function runSide(label, task, prompt, mcpConfig, tools) {
  const runs = [];
  for (let i = 0; i < repeat; i++) {
    process.stdout.write(`[${task.id}] ${label}${repeat > 1 ? ` ${i + 1}/${repeat}` : ''}... `);
    const run = runHeadless({ cwd: repoRoot, prompt, mcpConfig, tools, model });
    console.log(`${run.contextTokens}t ($${run.costUsd.toFixed(3)})`);
    runs.push(run);
  }
  const median = medianRun(runs);
  return repeat > 1 ? { ...median, runs: runs.map((r) => r.contextTokens) } : median;
}

const results = [];
for (const task of tasks) {
  const prompt = promptFor(task.intent);
  const naive = runSide('naive', task, prompt, naiveMcpConfig, NAIVE_TOOLS);
  const core = runSide('core ', task, prompt, coreMcpConfig, CORE_TOOLS);

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
  persist(results, false);
}

// ---- report ----
function stats(values) {
  const v = values.filter((x) => x !== null).sort((a, b) => a - b);
  if (!v.length) return null;
  const avg = v.reduce((a, b) => a + b, 0) / v.length;
  const median = v.length % 2 === 1 ? v[(v.length - 1) / 2] : (v[v.length / 2 - 1] + v[v.length / 2]) / 2;
  return { avg, median, min: v[0], max: v[v.length - 1], n: v.length };
}

function summarize(results) {
  return {
    cumulative: stats(results.map((r) => r.reductionPct)),
    final: stats(results.map((r) => r.finalReductionPct)),
    exploration: stats(results.map((r) => r.explorationReductionPct)),
    totalCostUsd: results.reduce((sum, r) => sum + r.naive.costUsd + r.core.costUsd, 0),
  };
}

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

const describe = (label, s) =>
  s === null
    ? `${label}: n/a`
    : `${label} — avg: ${s.avg.toFixed(1)}%  median: ${s.median.toFixed(1)}%  min: ${s.min.toFixed(1)}%  max: ${s.max.toFixed(1)}%  (n=${s.n})`;

const { cumulative, final, exploration, totalCostUsd } = summarize(results);
console.log('');
console.log(describe('cumulative reduction ', cumulative));
console.log(describe('final-context reduction', final));
console.log(describe('exploration reduction ', exploration));
if (exploration !== null) {
  console.log(`80% target (exploration, avg): ${exploration.avg >= 80 ? 'MET' : 'not met'} (${exploration.avg.toFixed(1)}%)`);
}
console.log(`plan usage this run (list-price equivalent): $${totalCostUsd.toFixed(3)}`);

persist(results, true);
console.log(`
saved: ${path.relative(repoRoot, outFile)}`);
