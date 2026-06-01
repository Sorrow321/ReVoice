#pragma once

#include "revoice_shared.h"

class CSteamP2PCodec;
class VoiceCodec_Frame;

// Voice playback system for playing WAV files through voice chat
class CVoicePlayback {
private:
	struct PlaybackState {
		FILE* file;
		int sampleRate;
		int channels;
		int bitsPerSample;
		unsigned int dataSize;
		unsigned int dataPos;
		long dataStart;   // absolute file offset of the WAV 'data' payload (for seeking)
		int targetPlayerIndex;
		double nextChunkTime;
		bool active;
		bool paused;   // true: playback suspended, file/position kept for Resume()
		float volume;
		bool targetMask[MAX_PLAYERS];
		// Carry buffer at 8kHz mono PCM16 to feed encoders exact frame sizes (Opus/Silk/Speex stability).
		// Sized to hold several frames at the max 1000 ms frame size (8000 samples/frame).
		short pcm8kCarry[65536];
		int pcm8kCarrySamples;
		int framesSinceReset;
		int resetGapFramesRemaining;
		float aaZ[4];   // anti-alias filter state (two cascaded biquads), persisted across frames
	};
	
	PlaybackState m_State;

	// Dedicated codec instances for playback. Kept separate from per-player codecs so that
	// broadcasting playback audio does not pollute the encoder state of an actively speaking player
	// (shared Opus/Silk encoders carry overflow buffers and sequence counters across calls).
	CSteamP2PCodec* m_OpusCodec;
	CSteamP2PCodec* m_SilkCodec;
	VoiceCodec_Frame* m_SpeexCodec;
	bool m_CodecsReady;

	bool ReadWavHeader(FILE* file, int& sampleRate, int& channels, int& bitsPerSample, unsigned int& dataSize);

public:
	CVoicePlayback();
	~CVoicePlayback();

	void InitCodecs();
	void DeInitCodecs();

	CSteamP2PCodec* GetOpusCodec()    const { return m_OpusCodec; }
	CSteamP2PCodec* GetSilkCodec()    const { return m_SilkCodec; }
	VoiceCodec_Frame* GetSpeexCodec() const { return m_SpeexCodec; }
	
	// Start playing a WAV file
	bool StartPlayback(const char* filename, int playerIndex, float volume, const bool* targetMask);
	
	// Stop current playback
	void StopPlayback();

	// Seek relative to the current position by +/- seconds. Clamps to [start, end]:
	// seeking before the start snaps to the beginning; seeking past the end ends playback
	// cleanly. No-op (returns false) if nothing is currently playing.
	bool SeekRelative(double seconds);

	// Pause: suspend emitting but keep the file open and the position/buffer intact.
	// Resume: continue from exactly where Pause() left off. Both are no-ops (return
	// false) if there is nothing to pause / nothing paused.
	bool Pause();
	bool Resume();
	
	// Called each frame to process playback
	void Update();
	
	// Check current state
	bool IsPlaying() const { return m_State.active; }
	bool IsPaused()  const { return m_State.paused; }
};

extern CVoicePlayback g_VoicePlayback;

// Initialize voice playback system
void Revoice_VoicePlayback_Init();

// Command handlers
void Cmd_PlayVoice();
void Cmd_PlayVoice_Ex();
void Cmd_StopVoice();
void Cmd_VoiceSeek();
void Cmd_PauseVoice();
void Cmd_ResumeVoice();
void Cmd_PlayVoice_Client(edict_t* pEntity);

