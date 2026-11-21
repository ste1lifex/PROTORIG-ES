/*
 * face-ids.mjs —— 固件表盘 id（research/76 §1）
 *
 * 固件只保留两个表盘（另外两个测试用表盘已删除）：
 *   0 = 默认表盘（face_default，下半区可用 facepanel 换图）
 *   3 = 图片表盘（face_image）
 *
 * 插件侧所有入口（面板服务 / agent 工具 / 设置页）共用这一份常量，
 * 不要再各自写一份 [0,1,2,3]，否则已删除的 1/2 会从某个角落漏回来。
 */
export const FACE_IDS = Object.freeze([0, 3]);
export const FACE_LABELS = Object.freeze({ 0: '默认', 3: '图片' });

/** 归一化表盘 id：只接受 0/3（数字或数字字符串），其它返回 null。 */
export function normalizeFaceId(value) {
  /* 注意 Number('') === 0、Number([]) === 0：空值必须先挡掉，否则会被当成"默认表盘"。 */
  if (typeof value === 'string' && value.trim() === '') return null;
  if (typeof value !== 'number' && typeof value !== 'string') return null;
  const id = Number(value);
  return FACE_IDS.includes(id) ? id : null;
}

/** 展示用短名（'默认' / '图片'；非法 id 返回 '?'）。 */
export function faceLabel(id) {
  return FACE_LABELS[Number(id)] ?? '?';
}
