/*
 * questions.mjs —— 问答 waterfall（FR-9）
 *
 * `user-questions/request` 同样是 Cordis waterfall：返回 { answers } 即认领，
 * `next()` 交给电脑（Web UI）的 answerer。
 *
 * 只在手表上处理"每个问题都有选项"的请求；纯文本/无选项的问题直接交给电脑，
 * 因为手表设计稿只有 option/next/submit 三个热区，没有键盘。
 * 电脑先答（或超时中止）时 request.signal 会 abort，这里抛错让服务按
 * ASK_ABORTED 结算，避免把已经作废的请求再转给电脑、在电脑上留下幽灵卡。
 */
export function registerQuestions(ctx, host, logger) {
  const dispose = ctx.on(
    'user-questions/request',
    async (request, next) => {
      let token = null;
      try {
        if (!host.config.questions.enabled) return next();
        if (!request || typeof request !== 'object') return next();
        const questions = Array.isArray(request.questions) ? request.questions : [];
        if (!questions.length) return next();
        if (request.signal?.aborted) throw new Error('question aborted before the watch answered');
        // 没有选项的问题手表答不了（没有输入法）→ 电脑
        if (!questions.every((item) => Array.isArray(item?.options) && item.options.length > 0)) {
          return next();
        }
        const result = await host.requestQuestion(request);
        token = result.token ?? null;
        if (result.aborted) {
          if (token) host.clearQuestion(token);
          throw new Error('question aborted while the watch was answering');
        }
        if (result.answer) {
          if (token) host.clearQuestion(token);
          return result.answer;
        }
        if (result.fallback) {
          // 独占超时/排队超限：卡留在表上（"请到电脑查看"），等电脑答完再撤
          try {
            return await next();
          } finally {
            if (token) host.clearQuestion(token);
          }
        }
        if (token) host.clearQuestion(token);
        return next(); // 其他情况交给电脑
      } catch (error) {
        if (token) host.clearQuestion(token);
        else host.clearQuestion();
        if (request?.signal?.aborted) throw error;
        logger.debug?.(`问答处理异常，交给电脑：${error.message}`);
        return next();
      }
    },
    { prepend: true },
  );
  return () => {
    host.clearQuestion();
    if (typeof dispose === 'function') dispose();
  };
}
