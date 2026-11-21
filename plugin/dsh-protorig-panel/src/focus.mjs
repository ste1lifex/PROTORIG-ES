// focus.mjs —— DSH 会话投影（复用桌面样例的只读 focus 逻辑）
//
// 来源：local/desktop/examples/dsh-protorig/index.mjs（MIT，Copyright (c) 2026 Stellifex；
// 见本目录 LICENSE-focus-mit.txt）。去掉其中的 PROTORIG Console HTTP 桥 `apply()`，
// 只保留纯投影：createFocusState / projectEvent / buildCards / 标题与 CTX 读取。
// 与原文件一样，只描述"最近一条 session/event 所属的会话"，并且
// 不理解 DSH 私有详情时降级为只读事件，不硬解析（交接文档 §6.6）。

// 原始说明（保留，便于对照上游）：
// DSH-only, read-only focus bridge for PROTORIG Console (v0.3).
//
// One card describes only the session that most recently emitted a
// `session/event`. Prompt text, assistant text, tool arguments, paths, and
// API keys are never read or forwarded. The only text read from disk is the
// session title field `record.rows.title.val` in DSH's projection cache.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const name = 'dsh-protorig';

const RECEIPT_MS = 6000;
const TITLE_TTL_MS = 2000;
const SUBAGENT_TTL_MS = 2000;
const CONTEXT_TTL_MS = 2000;
const TITLE_CACHE_LIMIT = 32;
const SUBAGENT_CACHE_LIMIT = 32;
const CONTEXT_CACHE_LIMIT = 32;
const TITLE_MAX = 80;
const TOOL_LABELS = new Set(['bash', 'edit', 'read', 'search', 'other']);
const FAILURE_KINDS = new Set(['error', 'max-tokens', 'interrupted', 'aborted']);
const HANDLED_EVENTS = new Set([
  'turn/start', 'step/start', 'tool/call', 'tool/result', 'assistant/message',
  'turn/end', 'approval/asked', 'approval/decided', 'request/context',
]);
const SAFE_SESSION_ID = /^[A-Za-z0-9._-]{1,128}$/;
const VALUE = { IDLE: 'IDLE', RUNNING: 'RUNNING', NEEDS: 'NEEDS YOU', DONE: 'DONE', FAILED: 'FAILED' };
const TONE = { IDLE: 'neutral', RUNNING: 'neutral', NEEDS: 'warning', DONE: 'success', FAILED: 'error' };

const titleCache = new Map();
const subagentCache = new Map();
const contextCache = new Map();

/** One focus state for the session that most recently produced an event. */
export function createFocusState(sessionId = null) {
  return {
    sessionId: typeof sessionId === 'string' && sessionId ? sessionId : null,
    live: 'IDLE',
    turnOpen: false,
    blocked: false,
    approvals: new Set(),
    approvalSeq: 0,
    steps: 0,
    tool: '',
    contextWindow: 0,
    contextUsed: null,
    contextUpdatedAt: 0,
    contextWindowAt: 0,
    receipt: null,
    title: 'SESSION',
    titleAt: 0,
    lastActivity: 0,
  };
}

function normalize(state) {
  const focus = state && typeof state === 'object' ? state : createFocusState();
  focus.sessionId = typeof focus.sessionId === 'string' && focus.sessionId ? focus.sessionId : null;
  if (!['IDLE', 'RUNNING', 'NEEDS'].includes(focus.live)) focus.live = 'IDLE';
  focus.turnOpen = focus.turnOpen === true;
  focus.blocked = focus.blocked === true;
  if (!(focus.approvals instanceof Set)) focus.approvals = new Set();
  if (!Number.isInteger(focus.approvalSeq) || focus.approvalSeq < 0) focus.approvalSeq = 0;
  focus.steps = Number.isFinite(focus.steps) && focus.steps >= 0 ? Math.min(99999, Math.floor(focus.steps)) : 0;
  focus.tool = typeof focus.tool === 'string' && /^[\x20-\x7e]{0,8}$/.test(focus.tool) ? focus.tool : '';
  if (!Number.isFinite(focus.contextWindow) || focus.contextWindow <= 0) focus.contextWindow = 0;
  if (!Number.isFinite(focus.contextUsed) || focus.contextUsed < 0) focus.contextUsed = null;
  if (!Number.isFinite(focus.contextUpdatedAt) || focus.contextUpdatedAt < 0) focus.contextUpdatedAt = 0;
  if (!Number.isFinite(focus.contextWindowAt) || focus.contextWindowAt < 0) focus.contextWindowAt = 0;
  if (!focus.receipt || typeof focus.receipt !== 'object'
    || !['DONE', 'FAILED'].includes(focus.receipt.outcome) || !Number.isFinite(focus.receipt.at)) {
    focus.receipt = null;
  }
  focus.title = typeof focus.title === 'string' ? focus.title : 'SESSION';
  if (!Number.isFinite(focus.titleAt) || focus.titleAt < 0) focus.titleAt = 0;
  if (!Number.isFinite(focus.lastActivity) || focus.lastActivity < 0) focus.lastActivity = 0;
  return focus;
}

/**
 * 插件可能在回合中途才加载（安装/启用时任务正在跑），会错过 turn/start。
 * 只要还收到回合内事件，就说明这一轮开着：补一次 turnOpen + RUNNING，
 * 否则手表会一直停在 IDLE（"等待任务"）。
 */
function resumeTurn(focus, at) {
  if (!focus.turnOpen) {
    focus.turnOpen = true;
    focus.blocked = false;
    focus.receipt = null;
    if (focus.live !== 'NEEDS') focus.live = 'RUNNING';
  }
  focus.lastActivity = at;
  return focus;
}

/** Event wall-clock timestamp in ms; DSH stamps every SessionEvent with `time`. */
function eventTime(event) {
  return Number.isFinite(event?.time) && event.time >= 0 ? event.time : Date.now();
}

/** Map a real tool name to the short ASCII label the card may display. */
function shortTool(name) {
  if (typeof name !== 'string' || !name) return '';
  const value = name.toLowerCase();
  if (/bash|pwsh|shell|terminal|exec/.test(value)) return 'bash';
  if (/edit|write|patch|apply/.test(value)) return 'edit';
  if (/read|cat|view|open/.test(value)) return 'read';
  if (/search|grep|glob|find|query|lookup/.test(value)) return 'search';
  return 'other';
}

function nonNegativeInteger(value) {
  return Number.isFinite(value) && value > 0 ? Math.floor(value) : 0;
}

/** Prompt-side tokens of the latest request; response output is excluded. */
function usagePrompt(usage) {
  if (!usage || typeof usage !== 'object') return null;
  const input = Number.isFinite(usage.uncachedInputTokens) && usage.uncachedInputTokens >= 0
    ? usage.uncachedInputTokens
    : (Number.isFinite(usage.inputTokens) && usage.inputTokens >= 0 ? usage.inputTokens : null);
  const cacheRead = Number.isFinite(usage.cacheReadTokens) && usage.cacheReadTokens >= 0 ? usage.cacheReadTokens : null;
  const cacheWrite = Number.isFinite(usage.cacheWriteTokens) && usage.cacheWriteTokens >= 0 ? usage.cacheWriteTokens : null;
  if (input !== null || cacheRead !== null || cacheWrite !== null) {
    return nonNegativeInteger(input) + nonNegativeInteger(cacheRead) + nonNegativeInteger(cacheWrite);
  }
  const fallback = nonNegativeInteger(usage.totalTokens) || nonNegativeInteger(usage.total)
    || nonNegativeInteger(usage.promptTokens) || nonNegativeInteger(usage.prompt);
  return fallback > 0 ? fallback : null;
}

function approvalKey(id, focus) {
  if (typeof id === 'string' && id.length > 0) return id;
  if (typeof id === 'number' && Number.isFinite(id)) return String(id);
  focus.approvalSeq += 1;
  return `anonymous:${focus.approvalSeq}`;
}

function endTurn(focus, reason, at) {
  const kind = reason && typeof reason === 'object' && typeof reason.kind === 'string' ? reason.kind : '';
  focus.turnOpen = false;
  focus.approvals.clear();
  focus.lastActivity = at;
  if (kind === 'blocked') {
    focus.blocked = true;
    focus.live = 'NEEDS';
    focus.receipt = null;
    return focus;
  }
  focus.blocked = false;
  focus.live = 'IDLE';
  let outcome = null;
  if (kind === 'completed') outcome = 'DONE';
  else if (FAILURE_KINDS.has(kind)) outcome = 'FAILED';
  focus.receipt = outcome === null ? null : { outcome, at };
  return focus;
}

/**
 * Project one DSH session event into the focus state. Mutates and returns the
 * state; no I/O, no Cordis dependency, and no event text is retained.
 */
export function projectEvent(event, state = createFocusState()) {
  if (!event || typeof event !== 'object' || typeof event.type !== 'string') return null;
  if (!HANDLED_EVENTS.has(event.type)) return null;
  const data = event.data && typeof event.data === 'object' ? event.data : {};
  const focus = normalize(state);
  const at = eventTime(event);
  switch (event.type) {
    case 'turn/start':
      focus.live = 'RUNNING';
      focus.turnOpen = true;
      focus.blocked = false;
      focus.approvals.clear();
      focus.steps = 0;
      focus.tool = '';
      focus.receipt = null;
      focus.lastActivity = at;
      return focus;
    case 'step/start':
      focus.steps = Math.min(99999, focus.steps + 1);
      return resumeTurn(focus, at);
    case 'tool/call':
      focus.tool = shortTool(data.name);
      return resumeTurn(focus, at);
    case 'tool/result':
      /* 单个工具报错不等于整轮失败：agent 会重试/换工具，最终以 turn/end 的 reason 为准。 */
      return resumeTurn(focus, at);
    case 'assistant/message': {
      resumeTurn(focus, at);
      const prompt = usagePrompt(data.usage);
      if (prompt !== null) {
        focus.contextUsed = prompt;
        focus.contextUpdatedAt = at;
      }
      return focus;
    }
    case 'turn/end':
      return endTurn(focus, data.reason, at);
    case 'approval/asked':
      focus.approvals.add(approvalKey(data.id, focus));
      focus.turnOpen = true;
      focus.live = 'NEEDS';
      focus.lastActivity = at;
      return focus;
    case 'approval/decided':
      if (data.id !== undefined && data.id !== null) focus.approvals.delete(String(data.id));
      if (!focus.approvals.size && !focus.blocked) focus.live = focus.turnOpen ? 'RUNNING' : 'IDLE';
      focus.lastActivity = at;
      return focus;
    case 'request/context':
      if (Number.isFinite(data.contextWindow) && data.contextWindow > 0) {
        focus.contextWindow = Math.floor(data.contextWindow);
        focus.contextWindowAt = at;
      }
      focus.lastActivity = at;
      return focus;
    default:
      return focus;
  }
}

function contextPercent(focus) {
  if (Number.isFinite(focus.contextWindow) && focus.contextWindow > 0
    && Number.isFinite(focus.contextUsed) && focus.contextUsed >= 0) {
    return Math.min(100, Math.max(0, Math.round(focus.contextUsed / focus.contextWindow * 100)));
  }
  // Plain projection states used by tests may carry a resolved ctx directly.
  if (Number.isInteger(focus.ctx) && focus.ctx >= 0 && focus.ctx <= 100) return focus.ctx;
  return null;
}

function boundTitle(value) {
  let title = '';
  const cleaned = String(value).replace(/[\x00-\x1f\x7f]+/g, ' ').trim();
  for (const character of cleaned) {
    if (title.length + character.length > TITLE_MAX) break;
    title += character;
  }
  return title;
}

function focusTitle(focus) {
  return boundTitle(typeof focus.title === 'string' ? focus.title : '') || 'SESSION';
}

function focusTool(focus) {
  return typeof focus.tool === 'string' && TOOL_LABELS.has(focus.tool) ? focus.tool : '';
}

function focusCard(focus, stateName) {
  const tool = focusTool(focus);
  const steps = Number.isFinite(focus.steps) && focus.steps >= 0 ? Math.min(99999, Math.floor(focus.steps)) : 0;
  const percent = contextPercent(focus);
  const parts = [];
  if (tool) parts.push(tool);
  parts.push(`${steps} steps`);
  if (percent !== null) parts.push(`ctx ${percent}%`);
  const card = {
    id: 'focus',
    label: 'DSH',
    value: VALUE[stateName] || 'IDLE',
    detail: parts.join(' / '),
    tone: TONE[stateName] || 'neutral',
  };
  if (percent !== null) card.progress = percent;
  card.focus = {
    title: focusTitle(focus),
    state: stateName,
    tool,
    steps,
    ctx: percent === null ? -1 : percent,
  };
  return card;
}

/**
 * Build the single focus card at `now`. A turn that ended less than six
 * seconds ago shows DONE/FAILED; NEEDS always wins because it asks the user
 * for input. The title comes from `state.title` (resolved by `apply`).
 */
export function buildCards(state, now = Date.now()) {
  const time = Number.isFinite(now) ? now : Date.now();
  if (!state || typeof state !== 'object') return [focusCard(createFocusState(), 'IDLE')];
  const live = ['IDLE', 'RUNNING', 'NEEDS'].includes(state.live) ? state.live : 'IDLE';
  let stateName = live;
  const receipt = state.receipt;
  if (live !== 'NEEDS' && receipt && ['DONE', 'FAILED'].includes(receipt.outcome)
    && Number.isFinite(receipt.at) && time >= receipt.at && time - receipt.at < RECEIPT_MS) {
    stateName = receipt.outcome;
  }
  return [focusCard(state, stateName)];
}

function sessionDirectory(options = {}) {
  const direct = options.dir || options.titleDir || options.sessionsDir;
  if (typeof direct === 'string' && direct) return path.resolve(direct);
  // DSH 桌面版只把 profile 目录作为启动参数传入，不一定设置 DSH_HOME；
  // 默认按 ~/.dsh 找投影缓存（用户配置过 DSH_HOME 时优先用它）。
  const home = options.dshHome || process.env.DSH_HOME || path.join(os.homedir(), '.dsh');
  return home ? path.join(home, 'storages', 'session_projcache', 'sessions') : null;
}

function sessionFileNames(sessionId) {
  const names = [`${sessionId}.json`, `session-${sessionId}.json`];
  if (sessionId.startsWith('session-')) names.push(`${sessionId.slice(8)}.json`);
  return names;
}

function normalizeTitle(value) {
  return boundTitle(String(value).replace(/[\r\n\t]+/g, ' ')) || 'SESSION';
}

/**
 * Inspect projection-cache metadata only. `record.rows.subagent.identity` is
 * DSH's durable marker for a subagent session; future/older caches may carry
 * the header depth or parent instead.
 */
function projectionIsSubagent(record) {
  const header = record?.header;
  const identity = record?.identity;
  if (header?.parentSession || identity?.parentSession) return true;
  if (header?.origin === 'subagent' || identity?.origin === 'subagent') return true;
  const depth = header?.delegationDepth ?? identity?.delegationDepth;
  if (Number.isFinite(depth) && depth > 0) return true;
  const subagent = record?.rows?.subagent?.val;
  return Boolean(subagent && typeof subagent === 'object'
    && subagent.identity && typeof subagent.identity === 'object');
}

/** Fallback when the live Session object lacks delegation metadata. */
function cachedProjectionIsSubagent(sessionId, options) {
  const now = Number.isFinite(options.now) ? options.now : Date.now();
  const directory = sessionDirectory(options);
  if (!directory || typeof sessionId !== 'string' || !SAFE_SESSION_ID.test(sessionId)) return false;
  const key = `${directory}\u0000${sessionId}`;
  const cached = subagentCache.get(key);
  if (cached && now - cached.at < SUBAGENT_TTL_MS) {
    subagentCache.delete(key);
    subagentCache.set(key, cached);
    return cached.value;
  }
  let value = false;
  for (const name of sessionFileNames(sessionId)) {
    try {
      const record = JSON.parse(fs.readFileSync(path.join(directory, name), 'utf8'))?.record;
      if (record && typeof record === 'object') {
        value = projectionIsSubagent(record);
        break;
      }
    } catch { /* missing or malformed projection cache: treat as a main session */ }
  }
  subagentCache.set(key, { at: now, value });
  while (subagentCache.size > SUBAGENT_CACHE_LIMIT) subagentCache.delete(subagentCache.keys().next().value);
  return value;
}

/** Live Session metadata first; projection cache only when it is inconclusive. */
export function isSubagentSession(session, options = {}) {
  const header = session?.header;
  const depth = header?.delegationDepth ?? session?.delegationDepth;
  if (header?.parentSession || session?.parentId || session?.parentSessionId) return true;
  if (header?.origin === 'subagent' || session?.origin === 'subagent') return true;
  if (Number.isFinite(depth)) return depth > 0;
  const raw = options.sessionId !== undefined ? options.sessionId : session?.id;
  const sessionId = typeof raw === 'string' ? raw : (typeof raw === 'number' ? String(raw) : null);
  return cachedProjectionIsSubagent(sessionId, options);
}

export function clearSubagentCache() {
  subagentCache.clear();
}

/**
 * Read one session title from DSH's projection cache. Only
 * `record.rows.title.val` is accessed; a missing file or malformed cache
 * returns the ASCII fallback `SESSION`. Results are cached for two seconds.
 */
export function readSessionTitle(sessionId, options = {}) {
  const now = Number.isFinite(options.now) ? options.now : Date.now();
  const directory = sessionDirectory(options);
  if (!directory || typeof sessionId !== 'string' || !SAFE_SESSION_ID.test(sessionId)) return 'SESSION';
  const key = `${directory}\u0000${sessionId}`;
  const cached = titleCache.get(key);
  if (cached && now - cached.at < TITLE_TTL_MS) {
    titleCache.delete(key);
    titleCache.set(key, cached);
    return cached.title;
  }
  let title = 'SESSION';
  for (const name of sessionFileNames(sessionId)) {
    try {
      const record = JSON.parse(fs.readFileSync(path.join(directory, name), 'utf8'));
      const value = record?.record?.rows?.title?.val;
      if (typeof value === 'string' && value.trim()) {
        title = normalizeTitle(value);
        break;
      }
    } catch { /* missing or malformed projection cache */ }
  }
  titleCache.set(key, { at: now, title });
  while (titleCache.size > TITLE_CACHE_LIMIT) titleCache.delete(titleCache.keys().next().value);
  return title;
}

export function clearSessionTitleCache() {
  titleCache.clear();
}

function emptyContext() {
  return { tokens: null, window: 0, ctx: -1, at: 0 };
}

function contextTokens(value) {
  for (const key of ['projectedTokens', 'pressureTokens', 'surfaceTokens']) {
    const tokens = value?.[key];
    if (Number.isFinite(tokens) && tokens >= 0) return Math.floor(tokens);
  }
  return null;
}

/**
 * Read contextPressure from the same projection cache as the title. Tokens
 * prefer projectedTokens, then pressureTokens, then surfaceTokens; `at` is the
 * cache file mtime and lets newer event usage win. Results are cached 2s.
 */
export function readSessionContext(sessionId, options = {}) {
  const now = Number.isFinite(options.now) ? options.now : Date.now();
  const directory = sessionDirectory(options);
  if (!directory || typeof sessionId !== 'string' || !SAFE_SESSION_ID.test(sessionId)) return emptyContext();
  const key = `${directory}\u0000${sessionId}`;
  const cached = contextCache.get(key);
  if (cached && now - cached.at < CONTEXT_TTL_MS) {
    contextCache.delete(key);
    contextCache.set(key, cached);
    return { ...cached.snapshot };
  }
  let snapshot = emptyContext();
  for (const name of sessionFileNames(sessionId)) {
    try {
      const file = path.join(directory, name);
      const stat = fs.statSync(file);
      const record = JSON.parse(fs.readFileSync(file, 'utf8'));
      const value = record?.record?.rows?.contextPressure?.val;
      if (!value || typeof value !== 'object') continue;
      const tokens = contextTokens(value);
      const window = Number.isFinite(value.contextWindow) && value.contextWindow > 0
        ? Math.floor(value.contextWindow) : 0;
      const ctx = tokens !== null && window > 0
        ? Math.min(100, Math.max(0, Math.round(tokens / window * 100))) : -1;
      const mtime = Number.isFinite(stat.mtimeMs) && stat.mtimeMs > 0 ? stat.mtimeMs : now;
      const at = Math.min(mtime, now);
      snapshot = { tokens, window, ctx, at };
      break;
    } catch { /* missing or malformed projection cache */ }
  }
  contextCache.set(key, { at: now, snapshot });
  while (contextCache.size > CONTEXT_CACHE_LIMIT) contextCache.delete(contextCache.keys().next().value);
  return { ...snapshot };
}

/**
 * Merge a cache snapshot into focus state. A newer event usage keeps its value
 * because it carries a larger timestamp; a newer projection snapshot wins.
 * Tokens and window are compared independently.
 */
export function mergeContext(state, snapshot) {
  if (!state || typeof state !== 'object' || !snapshot || typeof snapshot !== 'object') return false;
  if (!Number.isFinite(snapshot.at) || snapshot.at <= 0) return false;
  const focus = normalize(state);
  let applied = false;
  if (Number.isFinite(snapshot.tokens) && snapshot.tokens >= 0 && snapshot.at > focus.contextUpdatedAt) {
    focus.contextUsed = Math.floor(snapshot.tokens);
    focus.contextUpdatedAt = snapshot.at;
    applied = true;
  }
  if (Number.isFinite(snapshot.window) && snapshot.window > 0 && snapshot.at > focus.contextWindowAt) {
    focus.contextWindow = Math.floor(snapshot.window);
    focus.contextWindowAt = snapshot.at;
    applied = true;
  }
  return applied;
}
