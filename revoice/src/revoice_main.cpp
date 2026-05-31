#include "precompiled.h"
// Upload subsystem: cmd-triggered only. The module exposes a single entry
// point (Cmd_UploadDump) that is registered as a server command in
// Revoice_Load. There is no per-frame work, no hook, no auto-init — if
// nobody types `rv_upload_dump` in the console, none of revoice_upload.cpp
// runs (apart from passive static-global zero-init at module load).
#include "revoice_upload.h"
#include <stdlib.h>

// Server-cmd handlers temporarily disabled to isolate the wav-save + ASR path:
//   - sv_voice_volume / sv_voice_pitch (per-player gain & pitch)
//   - rv_upload_dump (HTTP dumper to external server)
// rv_asr_record stays registered (it is a debugged subject).
// static void Cmd_VoiceVolume();
// static void Cmd_VoicePitch();
static void Cmd_AsrRecord();

void SV_DropClient_hook(IRehldsHook_SV_DropClient *chain, IGameClient *cl, bool crash, const char *msg)
{
	int slot = cl ? cl->GetId() : -1;
	RvLog("[HOOK] SV_DropClient slot=%d crash=%d msg=%s",
		slot, crash ? 1 : 0, msg ? msg : "(null)");

	CRevoicePlayer *plr = GetPlayerByClientPtr(cl);

	plr->OnDisconnected();

	chain->callNext(cl, crash, msg);
}

void CvarValue2_PreHook(const edict_t *pEnt, int requestID, const char *cvarName, const char *cvarValue)
{
	CRevoicePlayer *plr = GetPlayerByEdict(pEnt);
	if (plr->GetRequestId() != requestID) {
		RETURN_META(MRES_IGNORED);
	}

	const char *lastSeparator = strrchr(cvarValue, ',');
	if (lastSeparator)
	{
		int buildNumber = atoi(lastSeparator + 1);
		if (buildNumber > 4554) {
			plr->SetCodecType(vct_opus);
		}
	}

	RETURN_META(MRES_IGNORED);
}

// Restored to the original ReVoice transcoding shape: decode once with the
// source codec, append PCM to the player's wav buffer, encode once into the
// "complementary" codec (speex<->silk/opus). Each destination then gets
// whichever of the two pre-encoded buffers matches its preferred codec.
//
// The recently-added "triple-encode + per-player gain + per-player pitch"
// path is intentionally absent here while we isolate the wav-save + ASR
// crash. Restoring it requires re-introducing this function's body and
// the corresponding code in SV_ParseVoiceData_emu.
int TranscodeVoice(CRevoicePlayer *srcPlayer, const char *srcBuf, int srcBufLen, IVoiceCodec *srcCodec, IVoiceCodec *dstCodec, char *dstBuf, int dstBufSize)
{
	char decodedBuf[32768];

	int numDecodedSamples = srcCodec->Decompress(srcBuf, srcBufLen, decodedBuf, sizeof(decodedBuf));
	if (numDecodedSamples <= 0) {
		return 0;
	}

	// All voice codecs in CS 1.6 produce 8 kHz PCM16.
	srcPlayer->AppendPcm(decodedBuf, numDecodedSamples, 8000);

	int compressedSize = dstCodec->Compress(decodedBuf, numDecodedSamples, dstBuf, dstBufSize, false);
	if (compressedSize <= 0) {
		return 0;
	}

	return compressedSize;
}

void SV_ParseVoiceData_emu(IGameClient *cl)
{
	char chReceived[4096];
	unsigned int nDataLength = g_RehldsFuncs->MSG_ReadShort();

	if (nDataLength == 0 || nDataLength > sizeof(chReceived)) {
		g_RehldsFuncs->DropClient(cl, FALSE, "Invalid voice data\n");
		return;
	}

	g_RehldsFuncs->MSG_ReadBuf(nDataLength, chReceived);

	if (g_pcv_sv_voiceenable->value == 0.0f) {
		return;
	}

	CRevoicePlayer *srcPlayer = GetPlayerByClientPtr(cl);
	srcPlayer->SetLastVoiceTime(g_RehldsSv->GetTime());
	srcPlayer->IncreaseVoiceRate(nDataLength);

	char transcodedBuf[4096];

	char *silkData = nullptr;
	char *speexData = nullptr;

	int silkDataLen = 0;
	int speexDataLen = 0;

	switch (srcPlayer->GetCodecType())
	{
	case vct_silk:
	{
		if (nDataLength > MAX_SILK_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_SILK_VOICE_RATE)
			return;

		silkData = chReceived; silkDataLen = nDataLength;
		speexData = transcodedBuf;
		speexDataLen = TranscodeVoice(srcPlayer, silkData, silkDataLen, srcPlayer->GetSilkCodec(), srcPlayer->GetSpeexCodec(), transcodedBuf, sizeof(transcodedBuf));
		break;
	}
	case vct_opus:
	{
		if (nDataLength > MAX_OPUS_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_OPUS_VOICE_RATE)
			return;

		silkData = chReceived; silkDataLen = nDataLength;
		speexData = transcodedBuf;
		speexDataLen = TranscodeVoice(srcPlayer, silkData, silkDataLen, srcPlayer->GetOpusCodec(), srcPlayer->GetSpeexCodec(), transcodedBuf, sizeof(transcodedBuf));
		break;
	}
	case vct_speex:
	{
		if (nDataLength > MAX_SPEEX_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_SPEEX_VOICE_RATE)
			return;

		speexData = chReceived; speexDataLen = nDataLength;
		silkData = transcodedBuf;
		silkDataLen = TranscodeVoice(srcPlayer, speexData, speexDataLen, srcPlayer->GetSpeexCodec(), srcPlayer->GetSilkCodec(), transcodedBuf, sizeof(transcodedBuf));
		break;
	}
	default:
		return;
	}

	int maxclients = g_RehldsSvs->GetMaxClients();
	for (int i = 0; i < maxclients; i++)
	{
		CRevoicePlayer *dstPlayer = &g_Players[i];
		IGameClient *dstClient = dstPlayer->GetClient();

		if (!dstClient)
			continue;

		if (!((1 << i) & cl->GetVoiceStream(0)) && dstPlayer != srcPlayer)
			continue;

		if (!dstClient->IsActive() && !dstClient->IsConnected() && dstPlayer != srcPlayer)
			continue;

		char *sendBuf = nullptr;
		int nSendLen = 0;
		switch (dstPlayer->GetCodecType())
		{
		case vct_silk:
		case vct_opus:
			sendBuf  = silkData;
			nSendLen = silkDataLen;
			break;
		case vct_speex:
			sendBuf  = speexData;
			nSendLen = speexDataLen;
			break;
		default:
			break;
		}

		if (sendBuf == nullptr || nSendLen == 0)
			continue;

		if (dstPlayer == srcPlayer && !dstClient->GetLoopback())
			nSendLen = 0;

		sizebuf_t *dstDatagram = dstClient->GetDatagram();
		if (dstDatagram->cursize + nSendLen + 6 < dstDatagram->maxsize) {
			g_RehldsFuncs->MSG_WriteByte(dstDatagram, svc_voicedata);
			g_RehldsFuncs->MSG_WriteByte(dstDatagram, cl->GetId());
			g_RehldsFuncs->MSG_WriteShort(dstDatagram, nSendLen);
			g_RehldsFuncs->MSG_WriteBuf(dstDatagram, nSendLen, sendBuf);
		}
	}
}

void Rehlds_HandleNetCommand(IRehldsHook_HandleNetCommand *chain, IGameClient *cl, int8 opcode)
{
	const int clc_voicedata = 8;

	if (opcode == clc_voicedata) {
		SV_ParseVoiceData_emu(cl);
		return;
	}

	chain->callNext(cl, opcode);
}

#if 0   // ---- BEGIN disabled: per-player pitch / volume cmds ----
// Server command: sv_voice_pitch <player_id> <pitch>
// Adjusts outgoing mic pitch for that player (0.5 = half, 1.0 = normal, 2.0 = double)
static void Cmd_VoicePitch()
{
	int argc = CMD_ARGC();
	if (argc < 3) {
		SERVER_PRINT("Usage: sv_voice_pitch <player_id> <pitch 0.5..2.0>\n");
		return;
	}

	int playerId = atoi(CMD_ARGV(1));
	float pitch = (float)atof(CMD_ARGV(2));

	if (playerId < 1 || playerId > g_RehldsSvs->GetMaxClients()) {
		SERVER_PRINT("[ReVoice] Invalid player id\n");
		return;
	}

	CRevoicePlayer* plr = &g_Players[playerId - 1];
	if (!plr->IsConnected()) {
		SERVER_PRINT("[ReVoice] Player not connected\n");
		return;
	}

	plr->SetVoicePitch(pitch);

	if (!g_pcv_rev_voicecmd_verbose || g_pcv_rev_voicecmd_verbose->value != 0.0f) {
		char buf[128];
		snprintf(buf, sizeof(buf), "[ReVoice] Set voice pitch for player %d to %.2f\n", playerId, plr->GetVoicePitch());
		SERVER_PRINT(buf);
	}
}

// Server command: sv_voice_volume <player_id> <volume>
// Adjusts outgoing mic volume for that player (0.0 = mute, 1.0 = normal, up to 10.0 boost)
static void Cmd_VoiceVolume()
{
	int argc = CMD_ARGC();
	if (argc < 3) {
		SERVER_PRINT("Usage: sv_voice_volume <player_id> <volume 0.0..10.0>\n");
		return;
	}

	int playerId = atoi(CMD_ARGV(1));
	float volume = (float)atof(CMD_ARGV(2));

	if (playerId < 1 || playerId > g_RehldsSvs->GetMaxClients()) {
		SERVER_PRINT("[ReVoice] Invalid player id\n");
		return;
	}

	CRevoicePlayer* plr = &g_Players[playerId - 1];
	if (!plr->IsConnected()) {
		SERVER_PRINT("[ReVoice] Player not connected\n");
		return;
	}

	plr->SetVoiceVolume(volume);

	if (!g_pcv_rev_voicecmd_verbose || g_pcv_rev_voicecmd_verbose->value != 0.0f) {
		char buf[128];
		snprintf(buf, sizeof(buf), "[ReVoice] Set voice volume for player %d to %.2f\n", playerId, plr->GetVoiceVolume());
		SERVER_PRINT(buf);
	}
}
#endif  // ---- END disabled: per-player pitch / volume cmds ----

static void Cmd_AsrRecord()
{
	int argc = CMD_ARGC();
	if (argc < 3) {
		SERVER_PRINT("Usage: rv_asr_record <player_id 1..32> <0|1>\n");
		return;
	}

	int playerId = atoi(CMD_ARGV(1));
	int enable = atoi(CMD_ARGV(2));

	if (playerId < 1 || playerId > g_RehldsSvs->GetMaxClients()) {
		SERVER_PRINT("[ReVoice] rv_asr_record: invalid player id\n");
		return;
	}

	int idx = playerId - 1;
	bool wasActive = g_asrActive[idx];
	g_asrActive[idx] = (enable != 0);

	RvLog("[ASR] cmd slot=%d enable=%d wasActive=%d transition=%d",
		idx, enable, wasActive ? 1 : 0,
		(wasActive != g_asrActive[idx]) ? 1 : 0);

	// Flush on any transition. true→false: release the buffered check
	// audio to AMXX. false→true: drop any cvar-mode buffer in flight so
	// its content can't bleed into the new check's transcription.
	if (wasActive != g_asrActive[idx]) {
		g_Players[idx].FlushWav("asr-toggle");
	}
}

qboolean ClientConnect_PreHook(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128])
{
	RvLog("[HOOK] ClientConnect name=%s addr=%s",
		pszName ? pszName : "(null)",
		pszAddress ? pszAddress : "(null)");

	CRevoicePlayer *plr = GetPlayerByEdict(pEntity);
	plr->OnConnected();

	RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void ClientCommand_PreHook(edict_t *pEntity)
{
	// Client-driven "playvoice" is part of the disabled playback subsystem;
	// fall through to the game DLL untouched.
	// const char* cmd = CMD_ARGV(0);
	// if (cmd && _stricmp(cmd, "playvoice") == 0) {
	//     Cmd_PlayVoice_Client(pEntity);
	//     RETURN_META(MRES_SUPERCEDE);
	// }

	RETURN_META(MRES_IGNORED);
}

void ServerActivate_PostHook(edict_t *pEdictList, int edictCount, int clientMax)
{
	Revoice_Exec_Config();

	// Once-per-map-boundary check for the auto-dump scheduler. This is the
	// only place outside the explicit `rv_upload_dump` command that can
	// reach Cmd_UploadDump — and even then, only when REV_AutoUploadDump=1
	// AND today's calendar date hasn't been dumped yet AND it is past 04:00.
	// If REV_AutoUploadDump is 0 (the default), this is a one-cvar-read no-op.
	Revoice_AutoDump_MaybeTrigger();

	SET_META_RESULT(MRES_IGNORED);
}

// Map ending: drop everything still buffered for every player. Without
// this, sv.time resets at the boundary and the silence-gap check in
// FlushWavIfStale misfires (the "now < lastVoice" branch eventually
// catches it on the new map, but flushing here makes the map boundary
// clean and keeps each map's audio in its own file.)
void ServerDeactivate_PreHook()
{
	RvLog("[HOOK] ServerDeactivate");
	Revoice_FlushAll_Players();
	SET_META_RESULT(MRES_IGNORED);
}

void StartFrame_PreHook()
{
	// Voice playback (bot-emitted music) — disabled while we isolate.
	// g_VoicePlayback.Update();

	double now = g_RehldsSv->GetTime();
	int maxclients = g_RehldsSvs->GetMaxClients();
	// Idle flush for every slot. The per-slot accumulator returns
	// immediately if there is nothing buffered.
	for (int i = 0; i < maxclients; i++) {
		g_Players[i].FlushWavIfStale(now, WAV_FLUSH_GAP_SEC);
	}

	// Periodic state snapshot to RV*.log. Self-rate-limits to ~30s.
	Revoice_LogHeartbeatTick();

	// Note: the external dumper subsystem deliberately does *not* hook
	// into this frame loop. The upload worker writes its own log lines
	// directly (RvLogUpload → RV_upload_YYYYMMDD.log), so there is no
	// per-frame queue-drain to maintain here.

	RETURN_META(MRES_IGNORED);
}

void StartFrame_PostHook()
{
	SET_META_RESULT(MRES_IGNORED);
}

void SV_WriteVoiceCodec_hooked(IRehldsHook_SV_WriteVoiceCodec *chain, sizebuf_t *sb)
{
	IGameClient *cl = g_RehldsFuncs->GetHostClient();
	CRevoicePlayer *plr = GetPlayerByClientPtr(cl);

	switch (plr->GetCodecType())
	{
		case vct_silk:
		case vct_opus:
		case vct_speex:
		{
			g_RehldsFuncs->MSG_WriteByte(sb, svc_voiceinit);
			g_RehldsFuncs->MSG_WriteString(sb, "voice_speex");	// codec id
			g_RehldsFuncs->MSG_WriteByte(sb, 5);				// quality
			break;
		}
		default:
			LCPrintf(true, "SV_WriteVoiceCodec() called on client(%d) with unknown voice codec\n", cl->GetId());
			break;
	}
}

bool Revoice_Load()
{
	if (!Revoice_Utils_Init())
		return false;

	if (!Revoice_RehldsApi_Init()) {
		LCPrintf(true, "Failed to locate REHLDS API\n");
		return false;
	}

	if (!Revoice_ReunionApi_Init())
		return false;

	Revoice_Init_Cvars();
	Revoice_Init_Config();
	Revoice_Init_Players();

	// Disabled while we isolate the wav-save + ASR path:
	//   g_engfuncs.pfnAddServerCommand("sv_voice_volume", Cmd_VoiceVolume);
	//   g_engfuncs.pfnAddServerCommand("sv_voice_pitch",  Cmd_VoicePitch);
	g_engfuncs.pfnAddServerCommand("rv_asr_record", Cmd_AsrRecord);

	// External dumper subsystem — registered only as a server cmd. No
	// init function, no per-frame work, no listeners. The worker thread
	// is spawned only inside Cmd_UploadDump and logs to its own file
	// (RV_upload_YYYYMMDD.log) via RvLogUpload.
	g_engfuncs.pfnAddServerCommand("rv_upload_dump", Cmd_UploadDump);

	if (!Revoice_Main_Init()) {
		LCPrintf(true, "Initialization failed\n");
		return false;
	}

	SERVER_PRINT("[ReVoice] WAV recording + ASR isolated mode\n");
	RvLog("[BOOT] ReVoice loaded (WAV-save + ASR isolated build), version=%s", APP_VERSION);
	return true;
}

bool Revoice_Main_Init()
{
	g_RehldsHookchains->SV_DropClient()->registerHook(&SV_DropClient_hook, HC_PRIORITY_DEFAULT + 1);
	g_RehldsHookchains->HandleNetCommand()->registerHook(&Rehlds_HandleNetCommand, HC_PRIORITY_DEFAULT + 1);
	g_RehldsHookchains->SV_WriteVoiceCodec()->registerHook(&SV_WriteVoiceCodec_hooked, HC_PRIORITY_DEFAULT + 1);

	return true;
}

void Revoice_Main_DeInit()
{
	g_RehldsHookchains->SV_DropClient()->unregisterHook(&SV_DropClient_hook);
	g_RehldsHookchains->HandleNetCommand()->unregisterHook(&Rehlds_HandleNetCommand);
	g_RehldsHookchains->SV_WriteVoiceCodec()->unregisterHook(&SV_WriteVoiceCodec_hooked);

	Revoice_DeInit_Cvars();
}
