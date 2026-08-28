import React, { useEffect, useMemo, useRef, useState } from 'react';
import { NF } from '../lib/icons.js';
import { fmtMs } from '../lib/parser.js';

// 三泳道定义：Input / Model / Tools（AI 处理请求的标准工作流层级）
const LANES = [
  { key: 'input', label: 'Input', sub: '用户输入 · 系统提示词', color: '#5c9cf5', bg: 'rgba(92,156,245,.07)' },
  { key: 'model', label: 'Model', sub: 'LLM 思考 · 推理 · 生成', color: '#a78bfa', bg: 'rgba(167,139,250,.07)' },
  { key: 'tools', label: 'Tools', sub: '外部工具调用', color: '#2ea672', bg: 'rgba(46,166,114,.07)' },
];

const PAD = 24;          // 左右留白
const RULER_H = 28;
const LANE_H = 104;      // 每条泳道高度
const LANE_TITLE_H = 30; // 泳道内标题区
const META_H = 30;       // 元事件条高度
const MIN_W = 20;        // 无时长事件的默认宽度
const MAX_LABEL = 200;   // 标签最大像素宽

function laneOf(ev) {
  switch (ev.type) {
    case 'user':
    case 'system_prompt':
      return 'input';
    case 'assistant':
    case 'sub_agent':
      return 'model';
    case 'tool':
      return 'tools';
    default:
      return 'meta';
  }
}

function parseMs(ts) {
  if (!ts) return NaN;
  const n = Date.parse(ts);
  return Number.isFinite(n) ? n : NaN;
}

function durationMs(ev) {
  const j = ev.j || {};
  return Number(j.durationMs) || Number(j.reasoningMs) || 0;
}

// 计算可见事件的时间范围（t0/t1/span），无时间戳时退化为按索引
function computeRange(events) {
  const times = [];
  let idx = 0;
  for (const ev of events) {
    const ms = parseMs(ev.timestamp);
    if (Number.isFinite(ms)) times.push({ ms, idx });
    idx++;
  }
  if (times.length >= 2) {
    const min = Math.min(...times.map((t) => t.ms));
    const max = Math.max(...times.map((t) => t.ms));
    return { t0: min, t1: max, span: Math.max(max - min, 1000), hasTime: true };
  }
  return { t0: 0, t1: Math.max(events.length - 1, 1), span: Math.max(events.length - 1, 1), hasTime: false };
}

// 时间刻度步长：在 span 内产生约 6~14 个刻度
function niceSteps(spanMs) {
  const steps = [1000, 2000, 5000, 10000, 30000, 60000, 120000, 300000, 600000, 1800000, 3600000, 7200000, 21600000, 43200000, 86400000];
  for (const s of steps) if (spanMs / s <= 14) return s;
  return steps[steps.length - 1];
}

function tickLabel(ms, step) {
  const d = new Date(ms);
  const p = (x) => String(x).padStart(2, '0');
  const hm = `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  if (step >= 3600000) return `${p(d.getHours())}:${p(d.getMinutes())}`;
  return hm;
}

function snippet(s, n = 26) {
  if (!s) return '';
  const t = String(s).replace(/\s+/g, ' ');
  return t.length > n ? t.slice(0, n) + '…' : t;
}

function sysPromptLabel(sp, i, list) {
  const prevHash = i > 0 ? list[i - 1].hash : null;
  const reason = sp.reason || 'set';
  const changed = sp.hash && prevHash && sp.hash !== prevHash;
  const base = REASON[reason] || reason;
  return changed ? `⚡ ${base}（第 ${i + 1} 版）` : `${base} v${i + 1}`;
}

const REASON = { initial: '初始提示词', changed: '变更', resume: '恢复' };

export default function TrackView({ session, events, onSelect, selectedIdx }) {
  const wrapRef = useRef(null);
  const [cw, setCw] = useState(900);
  const [zoom, setZoom] = useState(1); // 1 = 适应宽度

  useEffect(() => {
    const el = wrapRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setCw(el.clientWidth || 900));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const range = useMemo(() => computeRange(events), [events]);
  const fit = (cw - PAD * 2) / Math.max(range.span, 1);
  const pxPerMs = fit * zoom;
  const contentW = Math.max(cw, PAD * 2 + range.span * pxPerMs + 120);
  const step = range.hasTime ? niceSteps(range.span) : null;

  // 滚轮缩放：上滚放大、下滚缩小（阻止默认滚动）
  function handleWheel(e) {
    if (e.deltaY === 0) return;
    e.preventDefault();
    const factor = e.deltaY < 0 ? 1.12 : 1 / 1.12;
    setZoom((z) => Math.min(20, Math.max(0.2, +(z * factor).toFixed(3))));
  }

  // 泳道统计
  const laneStats = useMemo(() => {
    const s = { input: { n: 0, dur: 0 }, model: { n: 0, dur: 0 }, tools: { n: 0, dur: 0 }, meta: { n: 0, dur: 0 } };
    for (const ev of events) {
      const l = laneOf(ev);
      s[l].n++;
      s[l].dur += durationMs(ev);
    }
    return s;
  }, [events]);

  const xOf = (ev) => {
    if (range.hasTime) {
      const ms = parseMs(ev.timestamp);
      const x = Number.isFinite(ms) ? (ms - range.t0) * pxPerMs : null;
      return x == null ? null : PAD + x;
    }
    // 无时间戳：按事件顺序等距排列
    const firstIdx = events.length ? events[0].idx : 0;
    return PAD + (ev.idx - firstIdx) * (MIN_W + 12);
  };

  // 预计算 system_prompt 变更标记（hash 对比）
  const spList = useMemo(() => (session ? session.systemPrompts : []), [session]);

  // 元事件（标题/待办/开始/结束）在底部条渲染为菱形标记
  const metaEvents = events.filter((e) => laneOf(e) === 'meta');
  const trackEvents = events.filter((e) => laneOf(e) !== 'meta');

  const totalH = RULER_H + LANES.length * LANE_H + META_H;

  return (
    <div className="track-root">
      <div className="track-ctl">
        <div className="track-legend">
          {LANES.map((l) => (
            <span className="lg" key={l.key}>
              <span className="dot" style={{ background: l.color }} />
              <b style={{ color: l.color }}>{l.label}</b>
              <span style={{ color: 'var(--dim)' }}>
                {laneStats[l.key].n} 次
                {l.key === 'model' && laneStats[l.key].dur > 0 ? ` · 推理 ${fmtMs(laneStats[l.key].dur)}` : ''}
              </span>
            </span>
          ))}
          <span className="lg">
            <span className="dot" style={{ background: 'var(--system)' }} />
            <span style={{ color: 'var(--system)' }}>提示词变更</span>
          </span>
        </div>
        <div className="spacer" />
        <span className="result-count">
          {range.hasTime ? `时间跨度 ${fmtMs(range.span)}` : `${events.length} 事件（无时间戳，按序排列）`}
        </span>
        <button className="icon-btn" title="缩小" onClick={() => setZoom((z) => Math.max(0.2, +(z * 0.8).toFixed(3)))}>
          <span className="nf">{NF.angleDown}</span>
        </button>
        <input
          type="range" className="seq-zoom" min="20" max="400" value={Math.round(zoom * 100)}
          onChange={(e) => setZoom(e.target.value / 100)} title="缩放"
        />
        <button className="icon-btn" title="放大" onClick={() => setZoom((z) => Math.min(20, +(z * 1.25).toFixed(3)))}>
          <span className="nf">{NF.angleUp}</span>
        </button>
        <button className="btn ghost" title="适应宽度" onClick={() => setZoom(1)}>
          <span className="nf">{NF.expand}</span> 适应
        </button>
      </div>

      <div className="track-scroll" ref={wrapRef} onWheel={handleWheel}>
        <svg className="track-svg" width={contentW} height={totalH}
          onMouseLeave={() => {/* 保持选中态 */}}>
          {/* 泳道背景 + 标题 */}
          {LANES.map((l, i) => {
            const y = RULER_H + i * LANE_H;
            return (
              <g key={l.key}>
                <rect x={0} y={y} width={contentW} height={LANE_H} fill={l.bg} />
                <line x1={0} x2={contentW} y1={y} y2={y} stroke="var(--border-soft)" strokeWidth="1" />
                <line x1={0} x2={contentW} y1={y + LANE_H} y2={y + LANE_H} stroke="var(--border-soft)" strokeWidth="1" />
                <rect x={PAD} y={y + 6} width={3} height={14} rx={1.5} fill={l.color} />
                <text x={PAD + 10} y={y + 17} className="track-lane-title" fill={l.color}>{l.label}</text>
                <text x={PAD + 20 + l.label.length * 8} y={y + 17} className="track-lane-sub">{l.sub}</text>
              </g>
            );
          })}

          {/* 时间标尺 */}
          {step ? (
            <g>
              <line x1={PAD} x2={PAD + range.span * pxPerMs} y1={RULER_H - 6} y2={RULER_H - 6} stroke="var(--border)" />
              {Array.from({ length: Math.floor(range.span / step) + 1 }, (_, k) => {
                const x = PAD + k * step * pxPerMs;
                const ms = range.t0 + k * step;
                return (
                  <g key={k}>
                    <line x1={x} x2={x} y1={RULER_H - 10} y2={RULER_H - 2} stroke="var(--dim-2)" />
                    <text x={x + 3} y={12} className="track-tick">{tickLabel(ms, step)}</text>
                  </g>
                );
              })}
            </g>
          ) : (
            <text x={PAD} y={16} className="track-tick" fill="var(--dim)">事件按写入顺序排列（无时间戳）</text>
          )}

          {/* 轨道事件条 */}
          {trackEvents.map((ev) => {
            const l = LANES.find((x) => x.key === laneOf(ev));
            const laneIdx = LANES.indexOf(l);
            const y = RULER_H + laneIdx * LANE_H + LANE_TITLE_H;
            const x = xOf(ev);
            const dur = durationMs(ev);
            const w = Math.max(MIN_W, dur * pxPerMs);
            const selected = selectedIdx === ev.idx;
            const isSys = ev.type === 'system_prompt';
            const spIdx = isSys ? spList.findIndex((s) => s.idx === ev.idx) : -1;
            const isChanged = isSys && spIdx > 0 && spList[spIdx].hash && spList[spIdx - 1].hash && spList[spIdx].hash !== spList[spIdx - 1].hash;

            let label, color;
            if (ev.type === 'user') { label = snippet(ev.j && ev.j.content, 30); color = l.color; }
            else if (isSys) { label = sysPromptLabel(spList[spIdx] || {}, spIdx, spList); color = 'var(--system)'; }
            else if (ev.type === 'tool') { label = `${ev.j && ev.j.toolName || 'tool'}(${snippet(ev.j && ev.j.toolCallId, 8)})`; color = l.color; }
            else if (ev.type === 'sub_agent') { label = `子Agent ${snippet(ev.j && ev.j.taskId, 8)}`; color = '#e08a3c'; }
            else { label = snippet(ev.j && ev.j.content, 30); color = l.color; }

            if (x == null) return null;
            const labelX = w > MAX_LABEL ? x + 6 : x + w + 6;
            const isError = ev.type === 'tool' && ev.j && ev.j.isError;

            return (
              <g key={ev.idx} className="track-bar-g">
                <rect
                  x={x} y={y} width={w} height={18} rx={4}
                  className="track-bar"
                  fill={isSys
                    ? (isChanged ? 'rgba(255,123,114,.28)' : 'rgba(255,123,114,.12)')
                    : (isError ? 'rgba(229,83,75,.25)' : `${color}22`)}
                  stroke={selected ? 'var(--accent)' : isError ? 'var(--error)' : isSys ? 'var(--system)' : color}
                  strokeWidth={selected ? 2 : 1.2}
                  strokeDasharray={isSys ? (isChanged ? '5 3' : '') : ''}
                  onClick={() => onSelect(ev.idx)}
                  style={{ cursor: 'pointer' }}
                >
                  <title>{eventTooltip(ev)}</title>
                </rect>
                {w > 42 && (
                  <text x={x + 6} y={y + 13} className="track-bar-label" fill={selected ? 'var(--accent)' : '#dfe6f0'}>
                    {snippet(label, Math.max(4, Math.floor((w - 10) / 7)))}
                  </text>
                )}
                <text x={labelX} y={y + 13} className="track-ev-label" fill={color}>
                  {label}
                </text>
              </g>
            );
          })}

          {/* 元事件条（标题/待办/开始/结束） */}
          <g>
            <rect x={0} y={RULER_H + LANES.length * LANE_H} width={contentW} height={META_H}
              fill="var(--panel-3)" />
            <text x={PAD} y={RULER_H + LANES.length * LANE_H + 18} className="track-meta-title" fill="var(--dim)">元事件</text>
            {metaEvents.map((ev) => {
              const x = xOf(ev);
              if (x == null) return null;
              const y = RULER_H + LANES.length * LANE_H + META_H / 2;
              const isEnd = ev.type === 'session_start' || ev.type === 'session_end';
              return (
                <g key={ev.idx} onClick={() => onSelect(ev.idx)} style={{ cursor: 'pointer' }}>
                  <title>{eventTooltip(ev)}</title>
                  <rect x={x - 5} y={y - 5} width={10} height={10} rx={2} transform={`rotate(45 ${x} ${y})`}
                    fill={isEnd ? 'var(--meta)' : 'var(--dim-2)'} stroke="var(--border)" strokeWidth="1" />
                </g>
              );
            })}
          </g>
        </svg>
      </div>
    </div>
  );
}

function eventTooltip(ev) {
  const j = ev.j || {};
  const t = ev.timestamp || '';
  const dur = durationMs(ev);
  let s = `${ev.type} @ ${t}`;
  if (dur > 0) s += `\n耗时 ${fmtMs(dur)}`;
  if (j.toolName) s += `\n工具 ${j.toolName}`;
  if (j.isError) s += '\n[ERROR]';
  const c = String(j.content || '').slice(0, 400);
  if (c) s += `\n\n${c}`;
  return s;
}
