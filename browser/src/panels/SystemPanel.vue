<template>
  <div class="q-gutter-y-md">
    <PanelHeading>This System</PanelHeading>
    <div class="q-gutter-y-sm">
      <SettingText label="Hostname" k="s.net.hostname" />
    </div>

    <q-separator dark />

    <PanelHeading>Time &amp; Date</PanelHeading>
    <div class="q-gutter-y-sm">
      <div class="row items-center no-wrap">
        <div class="col-4 text-caption">Timezone</div>
        <q-select
          class="col"
          :model-value="currentTz"
          :options="filteredTzOptions"
          dense outlined
          emit-value map-options
          options-dense
          use-input
          input-debounce="100"
          @filter="filterTz"
          @update:model-value="onTzChange"
        />
      </div>
      <SettingText label="NTP Server" k="s.ntp.server" />
    </div>

    <q-separator dark />

    <PanelHeading>Backup &amp; Recovery</PanelHeading>
    <div class="q-gutter-y-sm">
      <div class="text-caption" style="opacity:0.7; line-height:1.35">
        Each of these reboots the device into a safe mode that runs the
        operation with nothing else touching its state, then reboots again.
        Everything else stays off for the duration.
      </div>
      <div class="row q-gutter-sm">
        <q-btn dense no-caps outline color="primary" label="Back up state"
               @click="backupAsk = true" />
        <q-btn dense no-caps outline color="primary" label="Restore from backup"
               @click="restoreAsk = true" />
        <q-btn dense no-caps outline color="negative" label="Factory reset"
               @click="factoryAsk = true" />
      </div>
    </div>

    <!-- Every one of the three takes the device away for a reboot, so every one
         says so before it happens; restore and factory reset additionally have
         no undo, and say that too. -->
    <q-dialog v-model="backupAsk">
      <q-card dark style="max-width:26rem">
        <q-card-section class="text-h6">Back up state</q-card-section>
        <q-card-section class="text-body2">
          To back up all the user state on this device, it needs to reboot in
          safe mode, send you the file and reboot again.
        </q-card-section>
        <q-card-actions align="right">
          <q-btn flat no-caps label="Cancel" v-close-popup />
          <q-btn flat no-caps color="primary" label="OK"
                 v-close-popup @click="enterSafeMode('backup')" />
        </q-card-actions>
      </q-card>
    </q-dialog>

    <q-dialog v-model="restoreAsk">
      <q-card dark style="max-width:26rem">
        <q-card-section class="text-h6">Restore from a backup</q-card-section>
        <q-card-section class="text-body2">
          The device reboots into safe mode and asks for an archive. Everything
          on its state store — settings, identities, keys, messages — is erased
          before the archive is written, and there is no way back to what is
          there now.
        </q-card-section>
        <q-card-actions align="right">
          <q-btn flat no-caps label="Cancel" v-close-popup />
          <q-btn flat no-caps color="primary" label="Reboot and restore"
                 v-close-popup @click="enterSafeMode('restore')" />
        </q-card-actions>
      </q-card>
    </q-dialog>

    <q-dialog v-model="factoryAsk">
      <q-card dark style="max-width:26rem">
        <q-card-section class="text-h6">Factory reset</q-card-section>
        <q-card-section class="text-body2">
          Every trace of this device's configuration, keys and identity is
          overwritten with random bytes — not deleted, overwritten, so nothing
          survives a flash dump. It comes back as a brand-new device on its own
          access point, and this browser will no longer find it here.
        </q-card-section>
        <q-card-section v-if="hasSd">
          <div class="text-caption q-mb-xs">What to erase</div>
          <q-option-group dark dense v-model="wipeTarget" :options="wipeOptions" />
        </q-card-section>
        <q-card-actions align="right">
          <q-btn flat no-caps label="Cancel" v-close-popup />
          <q-btn flat no-caps color="negative" label="Erase everything"
                 v-close-popup @click="enterSafeMode('factory', wipeTarget)" />
        </q-card-actions>
      </q-card>
    </q-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch, onMounted } from 'vue'
import { useDeviceStore } from '../stores/device'
import { enterSafeMode, WIPE_FLASH, WIPE_SD, WIPE_BOTH } from '../lib/safeMode'

const device = useDeviceStore()

const backupAsk = ref(false)
const restoreAsk = ref(false)
const factoryAsk = ref(false)

/* The wipe target is only a question on a device with a card: the flash region
 * can hold a stale store even when the active one is on SD, which is why
 * "both" exists. Without a card there is only one answer. */
const hasSd = computed(() => Number(device.get('sys.sd.present') ?? 0) === 1)
const wipeTarget = ref(WIPE_FLASH)
const wipeOptions = [
  { label: 'Device flash', value: WIPE_FLASH },
  { label: 'SD card state', value: WIPE_SD },
  { label: 'Both', value: WIPE_BOTH },
]

watch(hasSd, (sd) => { if (sd) wipeTarget.value = WIPE_BOTH })

const currentTz = computed(() => String(device.get('s.ntp.tz') ?? ''))

/* The IANA→POSIX map no longer lives in config — it's a plain file on the
 * device at /state/timezones.json. Fetch it once to populate the dropdown.
 * onTzChange sends only s.ntp.tz; the device resolves POSIX from that file. */
const tzOptions = ref<{ label: string; value: string }[]>([])

function collectZones(node: any, prefix: string, out: { label: string; value: string }[]) {
  for (const [name, val] of Object.entries(node as Record<string, any>)) {
    if (name === 'updated') continue
    const iana = prefix ? `${prefix}/${name}` : name
    if (typeof val === 'string') out.push({ label: iana.replace(/_/g, ' '), value: iana })
    else if (val && typeof val === 'object') collectZones(val, iana, out)
  }
}

onMounted(async () => {
  try {
    const r = await fetch('/state/timezones.json', { credentials: 'same-origin', cache: 'no-cache' })
    if (!r.ok) return
    const zones = await r.json()
    if (!zones || typeof zones !== 'object') return
    const opts: { label: string; value: string }[] = []
    collectZones(zones, '', opts)
    opts.sort((a, b) => a.label.localeCompare(b.label))
    tzOptions.value = opts
    filteredTzOptions.value = opts
  } catch { /* offline — leave dropdown empty */ }
})

const filteredTzOptions = ref<{ label: string; value: string }[]>([])

watch(tzOptions, () => { filteredTzOptions.value = tzOptions.value })

function filterTz(val: string, update: (fn: () => void) => void) {
  update(() => {
    const needle = val.toLowerCase()
    filteredTzOptions.value = needle
      ? tzOptions.value.filter(o => o.label.toLowerCase().includes(needle))
      : tzOptions.value
  })
}

function onTzChange(ianaName: string) {
  device.set('s.ntp.tz', ianaName)
}
</script>
