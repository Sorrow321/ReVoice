#pragma once

#include "revoice_shared.h"
#include "VoiceEncoder_Silk.h"
#include "SteamP2PCodec.h"
#include "VoiceEncoder_Speex.h"
#include "voice_codec_frame.h"
#include "revoice_pitchshift.h"
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

	// ---- Voice FX (per-player volume / pitch). OFF by default. ----
	// ISOLATION INVARIANT: while both values are 1.0 (the default), the
	// per-packet voice path only ever calls HasActiveVoiceFx() — two float
	// compares — and never touches anything else in this block. The FX codecs
	// below stay nullptr until the FIRST packet of a player that an admin
	// actually targeted with sv_voice_volume / sv_voice_pitch, so on a server
	// where the commands were never used these components are never even
	// constructed. Every change of the two values is logged ([FX] lines) by
	// the command handlers via RvLogAction — the always-on RV_actions_*.log.
	float m_VoiceVolume;          // 0.0 (mute) .. 10.0 (boost); 1.0 = off
	float m_VoicePitch;           // 0.5 .. 2.0; 1.0 = off
	bool  m_FxStreamContinuous;   // shifter/encoder state continues the current stream
	bool  m_FxCodecsFailed;       // lazy init failed once — never retried, fx disabled
	CVoicePitchShifter m_PitchShifter;
	// Dedicated FX encoders. NEVER the per-player decode codecs above: those
	// carry the speaker's decoder stream state, and Decompress() may
	// ResetState() them mid-stream (see the playback system for the same
	// separation rationale).
	CSteamP2PCodec   *m_FxSilkCodec;
	CSteamP2PCodec   *m_FxOpusCodec;
	VoiceCodec_Frame *m_FxSpeexCodec;

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

	// Voice FX API (per-player volume / pitch). Setters clamp (NaN -> neutral)
	// and reset the fx stream state so a settings change never splices into
	// stale shifter/encoder state.
	void  SetVoiceVolume(float volume);
	float GetVoiceVolume() const { return m_VoiceVolume; }
	void  SetVoicePitch(float pitch);
	float GetVoicePitch() const { return m_VoicePitch; }
	// The gate the per-packet path checks. Exact compares on purpose: the only
	// writers are the clamped setters and the connect/disconnect resets, all of
	// which store a literal 1.0f for "off".
	bool  HasActiveVoiceFx() const { return m_VoiceVolume != 1.0f || m_VoicePitch != 1.0f; }
	void  ResetVoiceFxStream();          // drop carried fx state; next fx packet starts clean
	// Reset vol/pitch to neutral and drop ALL carried fx state (shifter ring,
	// stream continuity, fx encoder streams). Logs an [FX] line only if
	// something was actually active. 'reason' goes into the audit line.
	void  ClearVoiceFx(const char *reason);
	bool  EnsureFxCodecs();              // lazy alloc+init; false -> fx unusable for this slot
	void  ApplyVoiceFx(short *pcm, int numSamples, bool freshStream); // in-place pitch+volume
	CSteamP2PCodec   *GetFxSilkCodec() const  { return m_FxSilkCodec;  }
	CSteamP2PCodec   *GetFxOpusCodec() const  { return m_FxOpusCodec;  }
	VoiceCodec_Frame *GetFxSpeexCodec() const { return m_FxSpeexCodec; }

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
// Clear every slot's voice fx at the map boundary. Required because Metamod
// plugins do NOT see clients as disconnected/reconnected across changelevel
// (no SV_DropClient, and OnConnected early-returns for staying players), so
// the per-connection clears never fire there.
void Revoice_ClearAllVoiceFx();
void Revoice_Update_Players(const char *pszNewValue);
void Revoice_Update_Hltv(const char *pszNewValue);

// Periodic state snapshot to RV*.log. Call from StartFrame_PreHook; the
// function rate-limits itself to one line per ~30 wall-clock seconds.
void Revoice_LogHeartbeatTick();
