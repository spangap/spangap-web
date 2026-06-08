<template>
  <!-- Teleported to <body> so it covers the ENTIRE viewport — menu bar,
       drawers, dialogs and all — regardless of where this component is
       mounted. Shown whenever the device link is down (no pong for >4s, or a
       dropped channel) and held until the link is back AND the storage tree
       has resynced. While visible it sits above everything and swallows all
       pointer/keyboard interaction, so nothing on the page can be touched. -->
  <Teleport to="body">
    <Transition name="conn-fade">
      <div
        v-if="device.linkDown"
        class="conn-overlay"
        role="alertdialog"
        aria-live="assertive"
        @click.capture.stop.prevent
        @mousedown.capture.stop.prevent
        @keydown.capture.stop.prevent
        @wheel.capture.stop.prevent
        @touchstart.capture.stop.prevent
        @contextmenu.capture.stop.prevent
      >
        <div class="conn-overlay__msg">Disconnected, stand by for reconnect.</div>
      </div>
    </Transition>
  </Teleport>
</template>

<script setup lang="ts">
import { useDeviceStore } from '../stores/device'

const device = useDeviceStore()
</script>

<style scoped>
.conn-overlay {
  position: fixed;
  inset: 0;
  /* Above Quasar's highest layers (notifications sit at 9998). */
  z-index: 100000;
  /* Dark, slightly-transparent scrim: the UI behind shows through just enough
   * to be recognizable, but is clearly dimmed/disabled. */
  background: rgba(0, 0, 0, 0.55);
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  cursor: default;
  /* Belt-and-braces over the per-event guards above. */
  user-select: none;
  -webkit-user-select: none;
}
.conn-overlay__msg {
  color: #fff;
  font-size: clamp(20px, 4vw, 34px);
  font-weight: 700;
  letter-spacing: 0.2px;
  text-align: center;
  max-width: 90vw;
  text-shadow: 0 2px 8px rgba(0, 0, 0, 0.6);
}
.conn-fade-enter-active,
.conn-fade-leave-active {
  transition: opacity 0.18s ease;
}
.conn-fade-enter-from,
.conn-fade-leave-to {
  opacity: 0;
}
</style>
