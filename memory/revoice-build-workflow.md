---
name: revoice-build-workflow
description: How the ReVoice plugin gets built and tested during this work
metadata:
  type: project
---

The user compiles the plugin on a **separate machine**, not here — don't attempt local builds of the metamod plugin. After I make code changes the user compiles, uploads to their **remote CS 1.6 server**, and tests in-game. Iteration is therefore slow (a compile+deploy+listen loop), so favor cvar-gated changes that can be A/B tested in one build, and avoid shipping large speculative changes without flagging the risk.

Offline audio analysis is done locally in `audio_experiment/` with Python (numpy/scipy/torchaudio/soundfile, and PyAV for raw Opus decode).
