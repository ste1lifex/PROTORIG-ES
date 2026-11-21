/*
 * log-file.mjs —— 可选的文件日志
 *
 * DSH 宿主的 ctx.logger 只在内存缓冲里（web 端另有 exporter），命令行/dev 副本
 * 里看不到。调试链路（握手、透传、审批、问答）时需要一份落盘日志，所以给插件
 * 加一个 `logFile` 配置：填了就把 info/warn/error/debug 同步追加到文件。
 *
 * 默认关闭；只有开发副本的 profile patch 打开它，正式用户无感。
 * 用 appendFileSync 而不是常驻 stream：日志量很低（状态跳变级），换来的好处是
 * 进程被强杀时最后几行还在。
 */
import fs from 'node:fs';
import path from 'node:path';

const LEVEL = { debug: 10, info: 20, warn: 30, error: 40 };

function formatArg(value) {
  if (typeof value === 'string') return value;
  if (value instanceof Error) return value.stack ?? value.message;
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

export function withFileLog(base, filePath) {
  if (typeof filePath !== 'string' || !filePath.trim()) return base;
  const file = path.resolve(filePath.trim());
  try {
    fs.mkdirSync(path.dirname(file), { recursive: true });
  } catch {
    return base;
  }
  let broken = false;
  const write = (level, args) => {
    if (broken) return;
    const line = `${new Date().toISOString()} ${String(level).toUpperCase()} ${args.map(formatArg).join(' ')}\n`;
    try {
      fs.appendFileSync(file, line);
    } catch {
      broken = true; // 盘满/权限变了就别再拖累面板
    }
  };
  const logger = { file };
  for (const level of Object.keys(LEVEL)) {
    logger[level] = (...args) => {
      write(level, args);
      base?.[level]?.(...args);
    };
  }
  return logger;
}
