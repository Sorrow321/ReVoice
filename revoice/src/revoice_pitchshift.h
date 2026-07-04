#pragma once

// Duration-preserving real-time pitch shifter for live voice (8 kHz mono PCM16).
//
// WHY THIS SHAPE: live mic audio arrives at exactly 8000 samples/s, so a
// tape-style shifter (like the bot-playback speed feature, which consumes a
// FILE faster/slower) cannot work here — changing duration would starve or
// backlog the stream. This is the classic dual-tap granular ("delay line")
// shifter instead: N samples in -> N samples out on the same timeline, pitch
// scaled by P inside.
//
// HOW: input is written into a ring buffer at rate 1. Two read taps sweep the
// ring at rate P (their delay ramps at rate 1-P). Each tap periodically has to
// jump back/forward by one grain window; the taps are half a window apart in
// phase and crossfaded with triangle gains that are exactly 0 at the moment
// their tap jumps, so the jumps are inaudible. All cross-packet continuity
// lives here (ring + phase persist between packets) — this is what removes the
// per-packet clicks the old frame-local resampler produced.
//
// For upward shifts the read taps sweep faster than the write head, so input
// content above (outputNyquist / P) would alias; a 4th-order Butterworth
// low-pass (same math as AntiAliasLowpassMono in revoice_voice_playback.cpp)
// filters the input before it enters the ring.
//
// SAFETY INVARIANTS (the reasons this cannot read/write out of bounds):
//   1. Every ring access is masked with RING_MASK (power-of-two size), so any
//      index value lands inside m_Ring — for ANY pitch, phase or write count.
//   2. Tap delays are hard-clamped to [BASE_DELAY, BASE_DELAY + WINDOW] before
//      use; the clamp also catches NaN. The static_asserts below prove the
//      maximum look-back fits the ring.
//   3. m_WritePos is unsigned and free-running: wraparound is well-defined
//      modular arithmetic, consistent with the masked reads.
//   4. Every float -> int16 conversion goes through ClampPcm16, which handles
//      +/-inf and NaN explicitly (a raw cast of NaN would be UB).
//   5. No heap, no pointers, no destructor: the entire state is a flat POD
//      block inside the owning object.
// The 0.5..2.0 pitch clamp is therefore a QUALITY choice, not a safety
// requirement.
//
// Threading: main game thread only (same rule as the rest of the voice path).

#include <string.h>
#include <math.h>

class CVoicePitchShifter {
public:
	static const int RING_SIZE  = 2048; // 256 ms @ 8 kHz, power of two
	static const int RING_MASK  = RING_SIZE - 1;
	static const int BASE_DELAY = 2;    // keeps the interpolation pair >= 1 sample behind the write head

	// Grain window in samples @ 8 kHz — the main quality knob. Large windows
	// minimize warble but read as a ~W/2 "doubled voice" echo; small windows
	// fuse into a chorus/flanger tinge but shimmer faster. Runtime-tunable
	// per stream within [MIN_WINDOW, MAX_WINDOW]; the value is LATCHED at the
	// first processed packet after Reset(), so a live change can never jump
	// the taps mid-utterance.
	static const int DEFAULT_WINDOW = 448; // 56 ms
	static const int MIN_WINDOW     = 80;  // 10 ms
	static const int MAX_WINDOW     = 800; // 100 ms

	// Aligned-jump mode (WSOLA-style): when a tap wraps, instead of jumping
	// exactly one window it searches +-ALIGN_RANGE samples for the offset that
	// best correlates with what the other (currently carrying) tap reads, so
	// the crossfade blends two phase-locked copies instead of arbitrarily
	// offset ones. ALIGN_CORR_LEN is the comparison segment (~1 pitch period
	// at 8 kHz speech). The search runs only at tap wraps — a few times per
	// second — never per sample.
	//
	// In aligned mode the tap base delay is biased by ALIGN_RANGE (+12 ms
	// latency): a tap sweeps its whole window carrying its adjust, so with a
	// negative adjust an unbiased tap would hit the BASE_DELAY floor before
	// its next wrap and sit clamped there, playing wrongly-pitched audio at
	// unity rate for the tail of the grain (found empirically: periodic RMS
	// dips at the adjust-pattern rate). The bias makes the worst-case sweep
	// floor exactly BASE_DELAY.
	static const int ALIGN_RANGE    = 96;  // +-12 ms search
	static const int ALIGN_CORR_LEN = 96;  // 12 ms reference segment

	static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be a power of two (mask indexing)");
	static_assert(BASE_DELAY + MAX_WINDOW + 2 * ALIGN_RANGE + 2 <= RING_SIZE, "max tap look-back must fit inside the ring");
	static_assert(BASE_DELAY + MAX_WINDOW + 2 * ALIGN_RANGE + ALIGN_CORR_LEN + 2 <= RING_SIZE, "correlation reads must fit inside the ring");
	static_assert(MIN_WINDOW >= 8 && MIN_WINDOW <= DEFAULT_WINDOW && DEFAULT_WINDOW <= MAX_WINDOW, "window bounds sane");

	CVoicePitchShifter() { Reset(); }

	// Forget all carried audio/state. Call at utterance boundaries and whenever
	// volume/pitch settings change, so a new stream never reads stale audio.
	// Also unlatches the grain window, so the next stream picks up a live
	// window/cvar change.
	void Reset()
	{
		memset(m_Ring, 0, sizeof(m_Ring));
		m_WritePos = 0;
		m_Phase    = 0.0;
		m_Window   = 0; // unlatched; set from windowSamples on first Process()
		m_Align    = false;
		m_AdjA     = 0;
		m_AdjB     = 0;
		m_LpZ[0] = m_LpZ[1] = m_LpZ[2] = m_LpZ[3] = 0.0f;
		m_LpWasOn = false;
	}

	// In-place shift of numSamples PCM16 samples by factor 'pitch'
	// (0.5 = octave down .. 2.0 = octave up). pitch outside that range
	// (including NaN/inf) is clamped; 1.0 is an exact bypass.
	//
	// windowSamples: grain window, clamped to [MIN_WINDOW, MAX_WINDOW] and
	// latched until the next Reset() (changing it mid-stream would jump the
	// tap delays audibly, so later values are ignored until then).
	// aaCutoffBaseHz: anti-alias low-pass base cutoff for upward shifts; the
	// effective cutoff is base/pitch. <= 0 (or NaN) disables the filter —
	// read live each call, safe to change mid-stream (state persists, same
	// convention as REV_PlaybackLowpassHz in the playback path).
	// alignJumps: false = classic dual-tap (taps jump exactly one window,
	// arbitrary phase between the copies -> comb/"doubling" coloration);
	// true = WSOLA-style correlation-aligned jumps (see FindAlignedAdjust).
	// Latched with the window so a live toggle applies from the next stream.
	void Process(short *pcm, int numSamples, float pitch,
	             int windowSamples = DEFAULT_WINDOW, float aaCutoffBaseHz = 3600.0f,
	             bool alignJumps = false)
	{
		if (pcm == nullptr || numSamples <= 0)
			return;

		if (pitch != pitch) pitch = 1.0f; // NaN -> neutral
		if (pitch < 0.5f)   pitch = 0.5f;
		if (pitch > 2.0f)   pitch = 2.0f;
		// Exact identity must bypass: with frozen taps the dual-tap sum would
		// act as a fixed comb filter, coloring unshifted voice.
		if (pitch == 1.0f)
			return;

		// Latch the grain window and jump mode for this stream. The clamp
		// keeps the maximum tap look-back provably inside the ring (see
		// static_asserts above) no matter what value the caller passes.
		if (m_Window <= 0) {
			if (windowSamples < MIN_WINDOW) windowSamples = MIN_WINDOW;
			if (windowSamples > MAX_WINDOW) windowSamples = MAX_WINDOW;
			m_Window = windowSamples;
			m_Align = alignJumps;
		}

		// NaN compares false with everything, so lowpassOn stays false for a
		// NaN cutoff; MakeLowpass additionally clamps fc to [10, 0.45*fs].
		const bool lowpassOn = (pitch > 1.0f) && (aaCutoffBaseHz > 0.0f);
		float c1[5] = { 0, 0, 0, 0, 0 };
		float c2[5] = { 0, 0, 0, 0, 0 };
		if (lowpassOn) {
			if (!m_LpWasOn)
				m_LpZ[0] = m_LpZ[1] = m_LpZ[2] = m_LpZ[3] = 0.0f;
			// Butterworth 4th-order, cutoff referenced to the post-shift band:
			// content above ~base/P Hz would alias once read P times faster.
			MakeLowpass(aaCutoffBaseHz / pitch, 8000.0f, 0.54119610f, c1);
			MakeLowpass(aaCutoffBaseHz / pitch, 8000.0f, 1.30656296f, c2);
		}
		m_LpWasOn = lowpassOn;

		// Taps drift at (1-P) samples per sample; one grain cycle spans
		// m_Window samples of drift. |phaseInc| <= 0.5/MIN_WINDOW < 1, so
		// single-step wrap handling below is sufficient.
		const double phaseInc = (1.0 - (double)pitch) / (double)m_Window;

		for (int i = 0; i < numSamples; i++) {
			float x = (float)pcm[i];
			if (lowpassOn) {
				float y1 = c1[0] * x + m_LpZ[0];
				m_LpZ[0] = c1[1] * x - c1[3] * y1 + m_LpZ[1];
				m_LpZ[1] = c1[2] * x - c1[4] * y1;
				float y2 = c2[0] * y1 + m_LpZ[2];
				m_LpZ[2] = c2[1] * y1 - c2[3] * y2 + m_LpZ[3];
				m_LpZ[3] = c2[2] * y1 - c2[4] * y2;
				x = y2;
			}

			m_Ring[m_WritePos & (unsigned int)RING_MASK] = ClampPcm16(x);
			m_WritePos++;

			const double prevPhase = m_Phase;
			m_Phase += phaseInc;
			bool aWrapped = false;
			if (m_Phase >= 1.0)     { m_Phase -= 1.0; aWrapped = true; }
			else if (m_Phase < 0.0) { m_Phase += 1.0; aWrapped = true; }

			double phA = m_Phase;
			double phB = phA + 0.5;
			if (phB >= 1.0) phB -= 1.0;

			// Aligned mode: re-anchor a tap's delay at the exact moment it
			// wraps — its gain is 0 there, so the re-anchor is inaudible, and
			// both read positions advance at the same rate P afterwards, so
			// the alignment holds for the entire crossfade. Tap B wraps when
			// phase A crosses 0.5 (phB crosses the 0/1 boundary); at most one
			// tap can wrap per sample since |phaseInc| << 0.5.
			// tapBase carries the aligned-mode headroom bias (see ALIGN_RANGE
			// comment); it is BASE_DELAY exactly in classic mode, keeping that
			// path bit-identical to the pre-alignment implementation.
			const double tapBase = (double)(BASE_DELAY + (m_Align ? ALIGN_RANGE : 0));
			if (m_Align) {
				if (aWrapped) {
					m_AdjA = FindAlignedAdjust(
						tapBase + phA * (double)m_Window,
						tapBase + phB * (double)m_Window + (double)m_AdjB);
				} else if ((prevPhase < 0.5) != (m_Phase < 0.5)) {
					m_AdjB = FindAlignedAdjust(
						tapBase + phB * (double)m_Window,
						tapBase + phA * (double)m_Window + (double)m_AdjA);
				}
			}

			// Triangle gains: exactly 0 when the corresponding tap wraps (phase
			// 0/1), and gA + gB == 1 -> the output is a convex combination of
			// two ring samples, so it can never exceed the PCM16 range.
			float gA = 1.0f - (float)fabs(2.0 * phA - 1.0);
			if (gA < 0.0f) gA = 0.0f;
			if (gA > 1.0f) gA = 1.0f;
			float gB = 1.0f - gA;

			// m_AdjA/m_AdjB are 0 and tapBase == BASE_DELAY in classic mode,
			// making the delays exactly the pre-alignment formula
			// (bit-identical legacy output). In aligned mode the sweep floor
			// is tapBase - ALIGN_RANGE == BASE_DELAY, so a negative adjust can
			// never hit the ReadTapDelay clamp and pin the tap.
			double dA = tapBase + phA * (double)m_Window + (double)m_AdjA;
			double dB = tapBase + phB * (double)m_Window + (double)m_AdjB;
			pcm[i] = ClampPcm16(gA * ReadTapDelay(dA) + gB * ReadTapDelay(dB));
		}
	}

private:
	// Linear-interpolated read at delay 'd' behind the write head.
	float ReadTapDelay(double d) const
	{
		// Hard clamp: legitimate delays are BASE_DELAY + phase*window (+ the
		// bounded alignment adjust); this makes an out-of-range delay
		// impossible even against corrupted state (m_Window <= 0 degrades to
		// a fixed BASE_DELAY read — still in bounds), and the inverted first
		// comparison also catches NaN.
		if (!(d >= (double)BASE_DELAY))                              d = (double)BASE_DELAY;
		if (d > (double)(BASE_DELAY + MAX_WINDOW + 2 * ALIGN_RANGE)) d = (double)(BASE_DELAY + MAX_WINDOW + 2 * ALIGN_RANGE);

		unsigned int di = (unsigned int)d;      // in [BASE_DELAY, BASE_DELAY+MAX_WINDOW+2*ALIGN_RANGE]
		float fr = (float)(d - (double)di);     // in [0,1)

		// The sample d behind the head sits between the samples di ("newer",
		// weight 1-fr) and di+1 ("older", weight fr) behind it. Masked
		// indexing keeps both reads inside m_Ring for any di / m_WritePos.
		unsigned int iNew = (m_WritePos - di)      & (unsigned int)RING_MASK;
		unsigned int iOld = (m_WritePos - di - 1u) & (unsigned int)RING_MASK;
		return (float)m_Ring[iNew] * (1.0f - fr) + (float)m_Ring[iOld] * fr;
	}

	// WSOLA-style jump alignment. Called at the moment a tap wraps: finds the
	// integer delay adjustment (within +-ALIGN_RANGE) that makes the wrapped
	// tap's upcoming read best correlate with the segment the OTHER tap — the
	// one currently carrying the output — is reading. Because both read
	// positions then advance at the same rate, the two crossfaded copies stay
	// phase-locked for the whole grain, which removes the comb/"doubling"
	// coloration of the classic mode on voiced material.
	//
	// Runs a few times per second (per tap wrap), never per sample:
	// (2*ALIGN_RANGE+1) candidates x ALIGN_CORR_LEN MACs. Falls back to 0
	// (the classic exact-window jump) on silence or uncorrelated content —
	// then behaves exactly like the unaligned mode. Every ring access is
	// masked, and all look-backs are compile-time bounded by the
	// static_asserts above, so no input can push a read outside m_Ring.
	int FindAlignedAdjust(double nominalDelay, double refDelay) const
	{
		// Integer anchor delays, clamped to the same envelope ReadTapDelay
		// enforces (NaN-safe via the inverted comparison).
		const int maxD = BASE_DELAY + MAX_WINDOW + 2 * ALIGN_RANGE;
		if (!(nominalDelay >= (double)BASE_DELAY)) nominalDelay = (double)BASE_DELAY;
		if (nominalDelay > (double)maxD)           nominalDelay = (double)maxD;
		if (!(refDelay >= (double)BASE_DELAY))     refDelay = (double)BASE_DELAY;
		if (refDelay > (double)maxD)               refDelay = (double)maxD;
		const int canD = (int)(nominalDelay + 0.5);
		const int refD = (int)(refDelay + 0.5);

		// Reference: the last ALIGN_CORR_LEN samples as the carrying tap reads
		// them (index wp - D - k, matching ReadTapDelay's addressing).
		double ref[ALIGN_CORR_LEN];
		double refEnergy = 0.0;
		for (int k = 0; k < ALIGN_CORR_LEN; k++) {
			ref[k] = (double)m_Ring[(m_WritePos - (unsigned int)refD - (unsigned int)k) & (unsigned int)RING_MASK];
			refEnergy += ref[k] * ref[k];
		}
		// Silence / near-silence: nothing to lock onto — keep the exact jump.
		if (refEnergy <= (double)ALIGN_CORR_LEN)
			return 0;

		int bestAdj = 0;
		double bestScore = 0.0;
		for (int adj = -ALIGN_RANGE; adj <= ALIGN_RANGE; adj++) {
			// Keep the candidate delay inside the legitimate envelope; masked
			// reads below are safe regardless, this only skips silly anchors.
			const int d = canD + adj;
			if (d < BASE_DELAY || d > maxD)
				continue;

			double dot = 0.0, energy = 0.0;
			for (int k = 0; k < ALIGN_CORR_LEN; k++) {
				double v = (double)m_Ring[(m_WritePos - (unsigned int)d - (unsigned int)k) & (unsigned int)RING_MASK];
				dot += v * ref[k];
				energy += v * v;
			}
			if (dot <= 0.0)
				continue; // anti-phase or uncorrelated — never pick those

			// Normalized score without the sqrt: maximizing dot/sqrt(energy)
			// == maximizing dot^2/energy for dot > 0. (+1 avoids div by zero.)
			double score = (dot * dot) / (energy + 1.0);
			if (score > bestScore) {
				bestScore = score;
				bestAdj = adj;
			}
		}
		return bestAdj;
	}

	// Same lowpass design as REV_BiquadSetLowpass in revoice_voice_playback.cpp.
	// c = { b0, b1, b2, a1, a2 }, normalized so a0 == 1.
	static void MakeLowpass(float fc, float fs, float Q, float c[5])
	{
		if (fc < 10.0f)        fc = 10.0f;
		if (fc > 0.45f * fs)   fc = 0.45f * fs;
		float w0    = 2.0f * 3.14159265358979f * fc / fs;
		float cosw0 = cosf(w0);
		float sinw0 = sinf(w0);
		float alpha = sinw0 / (2.0f * Q);
		float a0    = 1.0f + alpha;
		c[0] = ((1.0f - cosw0) * 0.5f) / a0;
		c[1] = (1.0f - cosw0) / a0;
		c[2] = ((1.0f - cosw0) * 0.5f) / a0;
		c[3] = (-2.0f * cosw0) / a0;
		c[4] = (1.0f - alpha) / a0;
	}

	// NaN/inf-proof float -> PCM16 conversion (a plain cast of NaN or an
	// out-of-range float is undefined behavior).
	static short ClampPcm16(float v)
	{
		if (v >= 32767.0f)  return 32767;
		if (v <= -32768.0f) return -32768;
		if (v != v)         return 0; // NaN (unreachable from bounded inputs; kept as a hard guarantee)
		return (short)v;
	}

	short        m_Ring[RING_SIZE];
	unsigned int m_WritePos; // free-running; masked on every access
	double       m_Phase;    // tap A phase in [0,1); tap B runs 0.5 apart
	int          m_Window;   // latched grain window in [MIN_WINDOW, MAX_WINDOW]; 0 = unlatched
	bool         m_Align;    // latched with the window: correlation-aligned tap jumps
	int          m_AdjA;     // per-tap delay adjustment from FindAlignedAdjust,
	int          m_AdjB;     //   in [-ALIGN_RANGE, ALIGN_RANGE]; always 0 in classic mode
	float        m_LpZ[4];   // anti-alias biquad state (2 cascaded sections)
	bool         m_LpWasOn;  // zero filter state when the lowpass re-engages
};
