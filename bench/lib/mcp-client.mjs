import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

// Minimal client for aistudio_core_cli's --mcp stdio mode: newline-
// delimited JSON-RPC 2.0, no Content-Length framing (that's --lsp only).
export class McpClient {
  constructor(cliPath, cwd) {
    this.cliPath = cliPath;
    this.cwd = cwd;
    this.nextId = 1;
    this.pending = new Map();
    this.stderrChunks = [];
  }

  async start() {
    this.proc = spawn(this.cliPath, ['--mcp'], { cwd: this.cwd, stdio: ['pipe', 'pipe', 'pipe'] });
    this.proc.stderr.on('data', (d) => this.stderrChunks.push(d));
    this.proc.on('exit', (code) => {
      const stderr = Buffer.concat(this.stderrChunks).toString('utf8').slice(0, 2000);
      for (const { reject } of this.pending.values()) {
        reject(new Error(`aistudio_core_cli exited (code ${code}) before responding.\nstderr: ${stderr}`));
      }
      this.pending.clear();
    });
    createInterface({ input: this.proc.stdout }).on('line', (line) => this._onLine(line));

    await this.call('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'context-reduction-bench', version: '0.1.0' },
    });
    this._notify('notifications/initialized', {});
  }

  _onLine(line) {
    if (!line.trim()) return;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      return;
    }
    if (msg.id !== undefined && this.pending.has(msg.id)) {
      const { resolve, reject } = this.pending.get(msg.id);
      this.pending.delete(msg.id);
      if (msg.error) reject(new Error(msg.error.message ?? JSON.stringify(msg.error)));
      else resolve(msg.result);
    }
  }

  call(method, params) {
    const id = this.nextId++;
    const payload = JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n';
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.proc.stdin.write(payload);
    });
  }

  _notify(method, params) {
    this.proc.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
  }

  callTool(name, args) {
    return this.call('tools/call', { name, arguments: args });
  }

  stop() {
    this.proc.stdin.end();
    this.proc.kill();
  }
}
