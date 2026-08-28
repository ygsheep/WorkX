// WorkX 会话 JSONL 解析库（纯函数，可独立测试）
// 事件类型：
//   session_start / user / assistant / tool / title / todo / sub_agent /
//   session_end / system_prompt

export const TYPE_LABEL = {
  session_start: '会话开始',
  user: '用户',
  assistant: '助手',
  tool: '工具',
  title: '标题',
  todo: '待办',
  sub_agent: '子Agent',
  session_end: '会话结束',
  system_prompt: '系统提示词',
  corrupt: '损坏行',
};

export const METADATA_TYPES = new Set(['session_start', 'session_end', 'title', 'todo']);

export function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

export function truncateUtf8(s, n) {
  let count = 0, i = 0;
  while (count < n && i < s.length) {
    const c = s.charCodeAt(i);
    i += c >= 0x10000 ? 2 : 1;
    count++;
  }
  return s.slice(0, i);
}

export function fmtSize(n) {
  if (!n) return '0B';
  if (n < 1024) return n + 'B';
  if (n < 1048576) return (n / 1024).toFixed(1) + 'KB';
  return (n / 1048576).toFixed(1) + 'MB';
}

export function fmtMs(ms) {
  ms = Number(ms) || 0;
  if (ms >= 60000) return (ms / 60000).toFixed(1) + 'min';
  if (ms >= 1000) return (ms / 1000).toFixed(1) + 's';
  return ms.toFixed(0) + 'ms';
}

export function fmtDate(ms) {
  if (!ms) return '';
  const d = new Date(ms);
  const p = (x) => String(x).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

// 计算一个字符串的近似显示宽度（CJK 全角按 1.0，ASCII 半角按 0.6）
export function strWidth(s, unit = 1) {
  let w = 0;
  for (const ch of String(s || '')) {
    const cp = ch.codePointAt(0);
    const wide = cp > 0x2e7f && cp < 0x3000 ? false : /[\u1100-\u115f\u2e80-\ua4cf\uac00-\ud7a3\uf900-\ufaff\ufe30-\ufe4f\uff00-\uff60\uffe0-\uffe6]/.test(ch);
    w += (wide || cp > 0x2e7f) ? unit : unit * 0.6;
  }
  return w;
}

export function parseSession({ id, project, events }) {
  const session = {
    id, project,
    events: [],
    meta: null,
    title: '',
    stats: {},
    anomalies: [],
    systemPrompts: [],
    corruptCount: 0,
  };

  const uuidIndex = new Map();
  const toolCalls = new Map();

  // 第一遍：逐行解析
  for (const line of events) {
    const rec = { idx: line.idx, raw: line.raw, type: 'unknown', valid: line.ok === true };
    if (line.ok && line.obj) {
      rec.j = line.obj;
      rec.type = typeof line.obj.type === 'string' ? line.obj.type : 'unknown';
      rec.timestamp = line.obj.timestamp || line.obj.createdAt || line.obj.endedAt || '';
    } else {
      session.corruptCount++;
      session.anomalies.push({ kind: 'corrupt', idx: line.idx, text: (line.raw || '').slice(0, 120) });
    }
    session.events.push(rec);
  }

  // 第二遍：建立索引与统计
  const stats = {};
  for (const ev of session.events) {
    if (!ev.valid) continue;
    const j = ev.j;
    stats[ev.type] = (stats[ev.type] || 0) + 1;
    if (ev.type === 'session_start' && !session.meta) session.meta = j;
    if (ev.type === 'title' && j.title) session.title = j.title;
    if (ev.type === 'system_prompt') {
      session.systemPrompts.push({
        idx: ev.idx, reason: j.reason || 'set', content: j.content || '',
        hash: j.hash || '', timestamp: j.timestamp || '',
      });
    }
    if (ev.type === 'user' && j.uuid) uuidIndex.set(j.uuid, ev);
    if (ev.type === 'assistant') {
      if (j.uuid) uuidIndex.set(j.uuid, ev);
      ev.toolUses = Array.isArray(j.toolUses) ? j.toolUses : [];
    }
    if (ev.type === 'tool') {
      if (j.toolCallId) toolCalls.set(j.toolCallId, ev);
      if (j.uuid) uuidIndex.set(j.uuid, ev);
    }
  }
  session.stats = stats;
  // 已压缩（用户/助手/工具 之外的 system_prompt 单独统计）
  stats._toolErrors = 0;

  // 第三遍：链接 + 异常
  const hasStart = session.events.some((e) => e.valid && e.type === 'session_start');
  const hasEnd = session.events.some((e) => e.valid && e.type === 'session_end');
  if (!hasStart) session.anomalies.push({ kind: 'no_start', text: '缺少 session_start 事件' });
  if (!hasEnd) session.anomalies.push({ kind: 'no_end', text: '缺少 session_end 事件（会话可能未正常结束）' });

  let firstUser = '';
  for (const ev of session.events) {
    if (!ev.valid) continue;
    const j = ev.j;
    if (ev.type === 'user' && !firstUser) firstUser = j.content || '';

    if (ev.type === 'user' || ev.type === 'assistant' || ev.type === 'tool') {
      const pu = j.parentUuid;
      if (pu) {
        ev.parent = uuidIndex.get(pu);
        if (!ev.parent) {
          ev.orphanParent = true;
          session.anomalies.push({
            kind: 'orphan_parent', idx: ev.idx,
            text: `${TYPE_LABEL[ev.type] || ev.type} 引用了不存在的 parentUuid: ${String(pu).slice(0, 12)}…`,
          });
        }
      }
    }
    if (ev.type === 'tool') {
      ev.matchedToolUse = findToolUse(session, j.toolCallId);
      if (!ev.matchedToolUse) {
        ev.orphanTool = true;
        session.anomalies.push({
          kind: 'orphan_tool', idx: ev.idx,
          text: `工具结果 ${j.toolName || ''}(${String(j.toolCallId || '').slice(0, 12)}…) 无匹配的 assistant 工具调用`,
        });
      }
      if (j.isError) stats._toolErrors++;
    }
  }
  for (const ev of session.events) {
    if (ev.valid && ev.type === 'assistant' && ev.toolUses && ev.toolUses.length) {
      ev.missingResults = ev.toolUses.filter((u) => !toolCalls.has(u.id));
      if (ev.missingResults.length) {
        session.anomalies.push({
          kind: 'missing_tool_result', idx: ev.idx,
          text: `assistant 发起 ${ev.missingResults.length} 个工具调用但无对应结果：${ev.missingResults.map((u) => u.name).join(', ')}`,
        });
      }
    }
  }

  // 标题 fallback
  if (!session.title) {
    session.title = firstUser
      ? truncateUtf8(firstUser, 24) + (firstUser.length > 24 ? '…' : '')
      : '未命名会话';
    session.anomalies.push({ kind: 'title_fallback', text: '无 title 事件，标题为首条用户消息的 fallback' });
  }
  return session;
}

export function findToolUse(session, toolCallId) {
  if (!toolCallId) return null;
  for (const ev of session.events) {
    if (!ev.valid || ev.type !== 'assistant') continue;
    const found = (ev.toolUses || []).find((u) => u.id === toolCallId);
    if (found) return { owner: ev, use: found };
  }
  return null;
}

export function eventText(ev) {
  if (!ev.valid) return '';
  const j = ev.j;
  switch (ev.type) {
    case 'user':
    case 'assistant':
    case 'tool':
      return j.content || '';
    case 'title':
      return j.title || '';
    case 'todo':
      return (j.todos && j.todos.length) ? JSON.stringify(j.todos) : '';
    case 'system_prompt':
      return (j.content || '') + ' ' + (j.reason || '');
    case 'sub_agent':
      return [j.content, j.thoughtText, j.toolInput, j.observation, j.finalAnswer].filter(Boolean).join(' ');
    default:
      return '';
  }
}

// 计算事件在 parentUuid 链上的深度
export function computeDepth(events) {
  const depth = new Map();
  for (const ev of events) {
    let d = 0, cur = ev, seen = 0;
    while (cur && cur.parent && seen++ < 30) { d++; cur = cur.parent; }
    depth.set(ev.idx, d);
  }
  return depth;
}

// 子 agent 分组：按 taskId 聚合步骤，按 stepNumber 排序去重
export function groupSubAgents(session) {
  const tasks = new Map();
  for (const ev of session.events) {
    if (!ev.valid || ev.type !== 'sub_agent') continue;
    const j = ev.j || {};
    const taskId = j.taskId || `idx-${ev.idx}`;
    if (!tasks.has(taskId)) tasks.set(taskId, { taskId, steps: [] });
    const list = tasks.get(taskId).steps;
    const key = `${j.stepNumber || 0}-${j.stepType || j.subType || ''}`;
    if (!list.some((s) => s.key === key)) {
      list.push({ ev, j, key });
    }
  }
  for (const t of tasks.values()) {
    t.steps.sort((a, b) => (a.j.stepNumber || 0) - (b.j.stepNumber || 0));
    t.duration = t.steps.reduce((a, s) => a + (s.j.durationMs || 0), 0);
    t.hasError = t.steps.some((s) => s.j.isError || s.j.wasError);
  }
  return tasks;
}
