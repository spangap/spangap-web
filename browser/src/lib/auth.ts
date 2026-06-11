/** Auth error codes (must match auth_err_t in auth.h) */
export const AUTH_OK = 0
export const AUTH_NO_SUCH_REALM = 1
export const AUTH_WRONG_PASSWORD = 2
export const AUTH_SAME_AS_OTHER_REALM = 3
export const AUTH_RATE_LIMITED = 4

/** Base URL for auth API calls (same origin, HTTP or HTTPS). */
function authBase(): string {
  return `${window.location.origin}/auth`
}

export interface LoginResult {
  result: number
  realm?: string
  cookie?: string
}

export interface PasswdResult {
  result: number
}

/** POST /auth/login */
export async function authLogin(password: string, realm = ''): Promise<LoginResult> {
  const body: Record<string, string> = { password }
  if (realm) body.realm = realm
  const r = await fetch(`${authBase()}/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  return r.json()
}

/** POST /auth/passwd */
export async function authPasswd(realm: string, oldPw: string, newPw: string): Promise<PasswdResult> {
  const r = await fetch(`${authBase()}/passwd`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ realm, old: oldPw, new: newPw }),
  })
  return r.json()
}

/** Set the session cookie from a login result. */
export function setSessionCookie(cookie: string) {
  document.cookie = `session=${cookie}; path=/; max-age=5184000; SameSite=Strict`
}

/** Clear the session cookie. */
export function clearSessionCookie() {
  document.cookie = 'session=; path=/; max-age=0; SameSite=Strict'
}

/** Check if admin password is set. Returns true if unset (needs onboarding). */
export async function isAdminUnset(): Promise<boolean> {
  const r = await authPasswd('admin', '', '')
  return r.result === AUTH_OK
}

/** POST /auth/logout — deletes the current session cookie server-side. */
export async function authLogout(): Promise<void> {
  await fetch(`${authBase()}/logout`, { method: 'POST' })
  clearSessionCookie()
}

/** Check auth state by probing the device's X-Authenticated header.
 *
 * Production: HEAD `/` — the device serves the SPA index with the header.
 * `spangap dev`: `/` is the Vite SPA (no header), so probe `/__authcheck`, which
 * the dev server reverse-proxies to the device root (see quasar.config.ts). The
 * device is always TLS, so a non-https origin means we're on the dev server. */
export async function checkAuth(): Promise<{ enabled: boolean; realm: string }> {
  const onDevServer =
    typeof window !== 'undefined' && window.location.protocol !== 'https:'
  const probe = onDevServer ? '/__authcheck' : '/'
  const r = await fetch(probe, { method: 'HEAD' })
  const header = r.headers.get('X-Authenticated')
  if (header === null) return { enabled: false, realm: '' }
  return { enabled: true, realm: header }
}
