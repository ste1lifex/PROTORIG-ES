'use strict';
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const path = require('node:path');
const si = require('systeminformation');
const execute = promisify(execFile);
// Fixed command, no interpolation or plugin-controlled shell arguments.
const SCRIPT = "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; $ErrorActionPreference='Stop'; @(Get-NetAdapter -Physical | Where-Object Status -eq 'Up' | Get-NetAdapterStatistics | Select-Object Name,ReceivedBytes,SentBytes) | ConvertTo-Json -Compress";

function createNetworkReader({ platform = process.platform, run = execute, now = () => Date.now() } = {}) {
  let previous = new Map();
  return async function read() {
    if (platform !== 'win32') return si.networkStats('*');
    const executable = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
    const { stdout } = await run(executable, ['-NoProfile', '-NonInteractive', '-Command', SCRIPT], {
      windowsHide: true, timeout: 8000, maxBuffer: 262144, encoding: 'utf8'
    });
    const parsed = stdout.trim() ? JSON.parse(stdout.replace(/^\uFEFF/, '')) : [];
    const rows = Array.isArray(parsed) ? parsed : [parsed];
    const at = now(), next = new Map();
    const stats = rows.filter(r => typeof r.Name === 'string' && Number.isFinite(r.ReceivedBytes) && Number.isFinite(r.SentBytes)).map(r => {
      const old = previous.get(r.Name), seconds = old ? (at - old.at) / 1000 : 0;
      const valid = old && seconds > 0 && r.ReceivedBytes >= old.rx && r.SentBytes >= old.tx;
      next.set(r.Name, { at, rx: r.ReceivedBytes, tx: r.SentBytes });
      return { iface: r.Name, rx_sec: valid ? (r.ReceivedBytes - old.rx) / seconds : null, tx_sec: valid ? (r.SentBytes - old.tx) / seconds : null };
    });
    previous = next;
    return stats;
  };
}
module.exports = { createNetworkReader };
