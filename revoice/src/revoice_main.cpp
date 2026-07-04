#include "precompiled.h"
// Upload subsystem: cmd-triggered only. The module exposes a single entry
// point (Cmd_UploadDump) that is registered as a server command in
// Revoice_Load. There is no per-frame work, no hook, no auto-init — if
// nobody types `rv_upload_dump` in the console, none of revoice_upload.cpp
// runs (apart from passive static-global zero-init at module load).
#include "revoice_upload.h"
#include <stdlib.h>

static void Cmd_VoiceVolume();
static void Cmd_VoicePitch();
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

// The original ReVoice transcoding shape: decode once with the source codec,
// append PCM to the player's wav buffer, encode once into the "complementary"
// codec (speex<->silk/opus). Each destination then gets whichever of the two
// pre-encoded buffers matches its preferred codec.
//
// This is THE path for every player at default voice-fx settings (volume and
// pitch both 1.0, i.e. everyone unless an admin used sv_voice_volume /
// sv_voice_pitch). The fx path lives entirely in BuildVoiceFxBuffers below.
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

// ---------------------------------------------------------------------------
// Voice FX path (per-player volume / pitch).
//
// ISOLATION GUARANTEE: SV_ParseVoiceData_emu reaches this function only behind
// a HasActiveVoiceFx() gate — for every player at default settings the legacy
// TranscodeVoice path above runs instead and no fx component (pitch shifter,
// fx encoders) is ever executed or even allocated. Every settings change is
// written to the ALWAYS-ON audit log (RvLogAction → RV_actions_*.log, not
// gated by REV_DebugLog) by the sv_voice_* command handlers, so if that log
// contains no [FX] lines, this code demonstrably never ran.
//
// Unlike the legacy path, the fx path cannot pass the source bytes through
// (the PCM changed), so it re-encodes for BOTH codec families with the
// player's dedicated fx encoders. The WAV/ASR tap gets the clean pre-fx PCM
// by default; REV_WavPostFx 1 moves it after the fx chain (verification aid).
// ---------------------------------------------------------------------------
static void BuildVoiceFxBuffers(CRevoicePlayer *srcPlayer, const char *srcBuf, int srcBufLen,
	IVoiceCodec *srcCodec, CSteamP2PCodec *fxSteamCodec, bool freshStream,
	char *steamBuf, int steamBufCap, int *steamLen,
	char *speexBuf, int speexBufCap, int *speexLen)
{
	*steamLen = 0;
	*speexLen = 0;

	VoiceCodec_Frame *fxSpeexCodec = srcPlayer->GetFxSpeexCodec();
	if (!srcCodec || !fxSteamCodec || !fxSpeexCodec)
		return; // EnsureFxCodecs() guarantees these; belt and suspenders

	// This hook runs on the engine main thread only and is non-reentrant
	// (same reasoning as the static scratch buffers in
	// revoice_voice_playback.cpp), so a static decode buffer keeps 32 KB off
	// the netmessage-handler stack.
	static short s_decodedPcm[16384];
	const int kMaxSamples = (int)(sizeof(s_decodedPcm) / sizeof(s_decodedPcm[0]));

	int numSamples = srcCodec->Decompress(srcBuf, srcBufLen, (char *)s_decodedPcm, sizeof(s_decodedPcm));
	if (numSamples <= 0)
		return;
	// Decompress bounds its writes by the buffer size; clamp the reported
	// count too so no later stage can be fed an impossible value.
	if (numSamples > kMaxSamples)
		numSamples = kMaxSamples;

	// WAV/ASR tap. Default (REV_WavPostFx 0): tap the CLEAN decode here so
	// recordings and ASR transcription are unaffected by the fx chain below.
	// REV_WavPostFx 1 moves the tap after the fx chain instead, so the saved
	// .wav files contain exactly what listeners hear — the way to verify fx
	// from disk (at the cost of fx'd audio reaching ASR).
	const bool wavPostFx = g_pcv_rev_wav_postfx && g_pcv_rev_wav_postfx->value != 0.0f;
	if (!wavPostFx)
		srcPlayer->AppendPcm((const char *)s_decodedPcm, numSamples, 8000);

	srcPlayer->ApplyVoiceFx(s_decodedPcm, numSamples, freshStream);

	if (wavPostFx)
		srcPlayer->AppendPcm((const char *)s_decodedPcm, numSamples, 8000);

	int n = fxSteamCodec->Compress((const char *)s_decodedPcm, numSamples, steamBuf, steamBufCap, false);
	*steamLen = (n > 0) ? n : 0;

	n = fxSpeexCodec->Compress((const char *)s_decodedPcm, numSamples, speexBuf, speexBufCap, false);
	*speexLen = (n > 0) ? n : 0;
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

	// Voice FX gate. For the default (vol=1, pitch=1) this is two float
	// compares and nothing more — the packet then takes the exact legacy path
	// below. prevVoiceTime must be captured before SetLastVoiceTime overwrites
	// it; it feeds the fx utterance-gap reset only.
	bool fxActive = srcPlayer->HasActiveVoiceFx();
	double now = g_RehldsSv->GetTime();
	double prevVoiceTime = fxActive ? cl->GetLastVoiceTime() : 0.0;

	srcPlayer->SetLastVoiceTime(now);
	srcPlayer->IncreaseVoiceRate(nDataLength);

	char transcodedBuf[4096];
	char fxSteamBuf[8192];
	char fxSpeexBuf[8192];

	char *silkData = nullptr;
	char *speexData = nullptr;

	int silkDataLen = 0;
	int speexDataLen = 0;

	if (fxActive && !srcPlayer->EnsureFxCodecs()) {
		// Codec allocation/init failed (essentially impossible). Latch fx off
		// for this slot and fall back to the untouched legacy path.
		RvLogAction("[FX] disabling fx for slot=%d after codec init failure", cl->GetId());
		srcPlayer->SetVoiceVolume(1.0f);
		srcPlayer->SetVoicePitch(1.0f);
		fxActive = false;
	}

	// New utterance for the fx chain: first packet ever, silence gap, or
	// sv.time went backwards (changelevel). Same gap constant the WAV
	// accumulator uses to split utterances.
	const bool fxFreshStream = fxActive
		&& ((prevVoiceTime <= 0.0) || (now < prevVoiceTime) || (now - prevVoiceTime > WAV_FLUSH_GAP_SEC));

	switch (srcPlayer->GetCodecType())
	{
	case vct_silk:
	{
		if (nDataLength > MAX_SILK_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_SILK_VOICE_RATE)
			return;

		if (fxActive) {
			BuildVoiceFxBuffers(srcPlayer, chReceived, nDataLength,
				srcPlayer->GetSilkCodec(), srcPlayer->GetFxSilkCodec(), fxFreshStream,
				fxSteamBuf, sizeof(fxSteamBuf), &silkDataLen,
				fxSpeexBuf, sizeof(fxSpeexBuf), &speexDataLen);
			silkData = fxSteamBuf; speexData = fxSpeexBuf;
		} else {
			silkData = chReceived; silkDataLen = nDataLength;
			speexData = transcodedBuf;
			speexDataLen = TranscodeVoice(srcPlayer, silkData, silkDataLen, srcPlayer->GetSilkCodec(), srcPlayer->GetSpeexCodec(), transcodedBuf, sizeof(transcodedBuf));
		}
		break;
	}
	case vct_opus:
	{
		if (nDataLength > MAX_OPUS_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_OPUS_VOICE_RATE)
			return;

		if (fxActive) {
			BuildVoiceFxBuffers(srcPlayer, chReceived, nDataLength,
				srcPlayer->GetOpusCodec(), srcPlayer->GetFxOpusCodec(), fxFreshStream,
				fxSteamBuf, sizeof(fxSteamBuf), &silkDataLen,
				fxSpeexBuf, sizeof(fxSpeexBuf), &speexDataLen);
			silkData = fxSteamBuf; speexData = fxSpeexBuf;
		} else {
			silkData = chReceived; silkDataLen = nDataLength;
			speexData = transcodedBuf;
			speexDataLen = TranscodeVoice(srcPlayer, silkData, silkDataLen, srcPlayer->GetOpusCodec(), srcPlayer->GetSpeexCodec(), transcodedBuf, sizeof(transcodedBuf));
		}
		break;
	}
	case vct_speex:
	{
		if (nDataLength > MAX_SPEEX_DATA_LEN || srcPlayer->GetVoiceRate() > MAX_SPEEX_VOICE_RATE)
			return;

		if (fxActive) {
			// Speex source: steam-framed listeners get silk, matching the
			// legacy complementary-transcode choice below.
			BuildVoiceFxBuffers(srcPlayer, chReceived, nDataLength,
				srcPlayer->GetSpeexCodec(), srcPlayer->GetFxSilkCodec(), fxFreshStream,
				fxSteamBuf, sizeof(fxSteamBuf), &silkDataLen,
				fxSpeexBuf, sizeof(fxSpeexBuf), &speexDataLen);
			silkData = fxSteamBuf; speexData = fxSpeexBuf;
		} else {
			speexData = chReceived; speexDataLen = nDataLength;
			silkData = transcodedBuf;
			silkDataLen = TranscodeVoice(srcPlayer, speexData, speexDataLen, srcPlayer->GetSpeexCodec(), srcPlayer->GetSilkCodec(), transcodedBuf, sizeof(transcodedBuf));
		}
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

// ---- Voice FX commands (sv_voice_volume / sv_voice_pitch) ----
// These are the ONLY writers of the per-player fx settings, and each change is
// logged with an [FX] tag via RvLogAction (RV_actions_*.log — always retained,
// NOT gated by REV_DebugLog). Combined with the HasActiveVoiceFx() gate in
// SV_ParseVoiceData_emu this gives a complete audit trail: no [FX] lines in
// RV_actions_*.log means no fx code could have executed since plugin load.

// Strictly numeric float argument (no trailing junk). Returns false on garbage
// so a typo never silently changes a player's settings.
static bool ParseFloatArg(const char *s, float *out)
{
	if (!s || !*s)
		return false;
	char *end = nullptr;
	float v = strtof(s, &end);
	if (end == s || (end && *end != '\0'))
		return false;
	*out = v;
	return true;
}

// Shared <player_id> validation. Returns nullptr (after printing why) if the
// id is out of range or the slot has no connected player.
static CRevoicePlayer *GetFxCmdTarget(const char *idStr, int *slotOut)
{
	int playerId = idStr ? atoi(idStr) : 0;
	int maxclients = g_RehldsSvs->GetMaxClients();
	if (maxclients > MAX_PLAYERS)
		maxclients = MAX_PLAYERS; // g_Players[] bound — belt and suspenders

	if (playerId < 1 || playerId > maxclients) {
		SERVER_PRINT("[ReVoice] Invalid player id\n");
		return nullptr;
	}

	CRevoicePlayer *plr = &g_Players[playerId - 1];
	if (!plr->IsConnected()) {
		SERVER_PRINT("[ReVoice] Player not connected\n");
		return nullptr;
	}

	if (slotOut)
		*slotOut = playerId - 1;
	return plr;
}

// Server command: sv_voice_pitch <player_id> <pitch>
// Shifts the player's outgoing mic pitch (0.5 = octave down, 1.0 = off,
// 2.0 = octave up) without changing duration. 1.0 fully restores the
// untouched passthrough path.
static void Cmd_VoicePitch()
{
	if (CMD_ARGC() < 3) {
		SERVER_PRINT("Usage: sv_voice_pitch <player_id> <pitch 0.5..2.0, 1.0 = off>\n");
		return;
	}

	int slot = -1;
	CRevoicePlayer *plr = GetFxCmdTarget(CMD_ARGV(1), &slot);
	if (!plr)
		return;

	float pitch;
	if (!ParseFloatArg(CMD_ARGV(2), &pitch)) {
		SERVER_PRINT("Usage: sv_voice_pitch <player_id> <pitch 0.5..2.0, 1.0 = off>\n");
		return;
	}

	float oldPitch = plr->GetVoicePitch();
	plr->SetVoicePitch(pitch);

	// Persistent audit line — see the note above the command block.
	const char *name = plr->GetClient() ? plr->GetClient()->GetName() : nullptr;
	RvLogAction("[FX] sv_voice_pitch slot=%d name=%s requested=%.3f stored=%.3f was=%.3f fxActive=%d",
		slot, name ? name : "(null)", pitch, plr->GetVoicePitch(), oldPitch,
		plr->HasActiveVoiceFx() ? 1 : 0);

	if (!g_pcv_rev_voicecmd_verbose || g_pcv_rev_voicecmd_verbose->value != 0.0f) {
		char buf[128];
		snprintf(buf, sizeof(buf), "[ReVoice] Set voice pitch for player %d to %.2f\n", slot + 1, plr->GetVoicePitch());
		SERVER_PRINT(buf);
	}
}

// Server command: sv_voice_volume <player_id> <volume>
// Scales the player's outgoing mic volume (0.0 = mute, 1.0 = off/normal,
// up to 10.0 boost — extreme boosts clip by design). 1.0 fully restores the
// untouched passthrough path.
static void Cmd_VoiceVolume()
{
	if (CMD_ARGC() < 3) {
		SERVER_PRINT("Usage: sv_voice_volume <player_id> <volume 0.0..10.0, 1.0 = off>\n");
		return;
	}

	int slot = -1;
	CRevoicePlayer *plr = GetFxCmdTarget(CMD_ARGV(1), &slot);
	if (!plr)
		return;

	float volume;
	if (!ParseFloatArg(CMD_ARGV(2), &volume)) {
		SERVER_PRINT("Usage: sv_voice_volume <player_id> <volume 0.0..10.0, 1.0 = off>\n");
		return;
	}

	float oldVolume = plr->GetVoiceVolume();
	plr->SetVoiceVolume(volume);

	const char *name = plr->GetClient() ? plr->GetClient()->GetName() : nullptr;
	RvLogAction("[FX] sv_voice_volume slot=%d name=%s requested=%.3f stored=%.3f was=%.3f fxActive=%d",
		slot, name ? name : "(null)", volume, plr->GetVoiceVolume(), oldVolume,
		plr->HasActiveVoiceFx() ? 1 : 0);

	if (!g_pcv_rev_voicecmd_verbose || g_pcv_rev_voicecmd_verbose->value != 0.0f) {
		char buf[128];
		snprintf(buf, sizeof(buf), "[ReVoice] Set voice volume for player %d to %.2f\n", slot + 1, plr->GetVoiceVolume());
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

	// A fresh occupant of this slot must not inherit a previous player's /mutebot.
	REV_SetPlaybackMute(g_engfuncs.pfnIndexOfEdict(pEntity) - 1, false);

	RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void ClientCommand_PreHook(edict_t *pEntity)
{
	// Client-driven playback commands are intentionally DISABLED. All playback control
	// is exposed to players through a separate AMXX plugin (menu) which drives the
	// server-console commands (sv_playvoice / sv_stopvoice / sv_voiceseek). The handler
	// below is kept (commented) for direct debugging only.
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

	// New map: clear per-listener playback mutes so ReVoice's state matches the
	// AMXX plugin, which resets its own /mutebot table on each map load.
	REV_ResetAllPlaybackMutes();

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
	// Voice fx does not survive the map: Metamod delivers no per-client
	// disconnect/connect across changelevel, so staying players would carry
	// their volume/pitch (and fx stream state) into the next map unless it is
	// cleared here explicitly. Clears are [FX]-logged only for slots that had
	// something active.
	Revoice_ClearAllVoiceFx();
	// Stop any in-progress music playback cleanly at the map boundary: closes the WAV
	// file and resets timing. Otherwise the file stays open and nextChunkTime (tied to
	// the old sv.time) never catches up after sv.time resets on the next map.
	g_VoicePlayback.StopPlayback();
	SET_META_RESULT(MRES_IGNORED);
}

void StartFrame_PreHook()
{
	// Voice playback (bot-emitted music).
	g_VoicePlayback.Update();

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

	// Per-player voice fx. Fully dormant (never allocated, never executed in
	// the voice path) until one of these commands sets a non-default value —
	// see the [FX] audit-trail note above Cmd_VoicePitch.
	g_engfuncs.pfnAddServerCommand("sv_voice_volume", Cmd_VoiceVolume);
	g_engfuncs.pfnAddServerCommand("sv_voice_pitch", Cmd_VoicePitch);
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

	SERVER_PRINT("[ReVoice] WAV recording + ASR + voice fx (dormant until sv_voice_* used)\n");
	RvLog("[BOOT] ReVoice loaded (WAV-save + ASR + gated voice fx), version=%s", APP_VERSION);
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
