---
name: revoice-playback-crackle-rootcause
description: Root-cause investigation of the per-packet crackle in ReVoice bot music playback
metadata:
  type: project
---

The crackle/cracks heard when the bot streams music via the voice channel is a **client-side, per-packet voice-playout artifact** — NOT in the packets ReVoice sends.

**Proven (don't re-investigate these):**
- Not aliasing: spectrograms are cleanly band-limited; structure matches the source.
- Not our DSP: turning the resampler OFF (`REV_PlaybackResample 0`) and the low-pass OFF (`REV_PlaybackLowpassHz 0`) does not change it.
- Not bandwidth/choke: with `sv_maxrate`/`rate` raised, net_graph choke→0 and it persists.
- Not the bitstream: saved raw packets (`REV_VoiceSave 1` → `cstrike/vdump_*.bin`) decode **clean** with a standard Opus decoder — 0.0× modulation at the packet rate — while the in-game recording has a sharp peak at the packet rate. So the client adds it during playout.
- Not codec mode: real CS Opus voice is **Hybrid-SWB @24 kHz**; ours was **SILK-NB @8 kHz**. After adding native 24 kHz (`REV_PlaybackVoiceRate 24000`), our packets are byte-pattern-identical to a real client (Hybrid-SWB 20ms, declared 24000, same opcodes, same continuous seq) — crackle STILL persists.

The click rate == packet rate (50/25/10 Hz for FrameMs 20/40/100); server-side, only mitigation is fewer/bigger packets (`REV_PlaybackFrameMs`, capped ~300 ms by the engine datagram).

**ROOT CAUSE FOUND (in Xash3D's open-source client, very likely same in stock GoldSrc):** the engine's voice mixer `S_RawSamplesStereo` (engine/client/sound/s_main.c) is a nearest-neighbor resampler (voice 24000 -> sound-card 44100) whose fractional phase `samplefrac` is a LOCAL reset to 0 on every call = every packet. With the truncated `fracstep` (floor(24000/44100*16384)=8916 vs true 8916.44), each packet ends ~0.98 sample short of the grid; resetting to 0 dumps that as a ~1-sample stutter at every packet boundary -> click at packet rate. Confirms all observations (clean bitstream, per-packet, fewer-with-bigger-packets, masked on speech because the GS encoder skips silence frames). The client also IGNORES the declared PLT_SamplingRate (always 24000) and the decoder is continuous (matches earlier findings). cs16-client (Velaron) is NOT involved — it only has stock voice_status/banmgr UI, no codec/Voice_StartChannel.

**CONFIRMED FIXED (2026): on patched Xash, 8 kHz playback is clean.** Proven via staged client PCM dumps (voice_dump cvar): the crackle was present in the decoded PCM only after the per-packet phase reset; persisting the phase removed it.

**Separate regression:** the 24 kHz hybrid-SWB encode path WE added (REV_PlaybackVoiceRate 24000) has its own crackle baked into the bitstream (the 8 kHz bitstream was always clean). Likely our 44.1->24 kHz resample aliasing (ResampleMonoLinear is linear-interp; relies on AntiAliasLowpassMono cutoff being ~11 kHz, not the 8k-era 3800) or the Opus SWB/SIGNAL_VOICE settings. To fix cleanly: tune offline with the source file. Stock Steam CS clients (closed hw.dll) have the same phase-reset bug and CANNOT be patched -> they still crackle at 8 kHz; no clean server-side fix exists (zero-phase needs 2229-sample packets, unreachable with 480-sample Opus frames). Only Xash/cs16-client (mobile) players benefit from the client patch.

**FIX (applied to local xash3d-fwgs):** persist `samplefrac` per raw channel in `ch->engine_reserved[0]` instead of zeroing each packet (turns the per-packet stutter into a smooth ~0.005% pitch offset). Edits: sound.h proto, s_main.c `S_RawSamplesStereo`(+`psamplefrac` param, load/save, reset only on underrun, loop init from carried phase), `S_RawEntSamples` (carry via engine_reserved[0]), `S_FindRawChannel` (reset on new stream), avi_ffmpeg.c caller passes NULL (unchanged). User rebuilds Xash to confirm. Fix only helps Xash clients; stock-CS players can't be patched -> server mitigation only (bigger packets). A true server-side packet pre-distortion is impossible (zero-phase needs 2229-sample packets, unreachable with Opus frames + client-rate-specific).

Real win achieved: native 24 kHz Hybrid-SWB encoding (better audio bandwidth, the original "muffled 8 kHz" complaint). Still TODO if kept: downsample path so Silk/Speex listeners work in 24 kHz mode (currently Opus-only when `REV_PlaybackVoiceRate 24000`).

ReVoice cleaned up (diff vs ab7804649a32a02c5ad368dfc8d159c9588a5390). KEPT (genuine improvements): `REV_PlaybackFrameMs` cap raised 100->1000 + buffers (REV_MAX_FRAME_SAMPLES=8000, pcm8kCarry 32768, shared g_RevSilenceFrame); codec re-init on StartPlayback so REV_OpusBitrate/Complexity apply on music restart; anti-alias resampler (REV_Biquad/AntiAliasLowpassMono/ResampleMonoLinear replacing ConvertToMonoResample+ApplyLowpass8k) with REV_PlaybackLowpassHz where 0=off (low-pass = a real crackle-intensity lever: bright/steep audio -> louder per-packet clicks); the SERVER_PRINT(buf) one-liner so REV_PlaybackDebug stats actually print. REMOVED (debug + 24kHz regression): cvars REV_VoiceDump/REV_VoiceSave/REV_PlaybackPacketReset/REV_PlaybackResample/REV_PlaybackVoiceRate and all their code, the VDUMP prints, the vdump_*.bin dumps, and the whole 24 kHz encode path (VoiceEncoder_Opus/SteamP2PCodec/revoice_main reverted to base). Settled production config: REV_PlaybackVoiceRate gone (8kHz only), REV_PlaybackLowpassHz 0, REV_PlaybackFrameMs 345.

Xash side: the samplefrac phase-persist fix is the real cure but the xash3d-fwgs tree still has debug Con_Printfs + voice_dump/Stage-A/B dumps to strip before finalizing/upstreaming. Offline analysis scripts live in `audio_experiment/` (PyAV decodes raw Opus). See [[revoice-build-workflow]].
