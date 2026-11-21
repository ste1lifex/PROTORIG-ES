/*
 * approval.mjs —— 审批 waterfall（FR-8）
 *
 * `approval/request` 是 Cordis waterfall：返回 outcome 表示本监听器作答，
 * 调用 `next()` 表示交给下一个 answerer（电脑 / Web UI）。
 *
 * 策略：手表优先。注册时 `prepend: true` 排到最前，先于 Web UI 的转发器，
 * 这样电脑上的审批卡不会抢先拿到请求；10 s 内没人点手表就 `next()` 交给电脑。
 * 电脑答完时 `next()` 的 Promise 落定（与 approval/decided 同一时刻），据此撤卡。
 *
 * 命令显示：DSH 的 ApprovalRequestEvent 不携带工具参数，只有 toolName/reason/
 * callId。命令文本由 index 侧从最近一次 `tool/call` 事件里按 callId 取（只存在
 * 内存里，最多 16 条）；拿不到时审批卡显示 reason，并允许用户拒绝。
 */
export function registerApproval(ctx, host, logger) {
  const dispose = ctx.on(
    'approval/request',
    async (request, next) => {
      let token = null;
      try {
        if (!host.config.approval.enabled) return next();
        if (!request || typeof request !== 'object') return next();
        if (request.signal?.aborted) return 'cancelled';
        const result = await host.requestApproval({
          toolName: request.toolName,
          reason: request.reason,
          command: host.commandFor(request.callId),
          signal: request.signal,
        });
        token = result.token ?? null;
        if (result.aborted) {
          if (token) host.clearApproval(token);
          return 'cancelled';
        }
        if (result.fallback) {
          // 超时/排队超限：卡留在表上显示“电脑待确认”，这里等电脑的决定
          const outcome = await next();
          if (token) host.clearApproval(token);
          return outcome;
        }
        if (token) host.clearApproval(token);
        return result.outcome === 'allowed-once' ? 'allowed-once' : 'rejected';
      } catch (error) {
        if (token) host.clearApproval(token);
        else host.clearApproval();
        logger.debug?.(`审批处理异常，交给电脑：${error.message}`);
        return next();
      }
    },
    { prepend: true },
  );
  return () => {
    host.clearApproval();
    if (typeof dispose === 'function') dispose();
  };
}
