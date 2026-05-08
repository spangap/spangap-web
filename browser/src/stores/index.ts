// Re-export hub for diptych-browser store hooks.
//
// The pinia *instance* (createPinia()) lives in the consuming app — Quasar
// auto-installs `app/src/stores/index.ts`'s default export. defineStore()
// calls in this package register against whatever pinia is set as active
// at first use.
export { useDeviceStore } from './device'
export { useLogStore } from './log'
export { useMenuStore } from './menu'
