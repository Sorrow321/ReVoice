#include "precompiled.h"

CSteamP2PCodec::CSteamP2PCodec(IVoiceCodec *backend)
{
	m_BackendCodec = backend;
	m_Client = nullptr;
}

bool CSteamP2PCodec::Init(int quality)
{
	return m_BackendCodec->Init(quality);
}

void CSteamP2PCodec::SetClient(IGameClient *client)
{
	m_Client = client;
}

void CSteamP2PCodec::Release()
{
	m_BackendCodec->Release();
	delete this;
}

bool CSteamP2PCodec::ResetState()
{
	return m_BackendCodec->ResetState();
}

int CSteamP2PCodec::StreamDecode(const char *pCompressed, int compressedBytes, char *pUncompressed, int maxUncompressedBytes) const
{
	const char *maxReadPos = pCompressed + compressedBytes;
	const char *readPos = pCompressed;

	while (readPos < maxReadPos)
	{
		PayLoadType opcode = *(PayLoadType *)readPos;
		readPos++;

		switch (opcode)
		{
			// Set sampling rate
			case PLT_SamplingRate:
			{
				if (readPos + 2 > maxReadPos) {
					return 0;
				}
				readPos += 2;
				break;
			}
			// Voice payload
			// Note: Some clients/servers use different opcodes for the same "framed payload" format.
			// In all of these cases the stream layout is:
			//   [opcode][uint16 len][len bytes payload]
			// and payload decoding is performed by the backend codec.
			case PLT_Silk:
			case PLT_OPUS:
			case PLT_OPUS_PLC:
			{
				if (readPos + 2 > maxReadPos) {
					return 0;
				}

				uint16 len = *(uint16 *)readPos;
				readPos += 2;

				if (readPos + len > maxReadPos) {
					return 0;
				}

				int decompressedLen = m_BackendCodec->Decompress(readPos, len, pUncompressed, maxUncompressedBytes);
				return decompressedLen;
			}

			// Invalid or unknown opcode
			default:
				// LCPrintf(true, "CSteamP2PCodec::StreamDecode() called on client(%d) with unknown voice codec opcode (%d)\n", m_Client->GetId(), opcode);
				return 0;
		}
	}

	// no voice payload in the stream
	return 0;
}

int CSteamP2PCodec::StreamEncode(const char *pUncompressedBytes, int nSamples, char *pCompressed, int maxCompressedBytes, bool bFinal) const
{
	char *writePos = pCompressed;

	if (maxCompressedBytes < 10) { // no room
		return 0;
	}

	*(writePos++) = PLT_SamplingRate; // Set sampling rate
	// ReVoice encoders in this repo operate on 8 kHz PCM (see VoiceEncoder_Opus, VoiceEncoder_Silk, VoiceEncoder_Speex).
	*(uint16 *)writePos = 8000;
	writePos += 2;

	// Use a framed payload opcode that our decoder handles robustly (len-prefixed).
	// This keeps compatibility across clients that might label framed payloads differently.
	*(writePos++) = PLT_OPUS_PLC;

	int compressRes = m_BackendCodec->Compress(pUncompressedBytes, nSamples, writePos + 2, maxCompressedBytes - (1 + 2 + 1 + 2), bFinal);
	if (compressRes == 0) {
		return 0;
	}

	*(uint16 *)writePos = compressRes;
	writePos += 2;
	writePos += compressRes;

	return writePos - pCompressed;
}

int CSteamP2PCodec::Decompress(const char *pCompressed, int compressedBytes, char *pUncompressed, int maxUncompressedBytes)
{
	if (compressedBytes < 12) {
		return 0;
	}

	uint32 computedChecksum = crc32(pCompressed, compressedBytes - 4);
	uint32 wireChecksum = *(uint32 *)(pCompressed + compressedBytes - 4);

	if (computedChecksum != wireChecksum) {
		return 0;
	}

	return StreamDecode(pCompressed + 8, compressedBytes - 12, pUncompressed, maxUncompressedBytes);
}

int CSteamP2PCodec::Compress(const char *pUncompressedBytes, int nSamples, char *pCompressed, int maxCompressedBytes, bool bFinal)
{
	if (maxCompressedBytes < 12) { // no room
		return 0;
	}

	char *writePos = pCompressed;
	*(uint32 *)writePos = 0x00000011; // steamid (low part)
	writePos += 4;

	*(uint32 *)writePos = 0x01100001; // steamid (high part)
	writePos += 4;

	int encodeRes = StreamEncode(pUncompressedBytes, nSamples, writePos, maxCompressedBytes - 12, bFinal);
	if (encodeRes <= 0) {
		return 0;
	}

	writePos += encodeRes;

	uint32 cksum = crc32(pCompressed, writePos - pCompressed);
	*(uint32 *)writePos = cksum;
	writePos += 4;

	return writePos - pCompressed;
}
