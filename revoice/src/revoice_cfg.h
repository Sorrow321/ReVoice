#pragma once

#include "revoice_shared.h"

struct REVCmds {
	const char *name;
	void (*func)();
};

void Revoice_Exec_Config();
bool Revoice_Init_Config();
void Revoice_Init_Cvars();
void Revoice_DeInit_Cvars();

void Revoice_Cmds_Handler();

void Cmd_REV_Status();
void Cmd_REV_Version();

extern cvar_t *g_pcv_sv_voiceenable;
extern cvar_t *g_pcv_rev_hltv_codec;
extern cvar_t *g_pcv_rev_default_codec;
extern cvar_t *g_pcv_rev_record_voice;
extern cvar_t *g_pcv_rev_playback_debug;
extern cvar_t *g_pcv_rev_playback_frame_ms;
extern cvar_t *g_pcv_rev_playback_reset_frames;
extern cvar_t *g_pcv_rev_playback_reset_gap_ms;
extern cvar_t *g_pcv_rev_opus_bitrate;
extern cvar_t *g_pcv_rev_opus_complexity;
extern cvar_t *g_pcv_rev_playback_lp_hz;
extern cvar_t *g_pcv_rev_playvoice_client;
extern cvar_t *g_pcv_rev_playback_bot_name;
extern cvar_t *g_pcv_rev_playback_active;  // read-only status flag for external tools (AMXX): 0 stopped / 1 playing / 2 paused
extern cvar_t *g_pcv_rev_playback_volume;  // bot-playback volume multiplier (1.0 = 100%); bot path only
extern cvar_t *g_pcv_rev_playback_speed;   // bot-playback speed (tape-style, pitch shifts); bot path only
extern cvar_t *g_pcv_rev_voicecmd_verbose;
extern cvar_t *g_pcv_rev_wav_postfx;       // 0 = WAV/ASR taps clean pre-fx audio (default); 1 = taps post-fx (what listeners hear)
extern cvar_t *g_pcv_rev_pitch_grain_ms;   // pitch-shifter grain window, ms (10..100); latched per utterance
extern cvar_t *g_pcv_rev_pitch_lp_hz;      // pitch-shifter anti-alias base cutoff, Hz (1000..3900, <=0 = off); live
extern cvar_t *g_pcv_rev_upload_url;
extern cvar_t *g_pcv_rev_debug_log;        // 1 = write RVYYYYMMDD.log diagnostics; 0 = silence
extern cvar_t *g_pcv_rev_auto_upload_dump; // 1 = nightly auto-trigger of rv_upload_dump; 0 = manual-only
