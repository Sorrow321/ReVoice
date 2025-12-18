#pragma once

#include "revoice_shared.h"

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
		int targetPlayerIndex;
		double nextChunkTime;
		bool active;
		// Carry buffer at 8kHz mono PCM16 to feed encoders exact frame sizes (Opus/Silk/Speex stability)
		short pcm8kCarry[8192];
		int pcm8kCarrySamples;
	};
	
	PlaybackState m_State;
	
	bool ReadWavHeader(FILE* file, int& sampleRate, int& channels, int& bitsPerSample, unsigned int& dataSize);
	
public:
	CVoicePlayback();
	~CVoicePlayback();
	
	// Start playing a WAV file
	bool StartPlayback(const char* filename, int playerIndex);
	
	// Stop current playback
	void StopPlayback();
	
	// Called each frame to process playback
	void Update();
	
	// Check if currently playing
	bool IsPlaying() const { return m_State.active; }
};

extern CVoicePlayback g_VoicePlayback;

// Initialize voice playback system
void Revoice_VoicePlayback_Init();

// Command handlers
void Cmd_PlayVoice();
void Cmd_PlayVoice_Client(edict_t* pEntity);

