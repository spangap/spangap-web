<template>
  <div v-if="ready" class="auth-page flex flex-center">
    <q-card class="auth-card q-pa-lg" flat bordered>
      <q-card-section class="text-center q-pb-none">
        <div class="text-h5 text-weight-medium">Seccam Setup</div>
        <div class="text-caption text-grey-6 q-mt-xs">Set an admin password to get started</div>
      </q-card-section>

      <q-card-section>
        <q-form action="/setup" @submit.prevent="onSetPassword">
          <!-- Hidden username for password managers -->
          <input type="text" name="username" autocomplete="username" value="admin" class="hidden-input" tabindex="-1" />
          <q-input
            v-model="password"
            type="password"
            name="new-password"
            autocomplete="new-password"
            label="New password"
            outlined
            dense
            dark
            autofocus
            :error="!!errorMsg"
            @update:model-value="errorMsg = ''"
          />
          <q-input
            v-model="confirm"
            type="password"
            name="confirm-password"
            autocomplete="new-password"
            label="Confirm password"
            outlined
            dense
            dark
            class="q-mt-sm"
            :error="!!errorMsg"
            :error-message="errorMsg"
            @update:model-value="errorMsg = ''"
          />
          <q-btn
            type="submit"
            label="Set password"
            color="primary"
            class="full-width q-mt-md"
            :loading="loading"
            :disable="!password || !confirm"
          />
        </q-form>
      </q-card-section>
    </q-card>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { authPasswd, authLogin, setSessionCookie, isAdminUnset, AUTH_OK, AUTH_SAME_AS_OTHER_REALM } from '../lib/auth'

const router = useRouter()
const password = ref('')
const confirm = ref('')
const errorMsg = ref('')
const loading = ref(false)
const ready = ref(false)

onMounted(async () => {
  try {
    if (!await isAdminUnset()) { router.replace('/'); return }
  } catch { /* proceed — show form */ }
  ready.value = true
})

async function onSetPassword() {
  if (password.value !== confirm.value) {
    errorMsg.value = 'Passwords do not match'
    return
  }
  loading.value = true
  errorMsg.value = ''
  try {
    const r = await authPasswd('admin', '', password.value)
    if (r.result === AUTH_OK) {
      /* Auto-login with the password we just set */
      const login = await authLogin(password.value)
      if (login.result === AUTH_OK && login.cookie) {
        setSessionCookie(login.cookie)
      }
      ready.value = false
      await nextTick()
      router.push('/')
    } else if (r.result === AUTH_SAME_AS_OTHER_REALM) {
      errorMsg.value = 'Password already used by another realm'
    } else {
      errorMsg.value = 'Failed to set password'
    }
  } catch {
    errorMsg.value = 'Connection error'
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.auth-page {
  min-height: 100vh;
  background: #121212;
  color: white;
}
.auth-card {
  width: 320px;
  max-width: 90vw;
  background: #1e1e1e;
}
.hidden-input {
  position: absolute;
  left: -9999px;
  width: 1px;
  height: 1px;
}
</style>
