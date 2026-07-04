---
name: metamod-mapchange-state-clearing
description: Metamod plugins get NO disconnect/connect per client across changelevel — clear per-player state at the map boundary explicitly
metadata:
  type: feedback
---

For this Metamod plugin (unlike AMXX), players who stay through a `changelevel` are NOT reported as disconnected/reconnected: `SV_DropClient` does not fire for them, and the plugin's `OnConnected` early-returns on its already-connected branch. Any per-player state cleared only in connect/disconnect handlers therefore silently survives map changes.

**Why:** the user hit this class of bug before and explicitly flagged it while reviewing the voice-fx feature ("all buffers, etc, should be cleared on mapchange separately").

**How to apply:** whenever adding per-player (or per-slot) state, also clear it at the map boundary — the established place is `ServerDeactivate_PreHook` in revoice_main.cpp, alongside `Revoice_FlushAll_Players()` / `Revoice_ClearAllVoiceFx()` / `g_VoicePlayback.StopPlayback()`. (Per-listener playback mutes are the map-START variant: `REV_ResetAllPlaybackMutes()` in `ServerActivate_PostHook`.) Also remember sv.time resets at the boundary, so any "now vs last-time" staleness logic needs a `now < last` (clock-backwards) branch. See [[revoice-voicefx-volume-pitch]].
