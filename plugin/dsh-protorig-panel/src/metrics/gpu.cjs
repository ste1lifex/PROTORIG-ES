'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const si = require('systeminformation');
const { gpuVendor } = require('./gpu-vendor.cjs');
const execute = promisify(execFile);
const MIB = 1024 * 1024;
const OPTIONS = { windowsHide: true, timeout: 5000, maxBuffer: 1048576, encoding: 'utf8' };
const NVIDIA_FIELDS = ['index', 'uuid', 'name', 'utilization.gpu', 'memory.used', 'memory.total', 'temperature.gpu'];
const WDDM_SCRIPT = "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; $ErrorActionPreference='Stop'; $a=@(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory | Select-Object Name,DedicatedUsage,SharedUsage); $e=@(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine | Select-Object Name,UtilizationPercentage); @{adapters=$a;engines=$e} | ConvertTo-Json -Depth 4 -Compress";
function number(value) {
  if (value === null || value === undefined || String(value).trim() === '') return null;
  const n = Number(value); return Number.isFinite(n) && n >= 0 ? n : null;
}
function percent(value) { const n = number(value); return n === null ? null : Math.min(100, n); }
function csvRow(line) {
  const cells = []; let cell = '', quoted = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c === '"') { if (quoted && line[i + 1] === '"') { cell += '"'; i++; } else quoted = !quoted; }
    else if (c === ',' && !quoted) { cells.push(cell.trim()); cell = ''; }
    else cell += c;
  }
  if (quoted) throw new Error('Malformed GPU CSV');
  cells.push(cell.trim()); return cells;
}
function parseNvidia(stdout) {
  return stdout.trim().split(/\r?\n/).filter(Boolean).map(line => {
    const row = csvRow(line); if (row.length !== NVIDIA_FIELDS.length) throw new Error('Unexpected GPU CSV fields');
    const used = number(row[4]), total = number(row[5]);
    return {
      id: row[1], name: row[2], vendor: 'nvidia', source: 'nvidia-smi', gpuPercent: percent(row[3]),
      memoryUsedBytes: used === null ? null : used * MIB,
      memoryTotalBytes: total > 0 ? total * MIB : null,
      memoryPercent: used !== null && total > 0 ? Math.min(100, used / total * 100) : null,
      temperatureCelsius: number(row[6]), note: total > 0 ? null : '驱动未提供显存总容量'
    };
  });
}
function parseWddm(data) {
  const engines = new Map();
  for (const row of data.engines || []) {
    const match = row.Name?.match(/(luid_0x[0-9a-f]+_0x[0-9a-f]+_phys_\d+)_eng_(\d+)/i);
    const usage = number(row.UtilizationPercentage);
    if (match && usage !== null) {
      const group = engines.get(match[1]) || new Map();
      group.set(match[2], (group.get(match[2]) || 0) + usage); engines.set(match[1], group);
    }
  }
  return (data.adapters || []).map((row, index) => {
    const group = engines.get(row.Name);
    return {
      id: row.Name, name: `Windows GPU ${index + 1}`, source: 'windows-wddm',
      gpuPercent: group?.size ? Math.min(100, Math.max(...group.values())) : null,
      memoryUsedBytes: number(row.DedicatedUsage), memoryTotalBytes: null, memoryPercent: null,
      sharedMemoryUsedBytes: number(row.SharedUsage), temperatureCelsius: null,
      note: 'Windows 专用显存计数；此接口不提供可靠总容量'
    };
  }).sort((a, b) => (b.memoryUsedBytes || 0) - (a.memoryUsedBytes || 0));
}
function createGpuReader({ run = execute, platform = process.platform, graphics = () => si.graphics(), now = () => Date.now() } = {}) {
  const root = process.env.SystemRoot || 'C:\\Windows';
  const candidates = platform === 'win32' ? [path.join(root, 'System32', 'nvidia-smi.exe'), path.join(process.env.ProgramFiles || 'C:\\Program Files', 'NVIDIA Corporation', 'NVSMI', 'nvidia-smi.exe')] : [];
  const nvidia = candidates.find(file => fs.existsSync(file)) || (platform === 'win32' ? 'nvidia-smi.exe' : 'nvidia-smi');
  let mode = null, retryNvidiaAt = 0;
  let controllers = [], lastIdentityAt = -Infinity;
  return async () => {
    if (mode !== 'fallback' || now() >= retryNvidiaAt) {
      try {
        const { stdout } = await run(nvidia, [`--query-gpu=${NVIDIA_FIELDS.join(',')}`, '--format=csv,noheader,nounits'], OPTIONS);
        const devices = parseNvidia(stdout);
        if (!devices.length) throw new Error('No GPU returned');
        mode = 'nvidia'; return devices;
      } catch { mode = 'fallback'; retryNvidiaAt = now() + 60000; }
    }
    if (platform === 'win32') {
      const { stdout } = await run(path.join(root, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe'), ['-NoProfile', '-NonInteractive', '-Command', WDDM_SCRIPT], OPTIONS);
      const devices = parseWddm(JSON.parse(stdout.replace(/^\uFEFF/, '')));
      if (!devices.length) throw new Error('GPU counters not available');
      if (now() - lastIdentityAt >= 60000) {
        lastIdentityAt = now();
        try { controllers = (await graphics()).controllers || []; } catch { controllers = []; }
      }
      // WDDM LUID counters cannot safely be matched to Win32_VideoController order.
      // A single-vendor system is unambiguous; mixed vendors retain the default colour.
      const vendors = new Set(controllers.map(g => gpuVendor(`${g.vendor || ''} ${g.model || ''} ${g.vendorId || ''}`)));
      if (vendors.size === 1 && !vendors.has('unknown')) {
        for (const device of devices) device.vendor = [...vendors][0];
        if (devices.length === 1 && controllers.length === 1) devices[0].name = controllers[0].model || devices[0].name;
      }
      return devices;
    }
    const data = await graphics();
    return data.controllers.map((gpu, index) => {
      const used = number(gpu.memoryUsed), total = number(gpu.memoryTotal);
      return { id: String(gpu.pciBus || index), name: gpu.model, vendor: gpuVendor(`${gpu.vendor || ''} ${gpu.model || ''} ${gpu.vendorId || ''}`), source: 'systeminformation', gpuPercent: percent(gpu.utilizationGpu),
        memoryUsedBytes: used === null ? null : used * MIB, memoryTotalBytes: total > 0 ? total * MIB : null,
        memoryPercent: used !== null && total > 0 ? Math.min(100, used / total * 100) : null, temperatureCelsius: number(gpu.temperatureGpu) };
    });
  };
}
module.exports = { createGpuReader, parseNvidia, parseWddm };
