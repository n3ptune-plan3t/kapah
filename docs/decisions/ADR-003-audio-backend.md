# ADR-003: Audio backend

Status: accepted
Date: 2026-10-03
Roadmap: section 3, section 14.3

## Context

The bar, the OSD and the quick-settings panel all need default-sink volume,
mute and sink enumeration. Two candidates:

* **libpulse** (the PulseAudio client API).
* **Native PipeWire C API.**

Both are event-driven; neither requires polling.

## Decision

**libpulse**, and only libpulse.

* It works against a real `pulseaudio` server *and* against `pipewire-pulse`,
  which is what a PipeWire system actually runs. A kapah install does not care
  which audio server the user chose.
* The client API is small and stable: `pa_mainloop` + `pa_context` +
  `pa_context_subscribe` gives us `PA_CONTEXT_SUBSCRIBE_SINK|SOURCE|SINK_INPUT`
  and `PA_CONTEXT_EVENT_*` callbacks. That maps directly onto a `QSocketNotifier`
  on the libpulse fd, which is exactly the shape roadmap section 4.1 asks for.
* The PipeWire C API would mean a second event loop (PipeWire has its own) to
  bridge onto Qt's, which is where the "one short-lived thread with a clear
  owner" exception in 4.1 starts leaking. libpulse lets us stay single-threaded:
  we drive `pa_context` from the Qt event loop via the fd notifier and a
  `pa_mainloop` that is only ever stepped by `pa_mainloop_prepare`/`dispatch`
  around that notifier.
* Distro availability: `libpulse` is in every base repo of every target distro.
  `libpipewire-0.3` is too, but `pipewire-pulse` is the part that is not always
  installed, and if it is missing a native-PipeWire build has no audio at all
  while a libpulse build silently falls back to nothing either — but the failure
  is at least the same one, so there is one code path to test rather than two.

## Consequences

* `AudioService` lives in `src/services/audio.cpp` and is the *only* file that
  includes `<pulse/pulseaudio.h>`. Every other file talks to the `AudioService`
  signal interface.
* The service is unavailable, not broken, when there is no server: it exposes
  `available() == false`, the bar module renders a muted-slash icon, the OSD does
  not fire, and `kapahctl volume +5` exits non-zero with a clear message.
* Route enumeration (sink/source *names*) comes from the `PROPERTY` facility
  (`pa_context_get_server_info` + `pa_context_get_sink_info_list`). That is a
  blocking call, so it is issued from a `QDBusPendingCallWatcher`-equivalent:
  a `pa_mainloop` iteration posted via a zero-delay `QSocketNotifier` activation,
  never from the paint path. We only ever enumerate while the sink-selector
  popup is open (Phase 7.4).
* Volume changes are done with `pa_context_set_sink_volume_by_index` plus
  `pa_context_set_sink_mute_by_index`, then we wait for the resulting
  `PA_CONTEXT_EVENT_NEW`/`CHANGE` subscribe callback rather than assuming
  success. Round-tripping through the event is what makes holding a volume key
  from flickering the OSD (Phase 6 acceptance).
* Dependency: `pkg-config --libs libpulse`, no headers vendored, no build
  option to swap in PipeWire in v1. If a native PipeWire backend is ever wanted,
  it goes behind `ServiceConfig::audioBackend` behind a new ADR, because the
  event-loop bridge is not a small change.
