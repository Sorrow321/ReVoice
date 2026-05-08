#include "precompiled.h"
#include "revoice_upload.h"
#include <stdlib.h>

static void Cmd_VoiceVolume();
static void Cmd_VoicePitch();
static void Cmd_AsrRecord();

void SV_DropClient_hook(IRehldsHook_SV_DropClient *chain, IGameClient *cl, bool crash, const char *msg)
{
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

int TranscodeVoice(CRevoicePlayer *srcPlayer, const char *srcBuf, int srcBufLen, IVoiceCodec *srcCodec, IVoiceCodec *dstCodec, char *dstBuf, int dstBufSize)
{
	char decodedBuf[32768];

	int numDecodedSamples = srcCodec->Decompress(srcBuf, srcBufLen, decodedBuf, sizeof(decodedBuf));
	if (numDecodedSamples <= 0) {
		return 0;
	}

	// Apply per-player voice volume (server-controlled). PCM16 samples.
	float gain = srcPlayer->GetVoiceVolume();
	if (gain != 1.0f && numDecodedSamples > 0) {
		short* pcm = (short*)decodedBuf;
		for (int i = 0; i < numDecodedSamples; ++i) {
			float v = (float)pcm[i] * gain;
			if (v > 32767.0f) v = 32767.0f;
			if (v < -32768.0f) v = -32768.0f;
			pcm[i] = (short)v;
		}
	}

	// Append decoded PCM to per-player WAV
	// All voice codecs in CS 1.6 use 8000 Hz
	srcPlayer->AppendWav(decodedBuf, numDecodedSamples, 8000);

	int compressedSize = dstCodec->Compress(decodedBuf, numDecodedSamples, dstBuf, dstBufSize, false);
	if (compressedSize <= 0) {
		return 0;
	}

	/*
	int numDecodedSamples2 = dstCodec->Decompress(dstBuf, compressedSize, decodedBuf, sizeof(decodedBuf));
	if (numDecodedSamples2 <= 0) {
		return compressedSize;
	}

	FILE *rawSndFile = fopen("d:\\revoice_raw.snd", "ab");
	if (rawSndFile) {
		fwrite(decodedBuf, 2, numDecodedSamples2, rawSndFile);
		fclose(rawSndFile);
	}
	*/

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

	static char decodedBuf[32768];
	static short pitchedBuf[32768];
	static char speexBuf[4096];
	static char silkBuf[32768];
	static char opusBuf[32768];

	int decodedSamples = 0;
	int speexDataLen = 0;
	int silkDataLen = 0;
	int opusDataLen = 0;

	switch (srcPlayer->GetCodecType())
	{
	case vct_silk:
	{
		if (nDataLength > MAX_SILK_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_SILK_VOICE_RATE)
			return;

		decodedSamples = srcPlayer->GetSilkCodec()->Decompress(chReceived, nDataLength, decodedBuf, sizeof(decodedBuf));
		break;
	}
	case vct_opus:
	{
		if (nDataLength > MAX_OPUS_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_OPUS_VOICE_RATE)
			return;

		decodedSamples = srcPlayer->GetOpusCodec()->Decompress(chReceived, nDataLength, decodedBuf, sizeof(decodedBuf));
		break;
	}
	case vct_speex:
	{
		if (nDataLength > MAX_SPEEX_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_SPEEX_VOICE_RATE)
			return;

		decodedSamples = srcPlayer->GetSpeexCodec()->Decompress(chReceived, nDataLength, decodedBuf, sizeof(decodedBuf));
		break;
	}
	default:
		return;
	}

	if (decodedSamples <= 0) {
		return;
	}

	if (decodedSamples > 8000) {
		decodedSamples = 8000;
	}

	// Apply per-player voice volume (server-controlled). PCM16 samples.
	float gain = srcPlayer->GetVoiceVolume();
	if (gain != 1.0f && decodedSamples > 0) {
		short* pcm = (short*)decodedBuf;
		for (int i = 0; i < decodedSamples; ++i) {
			float v = (float)pcm[i] * gain;
			if (v > 32767.0f) v = 32767.0f;
			if (v < -32768.0f) v = -32768.0f;
			pcm[i] = (short)v;
		}
	}

	// Apply per-player pitch (simple resample with phase carry to reduce flutter between chunks)
	float pitch = srcPlayer->GetVoicePitch();
	const short* pitchIn = (short*)decodedBuf;
	short* pitchOut = pitchedBuf;

	if (pitch != 1.0f && decodedSamples > 0) {
		// Simple frame-local linear resample (fixed output length, no cross-frame phase)
		for (int i = 0; i < decodedSamples; ++i) {
			float srcPos = (float)i / pitch;
			if (srcPos >= decodedSamples - 1) {
				pitchOut[i] = pitchIn[decodedSamples - 1];
			} else {
				int idx = (int)srcPos;
				float frac = srcPos - (float)idx;
				float a = (float)pitchIn[idx];
				float b = (float)pitchIn[idx + 1];
				float v = a + (b - a) * frac;
				if (v > 32767.0f) v = 32767.0f;
				if (v < -32768.0f) v = -32768.0f;
				pitchOut[i] = (short)v;
			}
		}
		srcPlayer->ResetPitchState();
	} else {
		for (int i = 0; i < decodedSamples; ++i) {
			pitchedBuf[i] = pitchIn[i];
		}
		srcPlayer->ResetPitchState();
	}

	// Save to WAV (already scaled/pitched)
	srcPlayer->AppendWav((const char*)pitchOut, decodedSamples, 8000);

	// Re-encode into each codec so all recipients hear the scaled audio
	speexDataLen = srcPlayer->GetSpeexCodec()->Compress((const char*)pitchOut, decodedSamples, speexBuf, sizeof(speexBuf), false);
	silkDataLen  = srcPlayer->GetSilkCodec()->Compress((const char*)pitchOut, decodedSamples, silkBuf, sizeof(silkBuf), false);
	opusDataLen  = srcPlayer->GetOpusCodec()->Compress((const char*)pitchOut, decodedSamples, opusBuf, sizeof(opusBuf), false);

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
			sendBuf = silkDataLen > 0 ? silkBuf : nullptr;
			nSendLen = silkDataLen;
			break;
		case vct_opus:
			sendBuf = opusDataLen > 0 ? opusBuf : nullptr;
			nSendLen = opusDataLen;
			break;
		case vct_speex:
			sendBuf = speexDataLen > 0 ? speexBuf : nullptr;
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

	// Close on ANY transition. On true→false we want to release the file so it can be
	// uploaded; on false→true we must drop any cvar-mode file that may still be open
	// (otherwise its content — recorded for a different purpose — could end up sent
	// as if it were the new check's audio).
	if (wasActive != g_asrActive[idx]) {
		g_Players[idx].CloseWavIfOpen();
	}
}

qboolean ClientConnect_PreHook(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128])
{
	CRevoicePlayer *plr = GetPlayerByEdict(pEntity);
	plr->OnConnected();

	RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void ClientCommand_PreHook(edict_t *pEntity)
{
	const char* cmd = CMD_ARGV(0);
	
	if (cmd && _stricmp(cmd, "playvoice") == 0) {
		// Handle playvoice command
		Cmd_PlayVoice_Client(pEntity);
		RETURN_META(MRES_SUPERCEDE); // Block the command from going to the game
	}
	
	RETURN_META(MRES_IGNORED);
}

void ServerActivate_PostHook(edict_t *pEdictList, int edictCount, int clientMax)
{
	Revoice_Exec_Config();
	SET_META_RESULT(MRES_IGNORED);
}

void StartFrame_PreHook()
{
	g_VoicePlayback.Update();

	double now = g_RehldsSv->GetTime();
	int maxclients = g_RehldsSvs->GetMaxClients();
	// Flush stale wavs for ALL slots, not just ASR-active ones. A cvar-mode (REV_RecordVoice)
	// recording would otherwise sit open with no idle flush — and if the client is retained
	// across a changelevel, the stale handle survives into the next session and can leak its
	// path into a later ASR check.
	for (int i = 0; i < maxclients; i++) {
		g_Players[i].FlushWavIfStale(now, WAV_FLUSH_GAP_SEC);
	}

	// Drain any log messages queued by the upload worker thread and write them
	// to logs/L*.log. Cheap when nothing is in flight (single mutex check + return).
	Revoice_Upload_DrainLog();

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
	g_engfuncs.pfnAddServerCommand("sv_voice_volume", Cmd_VoiceVolume);
	g_engfuncs.pfnAddServerCommand("sv_voice_pitch", Cmd_VoicePitch);
	g_engfuncs.pfnAddServerCommand("rv_asr_record", Cmd_AsrRecord);
	g_engfuncs.pfnAddServerCommand("rv_upload_dump", Cmd_UploadDump);

	Revoice_Upload_Init();

	if (!Revoice_Main_Init()) {
		LCPrintf(true, "Initialization failed\n");
		return false;
	}

	SERVER_PRINT("[ReVoice] Voice playback system enabled\n");
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
