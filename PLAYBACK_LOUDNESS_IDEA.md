# Idea: louder bot playback without the "loud parts break up" distortion

## Problem
On loud passages the music "breaks" — feels like many frequencies hit a ceiling at once. That's
**clipping/saturation**, not the per-packet crackle (that was a separate, client-side bug — see
`memory/revoice-playback-crackle-rootcause.md`).

## Why it happens
- The source masters are slammed (peak ≈ -2.5 dBFS = almost no headroom).
- Opus is lossy and its **decoder can overshoot the input peak by ~1-3 dB**. With no headroom,
  loud peaks overshoot past digital full-scale and clip (in the decode / int16 stages). The harsh
  flat-topped harmonics get baked in, then the client's volume turns it down — so the final
  recording isn't peaking, yet it still sounds broken.
- Confirmed: lowering `REV_PlaybackVolume` (≈0.7) fixes it — that just adds headroom.

## Goal
Keep it LOUD but stop the breakup. Loudness = average (RMS) level; clipping = peaks. So:
**raise the average while capping the peaks** = compression + limiting (the loudness-war trick).
Practical ceiling is ~-3 dBFS true-peak, to leave Opus its overshoot room (can't push to 0).

## Option A — pre-master the files (no code change; recommended first)
Process each track once, then play at `REV_PlaybackVolume 1.0`:
```bash
# loud, even, peaks held at -3 dBTP (I=-14 ≈ Spotify loudness; use -12 for louder)
ffmpeg -i track.wav -af "loudnorm=I=-14:TP=-3.0:LRA=11" out.wav
# or a straight limiter: boost then brick-wall at ~-3 dBFS
ffmpeg -i track.wav -af "alimiter=level_in=3:level_out=1:limit=0.7" out.wav
```
Batch the whole `cstrike/data_music` folder. Push harder (lower I / higher level_in) until it
starts sounding squashed — that's the loudness-vs-dynamics tradeoff, the only cost.

## Option B — in-engine limiter in ReVoice (automatic for any uploaded file)
Replace the current hard clip in the playback path (`if (v > 32767) v = 32767`, in
`revoice_voice_playback.cpp` Update() volume stage) with a soft peak limiter that smoothly pulls
down peaks approaching a target ceiling, with optional makeup gain. Gate behind a cvar
(e.g. `REV_PlaybackLimiter` + target dBFS), default off. ~30 lines.
- IMPORTANT: target ~-3 dBFS, NOT 0 — the limiter runs *before* Opus, so it must leave the
  codec's decode-overshoot headroom or it'll still clip downstream.

## Status: parked. Start with Option A if revisiting.
