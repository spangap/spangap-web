// Hot-reload the workspace packages an app links with `file:` dependencies.
//
// Those packages ARE the code under development, but nothing watches them: the
// app resolves them with `resolve.preserveSymlinks`, so their module ids sit
// under <root>/node_modules/<name>/…, and Vite's watcher prepends
// `**/node_modules/**` to its ignore list — an ignore a config can add to but
// never subtract from. The symlink target is outside the root, so it isn't
// watched either. Without an event Vite's transform cache is never invalidated,
// which is why a reload of the page still serves the edit from before it.
//
// So watch the real directories here and re-emit each event on Vite's own
// watcher under the <root>/node_modules/<name>/… path the module graph is keyed
// by. From there it is an ordinary file change: HMR, or a reload, as usual.
//
//   vitePlugins: [ [ linkedDepsHmr, {} ] ]
//
// TWO mechanisms, because one of them is not dependable here. `fs.watch` is
// instant but rides on inotify, which a bind-mounted or FUSE-backed workspace —
// the build container's normal case — may deliver nothing through, silently. A
// dev server that sees no events is worse than one that is slow: the page keeps
// serving the code from whenever the server started, and the edit looks like it
// was never made. So an mtime scan runs alongside it and re-emits whatever the
// watch missed, which bounds the staleness at POLL_MS on any filesystem. Both
// go through the same table, so a file seen twice is still one event.
//
// Dev only — the plugin declares no build hooks.
import fs from 'node:fs'
import path from 'node:path'

/* Editors write a file as several syscalls (truncate, write, rename); one
 * settle window per path turns that back into the single change it was. */
const SETTLE_MS = 25

/* The scan is the floor on how stale a page can be where inotify is silent.
 * A source tree is a few hundred files — one stat each, once a second. */
const POLL_MS = 1000

/* Neither is source, and a package's own node_modules would multiply the watch
 * for nothing. */
const SKIP = new Set(['node_modules', '.git'])

/** Every `file:` dependency of the app that really is a symlink, as the pair of
 *  paths that matters: where the module graph thinks it is, and where it is. */
function linkedDeps (root) {
  let pkg
  try {
    pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'))
  } catch (_) {
    return []
  }
  const out = []
  for (const [ name, spec ] of Object.entries(pkg.dependencies ?? {})) {
    if (typeof spec !== 'string' || !spec.startsWith('file:')) continue
    const linked = path.join(root, 'node_modules', name)
    let real
    try {
      real = fs.realpathSync(linked)
    } catch (_) {
      continue   // declared but not installed
    }
    if (real !== linked) out.push({ name, linked, real })
  }
  return out
}

/** Every file under `dir` with the mtime it had when looked at. The table is
 *  both the record of what exists (so a create is not reported as a change) and
 *  the record of what it held (so the scan can tell a rewrite from a re-read). */
function scan (dir, into = new Map()) {
  let entries
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true })
  } catch (_) {
    return into
  }
  for (const e of entries) {
    if (SKIP.has(e.name)) continue
    const p = path.join(dir, e.name)
    if (e.isDirectory()) { scan(p, into); continue }
    try {
      into.set(p, fs.statSync(p).mtimeMs)
    } catch (_) { /* vanished between readdir and stat */ }
  }
  return into
}

export function linkedDepsHmr () {
  const stop = []

  return {
    name: 'spangap:linked-deps-hmr',
    apply: 'serve',

    configureServer (server) {
      const log = server.config.logger

      for (const dep of linkedDeps(server.config.root)) {
        /* path -> mtime, for every file of this package. */
        const seen = scan(dep.real)
        const pending = new Map()

        /** Report one file to Vite under the path its module graph knows, but
         *  only if it really moved: the watch and the scan both come through
         *  here, and the table is what stops the second one repeating it. */
        const settle = (real) => {
          let mtime = null
          try {
            mtime = fs.statSync(real).mtimeMs
          } catch (_) { /* gone */ }

          const had = seen.has(real)
          if (mtime === null) {
            if (!had) return
            seen.delete(real)
          } else {
            if (had && seen.get(real) === mtime) return
            seen.set(real, mtime)
          }
          const kind = mtime === null ? 'unlink' : (had ? 'change' : 'add')
          server.watcher.emit(kind, path.join(dep.linked, path.relative(dep.real, real)))
        }

        // One recursive watch per package: the trees are source directories,
        // and a watch per subdirectory would buy nothing but descriptors.
        try {
          const w = fs.watch(dep.real, { recursive: true }, (_event, rel) => {
            if (!rel) return
            const sub = rel.toString()
            if (sub.split(path.sep).some(seg => SKIP.has(seg))) return
            const real = path.join(dep.real, sub)
            clearTimeout(pending.get(real))
            pending.set(real, setTimeout(() => { pending.delete(real); settle(real) }, SETTLE_MS))
          })
          stop.push(() => w.close())
        } catch (err) {
          log.warn(`spangap:linked-deps-hmr: ${ dep.name } has no watch — ${ err.message }`)
        }

        // The scan: whatever the watch didn't tell us, on its own clock.
        const timer = setInterval(() => {
          const now = scan(dep.real)
          for (const file of now.keys()) settle(file)
          for (const file of [ ...seen.keys() ]) if (!now.has(file)) settle(file)
        }, POLL_MS)
        timer.unref?.()
        stop.push(() => clearInterval(timer))

        log.info(`spangap:linked-deps-hmr: watching ${ dep.name } -> ${ dep.real }`)
      }
    },

    closeBundle () {
      for (const fn of stop.splice(0)) fn()
    },
  }
}

export default linkedDepsHmr
