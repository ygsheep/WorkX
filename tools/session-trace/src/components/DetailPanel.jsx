import React, { useEffect, useMemo, useState } from 'react';
import { marked } from 'marked';
import DOMPurify from 'dompurify';
import { NF, TYPE_ICON } from '../lib/icons.js';
import { TYPE_LABEL, fmtMs } from '../lib/parser.js';
import { fetchSkills } from '../lib/api.js';

const TABS = [
  { key: 'summary', label: 'Summary' },
  { key: 'preview', label: 'Preview' },
  { key: 'raw', label: 'Raw' },
  { key: 'source', label: 'Source' },
];

// 系统提示词专属 tab（带图标）
const SP_TABS = [
  { key: 'sp', label: 'System Prompt', icon: NF.terminal },
  { key: 'tools', label: 'Tools', icon: NF.wrench },
  { key: 'skills', label: 'Skills', icon: NF.cogs },
  { key: 'mcp', label: 'MCP', icon: NF.server },
];

// 工具名 → 图标映射（按内置工具 prompt 开头辨识）
const TOOL_META = [
  { name: 'AskUser', icon: NF.infoCircle, re: /ask[\s-]*user.*question|用户提问/i },
  { name: 'Agent', icon: NF.ghost, re: /sub-?agent|子代理/i },
  { name: 'Bash', icon: NF.terminal, re: /Executes a shell command/i },
  { name: 'PowerShell', icon: NF.bolt, re: /PowerShell command/i },
  { name: 'FileRead', icon: NF.file, re: /Reads?( the contents of| a) file|读取文件/i },
  { name: 'FileWrite', icon: NF.pen, re: /Writes? a file|写文件/i },
  { name: 'FileEdit', icon: NF.pen, re: /exact string replacements|文件内精确替换/i },
  { name: 'Glob', icon: NF.folderOpen, re: /Finds files matching a glob|glob 模式/i },
  { name: 'Grep', icon: NF.search, re: /Searches? file contents for matching|ripgrep|用 .*搜索文件内容/i },
  { name: 'WebSearch', icon: NF.search, re: /web[\s-]*search|网络搜索/i },
  { name: 'WebFetch', icon: NF.link, re: /fetch/i },
  { name: 'TodoWrite', icon: NF.checkCircle, re: /todo|待办/i },
  { name: 'Skill', icon: NF.cogs, re: /detailed instructions of a skill|技能|的能力/i },
  { name: 'MCP', icon: NF.server, re: /MCP 工具|MCP server/i },
  { name: 'ListMcpResources', icon: NF.database, re: /列出已连接 MCP/ },
  { name: 'ReadMcpResource', icon: NF.database, re: /读取指定 MCP|读取 MCP 资源/i },
  { name: 'EnterPlanMode', icon: NF.sitemap, re: /EnterPlanMode/i },
  { name: 'ExitPlanMode', icon: NF.sitemap, re: /ExitPlanMode/i },
  { name: 'Task', icon: NF.listUl, re: /task|任务/i },
];

configureMarked();

export default function DetailPanel({ session, event, onClose, onSelect, width }) {
  const [tab, setTab] = useState('summary');
  const j = event.j || {};
  const isSys = event.type === 'system_prompt';
  const [spTab, setSpTab] = useState('sp');
  const sys = useMemo(() => (isSys ? splitSystemPrompt(j.content) : null), [isSys, j.content]);

  const related = useMemo(() => {
    if (!event.valid) return { parents: [], children: [], toolUse: null, results: [] };
    const uuid = j.uuid;
    const parents = [];
    let cur = event.parent;
    let guard = 0;
    while (cur && guard++ < 20) { parents.push(cur); cur = cur.parent; }

    const children = session.events.filter((e) => e.valid && e.j && e.j.parentUuid && e.j.parentUuid === uuid);

    let toolUse = null;
    let results = [];
    if (event.type === 'assistant' && Array.isArray(j.toolUses) && j.toolUses.length) {
      toolUse = j.toolUses[0];
      results = session.events.filter((e) => e.valid && e.type === 'tool' && e.j && e.j.toolCallId === j.toolUses[0].id);
    }
    if (event.type === 'tool') {
      const owner = findOwner(session, j.toolCallId);
      if (owner) toolUse = { owner, use: { name: j.toolName, input: j.toolInput, id: j.toolCallId } };
    }
    return { parents, children, toolUse, results };
  }, [event, j, session]);

  const text = useMemo(() => (event.valid ? pickText(event, j) : ''), [event, j]);
  const previewSource = useMemo(() => {
    const trimmed = String(text).trim();
    if (event.type === 'tool' && /^[\[{]/.test(trimmed)) return '```json\n' + trimmed + '\n```';
    return text;
  }, [text, event.type]);

  const inner = (
    <div className="trace-body">
      {!event.valid ? (
        <TraceSection title="原始行">
          <pre className="raw-json">{event.raw || ''}</pre>
        </TraceSection>
      ) : isSys ? (
        <SysTabContent spTab={spTab} sys={sys} event={event} session={session} />
      ) : tab === 'summary' ? (
        <SummaryTab event={event} j={j} related={related} session={session} onSelect={onSelect} />
      ) : tab === 'preview' ? (
        <PreviewTab source={previewSource} text={text} />
      ) : tab === 'raw' ? (
        <TraceSection title="格式化 JSON（点击折叠 / 展开，默认展开一层）">
          <div className="raw-json jq json-tree-slot">
            <JsonTree data={j} />
          </div>
        </TraceSection>
      ) : (
        <TraceSection title="JSONL 源行">
          <pre className="raw-json jq">{prettySource(event, j)}</pre>
        </TraceSection>
      )}
    </div>
  );

  return (
    <aside className="detail-panel" style={{ width }}>
      <div className="trace-head">
        <h3>
          <span className="nf" style={{ color: typeColor(event.type) }}>{TYPE_ICON[event.type] || NF.infoCircle}</span>
          {TYPE_LABEL[event.type] || event.type} 详情
        </h3>
        <div className="tt">#{event.idx}</div>
        <div className="spacer" />
        <button className="icon-btn" onClick={onClose} title="关闭"><span className="nf">{NF.times}</span></button>
      </div>
      <div className="detail-tabs">
        {(isSys ? SP_TABS : TABS).map((t) => {
          const on = isSys ? spTab === t.key : tab === t.key;
          return (
            <button key={t.key} className={`dtab ${on ? 'on' : ''}`} onClick={() => (isSys ? setSpTab(t.key) : setTab(t.key))}>
              {t.icon && <span className="nf">{t.icon}</span>}
              {t.label}
            </button>
          );
        })}
      </div>
      {inner}
    </aside>
  );
}

function SummaryTab({ event, j, related, session, onSelect }) {
  const duration = Number(j.reasoningMs) || Number(j.durationMs) || 0;
  return (
    <React.Fragment>
      <MetaRows j={j} event={event} duration={duration} />

      {event.type === 'user' && j.content && (
        <TraceSection title="内容">
          <pre className="raw-json">{j.content}</pre>
        </TraceSection>
      )}
      {event.type === 'assistant' && j.content && (
        <TraceSection title="内容">
          <pre className="raw-json">{j.content}</pre>
        </TraceSection>
      )}
      {event.type === 'assistant' && j.reasoningContent && (
        <TraceSection title="推理过程">
          <pre className="raw-json">{j.reasoningContent}</pre>
        </TraceSection>
      )}
      {event.type === 'tool' && (
        <TraceSection title={j.isError ? '错误输出' : '工具输出'}>
          <pre className={`raw-json ${j.isError ? 'err' : ''}`}>{j.content}</pre>
        </TraceSection>
      )}

      {related.parents.length > 0 && (
        <TraceSection title={`父链（${related.parents.length}）`}>
          {related.parents.map((p, i) => (
            <div className="trace-node" key={p.idx} style={{ marginLeft: i * 12 }} onClick={() => onSelect(p.idx)}>
              <span className="tn-type">{p.type} #{p.idx}</span>
              <div className="tn-text">{snippet(nodeText(p), 80)}</div>
            </div>
          ))}
        </TraceSection>
      )}

      {related.toolUse && related.toolUse.use && (
        <TraceSection title="关联工具调用">
          <div className="trace-node target">
            <span className="tn-type"><span className="nf">{NF.wrench}</span> {related.toolUse.use.name}</span>
            <div className="tn-text">{snippet(String(related.toolUse.use.input || ''), 160)}</div>
          </div>
        </TraceSection>
      )}

      {related.results.length > 0 && (
        <TraceSection title={`工具结果（${related.results.length}）`}>
          {related.results.map((r) => (
            <div className="trace-node" key={r.idx} onClick={() => onSelect(r.idx)}>
              <span className="tn-type">tool #{r.idx}</span>
              <div className="tn-text">{snippet(nodeText(r), 120)}</div>
            </div>
          ))}
        </TraceSection>
      )}

      {related.children.length > 0 && (
        <TraceSection title={`子事件（${related.children.length}）`}>
          {related.children.map((c) => (
            <div className="trace-node" key={c.idx} onClick={() => onSelect(c.idx)}>
              <span className="tn-type">{c.type} #{c.idx}</span>
              <div className="tn-text">{snippet(nodeText(c), 120)}</div>
            </div>
          ))}
        </TraceSection>
      )}
    </React.Fragment>
  );
}

function PreviewTab({ source, text }) {
  if (!String(text).trim()) return <div className="trace-empty">（无文本内容）</div>;
  let html;
  try {
    html = DOMPurify.sanitize(marked.parse(source));
  } catch {
    html = '';
  }
  return <div className="md-preview" dangerouslySetInnerHTML={{ __html: html || '' }} />;
}

function MetaRows({ j, event, duration }) {
  const rows = [];
  if (event.timestamp) rows.push(['时间', event.timestamp]);
  if (duration > 0) rows.push(['耗时', fmtMs(duration)]);
  if (j.toolName) rows.push(['工具', j.toolName]);
  if (j.toolCallId) rows.push(['调用ID', snippet(j.toolCallId, 20)]);
  if (j.uuid) rows.push(['UUID', snippet(j.uuid, 20)]);
  if (j.parentUuid) rows.push(['父UUID', snippet(j.parentUuid, 20)]);
  if (j.reason) rows.push(['原因', j.reason]);
  if (j.hash) rows.push(['Hash', j.hash]);
  if (j.model) rows.push(['模型', j.model]);
  if (j.isError) rows.push(['状态', 'ERROR']);
  if (!rows.length) return null;
  return (
    <div className="trace-section">
      <div className="meta-rows">
        {rows.map(([k, v]) => (
          <div className="meta-row" key={k}><span className="mr-k">{k}</span><span className="mr-v">{v}</span></div>
        ))}
      </div>
    </div>
  );
}

// 将系统提示词按段落切分并归类为 System Prompt / Tools / Skills / MCP。
// 依据 build_system_prompt 的实际结构：基础提示词 → # Environment → 项目记忆 → 各工具 prompt → @file 说明。
function splitSystemPrompt(content) {
  const blocks = String(content).split(/\n{2,}/).map((s) => s.trim()).filter(Boolean);
  const sec = { sp: [], tools: [], skills: [], mcp: [] };
  for (const b of blocks) {
    if (
      /用户消息中可能出现/im.test(b) ||          // @file 引用说明 → System Prompt
      /CLAUDE\.md|AGENT\.md|项目记忆|#\s*Environment|OS 信息|Operating System[:\s C]/im.test(b) ||
      /你是|你是一个|You are|you are|你的角色/im.test(b)  // 基础系统提示词
    ) {
      sec.sp.push(b);
    } else if (/Loads the detailed instructions of a skill|技能|的能力/im.test(b)) {
      sec.skills.push(b);
    } else if (/MCP 工具|MCP server|MCP 资源|resource/im.test(b)) {
      sec.mcp.push(b);
    } else {
      sec.tools.push(b);
    }
  }
  // 兜底：若基础段为空（内容里没有识别出基础提示词前缀），把最前面的工具段提升为 System Prompt
  if (!sec.sp.length && sec.tools.length) sec.sp = [sec.tools.shift()];
  return sec;
}

// 系统提示词详情的内层 Tab 内容（System Prompt / Tools / Skills / MCP）
function SysTabContent({ spTab, sys, event, session }) {
  const j = event.j || {};
  const idx = session.systemPrompts.findIndex((s) => s.idx === event.idx);
  const prev = idx > 0 ? session.systemPrompts[idx - 1] : null;
  const changed = prev && j.hash && prev.hash && j.hash !== prev.hash;
  const blocks = (sys && sys[spTab]) || [];
  const total = sys && (sys.sp.length + sys.tools.length + sys.skills.length + sys.mcp.length);
  return (
    <React.Fragment>
      <TraceSection title="系统提示词快照">
        <div className="trace-node target">
          <span className="tn-type">
            <span className="nf">{NF.terminal}</span> v{idx + 1} / {session.systemPrompts.length}
            {prev ? (changed ? ' · 内容变化' : ' · 内容相同') : ''} · {String(j.content || '').length} 字符 · {total} 段
          </span>
        </div>
        {prev && (
          <div className={`trace-node ${changed ? '' : 'dim-out'}`}>
            <span className="tn-type">上一版 v{idx}（{changed ? '内容不同' : '内容相同'}）</span>
            <div className="tn-text">{snippet(prev.content, 140)}</div>
          </div>
        )}
      </TraceSection>
      {spTab === 'mcp' && <McpStatus blocks={sys?.mcp || []} />}
      {spTab === 'tools' ? (
        <div className="tool-cards">
          {sys.tools.length === 0 ? (
            <div className="trace-empty">（没有可展示的内建工具）</div>
          ) : (
            sys.tools.map((b, i) => <ToolCard key={i} block={b} />)
          )}
        </div>
      ) : spTab === 'skills' ? (
        <SkillCards cwd={session.meta?.cwd} />
      ) : blocks.length === 0 ? (
        <div className="trace-empty">（该分类下没有可展示的内容）</div>
      ) : (
        <div className="sys-blocks">
          {blocks.map((b, i) => (
            <div className="sys-block" key={i}>{b}</div>
          ))}
        </div>
      )}
    </React.Fragment>
  );
}

// 从文本中稳健地提取一个可解析的 JSON 根对象。
// 优先匹配 ```json 代码围栏内的完整对象；否则逐个 '{' 起点扫描，
// 仅在闭合括号处 JSON.parse 成功才采纳，从而绕过前瞻文本里的散落花括号。
function extractJson(text) {
  for (const m of text.matchAll(/```(?:json|javascript|js)?\s*\n?([\s\S]*?)```/gi)) {
    const body = m[1].trim();
    if (body.startsWith('{')) {
      try { JSON.parse(body); return { json: body, start: m.index, end: m.index + m[0].length }; } catch { /* 继续尝试其他 */ }
    }
  }
  for (let s = 0; s < text.length; s++) {
    if (text[s] !== '{') continue;
    let depth = 0, inStr = false, esc = false;
    for (let i = s; i < text.length; i++) {
      const ch = text[i];
      if (inStr) {
        if (esc) esc = false;
        else if (ch === '\\') esc = true;
        else if (ch === '"') inStr = false;
        continue;
      }
      if (ch === '"') inStr = true;
      else if (ch === '{') depth++;
      else if (ch === '}') {
        depth--;
        if (depth === 0) {
          const cand = text.slice(s, i + 1);
          try { JSON.parse(cand); return { json: cand, start: s, end: i + 1 }; } catch { break; }
        }
      }
    }
  }
  return null;
}

// 从工具段中提取名称图标、描述文本与内嵌 JSON（若有）
function parseTool(block) {
  const text = String(block);
  const icon = (() => {
    for (const m of TOOL_META) if (m.re.test(text)) return m.icon;
    return NF.wrench;
  })();
  const nameMeta = TOOL_META.find((m) => m.re.test(text));
  const found = extractJson(text);
  let desc = found ? text.slice(0, found.start) + '\n' + text.slice(found.end) : text;
  desc = desc.replace(/^\s*```(?:json|javascript|js)?\s*$/gim, '').replace(/\n{2,}/g, '\n').trim();
  let name = nameMeta ? nameMeta.name : '';
  if (!name) {
    // 兜底：用描述首行的前几个词或默认名
    const first = desc.split('\n')[0].trim().replace(/:+$/, '');
    name = first ? first.slice(0, 24) : 'Tool';
  }
  return { name, icon, desc, jsonText: found ? found.json : null };
}

// 单个工具的折叠卡片：图标 + 名称，点击展开；内部 JSON 可单独收起/展开并高亮
function ToolCard({ block }) {
  const { name, icon, desc, jsonText } = useMemo(() => parseTool(block), [block]);
  const [open, setOpen] = useState(false);
  const [jsonOpen, setJsonOpen] = useState(true);
  const pretty = useMemo(() => {
    if (!jsonText) return null;
    try { return JSON.stringify(JSON.parse(jsonText), null, 2); } catch { return jsonText; }
  }, [jsonText]);
  return (
    <div className={`tocard ${open ? 'open' : ''}`}>
      <div className="tocard-h" onClick={() => setOpen(!open)}>
        <span className="toc-chevron nf">{open ? NF.angleDown : NF.angleRight}</span>
        <span className="nf toc-icon" style={{ color: 'var(--accent)' }}>{icon}</span>
        <span className="toc-name">{name}</span>
        <span className="toc-hint">{jsonText ? '含 Schema' : ''}</span>
      </div>
      {open && (
        <div className="tocard-b">
          {desc && <div className="toc-desc">{desc}</div>}
          {jsonText && (
            <div className="toc-json">
              <div className="toc-json-h" onClick={() => setJsonOpen(!jsonOpen)}>
                <span className="toc-chevron nf">{jsonOpen ? NF.angleDown : NF.angleRight}</span>
                <span className="nf" style={{ color: 'var(--dim)' }}>{NF.code}</span>
                <span className="toc-json-label">Schema (JSON)</span>
              </div>
              {jsonOpen && <pre className="raw-json jq">{renderHighlight(pretty)}</pre>}
            </div>
          )}
        </div>
      )}
    </div>
  );
}

// Skills Tab：调用后端以 src/agent 同样的磁盘扫描逻辑搜索已安装技能，
// 以会话的工作目录 cwd 为起点向上逐级扫描，并叠加用户级 skills 目录
function SkillCards({ cwd }) {
  const [items, setItems] = useState(null);
  const [roots, setRoots] = useState([]);
  const [err, setErr] = useState(null);
  useEffect(() => {
    let alive = true;
    fetchSkills(cwd)
      .then((d) => {
        if (!alive) return;
        setItems(d.skills);
        setRoots(d.roots || []);
      })
      .catch((e) => alive && setErr(String((e && e.message) || e)));
    return () => {
      alive = false;
    };
  }, [cwd]);
  if (err) return <div className="trace-empty">技能搜索失败：{err}</div>;
  if (!items) return <div className="trace-empty">正在搜索已安装技能…</div>;
  return (
    <div>
      <div className="tool-cards">
        {items.length === 0 ? (
          <div className="trace-empty">（没有找到已安装的技能）</div>
        ) : (
          items.map((s, i) => (
            <SkillCard key={s.name + i} skill={s} />
          ))
        )}
      </div>
      {roots.length > 0 && (
        <div className="skill-roots">
          <span className="nf" style={{ color: 'var(--dim-2)' }}>{NF.folderOpen}</span>
          扫描范围：{roots.join('；')}
        </div>
      )}
    </div>
  );
}

// 单个技能卡片：图标 + 名称，点击展开显示描述与来源目录
function SkillCard({ skill }) {
  const [open, setOpen] = useState(false);
  return (
    <div className={`tocard ${open ? 'open' : ''}`}>
      <div className="tocard-h" onClick={() => setOpen(!open)}>
        <span className="toc-chevron nf">{open ? NF.angleDown : NF.angleRight}</span>
        <span className="nf toc-icon" style={{ color: 'var(--tool)' }}>{NF.cogs}</span>
        <span className="toc-name">{skill.name}</span>
        {skill.aliases && skill.aliases.length > 0 && (
          <span className="toc-hint">别名 {skill.aliases.join(', ')}</span>
        )}
      </div>
      {open && (
        <div className="tocard-b">
          {skill.description && <div className="toc-desc">{skill.description}</div>}
          {skill.dir && <div className="toc-src">{skill.dir}</div>}
        </div>
      )}
    </div>
  );
}

// 可折叠 JSON 树视图：默认只展开第一层（根节点展开、其子孙折叠）
function JsonTree({ data }) {
  return (
    <div className="json-tree">
      <JsonNode name={null} value={data} depth={0} />
    </div>
  );
}

function JsonNode({ name, value, depth }) {
  const isArr = Array.isArray(value);
  const isObj = value !== null && typeof value === 'object';
  const hasKids = isObj || isArr;
  const [open, setOpen] = useState(hasKids && depth === 0);
  const pad = { paddingLeft: depth * 14 };

  const keyPart =
    name !== null ? (
      <span className="jq-key jt-key">
        <span className="jt-name">{quotedKey(name)}</span>
        <span className="jq-punct">: </span>
      </span>
    ) : null;

  const renderPrimitive = (v) => {
    if (v === null) return <span className="jq-null">null</span>;
    if (typeof v === 'string') return <span className="jq-str">&quot;{v}&quot;</span>;
    if (typeof v === 'boolean') return <span className="jq-bool">{String(v)}</span>;
    return <span className="jq-num">{String(v)}</span>;
  };

  if (!hasKids) {
    return (
      <div className="jt-row" style={pad}>
        {keyPart}
        {renderPrimitive(value)}
      </div>
    );
  }

  const bracketOpen = isArr ? '[' : '{';
  const bracketClose = isArr ? ']' : '}';
  const count = isArr ? value.length : Object.keys(value).length;
  const title = open ? '折叠' : `展开（${count}${isArr ? ' 项' : ' 个键'}）`;

  return (
    <div className="jt-node">
      <div
        className={`jt-row jt-toggle-row${open ? ' open' : ''}`}
        style={pad}
        onClick={() => setOpen(!open)}
        title={title}
      >
        <span className="nf jt-chev">{open ? NF.angleDown : NF.angleRight}</span>
        {keyPart}
        {count === 0 ? (
          <span className="jq-punct">{bracketOpen}{bracketClose}</span>
        ) : (
          <span className="jq-punct">{open ? bracketOpen : `${bracketOpen}…${bracketClose}`}</span>
        )}
      </div>
      {open && count > 0 && (
        <div className="jt-children">
          {isArr
            ? value.map((v, i) => <JsonNode key={i} name={i} value={v} depth={depth + 1} />)
            : Object.keys(value).map((k) => (
                <JsonNode key={k} name={k} value={value[k]} depth={depth + 1} />
              ))}
          <div className="jt-row" style={pad}>
            <span className="jq-punct">{bracketClose}</span>
          </div>
        </div>
      )}
    </div>
  );
}

function quotedKey(k) {
  return '"' + String(k).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
}

// MCP tab 顶部的服务器状态：从 MCP 段解析 "已连接 server" 列表
function McpStatus({ blocks }) {
  const text = blocks.join('\n');
  const servers = [];
  for (const line of text.split('\n')) {
    const m = line.match(/^\s*[-•]\s*([^（(]+)/);
    if (m) servers.push(m[1].trim());
  }
  const connected = servers.length > 0 && !/没有已连接/.test(text);
  return (
    <TraceSection title="MCP 服务器状态">
      <div className="mcp-status">
        <span className={`ms-chip ${connected ? 'on' : 'off'}`}>
          <span className="nf">{NF.server}</span>
          {connected ? `已连接 · ${servers.length} 个 server` : '未连接 / 无 MCP'}
        </span>
        {servers.length > 0 && (
          <div className="ms-list">
            {servers.map((s, i) => (
              <div className="ms-srv" key={i}><span className="nf">{NF.check}</span>{s}</div>
            ))}
          </div>
        )}
      </div>
    </TraceSection>
  );
}

function TraceSection({ title, children }) {
  return (
    <div className="trace-section">
      <div className="sec-title">{title}</div>
      {children}
    </div>
  );
}

function pickText(event, j) {
  switch (event.type) {
    case 'user':
    case 'assistant':
    case 'tool':
    case 'system_prompt':
      return j.content || '';
    case 'title':
      return j.title || '';
    case 'todo':
      return (j.todos && j.todos.length) ? JSON.stringify(j.todos, null, 2) : '';
    case 'sub_agent':
      return [j.content, j.thoughtText, j.observation, j.finalAnswer].filter(Boolean).join('\n\n');
    default:
      return '';
  }
}

// Source 页：优先展示原始 JSONL 行；无则为格式化 JSON
function prettySource(event, j) {
  if (event.raw) return event.raw;
  return JSON.stringify(j, null, 2);
}

function findOwner(session, toolCallId) {
  for (const e of session.events) {
    if (!e.valid || e.type !== 'assistant') continue;
    if ((e.toolUses || []).some((u) => u.id === toolCallId)) return e;
  }
  return null;
}

function nodeText(ev) {
  const j = ev.j || {};
  return j.content || j.thoughtText || j.finalAnswer || j.observation || j.toolInput || ev.type;
}

function snippet(s, n = 40) {
  if (!s) return '';
  const t = String(s).replace(/\s+/g, ' ');
  return t.length > n ? t.slice(0, n) + '…' : t;
}

function typeColor(t) {
  const map = { user: 'var(--user)', assistant: 'var(--assistant)', tool: 'var(--tool)', sub_agent: 'var(--sub)', system_prompt: 'var(--system)', session_start: 'var(--meta)', session_end: 'var(--meta)', title: 'var(--meta)', todo: 'var(--meta)' };
  return map[t] || 'var(--dim)';
}

// ---------- JSON 语法高亮 ----------
// 将 JSON 文本分词，返回带类名的 React 片段数组
function tokenizeJson(s) {
  const tokens = [];
  const re = /("(?:[^"\\]|\\.)*")(\s*:)?|\b(true|false|null)\b|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)/g;
  let last = 0, m;
  while ((m = re.exec(s))) {
    if (m.index > last) pushPunct(tokens, s.slice(last, m.index));
    if (m[1]) {
      if (m[2]) {
        tokens.push({ cls: 'jq-key', val: m[1] });
        pushPunct(tokens, m[2]);
      } else {
        tokens.push({ cls: 'jq-str', val: m[1] });
      }
    } else if (m[3]) {
      tokens.push({ cls: m[3] === 'true' || m[3] === 'false' ? 'jq-bool' : 'jq-null', val: m[3] });
    } else if (m[4]) {
      tokens.push({ cls: 'jq-num', val: m[4] });
    }
    last = re.lastIndex;
  }
  if (last < s.length) pushPunct(tokens, s.slice(last));
  return tokens;
}

function pushPunct(tokens, s) {
  if (!s) return;
  tokens.push({ cls: 'jq-punct', val: s });
}

function renderHighlight(str) {
  const tokens = tokenizeJson('' + str);
  return tokens.map((t, i) => <span className={t.cls} key={i}>{t.val}</span>);
}

// ---------- Markdown 渲染配置 ----------
function configureMarked() {
  marked.setOptions({
    breaks: false,
    gfm: true,
  });
}