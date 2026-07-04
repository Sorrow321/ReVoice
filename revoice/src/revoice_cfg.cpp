#include "precompiled.h"

char g_ExecConfigCmd[MAX_PATH];
const char REVOICE_CFG_FILE[] = "revoice.cfg";

void Revoice_Exec_Config()
{
	if (!g_ExecConfigCmd[0]) {
		return;
	}

	g_engfuncs.pfnServerCommand(g_ExecConfigCmd);
	g_engfuncs.pfnServerExecute();
}

bool Revoice_Init_Config()
{
	const char *pszGameDir = GET_GAME_INFO(PLID, GINFO_GAMEDIR);
	const char *pszPluginDir = GET_PLUGIN_PATH(PLID);

	char szRelativePath[MAX_PATH];
	strncpy(szRelativePath, &pszPluginDir[strlen(pszGameDir) + 1], sizeof(szRelativePath) - 1);
	szRelativePath[sizeof(szRelativePath) - 1] = '\0';
	NormalizePath(szRelativePath);

	char *pos = strrchr(szRelativePath, '/');
	if (pos) {
		*(pos + 1) = '\0';
	}

	snprintf(g_ExecConfigCmd, sizeof(g_ExecConfigCmd), "exec \"%s%s\"\n", szRelativePath, REVOICE_CFG_FILE);
	return true;
}

cvar_t g_cv_rev_hltv_codec    = { "REV_HltvCodec", "opus", 0, 0.0f, nullptr };
cvar_t g_cv_rev_default_codec = { "REV_DefaultCodec", "speex", 0, 0.0f, nullptr };
cvar_t g_cv_rev_record_voice  = { "REV_RecordVoice", "0", 0, 0.0f, nullptr };
cvar_t g_cv_rev_playback_debug = { "REV_PlaybackDebug", "0", 0, 0.0f, nullptr };
cvar_t g_cv_rev_playback_frame_ms = { "REV_PlaybackFrameMs", "40", 0, 0.0f, nullptr };
cvar_t g_cv_rev_playback_reset_frames = { "REV_PlaybackResetFrames", "0", 0, 0.0f, nullptr };
cvar_t g_cv_rev_playback_reset_gap_ms = { "REV_PlaybackResetGapMs", "0", 0, 0.0f, nullptr };
cvar_t g_cv_rev_opus_bitrate = { "REV_OpusBitrate", "64000", 0, 0.0f, nullptr };
cvar_t g_cv_rev_opus_complexity = { "REV_OpusComplexity", "10", 0, 0.0f, nullptr };
// Bot-playback anti-alias low-pass cutoff (Hz) applied before resampling. 0 (or negative) = filter OFF.
cvar_t g_cv_rev_playback_lp_hz = { "REV_PlaybackLowpassHz", "3800", 0, 0.0f, nullptr };
cvar_t g_cv_rev_playvoice_client = { "REV_PlayvoiceClient", "1", 0, 0.0f, nullptr };
cvar_t g_cv_rev_playback_bot_name = { "REV_PlaybackBotName", "vk.com/laguna_games", 0, 0.0f, nullptr };
cvar_t g_cv_rev_playback_active = { "REV_PlaybackActive", "0", 0, 0.0f, nullptr }; // status flag for AMXX (set by plugin)
cvar_t g_cv_rev_playback_volume = { "REV_PlaybackVolume", "1.0", 0, 0.0f, nullptr }; // bot-playback volume (1.0 = 100%)
cvar_t g_cv_rev_playback_speed  = { "REV_PlaybackSpeed",  "1.0", 0, 0.0f, nullptr }; // bot-playback speed (tape-style)
cvar_t g_cv_rev_voicecmd_verbose = { "REV_VoiceCmdVerbose", "1", 0, 0.0f, nullptr };
// WAV/ASR tap position for fx-active players. 0 (default) = record the CLEAN
// pre-fx audio (ASR transcription unaffected by pitch/volume). 1 = record the
// post-fx audio, i.e. exactly what listeners hear — useful to verify fx via
// the saved .wav files. Players at default settings are unaffected either way.
cvar_t g_cv_rev_wav_postfx = { "REV_WavPostFx", "0", 0, 0.0f, nullptr };
cvar_t g_cv_rev_upload_url        = { "REV_UploadURL", "", 0, 0.0f, nullptr };
cvar_t g_cv_rev_debug_log         = { "REV_DebugLog", "1", 0, 0.0f, nullptr };
cvar_t g_cv_rev_auto_upload_dump  = { "REV_AutoUploadDump", "0", 0, 0.0f, nullptr };
cvar_t g_cv_rev_version           = { "revoice_version", APP_VERSION, FCVAR_SERVER, 0.0f, nullptr };

cvar_t *g_pcv_rev_hltv_codec    = nullptr;
cvar_t *g_pcv_rev_default_codec = nullptr;
cvar_t *g_pcv_rev_record_voice  = nullptr;
cvar_t *g_pcv_rev_playback_debug = nullptr;
cvar_t *g_pcv_rev_playback_frame_ms = nullptr;
cvar_t *g_pcv_rev_playback_reset_frames = nullptr;
cvar_t *g_pcv_rev_playback_reset_gap_ms = nullptr;
cvar_t *g_pcv_rev_opus_bitrate = nullptr;
cvar_t *g_pcv_rev_opus_complexity = nullptr;
cvar_t *g_pcv_rev_playback_lp_hz = nullptr;
cvar_t *g_pcv_rev_playvoice_client = nullptr;
cvar_t *g_pcv_rev_playback_bot_name = nullptr;
cvar_t *g_pcv_rev_playback_active = nullptr;
cvar_t *g_pcv_rev_playback_volume = nullptr;
cvar_t *g_pcv_rev_playback_speed  = nullptr;
cvar_t *g_pcv_rev_voicecmd_verbose = nullptr;
cvar_t *g_pcv_rev_wav_postfx = nullptr;
cvar_t *g_pcv_rev_upload_url        = nullptr;
cvar_t *g_pcv_rev_debug_log         = nullptr;
cvar_t *g_pcv_rev_auto_upload_dump  = nullptr;
cvar_t *g_pcv_sv_voiceenable        = nullptr;

void Revoice_Init_Cvars()
{
	g_engfuncs.pfnAddServerCommand("rev", Revoice_Cmds_Handler);

	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_version);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_hltv_codec);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_default_codec);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_record_voice);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_debug);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_frame_ms);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_reset_frames);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_reset_gap_ms);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_opus_bitrate);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_opus_complexity);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_lp_hz);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playvoice_client);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_bot_name);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_active);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_volume);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_playback_speed);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_voicecmd_verbose);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_wav_postfx);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_upload_url);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_debug_log);
	g_engfuncs.pfnCvar_RegisterVariable(&g_cv_rev_auto_upload_dump);

	g_pcv_sv_voiceenable = g_engfuncs.pfnCVarGetPointer("sv_voiceenable");
	g_pcv_rev_hltv_codec = g_engfuncs.pfnCVarGetPointer(g_cv_rev_hltv_codec.name);
	g_pcv_rev_default_codec = g_engfuncs.pfnCVarGetPointer(g_cv_rev_default_codec.name);
	g_pcv_rev_record_voice = g_engfuncs.pfnCVarGetPointer(g_cv_rev_record_voice.name);
	g_pcv_rev_playback_debug = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_debug.name);
	g_pcv_rev_playback_frame_ms = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_frame_ms.name);
	g_pcv_rev_playback_reset_frames = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_reset_frames.name);
	g_pcv_rev_playback_reset_gap_ms = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_reset_gap_ms.name);
	g_pcv_rev_opus_bitrate = g_engfuncs.pfnCVarGetPointer(g_cv_rev_opus_bitrate.name);
	g_pcv_rev_opus_complexity = g_engfuncs.pfnCVarGetPointer(g_cv_rev_opus_complexity.name);
	g_pcv_rev_playback_lp_hz = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_lp_hz.name);
	g_pcv_rev_playvoice_client = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playvoice_client.name);
	g_pcv_rev_playback_bot_name = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_bot_name.name);
	g_pcv_rev_playback_active = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_active.name);
	g_pcv_rev_playback_volume = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_volume.name);
	g_pcv_rev_playback_speed  = g_engfuncs.pfnCVarGetPointer(g_cv_rev_playback_speed.name);
	g_pcv_rev_voicecmd_verbose = g_engfuncs.pfnCVarGetPointer(g_cv_rev_voicecmd_verbose.name);
	g_pcv_rev_wav_postfx = g_engfuncs.pfnCVarGetPointer(g_cv_rev_wav_postfx.name);
	g_pcv_rev_upload_url        = g_engfuncs.pfnCVarGetPointer(g_cv_rev_upload_url.name);
	g_pcv_rev_debug_log         = g_engfuncs.pfnCVarGetPointer(g_cv_rev_debug_log.name);
	g_pcv_rev_auto_upload_dump  = g_engfuncs.pfnCVarGetPointer(g_cv_rev_auto_upload_dump.name);

	g_RehldsFuncs->AddCvarListener(g_cv_rev_hltv_codec.name, Revoice_Update_Hltv);
	g_RehldsFuncs->AddCvarListener(g_cv_rev_default_codec.name, Revoice_Update_Players);

	// Voice playback subsystem (sv_playvoice / sv_playvoice_ex).
	Revoice_VoicePlayback_Init();
}

void Revoice_DeInit_Cvars()
{
	g_RehldsFuncs->RemoveCvarListener(g_cv_rev_hltv_codec.name, Revoice_Update_Hltv);
	g_RehldsFuncs->RemoveCvarListener(g_cv_rev_default_codec.name, Revoice_Update_Players);
}

REVCmds g_revoice_cmds[] = {
	{ "version", Cmd_REV_Version },
	{ "status",  Cmd_REV_Status  }
};

void Revoice_Cmds_Handler()
{
	const char *pcmd = CMD_ARGV(1);
	for (auto& cmds : g_revoice_cmds)
	{
		if (_stricmp(cmds.name, pcmd) == 0 && cmds.func) {
			cmds.func();
		}
	}
}

void Cmd_REV_Version()
{
	// print version
	g_engfuncs.pfnServerPrint("Revoice version: " APP_VERSION "\n");
	g_engfuncs.pfnServerPrint("Build date: " APP_COMMIT_TIME " " APP_COMMIT_DATE "\n");
	g_engfuncs.pfnServerPrint("Build from: " APP_COMMIT_URL APP_COMMIT_SHA "\n");
}

void Cmd_REV_Status()
{
	int nUsers =  0;
	UTIL_ServerPrintf("\n%-5s %-32s %-6s %-4s %5s", "#", "name", "codec", "rate", "proto");

	for (int i = 0; i < g_RehldsSvs->GetMaxClients(); i++) {
		auto plr = &g_Players[i];
		if (plr->IsConnected()) {
			printf("#%-4i %-32s %-6s %-4i %-2i %-3s", i + 1, UTIL_VarArgs("\"%s\"", plr->GetClient()->GetName()), plr->GetCodecTypeToString(),	plr->GetVoiceRate(), plr->GetProtocol(), plr->IsHLTV() ? "   (HLTV)" : "");
			nUsers++;
		}
	}

	if (!nUsers) {
		UTIL_ServerPrintf("0 users");
	}

	UTIL_ServerPrintf("\n");
}
