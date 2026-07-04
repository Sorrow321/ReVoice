#include "precompiled.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

// Silence threshold that ends one utterance. Used both as the inline
// "speech resumed after a gap" trigger inside AppendPcm and as the
// idle-flush timeout in FlushWavIfStale.
const double WAV_FLUSH_GAP_SEC = 0.8;

// Hard cap on accumulated PCM per utterance. At 8 kHz mono PCM16
// (16 KB/s) this is ~5 minutes — far longer than any realistic
// utterance. Hitting it means the mic is jammed open or there's a
// logic bug; force-flush so the buffer cannot grow unbounded.
static const size_t MAX_WAV_SAMPLES = 5 * 60 * 8000; // 5 min @ 8 kHz mono = ~4.6 MB

static bool CreateDirectoryRecursive(const char *path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = 0;

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

// Writes a 44-byte canonical WAV/RIFF header at the current file position.
// In the accumulator design we always call this at offset 0 with the full
// dataBytes count already known, so no in-place rewrite is ever needed.
static void Wav_WriteHeader(FILE *f, int sampleRate, unsigned int dataBytes)
{
	if (!f) return;
	unsigned int riffSize = 36 + dataBytes;
	unsigned short audioFormat = 1; // PCM
	unsigned short numChannels = 1;
	unsigned short bitsPerSample = 16;
	unsigned int byteRate = (unsigned int)sampleRate * numChannels * (bitsPerSample / 8);
	unsigned short blockAlign = (unsigned short)(numChannels * (bitsPerSample / 8));
	unsigned int subchunk1Size = 16;

	fwrite("RIFF", 1, 4, f);
	fwrite(&riffSize, 4, 1, f);
	fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f);
	fwrite(&subchunk1Size, 4, 1, f);
	fwrite(&audioFormat, 2, 1, f);
	fwrite(&numChannels, 2, 1, f);
	fwrite(&sampleRate, 4, 1, f);
	fwrite(&byteRate, 4, 1, f);
	fwrite(&blockAlign, 2, 1, f);
	fwrite(&bitsPerSample, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&dataBytes, 4, 1, f);
}

static void SanitizeId(char *s)
{
	for (; *s; ++s) {
		unsigned char c = (unsigned char)*s;
		if (c < 0x20 || c == 0x7F
		 || *s == ':' || *s == '\\' || *s == '/' || *s == ' '
		 || *s == '"' || *s == '\'' || *s == '`'
		 || *s == '*' || *s == '?' || *s == '<' || *s == '>' || *s == '|'
		 || *s == ';' || *s == '%' || *s == '$' || *s == '&')
		{
			*s = '_';
		}
	}
}

const char *CRevoicePlayer::m_szCodecType[] = {
	"none",
	"silk",
	"opus",
	"speex"
};

CRevoicePlayer g_Players[MAX_PLAYERS];
bool g_asrActive[MAX_PLAYERS];

// Cumulative counters since plugin load. Used by Revoice_LogHeartbeatTick
// to surface long-term drift. All access is single-threaded (main game
// thread), so no synchronization needed.
static unsigned long g_StatUtterances = 0;
static unsigned long g_StatFlushes    = 0;
static unsigned long g_StatIpc        = 0;
static unsigned long g_StatErrors     = 0;
static size_t        g_StatHighWater  = 0;   // largest single-utterance buffer ever observed

CRevoicePlayer::CRevoicePlayer()
{
	m_CodecType = vct_none;
	m_SpeexCodec = new VoiceCodec_Frame(new VoiceEncoder_Speex());
	m_SilkCodec  = new CSteamP2PCodec(new VoiceEncoder_Silk());
	m_OpusCodec  = new CSteamP2PCodec(new VoiceEncoder_Opus());

	m_SpeexCodec->Init(SPEEX_VOICE_QUALITY);
	m_SilkCodec ->Init(SILK_VOICE_QUALITY);
	m_OpusCodec ->Init(OPUS_VOICE_QUALITY);

	m_Protocol = 0;
	m_HLTV = false;
	m_Connected = false;
	m_Client = nullptr;

	m_WavSampleRate = 0;
	m_LastWavVoiceTime = 0.0;
	m_WavStartTs = 0;
	m_WavSeq = 0;
	m_WavForAsr = false;
	m_WavAuth[0] = '\0';

	// Voice FX: off. The dedicated fx encoders are lazily created on the first
	// fx-active packet (see EnsureFxCodecs) so unused slots never allocate them.
	m_VoiceVolume = 1.0f;
	m_VoicePitch = 1.0f;
	m_FxStreamContinuous = false;
	m_FxCodecsFailed = false;
	m_FxSilkCodec = nullptr;
	m_FxOpusCodec = nullptr;
	m_FxSpeexCodec = nullptr;
}

void CRevoicePlayer::AppendPcm(const char *pcm16, int numSamples, int sampleRate)
{
	if (numSamples <= 0 || pcm16 == nullptr)
		return;

	if (!m_Client)
		return;

	int clientIndex = m_Client->GetId();
	bool asrWanted  = (clientIndex >= 0 && clientIndex < MAX_PLAYERS) && g_asrActive[clientIndex];
	bool cvarWanted = g_pcv_rev_record_voice && g_pcv_rev_record_voice->value != 0.0f;
	if (!asrWanted && !cvarWanted)
		return;

	double currentTime = g_RehldsSv->GetTime();

	// Inline silence-gap flush: resumed speech after a long pause is a new
	// utterance. We also treat clock-going-backwards (sv.time resets on
	// changelevel) as "different session" — flush anything still in the
	// buffer so we don't splice old-map audio with new-map audio.
	if (!m_WavSamples.empty() && m_LastWavVoiceTime > 0
		&& (currentTime - m_LastWavVoiceTime > WAV_FLUSH_GAP_SEC
			|| currentTime < m_LastWavVoiceTime))
	{
		FlushWav("inline-gap");
	}

	// Sample-rate change mid-utterance: flush so the existing buffer is
	// written with its correct rate, then start a fresh one. In practice
	// CS 1.6 codecs always run at 8 kHz, so this is defensive.
	if (!m_WavSamples.empty() && m_WavSampleRate != sampleRate) {
		FlushWav("rate-change");
	}

	// New utterance — capture identity / start time once. Snapshotting at
	// start means a name change mid-utterance can't move the file.
	if (m_WavSamples.empty()) {
		g_StatUtterances++;
		const char *name = m_Client->GetName();
		INetChan *nc = m_Client->GetNetChan();
		const netadr_t *adr = nc ? nc->GetRemoteAdr() : nullptr;

		if (adr && (adr->ip[0] | adr->ip[1] | adr->ip[2] | adr->ip[3]) != 0) {
			snprintf(m_WavAuth, sizeof(m_WavAuth), "IP_%u.%u.%u.%u_%s",
				adr->ip[0], adr->ip[1], adr->ip[2], adr->ip[3],
				name ? name : "Unknown");
		} else {
			snprintf(m_WavAuth, sizeof(m_WavAuth), "Player_%d_%s",
				clientIndex + 1, name ? name : "Unknown");
		}
		SanitizeId(m_WavAuth);

		time(&m_WavStartTs);
		m_WavSampleRate = sampleRate;
		m_WavForAsr     = asrWanted;

		RvLog("[WAV] utterance start slot=%d auth=%s rate=%d asr=%d cvar=%d",
			clientIndex, m_WavAuth, sampleRate, asrWanted ? 1 : 0, cvarWanted ? 1 : 0);
	}

	// Append PCM. The cap is purely a runaway-defense net.
	size_t currentSize = m_WavSamples.size();
	size_t roomLeft    = (currentSize < MAX_WAV_SAMPLES) ? (MAX_WAV_SAMPLES - currentSize) : 0;
	size_t toAppend    = (size_t)numSamples;
	if (toAppend > roomLeft) toAppend = roomLeft;

	if (toAppend > 0) {
		const int16_t *src = (const int16_t *)pcm16;
		m_WavSamples.insert(m_WavSamples.end(), src, src + toAppend);
		if (m_WavSamples.size() > g_StatHighWater) g_StatHighWater = m_WavSamples.size();
	}

	m_LastWavVoiceTime = currentTime;

	if (m_WavSamples.size() >= MAX_WAV_SAMPLES) {
		RvLog("[WAV] cap-hit slot=%d samples=%zu — forcing flush", clientIndex, m_WavSamples.size());
		FlushWav("cap-hit");
	}
}

void CRevoicePlayer::FlushWav(const char *reason)
{
	int slotForLog = m_Client ? m_Client->GetId() : -1;
	if (!reason) reason = "(unknown)";

	if (m_WavSamples.empty()) {
		// Nothing to write. Reset auxiliary state defensively in case any
		// of it was set by a previous flow that bailed before appending.
		m_LastWavVoiceTime = 0.0;
		m_WavStartTs       = 0;
		m_WavSampleRate    = 0;
		m_WavForAsr        = false;
		m_WavAuth[0]       = '\0';
		return;
	}

	RvLog("[FLUSH] enter slot=%d reason=%s samples=%zu auth=%s asr=%d rate=%d",
		slotForLog, reason, m_WavSamples.size(),
		m_WavAuth[0] ? m_WavAuth : "(empty)",
		m_WavForAsr ? 1 : 0, m_WavSampleRate);

	char dirPath[260];
	snprintf(dirPath, sizeof(dirPath), "cstrike/data/%s", m_WavAuth);

	if (!CreateDirectoryRecursive(dirPath)) {
		// Couldn't create the destination — drop the buffer rather than
		// retry forever. We never produce a partial file because we never
		// got as far as fopen.
		g_StatErrors++;
		RvLog("[FLUSH] ERR mkdir slot=%d dir=%s errno=%d — dropping buffer",
			slotForLog, dirPath, errno);
		m_WavSamples.clear();
		m_LastWavVoiceTime = 0.0;
		m_WavStartTs       = 0;
		m_WavSampleRate    = 0;
		m_WavForAsr        = false;
		m_WavAuth[0]       = '\0';
		return;
	}

	struct tm *tmv = localtime(&m_WavStartTs);
	char tsbuf[32];
	if (tmv) {
		strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d_%H-%M-%S", tmv);
	} else {
		// localtime should never realistically fail; fall back rather than
		// dereference NULL.
		RvLog("[FLUSH] WARN localtime returned NULL slot=%d ts=%ld", slotForLog, (long)m_WavStartTs);
		snprintf(tsbuf, sizeof(tsbuf), "0000-00-00_00-00-00");
	}

	// Per-player counter avoids collisions when two utterances finish in
	// the same wall-clock second. With the 0.8 s minimum silence gap, the
	// mod-100 wrap is ≥80 s — far more than any AMXX/upload roundtrip.
	m_WavSeq = (m_WavSeq + 1) % 100;

	char fullPath[260];
	snprintf(fullPath, sizeof(fullPath), "%s/%s_%u.wav", dirPath, tsbuf, m_WavSeq);

	// Single fopen + write-all + fclose. The file is either fully present
	// on disk (after fclose returns) or absent (modulo the small window
	// between fopen and fwrite during which a process kill could leave a
	// 0-byte file — but in that case the IPC below is also never fired,
	// so AMXX never tries to open it).
	FILE *f = fopen(fullPath, "wb");
	if (!f) {
		g_StatErrors++;
		RvLog("[FLUSH] ERR fopen slot=%d path=%s errno=%d — dropping buffer",
			slotForLog, fullPath, errno);
	} else {
		unsigned int dataBytes = (unsigned int)(m_WavSamples.size() * sizeof(int16_t));
		Wav_WriteHeader(f, m_WavSampleRate, dataBytes);
		size_t wrote = fwrite(m_WavSamples.data(), sizeof(int16_t), m_WavSamples.size(), f);
		fclose(f);

		if (wrote != m_WavSamples.size()) {
			g_StatErrors++;
			RvLog("[FLUSH] WARN short write slot=%d wrote=%zu expected=%zu path=%s",
				slotForLog, wrote, m_WavSamples.size(), fullPath);
		} else {
			g_StatFlushes++;
			RvLog("[FLUSH] ok slot=%d path=%s bytes=%u",
				slotForLog, fullPath, dataBytes);
		}

		// IPC to AMXX ASR plugin: gated on (a) this utterance was opened
		// while ASR was active, AND (b) ASR is still active right now.
		// (b) prevents a delayed flush from triggering a check that the
		// player has since passed/failed/cancelled.
		int clientIndex = m_Client ? m_Client->GetId() : -1;
		if (clientIndex >= 0 && clientIndex < MAX_PLAYERS
			&& m_WavForAsr && g_asrActive[clientIndex])
		{
			char cmd[512];
			snprintf(cmd, sizeof(cmd), "rv_asr_ready %d \"%s\"\n", clientIndex + 1, fullPath);
			g_StatIpc++;
			RvLog("[FLUSH] IPC slot=%d cmd=rv_asr_ready %d \"%s\"",
				slotForLog, clientIndex + 1, fullPath);
			// Cbuf_AddText only — never Cbuf_Execute from inside hooks.
			g_engfuncs.pfnServerCommand(cmd);
		} else {
			RvLog("[FLUSH] no-IPC slot=%d wavForAsr=%d g_asrActive=%d",
				slotForLog,
				m_WavForAsr ? 1 : 0,
				(clientIndex >= 0 && clientIndex < MAX_PLAYERS) ? (g_asrActive[clientIndex] ? 1 : 0) : -1);
		}
	}

	// clear() keeps capacity, so the next utterance reuses the same heap
	// allocation — steady-state memory stabilizes at the per-player peak.
	m_WavSamples.clear();
	m_LastWavVoiceTime = 0.0;
	m_WavStartTs       = 0;
	m_WavSampleRate    = 0;
	m_WavForAsr        = false;
	m_WavAuth[0]       = '\0';
}

void CRevoicePlayer::FlushWavIfStale(double now, double timeout)
{
	if (m_WavSamples.empty() || m_LastWavVoiceTime <= 0)
		return;

	// Clock going backwards (sv.time reset on changelevel) is also a flush
	// signal — keep audio from one map session out of the next.
	if (now < m_LastWavVoiceTime) {
		int slot = m_Client ? m_Client->GetId() : -1;
		RvLog("[STALE] clock-backwards slot=%d now=%.3f last=%.3f", slot, now, m_LastWavVoiceTime);
		FlushWav("clock-backwards");
	} else if ((now - m_LastWavVoiceTime) > timeout) {
		int slot = m_Client ? m_Client->GetId() : -1;
		RvLog("[STALE] gap slot=%d gap=%.3f timeout=%.3f", slot, now - m_LastWavVoiceTime, timeout);
		FlushWav("stale-gap");
	}
}

void CRevoicePlayer::Initialize(IGameClient *cl)
{
	m_Client = cl;

	m_SpeexCodec->SetClient(cl);
	m_SilkCodec ->SetClient(cl);
	m_OpusCodec ->SetClient(cl);
}

void CRevoicePlayer::OnConnected()
{
	int slot = m_Client ? m_Client->GetId() : -1;

	// already connected, suppose now there is a change of level?
	if (m_Connected) {
		RvLog("[PLR] OnConnected (already-connected, changelevel?) slot=%d", slot);
		m_VoiceRate = 0;
		return;
	}

	int protocol = g_ReunionApi->GetClientProtocol(m_Client->GetId());
	if (protocol != 47 && protocol != 48) {
		RvLog("[PLR] OnConnected unsupported-proto slot=%d proto=%d", slot, protocol);
		return;
	}

	// reset codec state
	m_SilkCodec->ResetState();
	m_OpusCodec->ResetState();
	m_SpeexCodec->ResetState();

	// A fresh occupant of the slot must never inherit a previous player's
	// voice fx. Logs only if there was anything to clear (keeps the always-on
	// RV_actions_*.log free of [FX] lines unless the feature was actually used).
	ClearVoiceFx("connect");

	// default codec
	m_CodecType = GetCodecTypeByString(g_pcv_rev_default_codec->string);
	m_VoiceRate = 0;
	m_Connected = true;
	m_RequestId = MAKE_REQUESTID(PLID);
	m_Protocol = protocol;

	if (g_ReunionApi->GetClientAuthtype(m_Client->GetId()) == DP_AUTH_HLTV) {
		m_CodecType = GetCodecTypeByString(g_pcv_rev_hltv_codec->string);
		m_HLTV = true;
	}
	else if (m_Protocol == 48) {
		g_engfuncs.pfnQueryClientCvarValue2(m_Client->GetEdict(), "sv_version", m_RequestId);
	}

	const char *name = m_Client ? m_Client->GetName() : nullptr;
	RvLog("[PLR] connected slot=%d name=%s proto=%d hltv=%d codec=%s",
		slot, name ? name : "(null)", protocol, m_HLTV ? 1 : 0,
		GetCodecTypeToString());
}

void CRevoicePlayer::OnDisconnected()
{
	int slot = m_Client ? m_Client->GetId() : -1;
	RvLog("[PLR] disconnected slot=%d wasConnected=%d bufferedSamples=%zu",
		slot, m_Connected ? 1 : 0, m_WavSamples.size());

	m_HLTV = false;
	m_Connected = false;
	m_Protocol = 0;

	ClearVoiceFx("disconnect");

	FlushWav("disconnect");

	int clientIndex = m_Client ? m_Client->GetId() : -1;
	if (clientIndex >= 0 && clientIndex < MAX_PLAYERS)
		g_asrActive[clientIndex] = false;

	m_CodecType = vct_none;
	m_VoiceRate = 0;
	m_RequestId = 0;
}

void CRevoicePlayer::Update()
{
	if (m_HLTV) {
		m_CodecType = GetCodecTypeByString(g_pcv_rev_hltv_codec->string);
		return;
	}

	m_CodecType = GetCodecTypeByString(g_pcv_rev_default_codec->string);
	m_RequestId = MAKE_REQUESTID(PLID);

	if (m_Protocol == 48) {
		g_engfuncs.pfnQueryClientCvarValue2(m_Client->GetEdict(), "sv_version", m_RequestId);
	}
}

void Revoice_Init_Players()
{
	int maxclients = g_RehldsSvs->GetMaxClients();
	for (int i = 0; i < maxclients; i++) {
		g_Players[i].Initialize(g_RehldsSvs->GetClient(i));
	}
}

// Map boundaries (changelevel, server shutdown) reset sv.time, so the
// per-frame stale-detection in FlushWavIfStale would misfire across the
// boundary. We flush here so every map starts with empty buffers.
void Revoice_FlushAll_Players()
{
	int maxclients = g_RehldsSvs->GetMaxClients();
	RvLog("[MAP] flush-all begin maxclients=%d", maxclients);
	for (int i = 0; i < maxclients; i++) {
		g_Players[i].FlushWav("map-boundary");
	}
	RvLog("[MAP] flush-all end");
}

// Voice fx must be cleared at the map boundary explicitly: Metamod plugins do
// NOT see SV_DropClient / a fresh connect for players who stay through a
// changelevel, so the per-connection ClearVoiceFx calls never fire for them.
// Mirrors the AMXX side, which resets its own per-player tables on map load.
void Revoice_ClearAllVoiceFx()
{
	int maxclients = g_RehldsSvs->GetMaxClients();
	if (maxclients > MAX_PLAYERS)
		maxclients = MAX_PLAYERS;
	for (int i = 0; i < maxclients; i++) {
		g_Players[i].ClearVoiceFx("map-boundary");
	}
}

// Periodic state snapshot. Called every server frame; rate-limits itself
// to ~one line per 30 wall-clock seconds. Records:
//   - currently connected / asr-active / in-flight (non-empty buffer) slots
//   - total bytes currently buffered across all players
//   - cumulative counters since plugin load (utterances, flushes, IPCs, errors)
//   - high-water mark of largest buffer ever
// The point is to surface slow drift (memory creep, leaking ASR slots,
// growing error rate) that single events would not reveal.
void Revoice_LogHeartbeatTick()
{
	if (g_pcv_rev_debug_log && g_pcv_rev_debug_log->value == 0.0f)
		return;

	static time_t s_lastTick = 0;
	time_t nowWall = time(nullptr);
	if (s_lastTick != 0 && (nowWall - s_lastTick) < 30) return;
	s_lastTick = nowWall;

	int maxclients   = g_RehldsSvs ? g_RehldsSvs->GetMaxClients() : 0;
	int connected    = 0;
	int asrActive    = 0;
	int inflight     = 0;
	size_t totalBuf  = 0;
	size_t maxSlot   = 0;

	for (int i = 0; i < maxclients; i++) {
		if (g_Players[i].IsConnected()) connected++;
		if (g_asrActive[i]) asrActive++;
		size_t s = g_Players[i].GetBufferSamples();
		if (s > 0) inflight++;
		totalBuf += s;
		if (s > maxSlot) maxSlot = s;
	}

	RvLog("[STAT] conn=%d/%d asr=%d inflight=%d bufSamples=%zu (%zu KB) maxSlot=%zu hi=%zu | utts=%lu flushes=%lu ipc=%lu errors=%lu",
		connected, maxclients, asrActive, inflight,
		totalBuf, (totalBuf * sizeof(int16_t)) / 1024,
		maxSlot, g_StatHighWater,
		g_StatUtterances, g_StatFlushes, g_StatIpc, g_StatErrors);
}

void Revoice_Update_Players(const char *pszNewValue)
{
	for (int i = 0; i < g_RehldsSvs->GetMaxClients(); i++) {
		auto plr = &g_Players[i];
		if (plr->IsConnected()) {
			plr->Update();
		}
	}
}

void Revoice_Update_Hltv(const char *pszNewValue)
{
	int maxclients = g_RehldsSvs->GetMaxClients();
	for (int i = 0; i < g_RehldsSvs->GetMaxClients(); i++) {
		auto plr = &g_Players[i];
		if (plr->IsConnected() && plr->IsHLTV()) {
			plr->Update();
		}
	}
}

CRevoicePlayer *GetPlayerByClientPtr(IGameClient *cl)
{
	return &g_Players[ cl->GetId() ];
}

CRevoicePlayer *GetPlayerByEdict(const edict_t *ed)
{
	int clientId = g_engfuncs.pfnIndexOfEdict(ed) - 1;

	if (clientId < 0 || clientId >= g_RehldsSvs->GetMaxClients()) {
		// We are about to call util_syserror which terminates the process.
		// Log the offending id first so the post-mortem actually shows what
		// hit us (engine logs the error too, but RV*.log has the slot
		// timeline alongside it).
		RvLog("[FATAL] GetPlayerByEdict bad clientId=%d maxclients=%d ed=%p",
			clientId, g_RehldsSvs ? g_RehldsSvs->GetMaxClients() : -1, (const void *)ed);
		util_syserror("Invalid player edict id=%d\n", clientId);
	}

	return &g_Players[ clientId ];
}

void CRevoicePlayer::SetLastVoiceTime(double time)
{
	UpdateVoiceRate(time - m_Client->GetLastVoiceTime());
	m_Client->SetLastVoiceTime(time);
}

void CRevoicePlayer::UpdateVoiceRate(double delta)
{
	if (m_VoiceRate)
	{
		switch (m_CodecType)
		{
		case vct_silk:
			m_VoiceRate -= int(delta * MAX_SILK_VOICE_RATE) + MAX_SILK_DATA_LEN;
			break;
		case vct_opus:
			m_VoiceRate -= int(delta * MAX_OPUS_VOICE_RATE) + MAX_OPUS_DATA_LEN;
			break;
		case vct_speex:
			m_VoiceRate -= int(delta * MAX_SPEEX_VOICE_RATE) + MAX_SPEEX_DATA_LEN;
			break;
		default:
			break;
		}

		if (m_VoiceRate < 0)
			m_VoiceRate = 0;
	}
}

const char *CRevoicePlayer::GetCodecTypeToString()
{
	return m_szCodecType[ m_CodecType ];
}

void CRevoicePlayer::IncreaseVoiceRate(int dataLength)
{
	m_VoiceRate += dataLength;
}

CodecType CRevoicePlayer::GetCodecTypeByString(const char *codec)
{
#define REV_CODEC(know_codec)\
	if (_stricmp(codec, #know_codec) == 0) {\
		return vct_##know_codec;\
	}\

	REV_CODEC(opus);
	REV_CODEC(silk);
	REV_CODEC(speex);
#undef REV_CODEC

	return vct_none;
}

// ---------------------------------------------------------------------------
// Voice FX (per-player volume / pitch)
//
// None of the functions below run for a player at default settings: the voice
// path gates on HasActiveVoiceFx() before calling any of them. See the
// isolation note in revoice_player.h.
// ---------------------------------------------------------------------------

void CRevoicePlayer::SetVoiceVolume(float volume)
{
	if (volume != volume) volume = 1.0f; // NaN -> neutral
	if (volume < 0.0f)    volume = 0.0f;
	if (volume > 10.0f)   volume = 10.0f;
	m_VoiceVolume = volume;
	ResetVoiceFxStream();
}

void CRevoicePlayer::SetVoicePitch(float pitch)
{
	// Quality clamp only — the shifter's ring reads are masked+clamped and
	// stay in bounds for any value (see revoice_pitchshift.h). Keep in sync
	// with the clamps inside CVoicePitchShifter::Process.
	if (pitch != pitch) pitch = 1.0f; // NaN -> neutral
	if (pitch < 0.5f)   pitch = 0.5f;
	if (pitch > 2.0f)   pitch = 2.0f;
	m_VoicePitch = pitch;
	ResetVoiceFxStream();
}

void CRevoicePlayer::ResetVoiceFxStream()
{
	m_FxStreamContinuous = false;
	m_PitchShifter.Reset();
}

// Full fx wipe: settings back to neutral, shifter/continuity dropped, and the
// fx encoder streams reset immediately (they would be reset lazily on the next
// fx packet anyway, but a boundary should leave nothing carried at all).
// Called on connect, disconnect and — because Metamod does NOT deliver
// disconnect/connect for players staying through a changelevel — explicitly
// for every slot at the map boundary (Revoice_ClearAllVoiceFx).
void CRevoicePlayer::ClearVoiceFx(const char *reason)
{
	if (HasActiveVoiceFx()) {
		RvLogAction("[FX] cleared on %s slot=%d (was vol=%.3f pitch=%.3f)",
			reason ? reason : "(unknown)",
			m_Client ? m_Client->GetId() : -1,
			m_VoiceVolume, m_VoicePitch);
	}
	m_VoiceVolume = 1.0f;
	m_VoicePitch = 1.0f;
	ResetVoiceFxStream();
	if (m_FxSilkCodec)  m_FxSilkCodec->ResetState();
	if (m_FxOpusCodec)  m_FxOpusCodec->ResetState();
	if (m_FxSpeexCodec) m_FxSpeexCodec->ResetState();
}

// Lazy, once-per-slot allocation of the dedicated fx encoders. Called only
// from the fx-active packet path. On any Init failure the half-built codecs
// are released and fx is permanently latched off for this slot (the caller
// also resets volume/pitch to neutral, so the gate goes back to the legacy
// path from the next packet on).
bool CRevoicePlayer::EnsureFxCodecs()
{
	if (m_FxSilkCodec && m_FxOpusCodec && m_FxSpeexCodec)
		return true;
	if (m_FxCodecsFailed)
		return false;

	m_FxSpeexCodec = new VoiceCodec_Frame(new VoiceEncoder_Speex());
	m_FxSilkCodec  = new CSteamP2PCodec(new VoiceEncoder_Silk());
	m_FxOpusCodec  = new CSteamP2PCodec(new VoiceEncoder_Opus());

	bool ok = m_FxSpeexCodec->Init(SPEEX_VOICE_QUALITY);
	ok = m_FxSilkCodec->Init(SILK_VOICE_QUALITY) && ok;
	ok = m_FxOpusCodec->Init(OPUS_VOICE_QUALITY) && ok;

	if (!ok) {
		// Release() deletes the wrapper and its backend; a codec whose Init
		// failed must never see Compress().
		m_FxSpeexCodec->Release(); m_FxSpeexCodec = nullptr;
		m_FxSilkCodec->Release();  m_FxSilkCodec  = nullptr;
		m_FxOpusCodec->Release();  m_FxOpusCodec  = nullptr;
		m_FxCodecsFailed = true;
		RvLogAction("[FX] codec init FAILED slot=%d — fx latched off for this slot",
			m_Client ? m_Client->GetId() : -1);
		return false;
	}

	RvLogAction("[FX] codecs allocated slot=%d", m_Client ? m_Client->GetId() : -1);
	return true;
}

// In-place effect chain on 8 kHz mono PCM16: pitch first, then gain. The
// shifter is only touched when pitch != 1, so a volume-only setup provably
// never executes any pitch-shifter code.
void CRevoicePlayer::ApplyVoiceFx(short *pcm, int numSamples, bool freshStream)
{
	if (pcm == nullptr || numSamples <= 0)
		return;

	// New utterance (silence gap / map change) or settings changed since the
	// last fx packet: drop carried DSP+encoder state so nothing stale bleeds in.
	if (freshStream || !m_FxStreamContinuous) {
		m_PitchShifter.Reset();
		if (m_FxSilkCodec)  m_FxSilkCodec->ResetState();
		if (m_FxOpusCodec)  m_FxOpusCodec->ResetState();
		if (m_FxSpeexCodec) m_FxSpeexCodec->ResetState();
	}
	m_FxStreamContinuous = true;

	if (m_VoicePitch != 1.0f)
		m_PitchShifter.Process(pcm, numSamples, m_VoicePitch);

	float vol = m_VoiceVolume; // clamped finite by the setter
	if (vol != 1.0f) {
		for (int i = 0; i < numSamples; i++) {
			float v = (float)pcm[i] * vol;
			if (v >= 32767.0f)       v = 32767.0f;
			else if (v <= -32768.0f) v = -32768.0f;
			pcm[i] = (short)v;
		}
	}
}
