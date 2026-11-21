/*
 * balance.mjs —— DeepSeek 余额（照 @linxin666/dsh-usage 的做法）
 *
 * 那个"web-ui 全家桶"的用法服务是这么做的：
 *   1. 每个提供方一段 balance 适配器：build({apiKey}) → {url, headers}，parse(status, body) → {currency, totalBalance}；
 *   2. 凭据走 DSH 的 credentials seam，逐级降级：
 *        credentials.readRecord('llm-pi-ai/deepseek')  → { kind:'api-key', key }
 *        credentials.resolve(<apiKeyEnv>)              → 环境变量
 *        readNamespace('llm-deepseek').apiKeyEnv       → 默认 "DEEPSEEK_API_KEY"
 *      OAuth（kind:'grant'）不查余额；
 *   3. 域名从 llm-deepseek 的 baseURL 解析（用户可能走中转），不硬编码；
 *   4. 定时轮询 + 缓存，失败不造数。
 *
 * 我们只有一个提供方，就按同样的形状写：四态 ok / no-credential / failed / disabled，
 * 手表只收到格式化后的字符串（key 永远不出主机）。
 */

const DEFAULT_API_KEY_ENV = 'DEEPSEEK_API_KEY';
const DEFAULT_BASE_URL = 'https://api.deepseek.com';
const DEFAULT_INTERVAL_SEC = 60;
const TIMEOUT_MS = 15000;

/** 从 baseURL 解析 origin（多数配置是 https://host/v1，余额接口在 /user/balance）。 */
export function originOf(baseUrl) {
  const raw = typeof baseUrl === 'string' && baseUrl.trim() ? baseUrl.trim() : DEFAULT_BASE_URL;
  try {
    const url = new URL(raw);
    return `${url.protocol}//${url.host}`;
  } catch {
    return DEFAULT_BASE_URL;
  }
}

/** 把余额数字格式化成手表上那行字（失败返回 null，渲染层显示 --）。 */
export function formatBalance(view) {
  if (!view || view.state !== 'ok') return null;
  const amount = Number(view.amount);
  if (!Number.isFinite(amount)) return null;
  const symbol = view.currency === 'USD' ? '$' : '¥';
  return `${symbol}${amount.toFixed(2)}`;
}

/** DeepSeek 余额响应 → 归一化（照 usage 的 parse 形状）。 */
export function parseBalance(status, body) {
  if (status !== 200 || typeof body !== 'object' || body === null) return undefined;
  const infos = body.balance_infos;
  if (!Array.isArray(infos) || infos.length === 0) return undefined;
  const first = infos[0];
  if (typeof first !== 'object' || first === null) return undefined;
  const currency = typeof first.currency === 'string' ? first.currency : undefined;
  const total = first.total_balance;
  if (currency === undefined || total === undefined || total === null) return undefined;
  const amount = Number(total);
  if (!Number.isFinite(amount)) return undefined;
  return { currency, amount, available: body.is_available !== false };
}

export class BalanceService {
  /**
   * @param {{ctx?:object, config:object, logger?:object, fetchFn?:Function, now?:Function}} opts
   */
  constructor({ ctx, config, logger, fetchFn, now, clientVersion = '' } = {}) {
    this.ctx = ctx;
    this.config = config ?? {};
    this.clientVersion = clientVersion;
    /** 最近一次 DSH 账号余额查询的失败原因（用于 no-credential 时给准确提示）。 */
    this.accountNote = null;
    this.logger = logger ?? { info() {}, warn() {} };
    this.fetchFn = fetchFn ?? globalThis.fetch;
    this.now = now ?? (() => Date.now());
    this.view = { state: 'disabled', currency: 'CNY', amount: null, fetchedAt: 0, error: null };
    this.timer = null;
    this.inflight = null;
  }

  get enabled() {
    return this.config?.balance?.enabled !== false;
  }

  get intervalMs() {
    const sec = Number(this.config?.balance?.intervalSec);
    return (Number.isFinite(sec) && sec > 0 ? sec : DEFAULT_INTERVAL_SEC) * 1000;
  }

  /** llm-deepseek 命名空间里的配置（apiKeyEnv / baseURL）。 */
  #namespace() {
    try {
      return this.ctx?.get?.('llm-deepseek') ?? {};
    } catch {
      return {};
    }
  }

  baseUrl() {
    const ns = this.#namespace();
    return typeof ns.baseURL === 'string' && ns.baseURL ? ns.baseURL : DEFAULT_BASE_URL;
  }

  apiKeyEnv() {
    const ns = this.#namespace();
    return typeof ns.apiKeyEnv === 'string' && ns.apiKeyEnv ? ns.apiKeyEnv : DEFAULT_API_KEY_ENV;
  }

  /**
   * 解析凭据（照 usage 的顺序：模型路由记录 → 环境引用 → 官方适配器默认名）。
   * @returns {Promise<{kind:'api-key'|'env'|'oauth'|'none', key?:string}>}
   */
  async resolveCredential() {
    let credentials;
    try {
      credentials = this.ctx?.get?.('credentials');
    } catch {
      credentials = undefined;
    }
    /* 没有 credentials 服务（有些 profile 不装）不能直接判无凭据——
     * 还要走 launchEnvironment / process.env 两级兜底。 */
    for (const id of credentials ? ['deepseek', 'deepseek-official'] : []) {
      try {
        const record = await credentials.readRecord?.(`llm-pi-ai/${id}`);
        if (record?.kind === 'api-key' && typeof record.key === 'string' && record.key) {
          return { kind: 'api-key', key: record.key };
        }
        if (record?.kind === 'grant') return { kind: 'oauth' };
      } catch {
        /* 没这条记录，继续 */
      }
    }

    const envName = this.apiKeyEnv();
    try {
      const resolved = await credentials?.resolve?.(envName);
      const value = typeof resolved === 'string' ? resolved : resolved?.value;
      if (typeof value === 'string' && value) return { kind: 'env', key: value };
    } catch {
      /* resolve 失败就继续 */
    }

    /* 再退两级（照 dsh-pet-perlica 的顺序）：
     *   launchEnvironment 是宿主注入的环境视图，process.env 是最后的兜底。
     * 只在宿主进程内读，key 不会离开主机。 */
    try {
      const launch = this.ctx?.get?.('launchEnvironment');
      const value = launch?.get?.(envName);
      if (typeof value === 'string' && value) return { kind: 'env', key: value };
    } catch {
      /* 没有这个 seam */
    }
    const ambient = globalThis.process?.env?.[envName];
    if (typeof ambient === 'string' && ambient) return { kind: 'env', key: ambient };
    return { kind: 'none' };
  }

  /** 查一次余额（带 in-flight 去重）。 */
  async refresh({ force = false } = {}) {
    if (!this.enabled) {
      this.view = { state: 'disabled', currency: 'CNY', amount: null, fetchedAt: this.now(), error: null };
      return this.view;
    }
    if (this.inflight && !force) return this.inflight;
    this.inflight = this.#query().finally(() => {
      this.inflight = null;
    });
    return this.inflight;
  }

  /**
   * DSH 登录账号的余额（deepseekAccount Remote 服务）。
   * 这是设置页「账号与余额」里看到的同一个数：平台钱包 normal_wallets + bonus_wallets。
   * 不读账号 token、不自己拼 header，交给 DSH 的 account 服务处理登录/过期。
   * @returns {Promise<object|null>} 视图；服务不可用/未登录/失败时返回 null 走 API key 兜底。
   */
  async #accountBalance() {
    let account;
    try {
      account = this.ctx?.get?.('deepseekAccount');
    } catch {
      account = undefined;
    }
    if (!account || typeof account.getBalance !== 'function') {
      this.accountNote = '账号服务不可用';
      return null;
    }
    const client = {
      version: this.clientVersion || '0.0.0',
      locale: (() => {
        try { return Intl.DateTimeFormat().resolvedOptions().locale || 'zh-CN'; } catch { return 'zh-CN'; }
      })(),
      timezoneOffsetSeconds: -new Date().getTimezoneOffset() * 60,
    };
    let result;
    try {
      let timer;
      /* 直接走 host 侧 getDetail（和 Remote 的 getBalance 同一个实现），
       * 避免给 Remote 装饰器传一个非 RPC 的 client 元数据。 */
      const query = typeof account.getDetail === 'function' && typeof account.detailHeaders === 'function'
        ? account.getDetail('balance', account.detailHeaders(client))
        : account.getBalance(client);
      result = await Promise.race([
        query,
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error('账号余额查询超时')), 8000);
          timer.unref?.();
        }),
      ]);
      clearTimeout(timer);
    } catch (error) {
      this.accountNote = String(error?.message ?? error);
      return null;
    }
    if (result == null) {
      this.accountNote = 'DeepSeek 账号未登录';
      return null;
    }
    if (result.status !== 'ready') {
      this.accountNote = result.status === 'failed' ? '账号余额读取失败' : '账号余额不可用';
      return null;
    }
    const wallets = [...(Array.isArray(result.value) ? result.value : []),
      ...(Array.isArray(result.bonusWallets) ? result.bonusWallets : [])];
    const totals = new Map();
    for (const wallet of wallets) {
      const currency = typeof wallet?.currency === 'string' ? wallet.currency : '';
      const amount = Number(wallet?.balance);
      if (!currency || !Number.isFinite(amount)) continue;
      totals.set(currency, (totals.get(currency) || 0) + amount);
    }
    const entries = [...totals.entries()];
    if (!entries.length) {
      this.accountNote = '账号没有余额数据';
      return null;
    }
    /* 手表是中文界面：有 CNY 就显示 CNY，多币种时优先它；否则用第一个钱包。 */
    const [currency, amount] = entries.find(([code]) => code === 'CNY') ?? entries[0];
    this.accountNote = null;
    return {
      state: 'ok',
      currency,
      amount,
      fetchedAt: this.now(),
      error: null,
      source: 'dsh-account',
    };
  }

  async #query() {
    /* 优先用 DSH 登录账号的余额（与设置页「账号与余额」同源）；没有账号再回退 API key。 */
    const account = await this.#accountBalance();
    if (account) {
      this.view = account;
      return this.view;
    }
    const credential = await this.resolveCredential();
    if (credential.kind === 'oauth') {
      this.view = { state: 'unsupported', currency: 'CNY', amount: null, fetchedAt: this.now(), error: 'OAuth 凭据不查余额' };
      return this.view;
    }
    if (credential.kind === 'none' || !credential.key) {
      this.view = {
        state: 'no-credential',
        currency: 'CNY',
        amount: null,
        fetchedAt: this.now(),
        error: this.accountNote
          ? `${this.accountNote}；也没有 ${this.apiKeyEnv()}`
          : `没有 ${this.apiKeyEnv()}`,
      };
      return this.view;
    }
    const url = `${originOf(this.baseUrl())}/user/balance`;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
    try {
      const response = await this.fetchFn(url, {
        method: 'GET',
        headers: { authorization: `Bearer ${credential.key}`, accept: 'application/json' },
        signal: controller.signal,
      });
      let body = null;
      try {
        body = await response.json();
      } catch {
        body = null;
      }
      const parsed = parseBalance(response.status, body);
      if (!parsed) {
        const detail = body ? JSON.stringify(body).slice(0, 120) : `HTTP ${response.status}`;
        this.view = { state: 'failed', currency: 'CNY', amount: null, fetchedAt: this.now(), error: detail };
        return this.view;
      }
      this.view = {
        state: parsed.available ? 'ok' : 'failed',
        currency: parsed.currency,
        amount: parsed.amount,
        fetchedAt: this.now(),
        error: parsed.available ? null : '余额不可用',
      };
      return this.view;
    } catch (error) {
      this.view = {
        state: 'failed',
        currency: 'CNY',
        amount: null,
        fetchedAt: this.now(),
        error: error?.name === 'AbortError' ? '请求超时' : String(error?.message ?? error),
      };
      return this.view;
    } finally {
      clearTimeout(timer);
    }
  }

  /** 开始轮询（幂等）。 */
  start() {
    if (this.timer || !this.enabled) return;
    const tick = () => {
      void this.refresh().catch(() => {});
    };
    tick();
    this.timer = setInterval(tick, this.intervalMs);
    this.timer.unref?.();
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  /** 给设置页/面板用的视图。 */
  publicView() {
    return {
      ...this.view,
      enabled: this.enabled,
      intervalSec: Math.round(this.intervalMs / 1000),
      apiKeyEnv: this.apiKeyEnv(),
      baseUrl: this.baseUrl(),
      text: formatBalance(this.view),
    };
  }
}
