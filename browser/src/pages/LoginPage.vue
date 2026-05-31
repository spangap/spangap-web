<template>
  <div v-if="ready" class="auth-page flex flex-center">
    <q-card class="auth-card q-pa-lg" flat bordered>
      <q-card-section class="text-center q-pb-none">
        <div class="text-h5 text-weight-medium">Seccam</div>
        <div class="text-caption text-grey-6 q-mt-xs">Enter password to continue</div>
      </q-card-section>

      <q-card-section>
        <q-form action="/login" @submit.prevent="onLogin">
          <!-- Hidden username for password managers -->
          <input type="text" name="username" autocomplete="username" value="admin" class="hidden-input" tabindex="-1" />
          <q-input
            v-model="password"
            type="password"
            name="password"
            autocomplete="current-password"
            label="Password"
            outlined
            dense
            dark
            autofocus
            :error="!!errorMsg"
            :error-message="errorMsg"
            @update:model-value="errorMsg = ''"
          />
          <q-btn
            type="submit"
            label="Log in"
            color="primary"
            class="full-width q-mt-md"
            :loading="loading"
            :disable="!password"
          />
        </q-form>
      </q-card-section>
    </q-card>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { authLogin, setSessionCookie, checkAuth, isAdminUnset, AUTH_OK, AUTH_WRONG_PASSWORD, AUTH_RATE_LIMITED } from '../lib/auth'

const router = useRouter()
const password = ref('')
const errorMsg = ref('')
const loading = ref(false)
const ready = ref(false)

onMounted(async () => {
  try {
    if (await isAdminUnset()) { router.replace('/setup'); return }
    const auth = await checkAuth()
    if (!auth.enabled || auth.realm) { router.replace('/'); return }
  } catch { /* proceed — show form */ }
  ready.value = true
})

async function onLogin() {
  loading.value = true
  errorMsg.value = ''
  try {
    const r = await authLogin(password.value)
    if (r.result === AUTH_OK && r.cookie) {
      setSessionCookie(r.cookie)
      /* Remove form from DOM so Chrome's password manager detects submission
         (CanInferFormSubmitted checks form removal after fetch 2xx) */
      ready.value = false
      await nextTick()
      router.push('/')
    } else if (r.result === AUTH_RATE_LIMITED) {
      errorMsg.value = 'Too many attempts. Please wait.'
    } else if (r.result === AUTH_WRONG_PASSWORD) {
      errorMsg.value = 'Wrong password'
    } else {
      errorMsg.value = 'Login failed'
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
