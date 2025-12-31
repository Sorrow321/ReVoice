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
extern cvar_t *g_pcv_rev_voicecmd_verbose;
