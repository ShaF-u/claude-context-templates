#!/usr/bin/env node
// Measures the size of one context_retrieve response per task, without running
// Claude (free). Talks JSON-RPC to the Core CLI directly, so config changes
// such as scan.extra_ignore_patterns can be checked before a paid bench run.
//
//   node bench/payload.mjs                                   # tasks.json, ./aistudio.config
//   BENCH_CORE_CONFIG=bench/configs/gameengine.config BENCH_TASKS_FILE=bench/tasks-gameengine.json node bench/payload.mjs
//
// Prints total chars and the largest items (by "file:" header lines in the
// text-format response) per task. The intent is sent verbatim; the model
// usually shortens it, so treat the numbers as relative, not absolute.
// PAYLOAD_DUMP_DIR=<dir> also writes each full response to <dir>/<task id>.txt.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cli = process.env.CORE_CLI_PATH || path.join(root, 'build/Core/Debug/aistudio_core_cli.exe');
const tasks = JSON.parse(fs.readFileSync(process.env.BENCH_TASKS_FILE || path.join(root, 'bench/tasks.json'), 'utf8'));
const args = ['--mcp'];
if (process.env.BENCH_CORE_CONFIG) args.push('--config', path.resolve(process.env.BENCH_CORE_CONFIG));

const proc = spawn(cli, args, { cwd: root });
proc.stderr.on('data', () => {});
const pending = new Map();
let buf = '';
let nextId = 1;
proc.stdout.on('data', (d) => {
  buf += d.toString('utf8');
  let i;
  while ((i = buf.indexOf('\n')) !== -1) {
    const line = buf.slice(0, i);
    buf = buf.slice(i + 1);
    if (!line.trim()) continue;
    const msg = JSON.parse(line);
    pending.get(msg.id)?.(msg);
    pending.delete(msg.id);
  }
});
const call = (method, params) => new Promise((resolve) => {
  const id = nextId++;
  pending.set(id, resolve);
  proc.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
});

await call('initialize', { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'payload', version: '1' } });
let grand = 0;
for (const t of tasks) {
  const res = await call('tools/call', { name: 'context_retrieve', arguments: { intent: t.intent } });
  const text = (res.result?.content || []).map((c) => c.text || '').join('');
  grand += text.length;
  if (process.env.PAYLOAD_DUMP_DIR) fs.writeFileSync(path.join(process.env.PAYLOAD_DUMP_DIR, `${t.id}.txt`), text);
  // Split on lines that start an item and attribute chars to its source path.
  const bySource = new Map();
  let current = '(header)';
  for (const line of text.split('\n')) {
    const m = line.match(/([A-Za-z0-9_./\\-]+\.[A-Za-z0-9]+):\d+/);
    if (/^(#+ |\[|-{3}|={3})/.test(line) && m) current = m[1];
    bySource.set(current, (bySource.get(current) || 0) + line.length + 1);
  }
  const top = [...bySource].sort((a, b) => b[1] - a[1]).slice(0, 5)
    .map(([k, v]) => `${k} ${(v / 1024).toFixed(1)}K`).join(', ');
  console.log(`${t.id.padEnd(28)} ${String(text.length).padStart(7)} chars  ${top}`);
}
console.log(`${'TOTAL'.padEnd(28)} ${String(grand).padStart(7)} chars`);
proc.kill();
