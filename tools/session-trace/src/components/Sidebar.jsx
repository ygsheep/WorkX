import React from 'react';
import { NF, TYPE_ICON } from '../lib/icons.js';
import { fmtDate, fmtSize } from '../lib/parser.js';

export default function Sidebar({
  projects, sessionsByProject, selectedProject, selectedId, width,
  collapsed, onToggleProject, onSelectProject, onSelectSession,
}) {
  // 项目目录名是编码后的绝对路径（分隔符转 '-'），展示时裁剪为最后的目录名
  const shortName = (p) => (p.split('-').filter(Boolean).pop() || p);
  if (!projects.length) {
    return (
      <aside className="sidebar">
        <div className="empty-hint">
          <div className="nf" style={{ fontSize: 26, color: 'var(--dim-2)' }}>{NF.database}</div>
          <br />
          未发现任何项目目录<br />
          <code>~/.workx/projects</code> 为空或不存在
        </div>
      </aside>
    );
  }

  return (
    <aside className="sidebar" style={{ width }}>
      <div className="side-stats">
        <span className="nf">{NF.folderOpen}</span> {projects.length} 个项目
      </div>
      <div className="side-list">
        {projects.map((proj) => {
          const sessions = sessionsByProject[proj] || null;
          const isOpen = !collapsed[proj];
          const hasErr = sessions && sessions.length === 0;
          return (
            <div className="proj-group" key={proj}>
              <div
                className={`proj-head ${isOpen ? '' : 'collapsed'} ${selectedProject === proj ? 'selected' : ''}`}
                onClick={() => { onSelectProject(proj); if (!isOpen) onToggleProject(proj); }}
                title="点击选中并展开会话列表"
              >
                <span
                  className="caret nf"
                  onClick={(e) => { e.stopPropagation(); onToggleProject(proj); }}
                  title="展开/收起"
                >{NF.caretDown}</span>
                <span className="nf proj-folder">{NF.folderOpen}</span>
                <span className="pn" title={proj}>{shortName(proj)}</span>
                <span className="cnt">
                  {sessions ? `${sessions.length} 会话` : '…'}
                </span>
              </div>
              {isOpen && sessions && (
                <div className="proj-sessions">
                  {sessions.map((s) => {
                    const active = selectedProject === proj && selectedId === s.id;
                    const meta = {
                      size: fmtSize(s.size),
                      time: fmtDate(s.mtime),
                      model: '',
                    };
                    return (
                      <div
                        key={s.id}
                        className={`session-item ${active ? 'active' : ''}`}
                        onClick={() => { onSelectProject(proj); onSelectSession(s.id); }}
                        title={s.file}
                      >
                        <div className="t">
                          <span className="nf" style={{ color: active ? 'var(--accent)' : 'var(--dim-2)' }}>{NF.file}</span>
                          <span title={s.title || s.file}>{s.title || s.file.replace(/\.jsonl$/i, '')}</span>
                        </div>
                        <div className="m">
                          <span className="nf">{NF.clock}</span>
                          <span>{meta.time}</span>
                          <span className="nf">{NF.database}</span>
                          <span>{meta.size}</span>
                          <span className="proj" title={proj}>{shortName(proj)}</span>
                        </div>
                      </div>
                    );
                  })}
                  {hasErr && <div className="empty-hint">该项目暂无会话记录</div>}
                </div>
              )}
            </div>
          );
        })}
      </div>
    </aside>
  );
}
