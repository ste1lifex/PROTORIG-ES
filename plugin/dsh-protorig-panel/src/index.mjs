/*
 * index.mjs —— DSH 插件入口
 *
 * 承载：把手表当 480×480 副屏 + 遥控器（research/57、60）。
 * 这里只做"宿主接线"：
 *   · session/event  → 面板投影（focus.mjs，复用桌面样例的只读逻辑）
 *   · approval/request waterfall → 手表优先 10 s → 电脑（approval.mjs）
 *   · user-questions/request waterfall → 手表选择/提交（questions.mjs）
 *   · 其余（链路、渲染、脏块、提醒、BLE）在 panel-service.mjs。
 *
 * 被 DSH 加载的方式：package.json 的 `dsh.bundle.patch` → cordis.patch.yml
 * 插入 `{ id, name }`，宿主调用本模块的 apply(ctx, config)。
 */
import { Config, normalizeConfig } from './config.mjs';
import { flashBackend } from './platform.mjs';
import { PanelService } from './panel-service.mjs';
import { registerApproval } from './approval.mjs';
import { registerQuestions } from './questions.mjs';
import { FlashService } from './flash.mjs';
import { MacFlashService } from './flash-mac.mjs';
import { BalanceService } from './balance.mjs';
import { registerTools } from './tools.mjs';
import { registerRoutes } from './routes.mjs';
import { withFileLog } from './log-file.mjs';

export const name = 'dsh-protorig-panel';

export { Config };

const FALLBACK_LOGGER = {
  info: (...args) => console.log('[protorig]', ...args),
  warn: (...args) => console.warn('[protorig]', ...args),
  error: (...args) => console.error('[protorig]', ...args),
  debug: () => {},
};

/**
 * @param {import('@deepseek-ai/cordis').Context} ctx
 * @param {unknown} rawConfig cordis.patch.yml 的 config（schema 已校验、已补默认值）
 */
export function apply(ctx, rawConfig = {}) {
  const rawLogger = typeof ctx?.logger === 'function' ? ctx.logger(name) : FALLBACK_LOGGER;
  if (typeof rawLogger.debug !== 'function') rawLogger.debug = () => {};
  const config = normalizeConfig(rawConfig);
  const logger = withFileLog(rawLogger, config.logFile);
  const flashBackendKind = flashBackend();
  const flashLabel = flashBackendKind === 'powershell'
    ? ' · 烧录可用（Windows flash.ps1）'
    : flashBackendKind === 'blflashcommand'
      ? ' · 烧录可用（macOS BLFlashCommand）'
      : ' · 烧录不可用';
  logger.info(
    `dsh-protorig-panel 激活（${config.fps} fps${config.approval.enabled ? ` · 审批手表优先 ${config.approval.watchTimeoutMs / 1000}s` : ''}` +
      `${config.ble.enabled ? ' · BLE 降级开' : ''}${config.deviceId ? ` · 绑定设备 ${config.deviceId}` : ''}` +
      `${flashLabel}）`,
  );

  // 余额：照 @linxin666/dsh-usage 的凭据解析顺序（credentials seam → 环境变量）
  const balance = new BalanceService({ ctx, config, logger });
  const host = new PanelService({ config: rawConfig, logger, balance });
  balance.start();
  host.start().catch((error) => logger.warn(`面板启动失败：${error.message}`));

  /* 烧录后端按平台二选一：Windows = flash.ps1（PowerShell），
   * macOS = BLFlashCommand-macos（serialport + Rosetta，flash-mac.mjs）。 */
  const flash = flashBackendKind === 'blflashcommand'
    ? new MacFlashService({ panel: host, logger })
    : new FlashService({ panel: host, logger });
  registerTools(ctx, { panel: host, flash, logger });
  registerRoutes(ctx, { panel: host, flash, logger, balance });

  const disposers = [registerApproval(ctx, host, logger), registerQuestions(ctx, host, logger)];
  if (typeof ctx?.on === 'function') {
    disposers.push(ctx.on('session/event', (session, event) => host.onSessionEvent(session, event)));
  }

  return async () => {
    balance.stop();
    for (const dispose of disposers.reverse()) {
      try {
        await dispose?.();
      } catch {
        /* 卸载时单个监听器清理失败不阻塞其它清理 */
      }
    }
    flash.abort('plugin-dispose');
    await host.dispose().catch((error) => logger.debug?.(`面板停止异常：${error.message}`));
    logger.info('dsh-protorig-panel 已卸载');
  };
}
