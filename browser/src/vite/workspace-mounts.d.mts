// Types for the dev-server mounts plugin. quasar.config.ts is TypeScript, and
// the plugin itself is plain .mjs (it runs in the config loader, not in the app
// bundle), so its shape is declared here rather than inferred.
import type { Plugin } from 'vite'

/** Serve workspace directories as extra dev-server paths: { '<url prefix>': '<dir>' },
 *  each dir workspace-relative (SPANGAP_WORKSPACE) or absolute. */
export declare function workspaceMounts (mounts?: Record<string, string>): Plugin

export default workspaceMounts
