## ReVoice Add-on: Server-Side Voice Recording & WAV Playback

This add-on extends ReVoice with server-side voice utilities:
- **Record** any player's mic to per-utterance WAV files (8 kHz mono).
- **Stream WAVs** through the in-game voice channel (radio-bot style, no client precache).
- **Live mic controls** for admins: adjust volume and pitch on the fly.
- Configurable verbosity and targeting to specific players.

### Voice Recording
- Controlled by `REV_RecordVoice` (see config below).
- Files are saved under `cstrike/data/<STEAMID or IP>/` as 8 kHz mono WAV.
- Each detected utterance (gap > 0.1s) becomes a separate file.

### Voice Playback (Radio Bot)
Commands (server console or RCON):
- `sv_playvoice <path>`: quick play with default volume (1.0) to all players.
- `sv_playvoice_ex <path> <volume> [targets...]`: advanced play with targeting.
  - `path`: relative to `cstrike/` (e.g. `sound/custom/music1.wav`) or absolute (`/full/path/file.wav`).
  - `volume`: `0.0` (mute) to `4.0` (boost), `1.0` is original.
  - `targets`: `0` or omitted → all players; otherwise list client indices (1-based), e.g. `7 8 10`.
- Example: `sv_playvoice_ex sound/custom/music1.wav 0.8 7 8 10 15`

Client command (optional):
- `playvoice <path>`: same path rules as above. Only works if `REV_PlayvoiceClient` is enabled.

### Live Mic Control (server)
- `sv_voice_volume <player_id> <0.0..10.0>` — scales that player's outgoing mic audio server-side (0 mute, 1 normal).
- `sv_voice_pitch <player_id> <0.5..2.0>` — duration-preserving pitch shift (1.0 normal, 2.0 octave up, 0.5 octave down). Dual-tap granular shifter with cross-packet state, so packet boundaries stay click-free.
- Both settings reset on connect, disconnect and map change (cleared explicitly at the map boundary — Metamod does not report staying players as disconnected/reconnected across changelevel). While a player is at the defaults (1.0/1.0) the voice path is identical to stock ReVoice — no fx component runs or is even allocated.
- Audit trail: every change is appended to `cstrike/addons/amxmodx/logs/RV_actions_YYYYMMDD.log` as an `[FX]` line (always on, independent of `REV_DebugLog`). No `[FX]` lines = the fx code never executed.
- WAV recordings and ASR capture the clean pre-fx audio by default. Set `REV_WavPostFx 1` to record the post-fx audio instead (exactly what listeners hear) — handy for verifying fx from the saved files; revert to `0` so ASR gets clean speech.

### Config (cstrike/addons/revoice/revoice.cfg)
- `REV_RecordVoice` — `0/1` enable WAV recording of player mic (8 kHz mono).
- `REV_PlayvoiceClient` — `0/1` allow clients to run `playvoice`.
- `REV_PlaybackDebug` — `0` quiet, `1` once/sec stats, `2+` verbose per frame.
- `REV_PlaybackFrameMs` — frame size in ms (20..100, multiples of 20). Higher = fewer packets.
- `REV_PlaybackResetFrames` — force codec reset every N frames (0 disables).
- `REV_PlaybackResetGapMs` — silence gap after a reset (drops frames) to hard-reset mic state.
- `REV_OpusBitrate` — Opus encoder bitrate (bps), e.g. `64000`.
- `REV_OpusComplexity` — Opus encoder complexity `0..10`.
- `REV_PlaybackLowpassHz` — low-pass cutoff (1000..3900) applied to playback at 8 kHz.
- `REV_PlaybackBotName` — substring to find the bot used as the voice emitter.
- `REV_VoiceCmdVerbose` — `0/1` enable server console prints for `sv_voice_volume`/`sv_voice_pitch`.
- `REV_WavPostFx` — `0` (default) WAV/ASR records clean pre-fx audio; `1` records post-fx (what listeners hear).
- `REV_PitchGrainMs` — pitch-shifter grain window in ms (`10..100`, default `56`). The doubling↔shimmer knob: large = less warble but a ~window/2 "doubled voice" echo; small = tighter chorus-like sound but faster warble. Takes effect from the speaker's next utterance.
- `REV_PitchLowpassHz` — pitch-shifter anti-alias base cutoff in Hz for upward shifts (`1000..3900`, default `3600`; effective cutoff = value/pitch; `0` = filter off — brighter but aliases). Applies live.

### Notes
- Playback prefers a bot emitter if present; otherwise uses a real player slot.
- WAV requirements: PCM, mono/stereo, 8/16-bit; will be converted to 8 kHz mono PCM16 internally.

