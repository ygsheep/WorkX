// 临时校验：抓取真实会话 + 解析 + 输出摘要
import { parseSession } from '../src/lib/parser.js';

const base = 'http://localhost:5173/api';
const project = 'D--develop-Workspace-workx';

function djb2(s) {
  let h = 5381;
  for (let i = 0; i < s.length; i++) h = ((h * 33 + s.charCodeAt(i)) >>> 0);
  return h.toString(16).padStart(8, '0');
}

async function main() {
  const s = await (await fetch(`${base}/sessions?project=${project}`)).json();
  const target = s.sessions.find((x) => x.id === '9d9fc797-a403-43d0-a66d-6ace3210babf') || s.sessions[0];
  const d = await (await fetch(`${base}/session?project=${project}&id=${target.id}`)).json();
  const session = parseSession(d);
  console.log('title:', session.title);
  console.log('events:', session.events.length, 'corrupt:', session.corruptCount);
  console.log('stats:', JSON.stringify(session.stats));
  console.log('systemPrompts:', session.systemPrompts.length);
  console.log('anomalies:', session.anomalies.map((a) => a.text));
  const types = {};
  for (const e of session.events) types[e.type] = (types[e.type] || 0) + 1;
  console.log('type counts:', JSON.stringify(types));
  // 轨道映射冒烟测试
  for (const e of session.events.slice(0, 6)) {
    console.log(`  #${e.idx} ${e.type} ts=${e.timestamp || '-'} parent=${(e.j && e.j.parentUuid) ? 'yes' : 'no'}`);
  }

  // 合成 system_prompt 事件验证变更检测
  const sp = [
    { type: 'system_prompt', sessionId: 'x', timestamp: '2026-08-27T01:00:00Z', reason: 'initial', content: '你是助手 A', hash: djb2('你是助手 A') },
    { type: 'system_prompt', sessionId: 'x', timestamp: '2026-08-27T01:00:01Z', reason: 'changed', content: '你是助手 B', hash: djb2('你是助手 B') },
  ];
  const parsed = parseSession({ id: 'x', project: 'demo', events: sp.map((o, i) => ({ idx: i, ok: true, obj: o, raw: '' })) });
  console.log('\n--- synthetic system_prompt ---');
  console.log('count:', parsed.systemPrompts.length);
  const changedCount = parsed.systemPrompts.filter((p, i) => i > 0 && p.hash && parsed.systemPrompts[i - 1].hash && p.hash !== parsed.systemPrompts[i - 1].hash).length;
  console.log('changed markers:', changedCount);
  if (parsed.systemPrompts.length !== 2 || changedCount !== 1) {
    console.error('FAIL: synthetic system_prompt check');
    process.exit(1);
  }
  console.log('OK');
}

main().catch((e) => { console.error('FAIL', e); process.exit(1); });
