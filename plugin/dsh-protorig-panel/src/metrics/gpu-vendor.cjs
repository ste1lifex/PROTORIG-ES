'use strict';
// Stable wire codes: 0 unknown, 1 NVIDIA, 2 AMD, 3 Intel. Never infer from GPU usage.
function gpuVendor(value = '') {
  const text = String(value).trim().toLowerCase();
  if (/nvidia|geforce|quadro|\b(?:0x)?10de\b/.test(text)) return 'nvidia';
  if (/\bamd\b|radeon|advanced micro devices|\b(?:0x)?1002\b/.test(text)) return 'amd';
  if (/intel|\b(?:0x)?8086\b/.test(text)) return 'intel';
  return 'unknown';
}
function vendorCode(metrics = {}) {
  const vendor = gpuVendor(metrics.gpuVendor) !== 'unknown' ? gpuVendor(metrics.gpuVendor) : gpuVendor(metrics.gpuName);
  return ({ nvidia: 1, amd: 2, intel: 3 })[vendor] || 0;
}
module.exports = { gpuVendor, vendorCode };
