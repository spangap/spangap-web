// Serve directories of the spangap workspace as extra paths on the Quasar dev
// server, so a page that expects them alongside itself in production finds them
// there under `spangap dev` too.
//
// Vite has exactly one static root (publicDir), so extra trees can only be
// middleware. Mounts are registered in the configureServer body, which puts them
// AHEAD of Vite's own static and transform middlewares — otherwise a request
// would be resolved as an app asset before it ever reached us.
//
//   vitePlugins: [
//     [ workspaceMounts, { '/flashmon': 'flashmon/flashmon', '/builds': 'builds' } ],
//   ]
//
// Values are workspace-relative (SPANGAP_WORKSPACE, which `spangap dev` sets),
// or absolute. A mount whose directory isn't there is skipped with a warning: a
// bare `quasar dev` outside a workspace still starts, minus these paths.
//
// Dev only — the plugin declares no build hooks, and in production these trees
// are served by whatever serves the app.
import fs from 'node:fs'
import path from 'node:path'

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.yaml': 'text/plain; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
  '.md': 'text/plain; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.ico': 'image/x-icon',
  '.zip': 'application/zip',
  '.bin': 'application/octet-stream',
}

function send (res, status, body) {
  res.statusCode = status
  res.setHeader('Content-Type', 'text/plain; charset=utf-8')
  res.end(body)
}

// One mount's request handler. `root` is absolute and already known to exist.
function serveFrom (root, prefix) {
  return (req, res, next) => {
    if (req.method !== 'GET' && req.method !== 'HEAD') return next()

    // req.url is relative to the mount prefix (connect strips it). Drop the
    // query/hash before it reaches the filesystem.
    let rel
    try {
      rel = decodeURIComponent(req.url.split(/[?#]/)[ 0 ])
    } catch (_) {
      return send(res, 400, 'bad request')
    }

    // What the browser asked for, before the prefix was stripped. The mount root
    // arrives here as '/' whether or not the trailing slash was typed, so this is
    // the only way to tell `/flashmon` from `/flashmon/` — and the difference
    // matters: without the slash the page's base URL is the parent, so every
    // relative asset in it resolves one level too high.
    const orig = (req.originalUrl || prefix + rel).split(/[?#]/)[ 0 ]

    // resolve() collapses any ../ the URL carried; the prefix check is what
    // makes escaping the mount impossible rather than merely awkward.
    const file = path.resolve(root, '.' + rel)
    if (file !== root && !file.startsWith(root + path.sep)) {
      return send(res, 403, 'forbidden')
    }

    let target = file
    let st
    try {
      st = fs.statSync(target)
      if (st.isDirectory()) {
        // A directory URL without its trailing slash would make the page's own
        // relative links resolve one level too high — the mount root included,
        // which is why this is asked of the original URL and not of `rel`.
        if (!orig.endsWith('/')) {
          res.statusCode = 301
          res.setHeader('Location', orig + '/')
          return res.end()
        }
        target = path.join(file, 'index.html')
        st = fs.statSync(target)
      }
      if (!st.isFile()) throw new Error('not a file')
    } catch (_) {
      // Deliberately not next(): passing this on would hand the request to the
      // SPA history fallback, and a missing image would come back as the app's
      // index.html for a caller expecting a zip.
      return send(res, 404, 'not found')
    }

    // An extensionless file here is a small text marker (the catalogue's
    // `timestamp`), not a binary blob — the octet-stream default is for things
    // that named an extension we don't know.
    const ext = path.extname(target).toLowerCase()
    res.statusCode = 200
    res.setHeader('Content-Type',
      TYPES[ ext ] || (ext ? 'application/octet-stream' : 'text/plain; charset=utf-8'))
    res.setHeader('Content-Length', String(st.size))
    // These trees change under a running dev server — a rebuilt catalogue is the
    // point — so nothing here may be held.
    res.setHeader('Cache-Control', 'no-store')
    if (req.method === 'HEAD') return res.end()
    fs.createReadStream(target).pipe(res)
  }
}

export function workspaceMounts (mounts = {}) {
  return {
    name: 'spangap:workspace-mounts',
    apply: 'serve',
    configureServer (server) {
      const ws = process.env.SPANGAP_WORKSPACE
      for (const [ prefix, dir ] of Object.entries(mounts)) {
        const root = path.isAbsolute(dir) ? dir : path.resolve(ws || process.cwd(), dir)
        if (!fs.existsSync(root)) {
          server.config.logger.warn(
            `spangap:workspace-mounts: ${ prefix } not served — no ${ root }`
          )
          continue
        }
        server.middlewares.use(prefix, serveFrom(root, prefix))
        server.config.logger.info(`spangap:workspace-mounts: ${ prefix } -> ${ root }`)
      }
    },
  }
}

export default workspaceMounts
