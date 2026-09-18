import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { estimateTokens } from './tokenize.mjs';

// Models the baseline a plain coding agent falls back to without an
// index/retrieval tool: grep for the task's keywords, then read every
// matched file in full. `git grep` also keeps this naturally scoped to
// tracked files (build/ output stays excluded without extra config).
export function naiveBaseline(repoRoot, keywords) {
  const files = new Set();
  for (const kw of keywords) {
    let out;
    try {
      out = execFileSync('git', ['grep', '-l', '-i', '-e', kw, '--', '.'], { cwd: repoRoot, encoding: 'utf8' });
    } catch (e) {
      if (e.status === 1) continue; // git grep: no matches for this keyword
      throw e;
    }
    for (const line of out.split('\n')) {
      if (line.trim()) files.add(line.trim());
    }
  }

  let tokens = 0;
  const details = [];
  for (const rel of files) {
    let content;
    try {
      content = readFileSync(path.join(repoRoot, rel), 'utf8');
    } catch {
      continue;
    }
    const t = estimateTokens(content);
    tokens += t;
    details.push({ file: rel, tokens: t });
  }
  return { tokens, fileCount: files.size, details };
}
