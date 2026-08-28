// WorkX 会话 API 封装（Vite dev server 中间件）

async function getJson(url) {
  const res = await fetch(url);
  const data = await res.json();
  if (!data.ok) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

export function fetchProjects() {
  return getJson('/api/projects');
}

export function fetchSessions(project) {
  return getJson(`/api/sessions?project=${encodeURIComponent(project)}`);
}

export function fetchSession(project, id) {
  return getJson(`/api/session?project=${encodeURIComponent(project)}&id=${encodeURIComponent(id)}`);
}

export function fetchSkills(cwd) {
  const q = cwd ? `?cwd=${encodeURIComponent(cwd)}` : '';
  return getJson(`/api/skills${q}`);
}

export async function deleteSession(project, id) {
  const res = await fetch(`/api/session?project=${encodeURIComponent(project)}&id=${encodeURIComponent(id)}`, {
    method: 'DELETE',
  });
  const data = await res.json();
  if (!data.ok) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}
