## ReVoicePlusPlus (add-on)

This repository includes an **add-on** named **ReVoicePlusPlus** built on top of the original ReVoice Metamod plugin. The original ReVoice project, authors, and license remain unchanged; all rights and attribution belong to their respective owners. ReVoicePlusPlus is not a forked replacement—it extends the plugin with extra server-side utilities:

- Record player voice to 8 kHz mono WAV files (one file per utterance).
- Stream server-side WAV files over in-game voice (radio-bot style, no client precache).
- Admin mic controls: server commands to adjust per-player volume and pitch.
- Configurable verbosity and client command enablement; targeting for playback.

See `README-addon.md` for setup, commands, and configuration details.