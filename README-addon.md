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
- `sv_voice_pitch <player_id> <0.5..2.0>` — simple pitch shift (1.0 normal, 2.0 higher, 0.5 lower).

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

### Notes
- Playback prefers a bot emitter if present; otherwise uses a real player slot.
- WAV requirements: PCM, mono/stereo, 8/16-bit; will be converted to 8 kHz mono PCM16 internally.

