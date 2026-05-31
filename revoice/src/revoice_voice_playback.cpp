#include "precompiled.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

CVoicePlayback g_VoicePlayback;

static inline bool REV_PlaybackDebugEnabled()
{
	return (g_pcv_rev_playback_debug && g_pcv_rev_playback_debug->value != 0.0f);
}

static inline bool REV_PlaybackDebugVerbose()
{
	// 0 = off, 1 = stats-only, 2+ = verbose per-frame details
	return (g_pcv_rev_playback_debug && g_pcv_rev_playback_debug->value >= 2.0f);
}

static inline void REV_PlaybackDebugPrint(const char* msg)
{
	if (REV_PlaybackDebugEnabled() && REV_PlaybackDebugVerbose())
		SERVER_PRINT(msg);
}

static inline void REV_PlaybackDebugPrintf(const char* fmt, ...)
{
	if (!REV_PlaybackDebugEnabled() || !REV_PlaybackDebugVerbose())
		return;

	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	SERVER_PRINT(buf);
}

// If a client's datagram is temporarily full, dropping voice frames causes audible fades/silence.
// This queue lets us retry sending on subsequent ticks (small jitter buffer on server side).
static const int REV_VOICE_QUEUE_MAX_PAYLOAD = 4096;
static const int REV_VOICE_QUEUE_CAP_FRAMES = 64; // ~1.28s at 20ms frames

// Largest frame we ever emit: 100 ms at 8 kHz. frame_ms is clamped to <=100 in Update(),
// so FRAME_SAMPLES_8K can never exceed this. Used to size the silence scratch buffers.
static const int REV_MAX_FRAME_SAMPLES_8K = 800;

struct REV_QueuedVoiceFrame {
	int player0; // 0-based source player id written into svc_voicedata
	int len;
	char data[REV_VOICE_QUEUE_MAX_PAYLOAD];
};

struct REV_ClientVoiceQueue {
	int head;
	int tail;
	int count;
	REV_QueuedVoiceFrame frames[REV_VOICE_QUEUE_CAP_FRAMES];
};

static REV_ClientVoiceQueue g_PlaybackVoiceQueues[MAX_PLAYERS];

// Playback queue stats (aggregated, printed by Update() once/sec when REV_PlaybackDebug=1)
static int g_dbgQ_enqueued = 0;
static int g_dbgQ_dropped = 0;
static int g_dbgQ_flushed = 0;
static int g_dbgQ_blocked = 0;
static int g_dbgQ_maxDepth = 0;

static void REV_ResetPlaybackVoiceQueues()
{
	for (int i = 0; i < MAX_PLAYERS; i++) {
		g_PlaybackVoiceQueues[i].head = 0;
		g_PlaybackVoiceQueues[i].tail = 0;
		g_PlaybackVoiceQueues[i].count = 0;
	}
	g_dbgQ_enqueued = 0;
	g_dbgQ_dropped = 0;
	g_dbgQ_flushed = 0;
	g_dbgQ_blocked = 0;
	g_dbgQ_maxDepth = 0;
}

static bool REV_TryWriteVoicePacket(IGameClient* dstClient, int player0, const char* payload, int payloadLen)
{
	if (!dstClient || !dstClient->IsActive() || !payload || payloadLen <= 0)
		return false;

	sizebuf_t* dstDatagram = dstClient->GetDatagram();
	if (!dstDatagram || !dstDatagram->data || dstDatagram->maxsize <= 0)
		return false;

	// 1 byte cmd + 1 byte player + 2 bytes size + payload + a little slack like original code
	const int needed = 6 + payloadLen;
	if (dstDatagram->cursize + needed >= dstDatagram->maxsize)
		return false;

	g_RehldsFuncs->MSG_WriteByte(dstDatagram, svc_voicedata); // 53
	g_RehldsFuncs->MSG_WriteByte(dstDatagram, player0);
	g_RehldsFuncs->MSG_WriteShort(dstDatagram, payloadLen);
	g_RehldsFuncs->MSG_WriteBuf(dstDatagram, payloadLen, (void*)payload);
	return true;
}

static void REV_QueueVoiceFrame(int clientIdx, int player0, const char* payload, int payloadLen)
{
	if (clientIdx < 0 || clientIdx >= MAX_PLAYERS || !payload || payloadLen <= 0)
		return;

	if (payloadLen > REV_VOICE_QUEUE_MAX_PAYLOAD)
		return;

	REV_ClientVoiceQueue& q = g_PlaybackVoiceQueues[clientIdx];

	// If full, drop oldest (better to keep latest audio than lag forever)
	if (q.count >= REV_VOICE_QUEUE_CAP_FRAMES) {
		q.head = (q.head + 1) % REV_VOICE_QUEUE_CAP_FRAMES;
		q.count--;
		g_dbgQ_dropped++;
	}

	REV_QueuedVoiceFrame& f = q.frames[q.tail];
	f.player0 = player0;
	f.len = payloadLen;
	memcpy(f.data, payload, (size_t)payloadLen);

	q.tail = (q.tail + 1) % REV_VOICE_QUEUE_CAP_FRAMES;
	q.count++;
	g_dbgQ_enqueued++;
	if (q.count > g_dbgQ_maxDepth) g_dbgQ_maxDepth = q.count;
}

static void REV_FlushQueuedVoiceFrames(int clientIdx, IGameClient* dstClient)
{
	if (clientIdx < 0 || clientIdx >= MAX_PLAYERS || !dstClient || !dstClient->IsActive())
		return;

	REV_ClientVoiceQueue& q = g_PlaybackVoiceQueues[clientIdx];
	while (q.count > 0) {
		REV_QueuedVoiceFrame& f = q.frames[q.head];
		if (!REV_TryWriteVoicePacket(dstClient, f.player0, f.data, f.len))
		{
			g_dbgQ_blocked++;
			return; // still no room
		}

		q.head = (q.head + 1) % REV_VOICE_QUEUE_CAP_FRAMES;
		q.count--;
		g_dbgQ_flushed++;
	}
}

// Simple resampling and channel conversion
static int ConvertToMonoResample(const short* input, int inputSamples, int inputRate, int inputChannels,
	short* output, int maxOutputSamples, int targetRate)
{
	if (!input || !output || inputSamples <= 0 || inputChannels <= 0 || inputRate <= 0 || targetRate <= 0)
		return 0;

	const int inFrames = inputSamples / inputChannels;
	int outputSamples = (inFrames * targetRate) / inputRate;
	if (outputSamples > maxOutputSamples)
		outputSamples = maxOutputSamples;

	// Linear interpolation resampling + stereo->mono averaging.
	// This avoids the harsh artifacts / pitch weirdness from nearest-neighbor.
	for (int i = 0; i < outputSamples; i++) {
		float srcPos = (float)i * (float)inputRate / (float)targetRate;
		int srcFrame0 = (int)srcPos;
		int srcFrame1 = srcFrame0 + 1;
		float frac = srcPos - (float)srcFrame0;

		if (srcFrame0 >= inFrames)
			break;
		if (srcFrame1 >= inFrames)
			srcFrame1 = inFrames - 1;

		int sum0 = 0;
		int sum1 = 0;
		for (int ch = 0; ch < inputChannels; ch++) {
			int idx0 = srcFrame0 * inputChannels + ch;
			int idx1 = srcFrame1 * inputChannels + ch;
			if (idx0 < inputSamples) sum0 += input[idx0];
			if (idx1 < inputSamples) sum1 += input[idx1];
		}

		float mono0 = (float)sum0 / (float)inputChannels;
		float mono1 = (float)sum1 / (float)inputChannels;
		float out = mono0 + (mono1 - mono0) * frac;

		// clamp
		if (out > 32767.0f) out = 32767.0f;
		if (out < -32768.0f) out = -32768.0f;
		output[i] = (short)out;
	}

	return outputSamples;
}

// Very small one-pole low-pass to tame >4 kHz content when running at 8 kHz.
// cutoffHz defaults effectively to ~3.8 kHz; state is kept across frames.
static void ApplyLowpass8k(short* samples, int count, float cutoffHz, float sampleRate, float& z)
{
	if (!samples || count <= 0 || sampleRate <= 0.0f || cutoffHz <= 0.0f)
		return;
	float dt = 1.0f / sampleRate;
	float rc = 1.0f / (2.0f * 3.14159265f * cutoffHz);
	float alpha = dt / (rc + dt);
	for (int i = 0; i < count; i++) {
		float x = (float)samples[i];
		z = z + alpha * (x - z);
		float y = z;
		if (y > 32767.0f) y = 32767.0f;
		if (y < -32768.0f) y = -32768.0f;
		samples[i] = (short)y;
	}
}

// Broadcast voice data to all clients.
// IMPORTANT:
// The original ReVoice transcode path feeds *8 kHz PCM16* into all encoders (Speex, Silk, Opus),
// then wraps Silk/Opus into SteamP2P on-wire format where needed.
// So for playback we must also feed 8 kHz PCM16 to every encoder (do NOT upsample to 16 kHz).
static void BroadcastVoiceData(const short* pcm8k, int numSamples8k, int sourcePlayerIndex, bool bFinal, const bool* targetMask)
{
	char msg[256];
	
	if (!pcm8k || numSamples8k <= 0) {
		SERVER_PRINT("[ReVoice Playback] Invalid PCM data\n");
		return;
	}
	
	// Get the player we're emitting from
	if (sourcePlayerIndex < 1 || sourcePlayerIndex > gpGlobals->maxClients) {
		snprintf(msg, sizeof(msg), "[ReVoice Playback] Invalid player index: %d\n", sourcePlayerIndex);
		SERVER_PRINT(msg);
		return;
	}
	
	// Use playback-owned codec instances. Previously this function borrowed a live player's
	// codec, which caused encoder-state corruption (shared Opus overflow buffer / sequence
	// counters) whenever that player was speaking at the same time as playback.
	g_VoicePlayback.InitCodecs();

	if (REV_PlaybackDebugVerbose())
		REV_PlaybackDebugPrint("[ReVoice Playback] Broadcasting to all active clients (per-destination codec)\n");

	// Encode ONCE per codec per frame.
	// IMPORTANT: Opus encoder embeds sequence numbers; if we encode once per recipient,
	// each client will see huge seq gaps (looks like massive packet loss) and audio degrades badly
	// when player count increases. Broadcasting the same bytes keeps seq increments stable per frame.
	const char* opusBuf = nullptr;
	int opusLen = 0;
	const char* silkBuf = nullptr;
	int silkLen = 0;
	const char* speexBuf = nullptr;
	int speexLen = 0;

	static char opusOut[32768];
	static char silkOut[32768];
	static char speexOut[4096];

	if (pcm8k && numSamples8k > 0) {
		if (g_VoicePlayback.GetOpusCodec()) {
			int n = g_VoicePlayback.GetOpusCodec()->Compress((const char*)pcm8k, numSamples8k, opusOut, sizeof(opusOut), bFinal);
			if (n > 0) { opusBuf = opusOut; opusLen = n; }
		}
		if (g_VoicePlayback.GetSilkCodec()) {
			int n = g_VoicePlayback.GetSilkCodec()->Compress((const char*)pcm8k, numSamples8k, silkOut, sizeof(silkOut), bFinal);
			if (n > 0) { silkBuf = silkOut; silkLen = n; }
		}
		if (g_VoicePlayback.GetSpeexCodec()) {
			int n = g_VoicePlayback.GetSpeexCodec()->Compress((const char*)pcm8k, numSamples8k, speexOut, sizeof(speexOut), bFinal);
			if (n > 0) { speexBuf = speexOut; speexLen = n; }
		}
	}

	// Broadcast to all clients (bypass voice stream check for artificial playback)
	int maxclients = g_RehldsSvs->GetMaxClients();
	int sentCount = 0;
	for (int i = 0; i < maxclients; i++) {
		IGameClient* dstClient = g_RehldsSvs->GetClient(i);
		
		// Skip if not connected or active
		if (!dstClient || !dstClient->IsActive())
			continue;
		if (targetMask && !targetMask[i])
			continue;

		// Determine destination codec type using ReVoice player tracking
		CRevoicePlayer* dstPlayer = &g_Players[i];

		const char* sendBuf = nullptr;
		int sendLen = 0;

		// Pick pre-encoded buffer for destination codec
		switch (dstPlayer->GetCodecType())
		{
		case vct_speex:
		{
			sendBuf = speexBuf;
			sendLen = speexLen;
			break;
		}
		case vct_silk:
		{
			sendBuf = silkBuf;
			sendLen = silkLen;
			break;
		}
		case vct_opus:
		{
			sendBuf = opusBuf;
			sendLen = opusLen;
			break;
		}
		default:
			// Unknown / none
			break;
		}

		if (!sendBuf || sendLen <= 0) {
			// No compatible encoding for this client; skip.
			continue;
		}

		// First flush any queued frames for this client
		REV_FlushQueuedVoiceFrames(i, dstClient);

		// If still queued (datagram likely full), enqueue this frame to preserve continuity
		if (g_PlaybackVoiceQueues[i].count > 0) {
			REV_QueueVoiceFrame(i, sourcePlayerIndex - 1, sendBuf, sendLen);
			continue;
		}

		// Try write current frame; if no space, enqueue and retry later
		if (!REV_TryWriteVoicePacket(dstClient, sourcePlayerIndex - 1, sendBuf, sendLen)) {
			REV_QueueVoiceFrame(i, sourcePlayerIndex - 1, sendBuf, sendLen);
			continue;
		}
		
		sentCount++;
	}
	
	if (REV_PlaybackDebugVerbose())
		REV_PlaybackDebugPrintf("[ReVoice Playback] Sent to %d clients\n", sentCount);
}

// Publish tri-state playback status for external tools (AMXX menu reads this cvar):
//   0 = stopped, 1 = playing, 2 = paused.
static void REV_SetPlaybackStatusCvar(float v)
{
	if (g_pcv_rev_playback_active)
		g_engfuncs.pfnCVarSetFloat(g_pcv_rev_playback_active->name, v);
}

CVoicePlayback::CVoicePlayback()
{
	m_State.file = nullptr;
	m_State.active = false;
	m_State.paused = false;
	m_State.dataPos = 0;
	m_State.pcm8kCarrySamples = 0;
	m_State.framesSinceReset = 0;
	m_State.resetGapFramesRemaining = 0;
	m_State.volume = 1.0f;
	for (int i = 0; i < MAX_PLAYERS; i++) m_State.targetMask[i] = true;
	m_State.lowpassState = 0.0f;
	m_OpusCodec = nullptr;
	m_SilkCodec = nullptr;
	m_SpeexCodec = nullptr;
	m_CodecsReady = false;
	REV_ResetPlaybackVoiceQueues();
}

CVoicePlayback::~CVoicePlayback()
{
	StopPlayback();
	DeInitCodecs();
}

void CVoicePlayback::InitCodecs()
{
	if (m_CodecsReady)
		return;

	// Build each codec independently and verify it initialized. A codec whose Init()
	// fails (or that fails to allocate) is released and left null. BroadcastVoiceData
	// only touches non-null codecs, so a partial failure degrades that one codec
	// gracefully instead of crashing later in Compress — the Opus/Silk backends do
	// NOT null-check their encoder handle, so a half-initialized codec would segfault.
	m_SpeexCodec = new VoiceCodec_Frame(new VoiceEncoder_Speex());
	if (m_SpeexCodec && !m_SpeexCodec->Init(SPEEX_VOICE_QUALITY)) {
		m_SpeexCodec->Release();
		m_SpeexCodec = nullptr;
	}

	m_SilkCodec = new CSteamP2PCodec(new VoiceEncoder_Silk());
	if (m_SilkCodec && !m_SilkCodec->Init(SILK_VOICE_QUALITY)) {
		m_SilkCodec->Release();
		m_SilkCodec = nullptr;
	}

	m_OpusCodec = new CSteamP2PCodec(new VoiceEncoder_Opus());
	if (m_OpusCodec && !m_OpusCodec->Init(OPUS_VOICE_QUALITY)) {
		m_OpusCodec->Release();
		m_OpusCodec = nullptr;
	}

	// Mark ready even on partial failure so we don't re-attempt (and re-allocate)
	// every frame; the per-codec null checks handle any missing codec.
	m_CodecsReady = true;
}

// Release the playback-owned codecs. Release() frees the backend encoder and then
// the wrapper itself, so it must not be paired with an extra delete.
void CVoicePlayback::DeInitCodecs()
{
	if (m_OpusCodec)  { m_OpusCodec->Release();  m_OpusCodec  = nullptr; }
	if (m_SilkCodec)  { m_SilkCodec->Release();  m_SilkCodec  = nullptr; }
	if (m_SpeexCodec) { m_SpeexCodec->Release(); m_SpeexCodec = nullptr; }
	m_CodecsReady = false;
}

bool CVoicePlayback::ReadWavHeader(FILE* file, int& sampleRate, int& channels, int& bitsPerSample, unsigned int& dataSize)
{
	if (!file) return false;
	
	// Read RIFF header
	char riff[4];
	if (fread(riff, 1, 4, file) != 4 || memcmp(riff, "RIFF", 4) != 0)
		return false;
	
	fseek(file, 4, SEEK_CUR); // Skip file size
	
	// Read WAVE
	char wave[4];
	if (fread(wave, 1, 4, file) != 4 || memcmp(wave, "WAVE", 4) != 0)
		return false;
	
	// Find fmt chunk
	char chunk[4];
	unsigned int chunkSize;
	bool fmtFound = false;
	
	while (fread(chunk, 1, 4, file) == 4) {
		if (fread(&chunkSize, 4, 1, file) != 1)
			return false;
		
		if (memcmp(chunk, "fmt ", 4) == 0) {
			unsigned short audioFormat;
			if (fread(&audioFormat, 2, 1, file) != 1)
				return false;
			
			if (audioFormat != 1) // Must be PCM
				return false;
			
			unsigned short numChannels;
			unsigned int sRate;
			unsigned short bps;
			
			if (fread(&numChannels, 2, 1, file) != 1)
				return false;
			if (fread(&sRate, 4, 1, file) != 1)
				return false;
			fseek(file, 6, SEEK_CUR); // Skip byte rate and block align
			if (fread(&bps, 2, 1, file) != 1)
				return false;
			
			channels = numChannels;
			sampleRate = sRate;
			bitsPerSample = bps;
			fmtFound = true;
			
			// Skip rest of chunk. Guard against a malformed fmt chunk smaller than the
			// 16 bytes we just consumed (chunkSize is unsigned: chunkSize-16 would underflow).
			if (chunkSize > 16)
				fseek(file, chunkSize - 16, SEEK_CUR);
		}
		else if (memcmp(chunk, "data", 4) == 0) {
			dataSize = chunkSize;
			if (fmtFound)
				return true; // data chunk position is current file position
			else
				return false;
		}
		else {
			// Skip unknown chunk
			fseek(file, chunkSize, SEEK_CUR);
		}
	}
	
	return false;
}

bool CVoicePlayback::StartPlayback(const char* filename, int playerIndex, float volume, const bool* targetMask)
{
	char msg[512];
	snprintf(msg, sizeof(msg), "[ReVoice Playback] StartPlayback called: file=%s, player=%d\n", filename, playerIndex);
	REV_PlaybackDebugPrint(msg);
	
	// Stop any current playback
	StopPlayback();
	
	// Build full path:
	// - If filename starts with '/', treat as absolute.
	// - Else if it contains '/', prepend "cstrike/" so "sound/custom/foo.wav" works.
	// - Else fall back to radio folder like old behavior.
	char fullPath[512];
	if (filename[0] == '/') {
		snprintf(fullPath, sizeof(fullPath), "%s", filename);
	} else if (strchr(filename, '/')) {
		snprintf(fullPath, sizeof(fullPath), "cstrike/%s", filename);
	} else {
		snprintf(fullPath, sizeof(fullPath), "cstrike/sound/radio/%s", filename);
	}
	
	snprintf(msg, sizeof(msg), "[ReVoice Playback] Opening file: %s\n", fullPath);
	REV_PlaybackDebugPrint(msg);
	
	// Open file
	FILE* file = fopen(fullPath, "rb");
	if (!file) {
		snprintf(msg, sizeof(msg), "[ReVoice Playback] ERROR: Failed to open audio file: %s\n", fullPath);
		SERVER_PRINT(msg);
		return false;
	}
	
	REV_PlaybackDebugPrint("[ReVoice Playback] File opened successfully\n");
	
	// Read WAV header
	int sampleRate, channels, bitsPerSample;
	unsigned int dataSize;
	
	if (!ReadWavHeader(file, sampleRate, channels, bitsPerSample, dataSize)) {
		snprintf(msg, sizeof(msg), "[ReVoice Playback] ERROR: Invalid WAV file: %s\n", fullPath);
		SERVER_PRINT(msg);
		fclose(file);
		return false;
	}
	
	snprintf(msg, sizeof(msg), "[ReVoice Playback] WAV header parsed: %dHz, %d channels, %d-bit, %u bytes\n", 
		sampleRate, channels, bitsPerSample, dataSize);
	REV_PlaybackDebugPrint(msg);
	
	// Validate format (support 8-bit and 16-bit)
	if (bitsPerSample != 8 && bitsPerSample != 16) {
		snprintf(msg, sizeof(msg), "[ReVoice Playback] ERROR: WAV file must be 8-bit or 16-bit PCM: %s\n", fullPath);
		SERVER_PRINT(msg);
		fclose(file);
		return false;
	}

	// Validate channel count and sample rate. These header fields are attacker-controlled
	// for client-triggered playback; an out-of-range value would cause signed-overflow UB
	// in the chunk-size math (Update) or a divide path that never produces audio.
	if (channels < 1 || channels > 2) {
		snprintf(msg, sizeof(msg), "[ReVoice Playback] ERROR: Unsupported channel count (%d): %s\n", channels, fullPath);
		SERVER_PRINT(msg);
		fclose(file);
		return false;
	}
	if (sampleRate < 4000 || sampleRate > 48000) {
		snprintf(msg, sizeof(msg), "[ReVoice Playback] ERROR: Unsupported sample rate (%d): %s\n", sampleRate, fullPath);
		SERVER_PRINT(msg);
		fclose(file);
		return false;
	}

	// Clamp the header-declared data size to the bytes actually present in the file.
	// A lying/oversized 'data' size would otherwise make the EOF test (dataPos >= dataSize)
	// never trip, leaving the bot "mic" transmitting keepalive silence forever.
	{
		long dataStart = ftell(file);
		if (dataStart >= 0 && fseek(file, 0, SEEK_END) == 0) {
			long fileEnd = ftell(file);
			if (fileEnd > dataStart) {
				unsigned int avail = (unsigned int)(fileEnd - dataStart);
				// Clamp an oversized/lying size, and treat a declared-0 size
				// (some streamed WAVs) as "play to end of file".
				if (dataSize == 0 || dataSize > avail)
					dataSize = avail;
			} else {
				dataSize = 0;
			}
			fseek(file, dataStart, SEEK_SET); // restore to start of data
		}
	}

	if (dataSize == 0) {
		snprintf(msg, sizeof(msg), "[ReVoice Playback] ERROR: WAV has no audio data: %s\n", fullPath);
		SERVER_PRINT(msg);
		fclose(file);
		return false;
	}

	// Setup playback state
	m_State.file = file;
	m_State.sampleRate = sampleRate;
	m_State.channels = channels;
	m_State.bitsPerSample = bitsPerSample;
	m_State.dataSize = dataSize;
	m_State.dataPos = 0;
	m_State.dataStart = ftell(file); // file is positioned at start of 'data' payload here
	m_State.targetPlayerIndex = playerIndex;
	m_State.nextChunkTime = g_RehldsSv->GetTime();
	m_State.active = true;
	m_State.volume = volume;
	if (m_State.volume < 0.0f) m_State.volume = 0.0f;
	if (m_State.volume > 4.0f) m_State.volume = 4.0f;
	if (targetMask) {
		for (int i = 0; i < MAX_PLAYERS; i++) m_State.targetMask[i] = targetMask[i];
	} else {
		for (int i = 0; i < MAX_PLAYERS; i++) m_State.targetMask[i] = true;
	}
	m_State.pcm8kCarrySamples = 0;
	m_State.framesSinceReset = 0;
	m_State.resetGapFramesRemaining = 0;
	m_State.lowpassState = 0.0f;
	REV_ResetPlaybackVoiceQueues();
	
	m_State.paused = false;
	REV_SetPlaybackStatusCvar(1.0f); // playing

	snprintf(msg, sizeof(msg), "[ReVoice Playback] SUCCESS: Playback started for %s\n", filename);
	SERVER_PRINT(msg);

	return true;
}

void CVoicePlayback::StopPlayback()
{
	if (m_State.file) {
		fclose(m_State.file);
		m_State.file = nullptr;
	}
	m_State.active = false;
	m_State.paused = false;
	m_State.dataPos = 0;
	m_State.dataStart = 0;
	REV_SetPlaybackStatusCvar(0.0f); // stopped
	m_State.pcm8kCarrySamples = 0;
	m_State.framesSinceReset = 0;
	m_State.resetGapFramesRemaining = 0;
	m_State.volume = 1.0f;
	for (int i = 0; i < MAX_PLAYERS; i++) m_State.targetMask[i] = true;
	m_State.lowpassState = 0.0f;
	REV_ResetPlaybackVoiceQueues();
}

bool CVoicePlayback::SeekRelative(double seconds)
{
	// No-op if nothing is playing.
	if (!m_State.active || !m_State.file)
		return false;

	int bytesPerSample = (m_State.bitsPerSample / 8) * m_State.channels;
	if (bytesPerSample <= 0)
		return false; // validated at StartPlayback, but stay defensive

	long bytesPerSec = (long)m_State.sampleRate * bytesPerSample;
	if (bytesPerSec <= 0)
		return false;

	// Compute the target in double to avoid signed-overflow on extreme seek values
	// (e.g. an rcon 'sv_voiceseek 999999999'); dataPos/dataSize fit exactly in double.
	double newPosD = (double)m_State.dataPos + seconds * (double)bytesPerSec;
	if (newPosD < 0.0)
		newPosD = 0.0;                    // seeking before the start -> beginning

	// At/past the end -> let Update() emit the final frame and stop cleanly next tick.
	if (newPosD >= (double)m_State.dataSize) {
		m_State.dataPos = m_State.dataSize;
		m_State.pcm8kCarrySamples = 0;
		REV_ResetPlaybackVoiceQueues();
		return true;
	}

	long newPos = (long)newPosD;
	newPos -= (newPos % bytesPerSample);  // align to a whole sample frame

	if (m_State.dataStart < 0)
		return false; // ftell failed at open; cannot seek safely
	if (fseek(m_State.file, m_State.dataStart + newPos, SEEK_SET) != 0)
		return false;

	// Drop everything buffered so no stale (pre-seek) audio plays after the jump.
	m_State.dataPos = (unsigned int)newPos;
	m_State.pcm8kCarrySamples = 0;
	m_State.lowpassState = 0.0f;
	m_State.resetGapFramesRemaining = 0;
	REV_ResetPlaybackVoiceQueues();
	// Resync the send schedule to "now" so we don't burst catch-up frames after the jump.
	m_State.nextChunkTime = g_RehldsSv->GetTime();
	return true;
}

bool CVoicePlayback::Pause()
{
	// Only a live, non-paused playback can be paused.
	if (!m_State.active || !m_State.file || m_State.paused)
		return false;

	// Stop emitting but keep EVERYTHING (open file, dataPos, carry buffer, lowpass
	// state) so Resume() continues seamlessly. Update() early-returns while !active.
	m_State.active = false;
	m_State.paused = true;
	REV_SetPlaybackStatusCvar(2.0f); // paused
	return true;
}

bool CVoicePlayback::Resume()
{
	if (!m_State.paused || !m_State.file)
		return false;

	m_State.active = true;
	m_State.paused = false;
	// Wall-clock advanced while paused; resync so the catch-up loop doesn't flush a
	// burst of frames trying to "make up" the paused interval.
	m_State.nextChunkTime = g_RehldsSv->GetTime();
	REV_SetPlaybackStatusCvar(1.0f); // playing
	return true;
}

void CVoicePlayback::Update()
{
	if (!m_State.active || !m_State.file)
		return;

	// If the emitter slot is no longer an active client (e.g. the "bot" disconnected
	// mid-song), stop instead of attributing voice to an empty or reused slot.
	{
		int emitter0 = m_State.targetPlayerIndex - 1;
		if (emitter0 < 0 || emitter0 >= g_RehldsSvs->GetMaxClients()) {
			StopPlayback();
			return;
		}
		IGameClient* emitterClient = g_RehldsSvs->GetClient(emitter0);
		if (!emitterClient || !emitterClient->IsActive()) {
			StopPlayback();
			SERVER_PRINT("[ReVoice Playback] Emitter no longer active; stopping playback\n");
			return;
		}
	}

	double currentTime = g_RehldsSv->GetTime();

	// Read audio from source WAV, resample to 8kHz mono, then feed encoders in *exact* frame sizes.
	// IMPORTANT: We must avoid gaps in svc_voicedata; the client will fade out quickly when packets stop.
	// So we maintain a small 8kHz PCM jitter buffer and schedule sends strictly using nextChunkTime.
	int chunkDurationMs = 40; // default
	if (g_pcv_rev_playback_frame_ms && g_pcv_rev_playback_frame_ms->value > 0.0f) {
		chunkDurationMs = (int)g_pcv_rev_playback_frame_ms->value;
	}
	// Clamp to sane values (voice-friendly). Smaller -> more packets, bigger -> less bandwidth.
	if (chunkDurationMs < 20) chunkDurationMs = 20;
	if (chunkDurationMs > 100) chunkDurationMs = 100;
	// Snap to multiples of 20ms to keep Opus/Silk frame boundaries stable.
	chunkDurationMs = (chunkDurationMs / 20) * 20;
	if (chunkDurationMs <= 0) chunkDurationMs = 40;

	int frameSamples8k = (8000 * chunkDurationMs) / 1000;
	// Defensive: never exceed the silence-buffer capacity, even if the clamp above changes.
	if (frameSamples8k > REV_MAX_FRAME_SAMPLES_8K) frameSamples8k = REV_MAX_FRAME_SAMPLES_8K;
	const int FRAME_SAMPLES_8K = frameSamples8k;
	const double frameSec = (chunkDurationMs / 1000.0);
	const int carryCap = (int)(sizeof(m_State.pcm8kCarry) / sizeof(m_State.pcm8kCarry[0]));

	// Bot-playback speed (tape-style: pitch shifts with speed). Read live so menu
	// changes take effect mid-song. Clamped to the range the read buffers are sized for.
	float playbackSpeed = 1.0f;
	if (g_pcv_rev_playback_speed && g_pcv_rev_playback_speed->value > 0.0f)
		playbackSpeed = g_pcv_rev_playback_speed->value;
	if (playbackSpeed < 0.5f) playbackSpeed = 0.5f;
	if (playbackSpeed > 2.0f) playbackSpeed = 2.0f;

	// Debug counters (1 line/sec when REV_PlaybackDebug=1)
	static double s_dbgNextPrintTime = 0.0;
	static int s_dbgFramesSent = 0;
	static int s_dbgKeepaliveSent = 0;
	static int s_dbgUnderflowTicks = 0;
	static int s_dbgResets = 0;

	// Prefetch: keep a few frames buffered so we don't underflow if the send schedule catches up.
	const int targetBuffered = FRAME_SAMPLES_8K * 6; // ~240ms at 40ms frames
	int prefetchIters = 0;
	while (m_State.pcm8kCarrySamples < targetBuffered && m_State.pcm8kCarrySamples + FRAME_SAMPLES_8K <= carryCap) {
		if (m_State.dataPos >= m_State.dataSize)
			break;
		if (++prefetchIters > 8)
			break;

		// Calculate how many source samples make up one output frame duration. At speed S
		// we consume S times as much source per real-time frame (then resample it down into
		// the same 8 kHz frame), which speeds playback up/down and shifts pitch like tape.
		int sourceSamples = (int)(((m_State.sampleRate * chunkDurationMs) / 1000) * playbackSpeed);
		if (sourceSamples < 1) sourceSamples = 1;
		int bytesPerSample = (m_State.bitsPerSample / 8) * m_State.channels;
		int chunkSize = sourceSamples * bytesPerSample;

		// Sized so a full frame at the worst case (100 ms, 48 kHz, stereo16, 2x) still fits.
		const int kMaxRead = 49152;
		if (chunkSize > kMaxRead) {
			chunkSize = kMaxRead;
			sourceSamples = chunkSize / bytesPerSample;
		}
		if (m_State.dataPos + (unsigned int)chunkSize > m_State.dataSize) {
			chunkSize = (int)(m_State.dataSize - m_State.dataPos);
			sourceSamples = chunkSize / bytesPerSample;
		}
		if (chunkSize <= 0 || sourceSamples <= 0)
			break;

		// static: Update() runs once per server frame (single-threaded, non-reentrant),
		// so keeping these large scratch buffers off the per-frame stack is safe.
		static char audioBuffer[49152];
		size_t bytesRead = fread(audioBuffer, 1, (size_t)chunkSize, m_State.file);
		m_State.dataPos += (unsigned int)bytesRead;
		if (bytesRead == 0)
			break;

		// Convert to 16-bit PCM if needed
		static short pcm16Buffer[49152];
		int numSourceSamples = 0;
		if (m_State.bitsPerSample == 8) {
			numSourceSamples = (int)bytesRead;
			for (int i = 0; i < (int)bytesRead; i++) {
				unsigned char sample8 = (unsigned char)audioBuffer[i];
				pcm16Buffer[i] = (short)((sample8 - 128) * 256);
			}
		} else {
			numSourceSamples = (int)(bytesRead / 2);
			memcpy(pcm16Buffer, audioBuffer, bytesRead);
		}

		static short pcm8k[2048];
		// Resampling at (sampleRate * speed) maps the S-times-larger source chunk back into
		// one ~FRAME_SAMPLES_8K output frame, so output size stays bounded regardless of speed.
		int resampleInputRate = (int)(m_State.sampleRate * playbackSpeed);
		if (resampleInputRate < 1) resampleInputRate = 1;
		int numSamples8k = ConvertToMonoResample(
			pcm16Buffer, numSourceSamples, resampleInputRate, m_State.channels,
			pcm8k, (int)(sizeof(pcm8k) / sizeof(pcm8k[0])), 8000
		);
		// Low-pass to remove >4 kHz content now that we're at 8 kHz (Nyquist 4 kHz).
		if (numSamples8k > 0) {
			float cutoffHz = 3800.0f;
			if (g_pcv_rev_playback_lp_hz && g_pcv_rev_playback_lp_hz->value > 0.0f) {
				cutoffHz = g_pcv_rev_playback_lp_hz->value;
				if (cutoffHz > 3900.0f) cutoffHz = 3900.0f; // tiny headroom below Nyquist
				if (cutoffHz < 1000.0f) cutoffHz = 1000.0f; // keep it reasonable
			}
			ApplyLowpass8k(pcm8k, numSamples8k, cutoffHz, 8000.0f, m_State.lowpassState);
			// Apply volume scaling. Effective volume = the StartPlayback arg (m_State.volume,
			// 1.0 for the menu path / sv_playvoice) times the live REV_PlaybackVolume cvar.
			// Read live so menu changes take effect mid-song. Bot path only.
			float vol = m_State.volume;
			if (g_pcv_rev_playback_volume)
				vol *= g_pcv_rev_playback_volume->value;
			if (vol < 0.0f) vol = 0.0f;
			if (vol > 4.0f) vol = 4.0f;
			if (vol != 1.0f) {
				for (int i = 0; i < numSamples8k; i++) {
					float v = (float)pcm8k[i] * vol;
					if (v > 32767.0f) v = 32767.0f;
					if (v < -32768.0f) v = -32768.0f;
					pcm8k[i] = (short)v;
				}
			}
		}

		if (numSamples8k > 0) {
			int canCopy = carryCap - m_State.pcm8kCarrySamples;
			if (canCopy > numSamples8k) canCopy = numSamples8k;
			if (canCopy > 0) {
				memcpy(&m_State.pcm8kCarry[m_State.pcm8kCarrySamples], pcm8k, canCopy * sizeof(short));
				m_State.pcm8kCarrySamples += canCopy;
			}
		}
	}

	// Not time to send yet (but we may have prefetched data above).
	if (currentTime < m_State.nextChunkTime)
		return;

	// Send frames on schedule; if the server hitches, catch up (bounded).
	int catchupIters = 0;
	while (currentTime >= m_State.nextChunkTime) {
		if (++catchupIters > 25)
			break;

		if (m_State.pcm8kCarrySamples < FRAME_SAMPLES_8K) {
			// Keepalive silence: if we're due to send but we don't have audio yet, do NOT stop sending
			// voicedata entirely. The client fades out quickly when packets stop; sending silence frames
			// (non-final) keeps the "mic" alive until we refill the PCM buffer.
			if (m_State.dataPos < m_State.dataSize) {
				short silence[REV_MAX_FRAME_SAMPLES_8K] = {0};
				BroadcastVoiceData(silence, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, false, m_State.targetMask);
				s_dbgKeepaliveSent++;
				s_dbgUnderflowTicks++;
				m_State.nextChunkTime += frameSec;
				continue;
			}
			break; // true EOF (handled below once buffer drains)
		}

		// Optional "hard reset gap": simulate releasing mic by dropping/muting frames
		// (no voicedata packets sent at all) while still advancing time.
		if (m_State.resetGapFramesRemaining > 0) {
			memmove(m_State.pcm8kCarry, &m_State.pcm8kCarry[FRAME_SAMPLES_8K],
				(size_t)(m_State.pcm8kCarrySamples - FRAME_SAMPLES_8K) * sizeof(short));
			m_State.pcm8kCarrySamples -= FRAME_SAMPLES_8K;
			m_State.resetGapFramesRemaining--;
			m_State.nextChunkTime += frameSec;
			continue;
		}

		int resetEvery = 0;
		if (g_pcv_rev_playback_reset_frames && g_pcv_rev_playback_reset_frames->value > 0.0f) {
			resetEvery = (int)g_pcv_rev_playback_reset_frames->value;
		}
		if (resetEvery > 0 && m_State.framesSinceReset >= resetEvery) {
			int resetGapMs = 0;
			if (g_pcv_rev_playback_reset_gap_ms && g_pcv_rev_playback_reset_gap_ms->value > 0.0f) {
				resetGapMs = (int)g_pcv_rev_playback_reset_gap_ms->value;
			}
			if (resetGapMs < 0) resetGapMs = 0;

			int gapFrames = 0;
			if (resetGapMs > 0) {
				gapFrames = (resetGapMs + chunkDurationMs - 1) / chunkDurationMs;
				if (gapFrames < 1) gapFrames = 1;
			}

			short silence[REV_MAX_FRAME_SAMPLES_8K] = {0}; // supports up to 100ms at 8kHz
			BroadcastVoiceData(silence, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, true, m_State.targetMask);
			m_State.framesSinceReset = 0;
			m_State.resetGapFramesRemaining = gapFrames;
			s_dbgResets++;
			REV_ResetPlaybackVoiceQueues();

			// Consume one audio frame so the final silence occupies real playback time.
			memmove(m_State.pcm8kCarry, &m_State.pcm8kCarry[FRAME_SAMPLES_8K],
				(size_t)(m_State.pcm8kCarrySamples - FRAME_SAMPLES_8K) * sizeof(short));
			m_State.pcm8kCarrySamples -= FRAME_SAMPLES_8K;

			m_State.nextChunkTime += frameSec;
			continue;
		}

		BroadcastVoiceData(m_State.pcm8kCarry, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, false, m_State.targetMask);
		m_State.framesSinceReset++;
		s_dbgFramesSent++;
		memmove(m_State.pcm8kCarry, &m_State.pcm8kCarry[FRAME_SAMPLES_8K],
			(size_t)(m_State.pcm8kCarrySamples - FRAME_SAMPLES_8K) * sizeof(short));
		m_State.pcm8kCarrySamples -= FRAME_SAMPLES_8K;
		m_State.nextChunkTime += frameSec;
	}

	// If we reached true EOF and drained the buffer, send a final frame and stop.
	if (m_State.dataPos >= m_State.dataSize && m_State.pcm8kCarrySamples < FRAME_SAMPLES_8K) {
		if (m_State.pcm8kCarrySamples > 0) {
			for (int i = m_State.pcm8kCarrySamples; i < FRAME_SAMPLES_8K; i++)
				m_State.pcm8kCarry[i] = 0;
			BroadcastVoiceData(m_State.pcm8kCarry, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, true, m_State.targetMask);
		} else {
			short silence[REV_MAX_FRAME_SAMPLES_8K] = {0};
			BroadcastVoiceData(silence, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, true, m_State.targetMask);
		}
		StopPlayback();
		SERVER_PRINT("[ReVoice Playback] Playback finished\n");
		return;
	}

	// Periodic debug stats (once per second)
	if (REV_PlaybackDebugEnabled()) {
		if (s_dbgNextPrintTime <= 0.0)
			s_dbgNextPrintTime = currentTime + 1.0;
		if (currentTime >= s_dbgNextPrintTime) {
			char buf[256];
			snprintf(buf, sizeof(buf),
				"[ReVoice Playback] stats: frameMs=%d resetEvery=%d resets=%d framesSinceReset=%d keepGap=%d sent=%d keepalive=%d underflowTicks=%d q(enq=%d fl=%d blk=%d drop=%d max=%d) carry=%d next=%.3f now=%.3f\n",
				chunkDurationMs,
				(g_pcv_rev_playback_reset_frames ? (int)g_pcv_rev_playback_reset_frames->value : 0),
				s_dbgResets,
				m_State.framesSinceReset,
				m_State.resetGapFramesRemaining,
				s_dbgFramesSent, s_dbgKeepaliveSent, s_dbgUnderflowTicks,
				g_dbgQ_enqueued, g_dbgQ_flushed, g_dbgQ_blocked, g_dbgQ_dropped, g_dbgQ_maxDepth,
				m_State.pcm8kCarrySamples, m_State.nextChunkTime, currentTime);
			s_dbgFramesSent = 0;
			s_dbgKeepaliveSent = 0;
			s_dbgUnderflowTicks = 0;
			s_dbgResets = 0;
			g_dbgQ_enqueued = 0;
			g_dbgQ_flushed = 0;
			g_dbgQ_blocked = 0;
			g_dbgQ_dropped = 0;
			g_dbgQ_maxDepth = 0;
			s_dbgNextPrintTime = currentTime + 1.0;
		}
	}
}

// Find the emitter bot's player index (1-based) by name.
// Hard requirements:
//   - the client must be a server-side fake client (FL_FAKECLIENT); a real player
//     can never be selected, even if they set their name to match the bot;
//   - its name must contain the configured bot name.
// If no matching bot exists we deliberately DO NOT fall back to a human player:
// we log and return -1 so the caller aborts and nothing is emitted.
// The per-slot enumeration prints are gated behind REV_PlaybackDebug to avoid log spam.
static int FindVoiceEmitter()
{
	const char* botName = (g_pcv_rev_playback_bot_name && g_pcv_rev_playback_bot_name->string && g_pcv_rev_playback_bot_name->string[0])
		? g_pcv_rev_playback_bot_name->string
		: "vk.com/laguna_games";
	int maxclients = g_RehldsSvs->GetMaxClients();

	REV_PlaybackDebugPrint("[ReVoice] === Searching for emitter bot (using Rehlds API) ===\n");

	for (int i = 0; i < maxclients; i++) {
		IGameClient* client = g_RehldsSvs->GetClient(i);
		if (!client || !client->IsActive())
			continue;

		// Must be a bot (fake client). This is the authoritative check — names are spoofable.
		edict_t* ed = client->GetEdict();
		if (!ed || !(ed->v.flags & FL_FAKECLIENT))
			continue;

		const char* playerName = client->GetName();
		if (!playerName || playerName[0] == '\0')
			continue;

		REV_PlaybackDebugPrintf("[ReVoice] Bot candidate slot %d: '%s'\n", i + 1, playerName);

		if (strstr(playerName, botName) != nullptr) {
			int playerIndex = i + 1;
			REV_PlaybackDebugPrintf("[ReVoice] MATCH! Found bot '%s' at slot %d\n", playerName, playerIndex);
			return playerIndex;
		}
	}

	// No matching bot — do not borrow a real player; report and abort.
	char msg[256];
	snprintf(msg, sizeof(msg), "[ReVoice] No emitter bot matching name '%s' is connected; playback aborted\n", botName);
	SERVER_PRINT(msg);
	return -1;
}

void Revoice_VoicePlayback_Init()
{
	g_engfuncs.pfnAddServerCommand("sv_playvoice", Cmd_PlayVoice);
	g_engfuncs.pfnAddServerCommand("sv_playvoice_ex", Cmd_PlayVoice_Ex);
	g_engfuncs.pfnAddServerCommand("sv_stopvoice", Cmd_StopVoice);
	g_engfuncs.pfnAddServerCommand("sv_voiceseek", Cmd_VoiceSeek);
	g_engfuncs.pfnAddServerCommand("sv_pausevoice", Cmd_PauseVoice);
	g_engfuncs.pfnAddServerCommand("sv_resumevoice", Cmd_ResumeVoice);
	g_VoicePlayback.InitCodecs();
	SERVER_PRINT("[ReVoice] Voice playback commands registered: sv_playvoice, sv_playvoice_ex, sv_stopvoice, sv_voiceseek, sv_pausevoice, sv_resumevoice\n");
}

// Actor descriptor for the action log. The AMXX menu passes a quoted token
// "<steamid> (<ip>) <name>" as the trailing command argument; for rcon/console
// use (no such argument) we record "console".
static const char* REV_ActorArg(int idx)
{
	const char* a = CMD_ARGV(idx);
	return (a && a[0]) ? a : "console";
}

// Stop current playback. No-op (with a note) if nothing is playing/paused.
void Cmd_StopVoice()
{
	const char* actor = REV_ActorArg(1);

	if (!g_VoicePlayback.IsPlaying() && !g_VoicePlayback.IsPaused()) {
		SERVER_PRINT("[ReVoice] sv_stopvoice: nothing is playing\n");
		return;
	}
	g_VoicePlayback.StopPlayback();
	SERVER_PRINT("[ReVoice] sv_stopvoice: playback stopped\n");
	RvLogAction("%s stopped music", actor);
}

// Seek relative: sv_voiceseek <seconds> [actor] (e.g. 10 forward, -10 backward).
// Guarded: does nothing if not currently playing; clamps to [start, end].
void Cmd_VoiceSeek()
{
	const char* arg = CMD_ARGV(1);
	const char* actor = REV_ActorArg(2);

	if (!g_VoicePlayback.IsPlaying()) {
		SERVER_PRINT("[ReVoice] sv_voiceseek: nothing is playing\n");
		return;
	}

	if (!arg || !arg[0]) {
		SERVER_PRINT("Usage: sv_voiceseek <seconds>   (e.g. 10 or -10)\n");
		return;
	}

	double seconds = atof(arg);
	char msg[128];
	if (g_VoicePlayback.SeekRelative(seconds)) {
		snprintf(msg, sizeof(msg), "[ReVoice] sv_voiceseek: seeked %.1fs\n", seconds);
		RvLogAction("%s seeked music %.1fs", actor, seconds);
	} else {
		snprintf(msg, sizeof(msg), "[ReVoice] sv_voiceseek: failed\n");
		RvLogAction("%s FAILED to seek music %.1fs", actor, seconds);
	}
	SERVER_PRINT(msg);
}

// Pause current playback (keeps position for sv_resumevoice).
void Cmd_PauseVoice()
{
	const char* actor = REV_ActorArg(1);

	if (g_VoicePlayback.Pause()) {
		SERVER_PRINT("[ReVoice] sv_pausevoice: paused\n");
		RvLogAction("%s paused music", actor);
	} else {
		SERVER_PRINT("[ReVoice] sv_pausevoice: nothing to pause\n");
	}
}

// Resume a paused playback from where it left off.
void Cmd_ResumeVoice()
{
	const char* actor = REV_ActorArg(1);

	if (g_VoicePlayback.Resume()) {
		SERVER_PRINT("[ReVoice] sv_resumevoice: resumed\n");
		RvLogAction("%s resumed music", actor);
	} else {
		SERVER_PRINT("[ReVoice] sv_resumevoice: nothing to resume\n");
	}
}

// sv_playvoice <filename> [actor]
void Cmd_PlayVoice()
{
	SERVER_PRINT("[ReVoice] sv_playvoice command received\n");

	const char* filename = CMD_ARGV(1);
	const char* actor = REV_ActorArg(2);

	if (!filename || !filename[0]) {
		SERVER_PRINT("Usage: sv_playvoice <filename>\n");
		SERVER_PRINT("Example: sv_playvoice sound/custom/music1.wav\n");
		SERVER_PRINT("Path is relative to cstrike/ (absolute paths also allowed)\n");
		return;
	}

	char msg[256];
	snprintf(msg, sizeof(msg), "[ReVoice] Attempting to play: %s\n", filename);
	SERVER_PRINT(msg);

	// Find the best player to emit from (preferably the bot)
	int playerIndex = FindVoiceEmitter();

	if (playerIndex == -1) {
		SERVER_PRINT("[ReVoice] ERROR: No connected players to emit voice from\n");
		RvLogAction("%s FAILED to start music (no emitter bot): %s", actor, filename);
		return;
	}

	// Get and print the final player name
	if (playerIndex >= 1 && playerIndex <= gpGlobals->maxClients) {
		IGameClient* client = g_Players[playerIndex - 1].GetClient();
		const char* finalName = client ? client->GetName() : "Unknown";
		snprintf(msg, sizeof(msg), "[ReVoice] FINAL: Emitting voice from player '%s' (index %d)\n", finalName, playerIndex);
		SERVER_PRINT(msg);
	}

	if (!g_VoicePlayback.StartPlayback(filename, playerIndex, 1.0f, nullptr)) {
		SERVER_PRINT("[ReVoice] Failed to start playback\n");
		RvLogAction("%s FAILED to start music: %s", actor, filename);
	} else {
		RvLogAction("%s started music: %s", actor, filename);
	}
}

// Reject anything that could escape cstrike/: absolute paths, drive letters, UNC,
// and parent-directory traversal. Used only for the untrusted client-triggered path
// (the rcon/console sv_playvoice* commands are trusted and may use absolute paths).
static bool REV_IsSafeRelativePath(const char* path)
{
	if (!path || !path[0])
		return false;
	if (path[0] == '/' || path[0] == '\\') // unix/UNC absolute
		return false;
	if (path[1] == ':') // windows drive letter (e.g. C:\)
		return false;
	for (const char* p = path; *p; p++) {
		if (p[0] == '.' && p[1] == '.') // parent-dir traversal anywhere
			return false;
	}
	return true;
}

void Cmd_PlayVoice_Client(edict_t* pEntity)
{
	if (!pEntity)
		return;

	if (!g_pcv_rev_playvoice_client || g_pcv_rev_playvoice_client->value <= 0.0f) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] playvoice is disabled by server\n");
		return;
	}

	// Rate-limit client-triggered playback. Without this a client can spam the command
	// to churn fopen/fclose, restart playback, and flood the server console/log.
	// Store the last trigger time (not a deadline) so a sv.time reset at the map
	// boundary — which makes 'now' jump backwards — is treated as "cooldown expired"
	// instead of locking the command out for the length of the previous map.
	const double kClientPlayCooldownSec = 5.0;
	static double s_lastClientPlayTime = -1000.0;
	double now = g_RehldsSv->GetTime();
	if (now >= s_lastClientPlayTime && now < s_lastClientPlayTime + kClientPlayCooldownSec) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] Please wait before requesting playback again\n");
		return;
	}
	s_lastClientPlayTime = now; // 5s cooldown across all clients

	SERVER_PRINT("[ReVoice] playvoice command received from client\n");

	const char* filename = CMD_ARGV(1);

	if (!filename || !filename[0]) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Usage: playvoice <filename>\n");
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Example: playvoice sound/custom/music1.wav\n");
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Path is relative to cstrike/\n");
		return;
	}

	if (!REV_IsSafeRelativePath(filename)) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] Invalid path (must be relative, no '..')\n");
		return;
	}

	char msg[256];
	snprintf(msg, sizeof(msg), "[ReVoice] Client playback request: %s\n", filename);
	SERVER_PRINT(msg);
	
	// Find the best player to emit from (preferably the bot)
	int playerIndex = FindVoiceEmitter();
	
	if (playerIndex == -1) {
		SERVER_PRINT("[ReVoice] ERROR: No connected players to emit voice from\n");
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] No voice emitter found\n");
		return;
	}
	
	// Get and print the final player name
	if (playerIndex >= 1 && playerIndex <= gpGlobals->maxClients) {
		IGameClient* client = g_Players[playerIndex - 1].GetClient();
		const char* finalName = client ? client->GetName() : "Unknown";
		snprintf(msg, sizeof(msg), "[ReVoice] FINAL: Emitting voice from player '%s' (index %d)\n", finalName, playerIndex);
		SERVER_PRINT(msg);
	}
	
	if (!g_VoicePlayback.StartPlayback(filename, playerIndex, 1.0f, nullptr)) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] Failed to play audio\n");
	} else {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] Playing audio...\n");
	}
}

// Extended server command:
// sv_playvoice_ex <path> <volume> <targets...>
// - path: relative to cstrike/ (e.g. sound/custom/foo.wav) or absolute if starts with '/'
// - volume: 0.0 (mute) to 4.0 (boost). 1.0 = original.
// - targets: 0 = all clients; otherwise list of client indices (1-based). If omitted -> all.
void Cmd_PlayVoice_Ex()
{
	SERVER_PRINT("[ReVoice] sv_playvoice_ex command received\n");

	int argc = CMD_ARGC();
	if (argc < 3) {
		SERVER_PRINT("Usage: sv_playvoice_ex <path> <volume> [targets...]\n");
		SERVER_PRINT("Example: sv_playvoice_ex sound/custom/music1.wav 0.8 7 8 10 15\n");
		return;
	}

	const char* filename = CMD_ARGV(1);
	const char* volStr = CMD_ARGV(2);

	if (!filename || !filename[0]) {
		SERVER_PRINT("[ReVoice] ERROR: Filename required\n");
		return;
	}

	float volume = 1.0f;
	if (volStr && volStr[0]) {
		volume = (float)atof(volStr);
	}

	bool targetMask[MAX_PLAYERS];
	for (int i = 0; i < MAX_PLAYERS; i++) targetMask[i] = false;
	bool anyTargets = false;
	bool allTargets = false;

	if (argc <= 3) {
		allTargets = true;
	} else {
		for (int i = 3; i < argc; i++) {
			const char* arg = CMD_ARGV(i);
			if (!arg || !arg[0]) continue;
			int id = atoi(arg);
			if (id == 0) {
				allTargets = true;
				break;
			}
			if (id >= 1 && id <= gpGlobals->maxClients) {
				targetMask[id - 1] = true;
				anyTargets = true;
			}
		}
		if (!anyTargets && !allTargets) {
			// If nothing valid parsed, fall back to all
			allTargets = true;
		}
	}

	if (allTargets) {
		for (int i = 0; i < MAX_PLAYERS; i++) targetMask[i] = true;
	}

	char msg[256];
	snprintf(msg, sizeof(msg), "[ReVoice] Attempting to play: %s (vol=%.2f)\n", filename, volume);
	SERVER_PRINT(msg);

	int playerIndex = FindVoiceEmitter();

	if (playerIndex == -1) {
		SERVER_PRINT("[ReVoice] ERROR: No connected players to emit voice from\n");
		return;
	}

	if (playerIndex >= 1 && playerIndex <= gpGlobals->maxClients) {
		IGameClient* client = g_Players[playerIndex - 1].GetClient();
		const char* finalName = client ? client->GetName() : "Unknown";
		snprintf(msg, sizeof(msg), "[ReVoice] FINAL: Emitting voice from player '%s' (index %d)\n", finalName, playerIndex);
		SERVER_PRINT(msg);
	}

	if (!g_VoicePlayback.StartPlayback(filename, playerIndex, volume, allTargets ? nullptr : targetMask)) {
		SERVER_PRINT("[ReVoice] Failed to start playback\n");
	}
}

