---
name: revoice-voicefx-volume-pitch
description: "Per-player voice volume/pitch feature — restored 2026-07 with granular shifter, strict isolation gate, and always-on [FX] audit log"
metadata: 
  node_type: memory
  type: project
  originSessionId: c3070fd9-2fc4-4719-9fd4-299dbc29692a
---

The per-player `sv_voice_volume` / `sv_voice_pitch` feature (disabled since the wav-save/ASR crash isolation) was reimplemented from scratch in 2026-07. The old pitch DSP was structurally broken: frame-local fixed-length resample, inverted direction, dead phase-carry state → clicks at packet rate (worst on pitch-up: DC-hold tail). The old "segfault" hunt was never traced to this feature; it was disabled as collateral.

**New design (all in `revoice/src/`):**
- `revoice_pitchshift.h` — `CVoicePitchShifter`, header-only, self-contained. Duration-preserving dual-tap granular delay-line shifter (ring 2048, window 448 @ 8 kHz, triangle crossfade, per-player anti-alias biquads for pitch-up). Tape-style (like bot-playback speed) is impossible for live voice: real-time input rate is fixed, so duration must be preserved.
- Safety by construction: all ring indexing masked (`& RING_MASK`), tap delays hard-clamped, NaN-proof float→int16 clamps, no heap. The 0.5..2.0 clamp is a quality choice, NOT load-bearing for memory safety (unlike the old 0.8..1.2 clamp).
- `CRevoicePlayer` gained vol/pitch state + LAZY dedicated fx encoders (`m_FxSilk/Opus/SpeexCodec`, nullptr until first fx-active packet) — never reuse the speaker's decode codecs for re-encode (Decompress can ResetState mid-stream).
- `SV_ParseVoiceData_emu`: `HasActiveVoiceFx()` gate (two float compares). Default players take the byte-identical legacy passthrough/complementary-transcode path; fx players get decode → AppendPcm (pre-fx, so ASR/WAV stay clean) → pitch → volume → re-encode both families into 8 KB bufs. Fx stream state resets on utterance gap (`WAV_FLUSH_GAP_SEC`), settings change, connect/disconnect.
- Settings do NOT survive map change: `Revoice_ClearAllVoiceFx()` runs in `ServerDeactivate_PreHook` (user requested auto-clear; also required because Metamod delivers no disconnect/connect for staying players across changelevel — see [[metamod-mapchange-state-clearing]]). Shared clear path: `CRevoicePlayer::ClearVoiceFx(reason)` used by connect/disconnect/map-boundary, [FX]-logs only if something was active.

**Tuning cvars (added after user heard "doubled voice" at pitch-up — the dual-tap W/2=28ms echo, confirmed via spectrograms `audio_experiment/make_spectrograms.py`):** `REV_PitchGrainMs` (10..100, default 56) — grain window; LATCHED per utterance by the shifter (`m_Window`, unlatched on Reset) so live changes never jump taps mid-word; smaller = chorus-like fusion + faster shimmer, larger = cleaner + more "doubling". `REV_PitchLowpassHz` (1000..3900, default 3600, <=0=off) — AA base cutoff, effective = value/pitch, applied live. Both read ONLY inside ApplyVoiceFx under pitch≠1 (isolation intact). Shifter constants now DEFAULT/MIN/MAX_WINDOW (448/80/800); ReadTap clamps to MAX_WINDOW so even corrupt m_Window can't leave the ring.

**WAV tap & verification (came up on first deploy):** saved .wav files contain the CLEAN pre-fx audio by default — checking WAVs shows no effect even when fx works in-game. `REV_WavPostFx 1` moves the tap after the fx chain (WAVs = what listeners hear; use for verification, revert to 0 for ASR). Proof the fx path ran on live voice: `[FX] codecs allocated slot=N` in RV_actions log. ALSO: the user's AMXX menu (apply_voice_pitch) had `command_value = 2.0 - display` — a workaround for the OLD inverted DSP. With the new direction-correct shifter the menu must send `display` directly, or "higher" picks make voices lower.

**Isolation guarantee (user requirement, load-bearing):** if nobody ran `sv_voice_*`, no fx code executes and fx codecs are never even allocated. Every settings change writes an `[FX]` line via `RvLogAction` → `RV_actions_YYYYMMDD.log` (NOT `RvLog`, which is killed by `REV_DebugLog 0`). Post-crash: no `[FX]` lines in RV_actions ⇒ feature provably uninvolved. Don't add per-packet fx logging (user explicitly refused the spam) and don't put fx work on the default path.

**Also hardened (affects default path too):** `VoiceEncoder_Opus::Compress` and `VoiceEncoder_Silk::Compress` frame loops previously wrote 2–4 byte headers unchecked and trusted encoder return values — with PLC-inflated decodes (up to ~16k samples) they could overrun the output buffer (latent, pre-existing; reachable in the stock speex→silk transcode). Now: per-frame room check + error-result rollback + int16-budget cap (silk's old `-1` sentinel truncated to negative budgets).

**Verification done:** `audio_experiment/test_pitchshift.cpp` includes the production header; MSVC /O2 and /fsanitize=address runs both pass: identity bypass bit-exact, pitch centroids within grain-rate tolerance at 0.5/0.8/1.2/1.5/2.0, chunked-vs-oneshot output BIT-IDENTICAL (cross-packet continuity proof), 50M-sample hostile fuzz (NaN/inf/garbage pitch, 1..16384-sample packets, random resets) clean under ASan. All four edited plugin TUs pass `cl /Zs` syntax check (only pre-existing POSIX `mkdir` errors remain — the plugin's real target is the Linux build). Not yet compiled/listened-to on the real server (user does that remotely, see [[revoice-build-workflow]]).
