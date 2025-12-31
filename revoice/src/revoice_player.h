#pragma once

#include "revoice_shared.h"
#include "VoiceEncoder_Silk.h"
#include "SteamP2PCodec.h"
#include "VoiceEncoder_Speex.h"
#include "voice_codec_frame.h"

class CRevoicePlayer {
private:
	IGameClient *m_Client;
	CodecType m_CodecType;
	// WAV recording state
	FILE *m_WavFile = nullptr;
	unsigned int m_WavDataBytes = 0;
	int m_WavSampleRate = 0;
	time_t m_WavStartTs = 0;
	double m_LastWavVoiceTime = 0;
	CSteamP2PCodec *m_SilkCodec;
	CSteamP2PCodec *m_OpusCodec;
	VoiceCodec_Frame *m_SpeexCodec;
	int m_Protocol;
	int m_VoiceRate;
	int m_RequestId;
	bool m_Connected;
	bool m_HLTV;
	float m_VoiceVolume;
	float m_VoicePitch;
	float m_PitchPhase;
	short m_PitchPrevSample;
	bool m_PitchHasPrev;

public:
	CRevoicePlayer();
	// WAV recording API
	void AppendWav(const char *pcm16, int numSamples, int sampleRate);
	void CloseWavIfOpen();
	void Update();
	void Initialize(IGameClient *cl);
	void OnConnected();
	void OnDisconnected();

	void SetLastVoiceTime(double time);
	void UpdateVoiceRate(double delta);
	void IncreaseVoiceRate(int dataLength);
	CodecType GetCodecTypeByString(const char *codec);
	const char *GetCodecTypeToString();

	void SetVoiceVolume(float volume);
	float GetVoiceVolume() const { return m_VoiceVolume; }
	void SetVoicePitch(float pitch);
	float GetVoicePitch() const { return m_VoicePitch; }
	void ResetPitchState();
	void SetPitchPhase(float phase) { m_PitchPhase = phase; }
	float GetPitchPhase() const { return m_PitchPhase; }
	void SetPitchPrevSample(short s, bool has) { m_PitchPrevSample = s; m_PitchHasPrev = has; }
	short GetPitchPrevSample() const { return m_PitchPrevSample; }
	bool HasPitchPrev() const { return m_PitchHasPrev; }

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

CRevoicePlayer *GetPlayerByClientPtr(IGameClient *cl);
CRevoicePlayer *GetPlayerByEdict(const edict_t *ed);

void Revoice_Init_Players();
void Revoice_Update_Players(const char *pszNewValue);
void Revoice_Update_Hltv(const char *pszNewValue);
