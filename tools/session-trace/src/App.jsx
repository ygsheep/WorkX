import React, { useEffect, useMemo, useRef, useState } from 'react';
import { fetchProjects, fetchSessions, fetchSession, deleteSession } from './lib/api.js';
import { parseSession } from './lib/parser.js';
import { NF, TYPE_ICON } from './lib/icons.js';
import Sidebar from './components/Sidebar.jsx';
import TrackView from './components/TrackView.jsx';
import ListView from './components/ListView.jsx';
import DetailPanel from './components/DetailPanel.jsx';
import workxIcon from './assets/workx-icon.png';

const FILTER_GROUPS = [
  { key: 'user', label: '用户', type: 'user' },
  { key: 'assistant', label: '助手', type: 'assistant' },
  { key: 'tool', label: '工具', type: 'tool' },
  { key: 'sub_agent', label: '子Agent', type: 'sub_agent' },
  { key: 'system_prompt', label: '系统提示词', type: 'system_prompt' },
  { key: 'meta', label: '元事件', type: 'meta' },
];

const REASON_LABEL = { initial: '初始', changed: '变更', resume: '恢复' };

export default function App() {
  const [projects, setProjects] = useState([]);
  const [rootPath, setRootPath] = useState('');
  const [sessionsByProject, setSessionsByProject] = useState({});
  const [selectedProject, setSelectedProject] = useState('');
  const [selectedId, setSelectedId] = useState('');
  const [rawEvents, setRawEvents] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [collapsed, setCollapsed] = useState({});
  const [view, setView] = useState('track'); // track | list
  const [filters, setFilters] = useState(() => new Set(['user', 'assistant', 'tool', 'sub_agent', 'system_prompt', 'meta']));
  const [query, setQuery] = useState('');
  const [selectedIdx, setSelectedIdx] = useState(null);
  const [showSysPrompt, setShowSysPrompt] = useState(true);
  const [sideW, setSideW] = useState(240);
  const [detailW, setDetailW] = useState(560);
  const dragRef = useRef(null);
  const loadSeq = useRef(0);

  // 拖动阈值：侧边栏左缘/详情右缘宽度调整
  useEffect(() => {
    function move(e) {
      const which = dragRef.current;
      if (!which) return;
      if (which === 'sidebar') {
        setSideW(Math.min(520, Math.max(180, e.clientX)));
      } else if (which === 'detail') {
        setDetailW(Math.min(900, Math.max(300, window.innerWidth - e.clientX)));
      }
    }
    function up() {
      dragRef.current = null;
      document.body.style.cursor = '';
      document.body.style.userSelect = '';
    }
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
    return () => {
      window.removeEventListener('mousemove', move);
      window.removeEventListener('mouseup', up);
    };
  }, []);

  function startResize(which, e) {
    e.preventDefault();
    dragRef.current = which;
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  }

  // 初次加载项目列表
  useEffect(() => {
    let alive = true;
    fetchProjects()
      .then((d) => {
        if (!alive) return;
        setProjects(d.projects);
        setRootPath(d.root);
      })
      .catch((e) => alive && setError(String(e.message || e)));
    return () => { alive = false; };
  }, []);

  // 选择项目 → 拉取会话列表
  useEffect(() => {
    if (!selectedProject) return;
    let alive = true;
    setSessionsByProject((m) => (m[selectedProject] ? m : { ...m, [selectedProject]: [] }));
    fetchSessions(selectedProject)
      .then((d) => { if (alive) setSessionsByProject((m) => ({ ...m, [selectedProject]: d.sessions })); })
      .catch((e) => alive && setError(String(e.message || e)));
    return () => { alive = false; };
  }, [selectedProject]);

  // 选择会话 → 拉取并解析
  useEffect(() => {
    if (!selectedProject || !selectedId) return;
    const seq = ++loadSeq.current;
    setLoading(true);
    setError('');
    setSelectedIdx(null);
    fetchSession(selectedProject, selectedId)
      .then((d) => {
        if (seq !== loadSeq.current) return;
        setRawEvents(parseSession(d));
        setLoading(false);
      })
      .catch((e) => {
        if (seq !== loadSeq.current) return;
        setError(String(e.message || e));
        setRawEvents(null);
        setLoading(false);
      });
  }, [selectedProject, selectedId]);

  const session = rawEvents;

  // 过滤后的可见事件
  const visibleEvents = useMemo(() => {
    if (!session) return [];
    const q = query.trim().toLowerCase();
    return session.events.filter((ev) => {
      const t = ev.type;
      const group = FILTER_GROUPS.some((g) => g.type === t) ? t : 'meta';
      if (!filters.has(group)) return false;
      if (ev.type === 'system_prompt' && !showSysPrompt) return false;
      if (q) {
        const hay = [ev.raw, (ev.j && ev.j.content) || ''].join(' ').toLowerCase();
        if (!hay.includes(q)) return false;
      }
      return true;
    });
  }, [session, filters, query, showSysPrompt]);

  const sysPromptChanges = useMemo(() => {
    if (!session) return { list: [], changed: 0 };
    const list = session.systemPrompts.map((sp, i) => ({
      ...sp,
      changed: i > 0 && sp.hash && session.systemPrompts[i - 1].hash && sp.hash !== session.systemPrompts[i - 1].hash,
    }));
    return { list, changed: list.filter((x) => x.changed).length };
  }, [session]);

  function refresh() {
    setError('');
    fetchProjects()
      .then((d) => setProjects(d.projects))
      .catch((e) => setError(String(e.message || e)));
    if (selectedProject) {
      fetchSessions(selectedProject)
        .then((d) => setSessionsByProject((m) => ({ ...m, [selectedProject]: d.sessions })))
        .catch((e) => setError(String(e.message || e)));
    }
    if (selectedProject && selectedId) {
      const seq = ++loadSeq.current;
      fetchSession(selectedProject, selectedId)
        .then((d) => { if (seq === loadSeq.current) setRawEvents(parseSession(d)); })
        .catch((e) => setError(String(e.message || e)));
    }
  }

  async function handleDelete() {
    if (!selectedProject || !selectedId) return;
    const file = selectedId + '.jsonl';
    const sure = window.confirm(`确定删除会话「${session ? session.title : selectedId}」？\n文件：${file}\n\n此操作不可恢复。`);
    if (!sure) return;
    try {
      await deleteSession(selectedProject, selectedId);
      loadSeq.current++; // 使在途的会话加载失效
      setSelectedId('');
      setRawEvents(null);
      setSelectedIdx(null);
      setSelectedProject(''); // 回到未选中状态
    } catch (e) {
      setError(String((e && e.message) || e));
    }
    refresh();
  }

  function toggleFilter(key) {
    setFilters((prev) => {
      const next = new Set(prev);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  }

  const selEvent = session && selectedIdx != null ? session.events[selectedIdx] : null;

  return (
    <div className="app">
      <header className="header">
        <img className="logo-img" src={workxIcon} alt="WorkX" />
        <div>
          <h1>WorkX 会话轨迹调试器</h1>
          <div className="sub">JSONL 会话记录浏览 · 轨道分析</div>
        </div>
        <div className="spacer" />
        {rootPath && <div className="path-info" title={rootPath}>{NF.database} {rootPath}</div>}
        <button className="btn" onClick={refresh} title="刷新项目/会话列表">
          <span className="nf">{NF.refresh}</span> 刷新
        </button>
      </header>

      <div className="main">
        <Sidebar
          projects={projects}
          sessionsByProject={sessionsByProject}
          selectedProject={selectedProject}
          selectedId={selectedId}
          collapsed={collapsed}
          width={sideW}
          onToggleProject={(p) => setCollapsed((c) => ({ ...c, [p]: !c[p] }))}
          onSelectProject={setSelectedProject}
          onSelectSession={(id) => setSelectedId(id)}
        />
        <div className="vh-handle" title="拖拽调整侧边栏宽度"
          onMouseDown={(e) => startResize('sidebar', e)} />

        <section className="content">
          {!session && !loading && !error && (
            <div className="placeholder">
              <div className="big nf" style={{ fontSize: 34 }}>{NF.sitemap}</div>
              <div className="big">在左侧选择一个会话</div>
              <div className="tip">
                自动扫描 <code>{rootPath || '~/.workx/projects'}</code> 下的 JSONL 会话记录<br />
                轨道视图按 <b>Input / Model / Tools</b> 三泳道展示处理流程
              </div>
            </div>
          )}
          {loading && (
            <div className="placeholder">
              <div className="loading-bar" />
              <div className="tip">正在读取会话记录…</div>
            </div>
          )}
          {error && (
            <div className="placeholder">
              <div className="big nf" style={{ color: 'var(--error)' }}>{NF.exclamTriangle}</div>
              <div className="tip" style={{ color: 'var(--error)' }}>{error}</div>
              <button className="btn" onClick={refresh}>重试</button>
            </div>
          )}

          {session && (
            <React.Fragment>
              {/* 会话头部 */}
              <div className="sess-head">
                <div className="title-row">
                  <span className="nf" style={{ color: 'var(--accent)' }}>{NF.commentDots}</span>
                  <h2>{session.title}</h2>
                  <span className={`badge ${session.anomalies.length ? 'warn' : 'accent'}`}>
                    {session.anomalies.length ? `${NF.exclamTriangle} ${session.anomalies.length} 异常` : `${NF.check} 正常`}
                  </span>
                  <div className="spacer" />
                  <button className="btn btn-danger" onClick={handleDelete}
                    title="删除整个会话 JSONL 文件">
                    <span className="nf">{NF.trash}</span> 删除
                  </button>
                </div>
                <div className="chips">
                  <span className="chip"><span className="nf">{NF.clock}</span> {fmtMetaTime(session)}</span>
                  <span className="chip"><span className="nf">{NF.database}</span> <b>{session.events.length}</b> 事件</span>
                  {Object.entries(session.stats).filter(([k]) => !['session_start', 'session_end'].includes(k)).map(([k, v]) => (
                    <span className="chip" key={k}>{TYPE_ICON[k] ? <span className="nf">{TYPE_ICON[k]}</span> : null} <b>{v}</b> {k}</span>
                  ))}
                  {sysPromptChanges.list.length > 0 && (
                    <span className="chip" style={{ borderColor: '#4a2a26' }}>
                      <span className="nf" style={{ color: 'var(--system)' }}>{NF.terminal}</span>
                      系统提示词 <b>{sysPromptChanges.list.length}</b> 快照 · <b style={{ color: 'var(--system)' }}>{sysPromptChanges.changed}</b> 次变更
                    </span>
                  )}
                </div>
              </div>

              {/* 工具栏 */}
              <div className="toolbar">
                {FILTER_GROUPS.map((g) => {
                  const cnt = session.events.filter((e) => (e.type === g.type) || (g.type === 'meta' && !['user', 'assistant', 'tool', 'sub_agent', 'system_prompt'].includes(e.type))).length;
                  return (
                    <button
                      key={g.key}
                      className={`fchip ${filters.has(g.type) ? 'on' : ''}`}
                      data-type={g.type}
                      onClick={() => toggleFilter(g.type)}
                      title={g.label}
                    >
                      <span className="nf">{TYPE_ICON[g.type]}</span> {g.label} <span className="n">{cnt}</span>
                    </button>
                  );
                })}
                <label className="fchip" title="是否显示系统提示词快照">
                  <input type="checkbox" checked={showSysPrompt} onChange={(e) => setShowSysPrompt(e.target.checked)} style={{ accentColor: 'var(--accent)' }} />
                  提示词
                </label>
                <div className="search">
                  <span className="nf" style={{ color: 'var(--dim)' }}>{NF.search}</span>
                  <input className="search-input" style={{ width: 200 }}
                    placeholder="搜索内容…" value={query} onChange={(e) => setQuery(e.target.value)} />
                </div>
                <span className="result-count">{visibleEvents.length} 条可见</span>
                <div className="view-toggle">
                  <button className={view === 'track' ? 'on' : ''} onClick={() => setView('track')}>
                    <span className="nf">{NF.sliders}</span> 轨道
                  </button>
                  <button className={view === 'list' ? 'on' : ''} onClick={() => setView('list')}>
                    <span className="nf">{NF.listUl}</span> 列表
                  </button>
                </div>
              </div>

              {/* 视图主体 */}
              <div className="view-body">
                <div className="pane-main">
                  {view === 'track' ? (
                    <TrackView
                      session={session}
                      events={visibleEvents}
                      onSelect={(idx) => setSelectedIdx(idx)}
                      selectedIdx={selectedIdx}
                    />
                  ) : (
                    <ListView
                      session={session}
                      events={visibleEvents}
                      onSelect={(idx) => setSelectedIdx(idx)}
                      selectedIdx={selectedIdx}
                    />
                  )}
                </div>
                {selEvent && session && (
                  <>
                    <div className="vh-handle" title="拖拽调整详情宽度"
                      onMouseDown={(e) => startResize('detail', e)} />
                    <DetailPanel
                      session={session}
                      event={selEvent}
                      onClose={() => setSelectedIdx(null)}
                      onSelect={setSelectedIdx}
                      width={detailW}
                    />
                  </>
                )}
              </div>
            </React.Fragment>
          )}
        </section>
      </div>
    </div>
  );
}

function fmtMetaTime(session) {
  const start = session.events.find((e) => e.valid && e.type === 'session_start');
  if (start && start.j && start.j.createdAt) return start.j.createdAt.replace('T', ' ').replace('Z', '');
  const first = session.events.find((e) => e.timestamp);
  return first ? first.timestamp : '';
}
