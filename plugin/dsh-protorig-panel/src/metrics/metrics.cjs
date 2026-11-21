'use strict';
const os = require('node:os');
const si = require('systeminformation');
const { createNetworkReader } = require('./network.cjs');
const { createGpuReader } = require('./gpu.cjs');
const { gpuVendor } = require('./gpu-vendor.cjs');

class MetricsCollector {
  constructor({ networkStats = createNetworkReader(), gpuStats = createGpuReader(), memory = () => si.mem(), cpus = () => os.cpus(), now = () => Date.now() } = {}) {
    this.networkStats = networkStats; this.memory = memory; this.cpus = cpus; this.now = now;
    this.previous = this.cpuTimes();
    this.network = { rx: null, tx: null, sampledAt: null, error: '正在初始化网络计数器' };
    this.lastNetworkAttempt = null; this.networkBusy = false;
    this.gpuStats = gpuStats; this.gpu = { devices: [], sampledAt: null, error: '正在读取 GPU 驱动' };
    this.lastGpuAttempt = null; this.gpuBusy = false;
    this.host = { name: os.hostname(), platform: `${os.type()} ${os.release()}`, cpu: os.cpus()[0]?.model?.trim() || 'CPU', cores: os.cpus().length };
  }
  async refreshGpu() {
    if (this.gpuBusy) return;
    this.gpuBusy = true; this.lastGpuAttempt = this.now();
    try {
      const devices = await this.gpuStats();
      this.gpu = { devices, sampledAt: this.now(), error: devices.length ? null : '未检测到可用的 GPU 指标' };
    } catch { this.gpu = { devices: [], sampledAt: this.now(), error: 'GPU 指标暂不可用，正在重试' }; }
    finally { this.gpuBusy = false; }
  }
  async refreshNetwork() {
    if (this.networkBusy) return;
    this.networkBusy = true; this.lastNetworkAttempt = this.now();
    try {
      const stats = await this.networkStats();
      const valid = stats.filter(n => Number.isFinite(n.rx_sec) && n.rx_sec >= 0 && Number.isFinite(n.tx_sec) && n.tx_sec >= 0);
      this.network = {
        rx: valid.length ? valid.reduce((s, n) => s + n.rx_sec, 0) : null,
        tx: valid.length ? valid.reduce((s, n) => s + n.tx_sec, 0) : null,
        sampledAt: this.now(), error: valid.length ? null : '等待有效网络采样（需活动网卡及两次计数）'
      };
    } catch { this.network = { rx: null, tx: null, sampledAt: this.now(), error: '网络计数器读取失败' }; }
    finally { this.networkBusy = false; }
  }
  cpuTimes() {
    return this.cpus().reduce((sum, cpu) => {
      sum.idle += cpu.times.idle;
      sum.total += Object.values(cpu.times).reduce((a, b) => a + b, 0);
      return sum;
    }, { idle: 0, total: 0 });
  }
  async sample() {
    const current = this.cpuTimes();
    const total = current.total - this.previous.total;
    const idle = current.idle - this.previous.idle;
    const cpu = total > 0 ? Math.max(0, Math.min(100, (1 - idle / total) * 100)) : null;
    this.previous = current;
    let mem;
    try { mem = await this.memory(); }
    catch { mem = { total: os.totalmem(), available: os.freemem() }; }
    const available = Number.isFinite(mem.available) ? mem.available : mem.free;
    const used = mem.total - available;
    // Windows network counters can be slow; no overlapping process probes.
    if (this.lastNetworkAttempt === null || this.now() - this.lastNetworkAttempt >= 3000) void this.refreshNetwork();
    if (this.lastGpuAttempt === null || this.now() - this.lastGpuAttempt >= 2000) void this.refreshGpu();
    const gpuStale = this.gpu.sampledAt !== null && this.now() - this.gpu.sampledAt > 10000;
    const gpus = gpuStale ? [] : this.gpu.devices;
    const gpu = gpus[0];
    return {
      sampledAt: this.now(), cpuPercent: cpu === null ? null : Math.round(cpu * 10) / 10,
      memoryPercent: mem.total > 0 ? Math.round(used / mem.total * 1000) / 10 : null,
      memoryUsedBytes: used, memoryTotalBytes: mem.total,
      networkRxBps: this.network.rx, networkTxBps: this.network.tx,
      networkSampledAt: this.network.sampledAt, networkError: this.network.error,
      gpuPercent: gpu?.gpuPercent ?? null, gpuName: gpu?.name ?? null,
      gpuVendor: gpu?.vendor || gpuVendor(gpu?.name),
      gpuMemoryUsedBytes: gpu?.memoryUsedBytes ?? null, gpuMemoryTotalBytes: gpu?.memoryTotalBytes ?? null,
      gpuMemoryPercent: gpu?.memoryPercent ?? null, gpuTemperatureCelsius: gpu?.temperatureCelsius ?? null,
      gpuSampledAt: this.gpu.sampledAt, gpuNote: gpuStale ? 'GPU 数据已过期，等待驱动' : this.gpu.error || gpu?.note || null,
      gpus, uptimeSeconds: os.uptime()
    };
  }
}
module.exports = { MetricsCollector };
