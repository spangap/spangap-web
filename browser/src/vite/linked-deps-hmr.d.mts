// Types for the linked-package HMR plugin. quasar.config.ts is TypeScript and
// the plugin itself is plain .mjs (it runs in the config loader, not in the app
// bundle), so its shape is declared here rather than inferred.
import type { Plugin } from 'vite'

/** Watch the app's `file:` dependencies where they really live, so an edit to
 *  one reaches the browser without a dev-server restart. Takes no options. */
export declare function linkedDepsHmr (): Plugin

export default linkedDepsHmr
