import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// WorkX 会话 API：直接读取 ~/.workx/projects 下的 JSONL 会话记录
// 以 Vite dev server 中间件形式提供，前端无需手动选目录

const projectsRoot = () => path.join(os.homedir(), '.workx', 'projects');

function listProjects() {
  const root = projectsRoot();
  if (!fs.existsSync(root)) return [];
  return fs
    .readdirSync(root, { withFileTypes: true })
    .filter((d) => d.isDirectory())
    .map((d) => d.name)
    .sort((a, b) => a.localeCompare(b, 'zh'));
}

function listSessions(project) {
  const dir = path.join(projectsRoot(), project);
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir)
    .filter((f) => f.toLowerCase().endsWith('.jsonl'))
    .map((f) => {
      const fp = path.join(dir, f);
      const st = fs.statSync(fp);
      return { id: f.replace(/\.jsonl$/i, ''), file: f, size: st.size, mtime: st.mtimeMs, title: extractTitle(fp) };
    })
    .sort((a, b) => b.mtime - a.mtime);
}

// 读取文件前部内容（标题事件通常靠前），提取会话标题；无 title 事件时回退首条用户消息
function extractTitle(fp) {
  let fd;
  try {
    fd = fs.openSync(fp, 'r');
  } catch {
    return '';
  }
  let text = '';
  try {
    const buf = Buffer.alloc(1024 * 256); // 最多读取前 256KB，避免大文件卡顿
    const n = fs.readSync(fd, buf, 0, buf.length, 0);
    text = buf.slice(0, n).toString('utf8');
  } finally {
    fs.closeSync(fd);
  }
  let title = '';
  let firstUser = '';
  for (const raw of text.split(/\r?\n/)) {
    if (!raw.trim()) continue;
    let obj;
    try {
      obj = JSON.parse(raw);
    } catch {
      continue;
    }
    const type = obj && obj.type;
    if (type === 'title' && typeof obj.title === 'string' && obj.title) {
      title = obj.title;
    } else if (type === 'user' && !firstUser && typeof obj.content === 'string') {
      firstUser = obj.content;
    }
  }
  if (title) return title;
  if (firstUser) return Array.from(firstUser).slice(0, 20).join('') + (Array.from(firstUser).length > 20 ? '…' : '');
  return '';
}

// ============ Skill 搜索（对齐 src/agent/skill 的磁盘扫描逻辑） ============
// 采集用户级 skill 目录：~/.claude/skills 与 ~/.workx/skills
function findUserSkillDirs() {
  const home = os.homedir();
  const dirs = [];
  for (const sub of ['.claude', '.workx']) {
    const c = path.join(home, sub, 'skills');
    if (fs.existsSync(c) && fs.statSync(c).isDirectory()) dirs.push(c);
  }
  return dirs;
}

// 从 cwd 逐级向上收集 <dir>/.claude/skills 与 <dir>/.workx/skills（对齐 find_skill_dirs_up_to_home）
function findSkillDirsUpToHome(cwd) {
  const dirs = [];
  let p = path.resolve(cwd || '');
  for (;;) {
    for (const sub of ['.claude', '.workx']) {
      const c = path.join(p, sub, 'skills');
      if (fs.existsSync(c) && fs.statSync(c).isDirectory()) dirs.push(c);
    }
    const parent = path.dirname(p);
    if (parent === p || !parent) break;
    p = parent;
  }
  return dirs;
}

// 极简 frontmatter 解析：扁平 key: value（不支嵌套），字段对齐 agent::skill::SkillFrontmatter
function parseSkillContent(content, defaultName) {
  const fm = {
    name: defaultName,
    description: '',
    aliases: [],
    argument_hint: null,
    when_to_use: null,
    context: null,
    agent: null,
    hooks: [],
    model: null,
    user_invocable: true,
    disable_model_invocation: false,
    paths: [],
  };
  const trim = (s) => s.trim();
  const isFence = (line) => trim(line) === '---';
  const stripQuotes = (v) =>
    v.length >= 2 && ((v[0] === '"' && v[v.length - 1] === '"') || (v[0] === "'" && v[v.length - 1] === "'"))
      ? v.slice(1, -1)
      : v;
  const parseList = (v) => {
    let t = trim(v);
    if (t.length >= 2 && t[0] === '[' && t[t.length - 1] === ']') t = t.slice(1, -1);
    const out = [];
    let pos = 0;
    while (pos <= t.length) {
      const comma = t.indexOf(',', pos);
      const item = trim(t.substring(pos, comma === -1 ? t.length : comma));
      if (item) out.push(stripQuotes(item));
      if (comma === -1) break;
      pos = comma + 1;
    }
    return out;
  };
  const deriveDescription = (body) => {
    const nl = body.indexOf('\n');
    let line = trim(nl === -1 ? body : body.slice(0, nl));
    if (line[0] === '#') line = trim(line.slice(1));
    return line;
  };

  const firstNl = content.indexOf('\n');
  const firstLine = firstNl === -1 ? content : content.slice(0, firstNl);

  // 无 frontmatter：全部视为正文
  if (!content || !isFence(firstLine)) {
    fm.description = deriveDescription(content);
    return { fm, body: content };
  }

  // 分离 frontmatter 块与正文
  let rest = firstNl === -1 ? '' : content.slice(firstNl + 1);
  const secondFence = rest.indexOf('\n---');
  if (secondFence === -1) {
    // 未闭合：整个文档视为正文
    fm.description = deriveDescription(content);
    return { fm, body: content };
  }
  const frontPart = rest.slice(0, secondFence);
  let bodyStart = secondFence + 4;
  if (rest[bodyStart] === '\r') bodyStart++;
  if (rest[bodyStart] === '\n') bodyStart++;
  const body = rest.slice(bodyStart);

  // 逐行解析 key: value
  let lastListKey = '';
  const fmKey = {
    name: (v) => v && (fm.name = v),
    description: (v) => (fm.description = v),
    aliases: (v) => (fm.aliases = parseList(v)),
    argument_hint: (v) => (fm.argument_hint = v),
    when_to_use: (v) => (fm.when_to_use = v),
    context: (v) => (fm.context = v),
    agent: (v) => (fm.agent = v),
    hooks: (v) => (fm.hooks = parseList(v)),
    model: (v) => (fm.model = v),
    user_invocable: (v) => (fm.user_invocable = /^(true|yes|1)$/i.test(v) ? true : /^(false|no|0)$/i.test(v) ? false : true),
    disable_model_invocation: (v) => (fm.disable_model_invocation = /^(true|yes|1)$/i.test(v) ? true : false),
    paths: (v) => (fm.paths = parseList(v)),
  };
  const listKeys = { aliases: true, hooks: true, paths: true };
  for (const raw of frontPart.split('\n')) {
    const line = trim(raw);
    if (!line || line[0] === '#') continue;
    if (lastListKey && line[0] === '-') {
      const item = trim(line.slice(1));
      if (item && listKeys[lastListKey]) fm[lastListKey].push(stripQuotes(item));
      continue;
    }
    const colon = line.indexOf(':');
    if (colon === -1) { lastListKey = ''; continue; }
    const key = trim(line.slice(0, colon)).toLowerCase();
    const value = trim(line.slice(colon + 1));
    if (fmKey[key]) fmKey[key](value);
    lastListKey = listKeys[key] ? key : '';
  }
  if (!fm.description) fm.description = deriveDescription(body);
  return { fm, body };
}

// 扫描各 base dir 下的 <name>/SKILL.md，解析并按路径/名称去重（同 src/agent 的 load_skills_from_dirs）
function loadSkills(baseDirs) {
  const result = [];
  const seen = new Set();
  const seenNames = new Set();
  for (const baseDir of baseDirs) {
    let entries;
    try {
      entries = fs.readdirSync(baseDir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const entry of entries) {
      if (!entry.isDirectory()) continue;
      const skillFile = path.join(baseDir, entry.name, 'SKILL.md');
      let content;
      try {
        content = fs.readFileSync(skillFile, 'utf8');
      } catch {
        continue;
      }
      let key = skillFile;
      try {
        key = fs.realpathSync(skillFile);
      } catch { /* 忽略，回退相对路径 */ }
      if (seen.has(key)) continue;
      seen.add(key);
      const { fm } = parseSkillContent(content, entry.name);
      if (seenNames.has(fm.name)) continue; // 近目录优先，不重复注册同名技能
      seenNames.add(fm.name);
      result.push({ name: fm.name, description: fm.description, aliases: fm.aliases, dir: path.dirname(skillFile) });
    }
  }
  return result;
}

// 防止路径穿越：project/id 不允许含路径分隔符或 ..
function isSafeName(s) {
  return !!s && !/\.\.|[/\\]/.test(s) && s !== '.';
}

function readSession(project, id) {
  const fp = path.join(projectsRoot(), project, id + '.jsonl');
  if (!fs.existsSync(fp)) return null;
  const text = fs.readFileSync(fp, 'utf8');
  const lines = text.split(/\r?\n/).filter((l) => l.trim().length > 0);
  return lines.map((raw, i) => {
    try {
      return { idx: i, ok: true, obj: JSON.parse(raw), raw };
    } catch {
      return { idx: i, ok: false, raw };
    }
  });
}

function deleteSession(project, id) {
  const fp = path.join(projectsRoot(), project, id + '.jsonl');
  if (!fs.existsSync(fp)) return false;
  fs.rmSync(fp, { force: true });
  return true;
}

function send(res, obj, status = 200) {
  res.statusCode = status;
  res.setHeader('Content-Type', 'application/json; charset=utf-8');
  res.end(JSON.stringify(obj));
}

export function workxSessionApi() {
  return {
    name: 'workx-session-api',
    configureServer(server) {
      server.middlewares.use('/api', (req, res, next) => {
        const u = new URL(req.url, 'http://localhost');
        const q = u.searchParams;
        try {
          switch (u.pathname) {
            case '/projects':
              return send(res, { ok: true, root: projectsRoot(), projects: listProjects() });
            case '/sessions': {
              const project = q.get('project') || '';
              return send(res, { ok: true, project, sessions: listSessions(project) });
            }
            case '/session': {
              const project = q.get('project') || '';
              const id = q.get('id') || '';
              if (!isSafeName(project) || !isSafeName(id)) return send(res, { ok: false, error: '非法参数' }, 400);
              if (req.method === 'DELETE') {
                const existed = deleteSession(project, id);
                if (!existed) return send(res, { ok: false, error: '会话不存在' }, 404);
                return send(res, { ok: true, project, id });
              }
              const data = readSession(project, id);
              if (!data) return send(res, { ok: false, error: '会话不存在' }, 404);
              return send(res, { ok: true, project, id, events: data });
            }
            case '/skills': {
              const cwd = q.get('cwd') || '';
              const baseDirs = findUserSkillDirs();
              if (cwd) baseDirs.unshift(...findSkillDirsUpToHome(cwd));
              return send(res, { ok: true, roots: baseDirs, skills: loadSkills(baseDirs) });
            }
            default:
              return next();
          }
        } catch (err) {
          return send(res, { ok: false, error: String((err && err.message) || err) }, 500);
        }
      });
    },
  };
}
