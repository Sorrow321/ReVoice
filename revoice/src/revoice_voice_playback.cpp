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
	
	// Choose a "codec source" player with initialized codecs.
	// We use its codec objects for encoding (they're already Init()'d and SetClient()'d).
	int codecSrcSlot = -1;
	for (int i = 0; i < gpGlobals->maxClients; i++) {
		if (!g_Players[i].IsConnected())
			continue;
		IGameClient* cl = g_Players[i].GetClient();
		if (cl && cl->IsActive()) {
			codecSrcSlot = i + 1;
			break;
		}
	}
	if (codecSrcSlot == -1) {
		SERVER_PRINT("[ReVoice Playback] ERROR: No active player found to source codecs from\n");
		return;
	}

	CRevoicePlayer* codecSrc = &g_Players[codecSrcSlot - 1];

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

	static char opusOut[4096];
	static char silkOut[4096];
	static char speexOut[2048];

	if (pcm8k && numSamples8k > 0) {
		if (codecSrc->GetOpusCodec()) {
			int n = codecSrc->GetOpusCodec()->Compress((const char*)pcm8k, numSamples8k, opusOut, sizeof(opusOut), bFinal);
			if (n > 0) { opusBuf = opusOut; opusLen = n; }
		}
		if (codecSrc->GetSilkCodec()) {
			int n = codecSrc->GetSilkCodec()->Compress((const char*)pcm8k, numSamples8k, silkOut, sizeof(silkOut), bFinal);
			if (n > 0) { silkBuf = silkOut; silkLen = n; }
		}
		if (codecSrc->GetSpeexCodec()) {
			int n = codecSrc->GetSpeexCodec()->Compress((const char*)pcm8k, numSamples8k, speexOut, sizeof(speexOut), bFinal);
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

CVoicePlayback::CVoicePlayback()
{
	m_State.file = nullptr;
	m_State.active = false;
	m_State.dataPos = 0;
	m_State.pcm8kCarrySamples = 0;
	m_State.framesSinceReset = 0;
	m_State.resetGapFramesRemaining = 0;
	m_State.volume = 1.0f;
	for (int i = 0; i < MAX_PLAYERS; i++) m_State.targetMask[i] = true;
	m_State.lowpassState = 0.0f;
	REV_ResetPlaybackVoiceQueues();
}

CVoicePlayback::~CVoicePlayback()
{
	StopPlayback();
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
			
			// Skip rest of chunk
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
	
	// Setup playback state
	m_State.file = file;
	m_State.sampleRate = sampleRate;
	m_State.channels = channels;
	m_State.bitsPerSample = bitsPerSample;
	m_State.dataSize = dataSize;
	m_State.dataPos = 0;
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
	m_State.dataPos = 0;
	m_State.pcm8kCarrySamples = 0;
	m_State.framesSinceReset = 0;
	m_State.resetGapFramesRemaining = 0;
	m_State.volume = 1.0f;
	for (int i = 0; i < MAX_PLAYERS; i++) m_State.targetMask[i] = true;
	m_State.lowpassState = 0.0f;
	REV_ResetPlaybackVoiceQueues();
}

void CVoicePlayback::Update()
{
	if (!m_State.active || !m_State.file)
		return;
	
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

	const int FRAME_SAMPLES_8K = (8000 * chunkDurationMs) / 1000;
	const double frameSec = (chunkDurationMs / 1000.0);
	const int carryCap = (int)(sizeof(m_State.pcm8kCarry) / sizeof(m_State.pcm8kCarry[0]));

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

		// Calculate how many bytes to read from source for approximately one output frame duration.
		int sourceSamples = (m_State.sampleRate * chunkDurationMs) / 1000;
		int bytesPerSample = (m_State.bitsPerSample / 8) * m_State.channels;
		int chunkSize = sourceSamples * bytesPerSample;

		const int kMaxRead = 16384;
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

		char audioBuffer[16384];
		size_t bytesRead = fread(audioBuffer, 1, (size_t)chunkSize, m_State.file);
		m_State.dataPos += (unsigned int)bytesRead;
		if (bytesRead == 0)
			break;

		// Convert to 16-bit PCM if needed
		short pcm16Buffer[16384];
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

		short pcm8k[2048];
		int numSamples8k = ConvertToMonoResample(
			pcm16Buffer, numSourceSamples, m_State.sampleRate, m_State.channels,
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
			// Apply volume scaling
			if (m_State.volume != 1.0f) {
				for (int i = 0; i < numSamples8k; i++) {
					float v = (float)pcm8k[i] * m_State.volume;
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
				short silence[800] = {0};
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

			short silence[800] = {0}; // supports up to 100ms at 8kHz
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
			short silence[800] = {0};
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

// Helper function to find the best player index for voice emission
static int FindVoiceEmitter()
{
	char msg[256];
	int playerIndex = -1;
	const char* botName = (g_pcv_rev_playback_bot_name && g_pcv_rev_playback_bot_name->string && g_pcv_rev_playback_bot_name->string[0])
		? g_pcv_rev_playback_bot_name->string
		: "vk.com/laguna_games";
	int maxclients = g_RehldsSvs->GetMaxClients();
	
	SERVER_PRINT("[ReVoice] === Searching for voice emitter (using Rehlds API) ===\n");
	
	// Use Rehlds API to get all clients (including bots)
	for (int i = 0; i < maxclients; i++) {
		IGameClient* client = g_RehldsSvs->GetClient(i);
		if (client && client->IsActive()) {
			const char* playerName = client->GetName();
			if (playerName && playerName[0] != '\0') {
				snprintf(msg, sizeof(msg), "[ReVoice] Slot %d: '%s' (active=%d, spawned=%d)\n", 
					i + 1, playerName, client->IsActive() ? 1 : 0, client->IsSpawned() ? 1 : 0);
				SERVER_PRINT(msg);
				
				if (strstr(playerName, botName) != nullptr) {
					playerIndex = i + 1;
					snprintf(msg, sizeof(msg), "[ReVoice] MATCH! Found bot '%s' at slot %d\n", playerName, playerIndex);
					SERVER_PRINT(msg);
					return playerIndex;
				}
			}
		}
	}
	
	SERVER_PRINT("[ReVoice] === End of client list ===\n");
	
	// If bot not found, find any player that's not slot 1
	SERVER_PRINT("[ReVoice] Bot not found, searching for alternative player...\n");
	int firstPlayer = -1;
	
	for (int i = 0; i < maxclients; i++) {
		IGameClient* client = g_RehldsSvs->GetClient(i);
		if (client && client->IsActive()) {
			const char* playerName = client->GetName();
			if (!playerName || playerName[0] == '\0')
				playerName = "Unknown";
			
			if (firstPlayer == -1) {
				firstPlayer = i + 1;
				snprintf(msg, sizeof(msg), "[ReVoice] Found player '%s' at slot %d\n", playerName, i + 1);
				SERVER_PRINT(msg);
			}
			
			if (i != 0) { // Prefer not using slot 1
				playerIndex = i + 1;
				snprintf(msg, sizeof(msg), "[ReVoice] Using player '%s' at slot %d\n", playerName, playerIndex);
				SERVER_PRINT(msg);
				return playerIndex;
			}
		}
	}
	
	if (playerIndex == -1) {
		playerIndex = firstPlayer;
	}
	
	return playerIndex;
}

void Revoice_VoicePlayback_Init()
{
	g_engfuncs.pfnAddServerCommand("sv_playvoice", Cmd_PlayVoice);
	g_engfuncs.pfnAddServerCommand("sv_playvoice_ex", Cmd_PlayVoice_Ex);
	SERVER_PRINT("[ReVoice] Voice playback commands registered: sv_playvoice, playvoice\n");
}

void Cmd_PlayVoice()
{
	SERVER_PRINT("[ReVoice] sv_playvoice command received\n");
	
	const char* filename = CMD_ARGV(1);
	
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
	}
}

void Cmd_PlayVoice_Client(edict_t* pEntity)
{
	if (!pEntity)
		return;

	if (!g_pcv_rev_playvoice_client || g_pcv_rev_playvoice_client->value <= 0.0f) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] playvoice is disabled by server\n");
		return;
	}

	SERVER_PRINT("[ReVoice] playvoice command received from client\n");
	
	const char* filename = CMD_ARGV(1);
	
	if (!filename || !filename[0]) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Usage: playvoice <filename>\n");
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Example: playvoice sound/custom/music1.wav\n");
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Path is relative to cstrike/ (absolute paths allowed)\n");
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

