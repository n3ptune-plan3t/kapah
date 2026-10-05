# Security review notes — kapah

Living checklist for the security-sensitive parts of the tree. Every item
says what the invariant is, which code enforces it, and what the reviewer
should verify. The lock screen and the notification markup sanitiser are the
two components that must pass a second-agent review against this list.

## S1. Lock release requires authentication

**Invariant.** `SessionLock::unlockAndDestroy()` is reached only from the
PAM-success path in `LockView::onPamResult()`. No timeout, no close handler,
no error branch, no signal-handler path may ever call it.

**Code:** `src/wl/sessionlock.{h,cpp}` (private member, friend
`LockView`), `src/lock/lockview.{h,cpp}` (single call site in
`onPamResult`), `src/lock/main.cpp` (no --password-timeout logic that
unlocks; the parsed timeout is currently inert).

**Reviewer checklist.**
- [ ] Grep for `unlockAndDestroy` call sites; exactly one.
- [ ] The destructor path of `SessionLock` destroys protocol objects but
      never unlocks.
- [ ] If `kapah-lock` is SIGKILLed, niri keeps the session locked (protocol
      behaviour; cite ext-session-lock-v1.xml, §"if the client dies").
- [ ] On `lockLost`/`finished`, the process exits and does not attempt to
      re-create the lock silently.

## S2. First ack only after a fully opaque frame

**Invariant.** A configure is acknowledged only from the paint path after an
`ARGB8888`, alpha-255 frame has been attached, so the compositor can never
show a partially drawn or transparent lock surface (which would reveal the
frozen session behind it).

**Code:** `LockSurface::attachOpaqueBufferAndAcknowledge` is called only once
per surface (first configure); `updateOpaqueBuffer` thereafter.

**Reviewer checklist.**
- [ ] `PerScreen::firstConfigureSeen` guards the ack path.
- [ ] The attached QImage is `Format_ARGB32`, filled opaque before paint.
- [ ] No other code attaches/acknowledges.

## S3. PAM hygiene

**Invariant.** The password exists in plaintext only inside
`LockView::m_field` and the worker's `g_password` buffer, and both are
scrubbed (explicit_bzero or volatile-write fallback) before the buffer is
reused.

**Code:** `src/lock/pam.{h,cpp}` (`scrub`, `PamWorker::run`),
`src/lock/passwordfield.{h,cpp}` (`scrubString`, `takePassword`),
`LockView::startAuth` (local QString scrubbed).

**Reviewer checklist.**
- [ ] `pam_start("kapah", ...)` — the service name has a default; verify
      `/etc/pam.d/kapah` exists or document the `other` fallback.
- [ ] Only the echo-off prompt is answered; echo-on and error messages get no
      secret.
- [ ] `pam_acct_mgmt` is still checked (disabled accounts, expired
      passwords).
- [ ] The queued `invokeMethod` copy of the password on the GUI thread is
      freed on use but not scrubbed — accepted, documented above.
- [ ] One attempt in flight at a time; the field is dead while PAM works.

## S4. Password buffer never leaves the field unless authenticating

**Code:** `PasswordField` — no `QLineEdit`, no undo stack, hand-drawn bullets,
`takePassword()` scrubs the field's own copy. Clear() and destruction scrub.

## S5. Notification body markup sanitiser (when written)

**Invariant.** Whitelist: `<b>`, `<i>`, `<u>`, `<a href="https?://...">`
only; everything else is escaped or dropped. No `<script>`, `<style>`,
`javascript:` URLs, `data:` URLs, or inline event handlers. Entity expansion
is capped (billion-laughs).

**Code:** `src/components/notifications/markup.*` — to be written in Phase 4;
owns a second-agent review before merge.

## S6. IPC server access

**Invariant.** The kapah control socket (`paths::controlSocket()`) is a
per-user runtime-dir socket and accepts only newline-delimited JSON commands;
it is only writable by the session user.

## S7. Crash never unlocks

**Invariant.** A crash in `kapahd` (the main shell) never releases
`kapah-lock`'s hold, because the two are separate processes (ADR-001 Model
B). The lock spawner holds a logind delay inhibitor while `kapah-lock`
starts; on `kapah-lock` failure the machine stays locked, never unlocked.

## S8. Wayland session state is owned by the compositor

All locked-state transitions (`locked`, `finished`, `unlock`) are driven by
`ext-session-lock-v1` events; kapah-lock is a renderer and authenticator,
never the owner.
