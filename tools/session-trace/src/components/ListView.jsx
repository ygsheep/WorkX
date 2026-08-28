import React, { useState } from 'react';
import { NF, TYPE_ICON } from '../lib/icons.js';
import { eventText, TYPE_LABEL, fmtMs } from '../lib/parser.js';

export default function ListView({ session, events, onSelect, selectedIdx }) {
  return (
    <div className="scroll-area">
      <div className="turn-rail" />
      {events.length === 0 && (
        <div className="empty-hint">没有匹配的事件（调整筛选或搜索条件）</div>
      )}
      {events.map((ev) => (
        <EventCard key={ev.idx} ev={ev} session={session}
          selected={selectedIdx === ev.idx} onSelect={onSelect} />
      ))}
    </div>
  );
}

function EventCard({ ev, session, selected, onSelect }) {
  const [expanded, setExpanded] = useState(false);
  const [reasoningOpen, setReasoningOpen] = useState(false);

  if (!ev.valid) {
    return (
      <div className={`evt ${selected ? 'selected' : ''}`} data-type="corrupt" onClick={() => onSelect(ev.idx)}>
        <div className="evt-head">
          <span className="evt-type" style={{ background: 'rgba(229,83,75,.15)', color: 'var(--error)' }}>
            <span className="nf">{NF.exclamTriangle}</span> 损坏行
          </span>
          <span className="evt-seq">#{ev.idx}</span>
        </div>
        <div className="evt-content" style={{ color: 'var(--error)', fontFamily: 'var(--mono)', fontSize: 11.5 }}>
          {(ev.raw || '').slice(0, 300)}
        </div>
      </div>
    );
  }

  const j = ev.j || {};
  const isMeta = ['session_start', 'session_end', 'title', 'todo'].includes(ev.type);
  const text = eventText(ev);
  const duration = Number(j.reasoningMs) || Number(j.durationMs) || 0;
  const toolUses = ev.toolUses || [];
  const missing = (ev.missingResults || []).length > 0;

  if (ev.type === 'system_prompt') {
    return (
      <SystemPromptCard ev={ev} j={j} session={session}
        selected={selected} onSelect={onSelect} />
    );
  }

  if (ev.type === 'sub_agent') {
    return (
      <div className={`sub-agent-block ${expanded ? '' : 'collapsed'} ${selected ? 'selected' : ''}`}
        onClick={() => onSelect(ev.idx)}>
        <div className="sub-head" onClick={(e) => { e.stopPropagation(); setExpanded((v) => !v); }}>
          <span className="caret nf">{NF.chevronRight}</span>
          <span className="nf" style={{ color: 'var(--sub)' }}>{NF.ghost}</span>
          <span className="sub-title">子 Agent</span>
          <span className="sub-id">task={snippet(j.taskId, 14)} · step {j.stepNumber ?? ''} {j.stepType || ''}</span>
          {j.isError || j.wasError ? <span className="tag err">出错</span> : null}
          <span className="spacer" />
          <span className="evt-time">{duration ? fmtMs(duration) : ''}</span>
        </div>
        {!expanded && <SubStepBody j={j} />}
      </div>
    );
  }

  const cls = isMeta ? 'evt meta-evt' : 'evt';
  const typeLabel = TYPE_LABEL[ev.type] || ev.type;

  return (
    <div
      className={`${cls} ${selected ? 'selected' : ''} ${ev.dimOut ? 'dim-out' : ''}`}
      data-type={ev.type}
      onClick={() => onSelect(ev.idx)}
    >
      <div className="evt-head">
        <span className="evt-type">
          <span className="nf">{TYPE_ICON[ev.type] || NF.infoCircle}</span> {typeLabel}
        </span>
        {ev.type === 'assistant' && duration > 0 && (
          <span className="tag ok"><span className="nf">{NF.clock}</span> {fmtMs(duration)}</span>
        )}
        {ev.type === 'tool' && (
          <>
            <span className="tag">{j.toolName}</span>
            {j.isError ? <span className="tag err"><span className="nf">{NF.exclamTriangle}</span> 错误</span> : <span className="tag ok">成功</span>}
            {ev.orphanTool && <span className="tag warn">无匹配调用</span>}
          </>
        )}
        {ev.type === 'user' && ev.parent && <span className="tag">父 #{ev.parent.idx}</span>}
        {missing && <span className="tag err">{NF.exclamTriangle} 缺 {missing} 结果</span>}
        <span className="spacer" />
        <span className="evt-seq">#{ev.idx}</span>
        <span className="evt-time">{ev.timestamp || ''}</span>
      </div>

      {ev.type === 'assistant' && j.reasoningContent ? (
        <details className="reasoning" open={reasoningOpen}
          onClick={(e) => e.stopPropagation()}
          onToggle={(e) => setReasoningOpen(e.target.open)}>
          <summary><span className="nf">{NF.code}</span> 推理过程（{String(j.reasoningContent).length} 字）</summary>
          <div className="body">{j.reasoningContent}</div>
        </details>
      ) : null}

      {ev.type === 'assistant' && toolUses.length > 0 && (
        <div className="tool-uses">
          {toolUses.map((u) => {
            const hasResult = !ev.missingResults || !ev.missingResults.some((m) => m.id === u.id);
            return (
              <span key={u.id} className={`tool-use ${hasResult ? '' : 'missing'}`} title={u.name}>
                <span className="nf">{NF.wrench}</span> {u.name}
                <span className="st">{snippet(String(u.input || ''), 12)}</span>
              </span>
            );
          })}
        </div>
      )}

      {text && (
        <div className={`evt-content ${text.length > 300 && !expanded ? 'collapsed-text' : ''}`}
          onClick={(e) => { e.stopPropagation(); setExpanded((v) => !v); }}>
          {text}
        </div>
      )}
      {ev.type === 'todo' && j.todos && (
        <div className="evt-content" style={{ fontSize: 11.5, color: 'var(--dim)' }}>
          {j.todos.length} 项待办：{j.todos.map((t) => t.title || t.text || t.content).filter(Boolean).join(' · ').slice(0, 300)}
        </div>
      )}
    </div>
  );
}

function SubStepBody({ j }) {
  const content = j.content || j.thoughtText || j.observation || j.finalAnswer || '';
  return (
    <div className="sub-steps">
      {content && (
        <div className="sub-step">
          <div className="st-type">{j.stepType || j.subType || 'step'}</div>
          <div className="st-title">{snippet(content, 60)}</div>
          <div className="st-body">{content}</div>
        </div>
      )}
    </div>
  );
}

function SystemPromptCard({ ev, j, session, selected, onSelect }) {
  const idx = session.systemPrompts.findIndex((s) => s.idx === ev.idx);
  const prev = idx > 0 ? session.systemPrompts[idx - 1] : null;
  const changed = prev && j.hash && prev.hash && j.hash !== prev.hash;
  const reason = REASON[j.reason] || j.reason || 'set';

  // 主区域仅作为指向详情页的条目：点击选中并打开详情面板，不再内联展开
  return (
    <div className={`sysprompt-card ${selected ? 'selected' : ''}`} onClick={() => onSelect(ev.idx)}>
      <div className="sp-head">
        <span className="caret nf">{NF.chevronRight}</span>
        <span className="nf" style={{ color: 'var(--system)' }}>{NF.terminal}</span>
        <span className="sp-title">系统提示词</span>
        <span className="sp-reason">{reason}</span>
        {changed && <span className="sp-reason" style={{ background: 'rgba(255,123,114,.25)' }}>⚡ 相对上一版有变更</span>}
        <span className="spacer" />
        <span className="sp-meta">#{ev.idx} · {snippet(j.hash, 10)} · {ev.timestamp || ''}</span>
      </div>
    </div>
  );
}

const REASON = { initial: '初始', changed: '变更', resume: '恢复' };

function snippet(s, n = 24) {
  if (!s) return '';
  const t = String(s).replace(/\s+/g, ' ');
  return t.length > n ? t.slice(0, n) + '…' : t;
}
