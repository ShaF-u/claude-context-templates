import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// Runs one headless Claude Code turn and returns the REAL token/cost usage
// the CLI itself reports for that run — no external tokenizer, no separate
// Anthropic Console API key. This is the same account (Pro/Max subscription
// or API key) already authenticating the interactive session, just invoked
// non-interactively via `-p`.
//
// `--strict-mcp-config` + `--mcp-config` fully replace whatever MCP servers
// the project's own .mcp.json would otherwise auto-connect, so passing
// `{ mcpServers: {} }` gives a clean "no MCP" baseline and passing the
// project's real config gives the "with Core MCP" run — including the fixed
// token cost of the tool definitions themselves, since that's baked into
// whatever the model billed for that turn.
// `tools` is passed to `--tools`, which sets the built-in tool set itself
// ("" = none, so only MCP tools remain). `--allowedTools` is NOT used: it is
// a permission allowlist, and under bypassPermissions it leaves every
// built-in tool available — found the hard way (2026-09-18) when a "core"
// run's transcript showed Grep/Read calls and zero context_retrieve calls.
//
// MCP_TIMEOUT: Claude Code's default MCP connect timeout is 30s, and Core
// takes ~25s to build its index before answering `initialize`. Without the
// override the server sometimes misses the window, the run silently
// proceeds with no MCP tools, and "core" degrades into a second naive run.
export function runHeadless({ cliBin = 'claude', cwd, prompt, mcpConfig, tools, model, timeoutMs = 600000 }) {
  const args = [
    '-p', prompt,
    '--output-format', 'json',
    '--strict-mcp-config',
    '--mcp-config', JSON.stringify(mcpConfig),
    '--permission-mode', 'bypassPermissions',
    '--tools', tools ?? 'default',
  ];
  if (model) args.push('--model', model);

  const res = spawnSync(cliBin, args, {
    cwd,
    encoding: 'utf8',
    maxBuffer: 1024 * 1024 * 64,
    timeout: timeoutMs,
    env: { ...process.env, MCP_TIMEOUT: '120000' },
  });

  if (res.error) throw res.error;
  if (res.status !== 0) {
    throw new Error(`claude exited ${res.status}: ${(res.stderr || '').slice(0, 2000)}`);
  }

  let result;
  try {
    result = JSON.parse(res.stdout);
  } catch (e) {
    throw new Error(`could not parse claude --output-format json output: ${e.message}\n${res.stdout.slice(0, 500)}`);
  }
  if (result.is_error) {
    throw new Error(`claude run reported an error: ${result.result || result.subtype}`);
  }
  return { ...summarizeUsage(result), ...summarizeTranscript(cwd, result.session_id) };
}

// Per-turn figures the aggregate result doesn't carry, read from the
// session's own transcript (~/.claude/projects/<cwd slug>/<session>.jsonl):
//   finalContextTokens — the last turn's context (input + cache create +
//     cache read), i.e. exactly what the statusline shows at the end of
//     the session. Unlike the cumulative sum it does not grow with the
//     number of turns, so it is the fairer "how much context did this
//     cost" number.
//   baseContextTokens — the first turn's context: system prompt + tool
//     schemas, before any investigation. What the run cannot get below.
//   toolCalls — how many tool invocations the model made.
function summarizeTranscript(cwd, sessionId) {
  const empty = { finalContextTokens: null, baseContextTokens: null, toolCalls: null };
  if (!sessionId) return empty;
  const slug = path.resolve(cwd).replace(/[^A-Za-z0-9]/g, '-');
  const file = path.join(os.homedir(), '.claude', 'projects', slug, `${sessionId}.jsonl`);
  let text;
  try {
    text = readFileSync(file, 'utf8');
  } catch {
    return empty;
  }
  let first = null;
  let last = null;
  let toolCalls = 0;
  for (const line of text.split('\n')) {
    let m;
    try { m = JSON.parse(line); } catch { continue; }
    if (m.type !== 'assistant' || !m.message) continue;
    const u = m.message.usage;
    if (u) {
      const ctx = (u.input_tokens || 0) + (u.cache_creation_input_tokens || 0) + (u.cache_read_input_tokens || 0);
      if (first === null) first = ctx;
      last = ctx;
    }
    for (const c of m.message.content || []) if (c.type === 'tool_use') toolCalls++;
  }
  return { finalContextTokens: last, baseContextTokens: first, toolCalls };
}

// "contextTokens" mirrors the statusline's own formula (input + cache
// creation + cache read), so the two numbers stay comparable at a glance.
//
// Summed across every entry of `modelUsage` rather than just the top-level
// `usage` field: `usage` is main-thread-only, while `modelUsage` carries one
// entry per model actually billed during the run — including subagent
// models (e.g. an Explore subagent spawned via the Task tool runs on its own
// model and shows up there). Reading `modelUsage` is how subagent context
// consumption gets counted instead of silently dropped.
function summarizeUsage(result) {
  const modelUsage = result.modelUsage || {};
  let contextTokens = 0;
  let outputTokens = 0;
  let costUsd = 0;
  const byModel = {};

  for (const [name, u] of Object.entries(modelUsage)) {
    const ctx = (u.inputTokens || 0) + (u.cacheCreationInputTokens || 0) + (u.cacheReadInputTokens || 0);
    contextTokens += ctx;
    outputTokens += u.outputTokens || 0;
    costUsd += u.costUSD || 0;
    byModel[name] = { contextTokens: ctx, outputTokens: u.outputTokens || 0, costUsd: u.costUSD || 0 };
  }

  return {
    contextTokens,
    outputTokens,
    costUsd,
    subagentsSpawned: result.subagent_stats?.spawned ?? 0,
    numTurns: result.num_turns ?? null,
    sessionId: result.session_id ?? null,
    byModel,
  };
}
