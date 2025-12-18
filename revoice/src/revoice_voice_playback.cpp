#include "precompiled.h"
#include <string.h>

CVoicePlayback g_VoicePlayback;

static inline bool REV_PlaybackDebugEnabled()
{
	return (g_pcv_rev_playback_debug && g_pcv_rev_playback_debug->value != 0.0f);
}

static inline void REV_PlaybackDebugPrint(const char* msg)
{
	if (REV_PlaybackDebugEnabled())
		SERVER_PRINT(msg);
}

static inline void REV_PlaybackDebugPrintf(const char* fmt, ...)
{
	if (!REV_PlaybackDebugEnabled())
		return;

	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	SERVER_PRINT(buf);
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

// Broadcast voice data to all clients.
// IMPORTANT:
// The original ReVoice transcode path feeds *8 kHz PCM16* into all encoders (Speex, Silk, Opus),
// then wraps Silk/Opus into SteamP2P on-wire format where needed.
// So for playback we must also feed 8 kHz PCM16 to every encoder (do NOT upsample to 16 kHz).
static void BroadcastVoiceData(const short* pcm8k, int numSamples8k, int sourcePlayerIndex, bool bFinal)
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
		
		// Get destination player's datagram
		sizebuf_t* dstDatagram = dstClient->GetDatagram();
		if (!dstDatagram)
			continue;
		
		// Check if datagram is properly initialized
		if (!dstDatagram->data || dstDatagram->maxsize <= 0)
			continue;
		
		// Check if there's enough space in the buffer
		int neededSize = 6 + sendLen; // Same as original: 6 bytes + data
		if (dstDatagram->cursize + neededSize >= dstDatagram->maxsize)
			continue;
		
		// Write voice data packet (matching original format exactly)
		g_RehldsFuncs->MSG_WriteByte(dstDatagram, svc_voicedata); // 53
		g_RehldsFuncs->MSG_WriteByte(dstDatagram, sourcePlayerIndex - 1); // Player ID (0-based)
		g_RehldsFuncs->MSG_WriteShort(dstDatagram, sendLen);
		g_RehldsFuncs->MSG_WriteBuf(dstDatagram, sendLen, (void*)sendBuf);
		
		if (sentCount == 0) {
			// Debug first packet only
			REV_PlaybackDebugPrintf("[ReVoice Playback] Packet: cmd=53, player=%d, size=%d, dstCodec=%s\n",
				sourcePlayerIndex - 1, sendLen, dstPlayer->GetCodecTypeToString());
		}
		
		sentCount++;
	}
	
	REV_PlaybackDebugPrintf("[ReVoice Playback] Sent to %d clients\n", sentCount);
}

CVoicePlayback::CVoicePlayback()
{
	m_State.file = nullptr;
	m_State.active = false;
	m_State.dataPos = 0;
	m_State.pcm8kCarrySamples = 0;
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

bool CVoicePlayback::StartPlayback(const char* filename, int playerIndex)
{
	char msg[512];
	snprintf(msg, sizeof(msg), "[ReVoice Playback] StartPlayback called: file=%s, player=%d\n", filename, playerIndex);
	REV_PlaybackDebugPrint(msg);
	
	// Stop any current playback
	StopPlayback();
	
	// Build full path: cstrike/sound/radio/filename.wav
	char fullPath[512];
	snprintf(fullPath, sizeof(fullPath), "cstrike/sound/radio/%s", filename);
	
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
	m_State.pcm8kCarrySamples = 0;
	
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
}

void CVoicePlayback::Update()
{
	if (!m_State.active || !m_State.file)
		return;
	
	double currentTime = g_RehldsSv->GetTime();
	
	// Check if it's time to send next chunk
	if (currentTime < m_State.nextChunkTime)
		return;
	
	// Read chunk of audio (20ms worth) from source WAV, resample to 8kHz mono, then feed encoders
	// in exact 20ms frames (160 samples @ 8kHz). Variable sample counts cause Opus/Silk to buffer
	// and can sound like "corruption" over time.
	// Real HL/CS voice is effectively streamed in small frames; large chunks cause "flicker"/chop.
	const int chunkDurationMs = 20;
	
	// Calculate how many samples to read from source
	int sourceSamples = (m_State.sampleRate * chunkDurationMs) / 1000;
	int bytesPerSample = (m_State.bitsPerSample / 8) * m_State.channels;
	int chunkSize = sourceSamples * bytesPerSample;
	
	// Safety: never read more than our stack buffer
	const int kMaxRead = 16384;
	if (chunkSize > kMaxRead) {
		chunkSize = kMaxRead;
		sourceSamples = chunkSize / bytesPerSample;
	}

	// Don't read more than remaining data
	if (m_State.dataPos + chunkSize > m_State.dataSize) {
		chunkSize = m_State.dataSize - m_State.dataPos;
		sourceSamples = chunkSize / bytesPerSample;
	}
	
	bool eof = false;
	if (chunkSize <= 0 || sourceSamples <= 0) {
		eof = true;
	}
	
	char audioBuffer[16384];
	size_t bytesRead = 0;
	if (!eof) {
		bytesRead = fread(audioBuffer, 1, (size_t)chunkSize, m_State.file);
		m_State.dataPos += (unsigned int)bytesRead;
		if (bytesRead == 0) {
			eof = true;
		}
	}
	
	// Convert to 16-bit if needed
	short pcm16Buffer[16384];
	int numSourceSamples;
	
	if (m_State.bitsPerSample == 8) {
		// Convert 8-bit unsigned to 16-bit signed
		numSourceSamples = (int)bytesRead;
		for (int i = 0; i < (int)bytesRead; i++) {
			unsigned char sample8 = (unsigned char)audioBuffer[i];
			pcm16Buffer[i] = (short)((sample8 - 128) * 256);
		}
	} else {
		// Already 16-bit
		numSourceSamples = (int)(bytesRead / 2);
		memcpy(pcm16Buffer, audioBuffer, bytesRead);
	}
	
	short pcm8k[2048];
	int numSamples8k = 0;
	if (!eof) {
		// Produce 8 kHz mono PCM16 (this is what all encoders in ReVoice expect).
		numSamples8k = ConvertToMonoResample(pcm16Buffer, numSourceSamples,
			m_State.sampleRate, m_State.channels, pcm8k, (int)(sizeof(pcm8k) / sizeof(pcm8k[0])), 8000);
	}
	
	// Append resampled samples into carry buffer
	if (numSamples8k > 0) {
		const int carryCap = (int)(sizeof(m_State.pcm8kCarry) / sizeof(m_State.pcm8kCarry[0]));
		int canCopy = carryCap - m_State.pcm8kCarrySamples;
		if (canCopy > numSamples8k) canCopy = numSamples8k;
		if (canCopy > 0) {
			memcpy(&m_State.pcm8kCarry[m_State.pcm8kCarrySamples], pcm8k, canCopy * sizeof(short));
			m_State.pcm8kCarrySamples += canCopy;
		}
	}

	// Send exact 20ms frames (160 samples @ 8kHz)
	const int FRAME_SAMPLES_8K = 160;
	while (m_State.pcm8kCarrySamples >= FRAME_SAMPLES_8K) {
		BroadcastVoiceData(m_State.pcm8kCarry, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, false);
		memmove(m_State.pcm8kCarry, &m_State.pcm8kCarry[FRAME_SAMPLES_8K],
			(size_t)(m_State.pcm8kCarrySamples - FRAME_SAMPLES_8K) * sizeof(short));
		m_State.pcm8kCarrySamples -= FRAME_SAMPLES_8K;
	}

	// If end-of-file reached, flush one final padded frame so decoders reset cleanly.
	if (eof) {
		if (m_State.pcm8kCarrySamples > 0) {
			// pad with silence
			for (int i = m_State.pcm8kCarrySamples; i < FRAME_SAMPLES_8K; i++)
				m_State.pcm8kCarry[i] = 0;
			BroadcastVoiceData(m_State.pcm8kCarry, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, true);
		} else {
			// send an empty final (silence) frame to force reset/terminator for codecs that need it
			short silence[FRAME_SAMPLES_8K] = {0};
			BroadcastVoiceData(silence, FRAME_SAMPLES_8K, m_State.targetPlayerIndex, true);
		}

		StopPlayback();
		SERVER_PRINT("[ReVoice Playback] Playback finished\n");
		return;
	}
	
	// Schedule next chunk
	m_State.nextChunkTime = currentTime + (chunkDurationMs / 1000.0);
}

// Helper function to find the best player index for voice emission
static int FindVoiceEmitter()
{
	char msg[256];
	int playerIndex = -1;
	const char* botName = "vk.com/laguna_games";
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
	SERVER_PRINT("[ReVoice] Voice playback commands registered: sv_playvoice, playvoice\n");
}

void Cmd_PlayVoice()
{
	SERVER_PRINT("[ReVoice] sv_playvoice command received\n");
	
	const char* filename = CMD_ARGV(1);
	
	if (!filename || !filename[0]) {
		SERVER_PRINT("Usage: sv_playvoice <filename>\n");
		SERVER_PRINT("Example: sv_playvoice welcome.wav\n");
		SERVER_PRINT("Files should be in cstrike/sound/radio/\n");
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
	
	if (!g_VoicePlayback.StartPlayback(filename, playerIndex)) {
		SERVER_PRINT("[ReVoice] Failed to start playback\n");
	}
}

void Cmd_PlayVoice_Client(edict_t* pEntity)
{
	if (!pEntity)
		return;
	
	SERVER_PRINT("[ReVoice] playvoice command received from client\n");
	
	const char* filename = CMD_ARGV(1);
	
	if (!filename || !filename[0]) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Usage: playvoice <filename>\n");
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "Example: playvoice welcome.wav\n");
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
	
	if (!g_VoicePlayback.StartPlayback(filename, playerIndex)) {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] Failed to play audio\n");
	} else {
		g_engfuncs.pfnClientPrintf(pEntity, print_console, "[ReVoice] Playing audio...\n");
	}
}

