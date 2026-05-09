#pragma once

#include "revoice_shared.h"
#include "VoiceEncoder_Silk.h"
#include "SteamP2PCodec.h"
#include "VoiceEncoder_Speex.h"
#include "voice_codec_frame.h"
#include <vector>
#include <stdint.h>

class CRevoicePlayer {
private:
	IGameClient *m_Client;
	CodecType m_CodecType;

	// WAV recording state — accumulator-only design.
	// We buffer PCM in memory for the duration of one utterance, then write
	// the file in a single fopen+fwrite+fclose at flush time. There is never
	// a partial WAV on disk: AMXX only ever sees finalized files via the IPC
	// fired from FlushWav, and the upload scanner cannot race the writer
	// because the writer never owns an open FD on disk.
	std::vector<int16_t> m_WavSamples;
	int                  m_WavSampleRate;
	double               m_LastWavVoiceTime;
	time_t               m_WavStartTs;
	unsigned int         m_WavSeq;
	bool                 m_WavForAsr;
	char                 m_WavAuth[256];      // captured at start of each utterance

	CSteamP2PCodec *m_SilkCodec;
	CSteamP2PCodec *m_OpusCodec;
	VoiceCodec_Frame *m_SpeexCodec;
	int m_Protocol;
	int m_VoiceRate;
	int m_RequestId;
	bool m_Connected;
	bool m_HLTV;

public:
	CRevoicePlayer();

	// WAV recording API
	void AppendPcm(const char *pcm16, int numSamples, int sampleRate);  // buffer PCM in memory
	void FlushWav(const char *reason);                   // write buffered PCM to disk (no-op if empty)
	void FlushWavIfStale(double now, double timeout);    // flush if silence-gap exceeded
	size_t GetBufferSamples() const { return m_WavSamples.size(); }

	void Update();
	void Initialize(IGameClient *cl);
	void OnConnected();
	void OnDisconnected();

	void SetLastVoiceTime(double time);
	void UpdateVoiceRate(double delta);
	void IncreaseVoiceRate(int dataLength);
	CodecType GetCodecTypeByString(const char *codec);
	const char *GetCodecTypeToString();

	int GetProtocol()  const { return m_Protocol;  }
	int GetVoiceRate() const { return m_VoiceRate; }
	int GetRequestId() const { return m_RequestId; }
	bool IsConnected() const { return m_Connected; }
	bool IsHLTV()      const { return m_HLTV;      }

	static const char *m_szCodecType[];
	void SetCodecType(CodecType codecType)     { m_CodecType = codecType; }

	CodecType GetCodecType() const             { return m_CodecType; }
	CSteamP2PCodec *GetSilkCodec() const       { return m_SilkCodec; }
	CSteamP2PCodec *GetOpusCodec() const       { return m_OpusCodec; }
	VoiceCodec_Frame *GetSpeexCodec() const    { return m_SpeexCodec;  }
	IGameClient *GetClient() const             { return m_Client; }
};

extern CRevoicePlayer g_Players[MAX_PLAYERS];
extern bool g_asrActive[MAX_PLAYERS];
extern const double WAV_FLUSH_GAP_SEC;

CRevoicePlayer *GetPlayerByClientPtr(IGameClient *cl);
CRevoicePlayer *GetPlayerByEdict(const edict_t *ed);

void Revoice_Init_Players();
void Revoice_FlushAll_Players();        // called on changelevel / shutdown
void Revoice_Update_Players(const char *pszNewValue);
void Revoice_Update_Hltv(const char *pszNewValue);

// Periodic state snapshot to RV*.log. Call from StartFrame_PreHook; the
// function rate-limits itself to one line per ~30 wall-clock seconds.
void Revoice_LogHeartbeatTick();
