# quake_sounds (Plugify)

Quake announcer sounds (kill streaks, first blood, headshots, knife/team/self kills, round events, C4 countdown) written directly on the native s2sdk API (no wrapper library). Ported from the Metamod:Source plugin cs2-quake-sounds (itself a port of Kandru's CounterStrikeSharp CS2 QuakeSounds). Reads the live server's quake_sounds.ini (sound names from workshop addon 3811020435 'Quake 7 Sounds') and translations/quake_sounds.phrases.txt, directly from <configs>/quake_sounds/ (HEAD paths) with the raw addons/configs and addons/translations spellings only as fallbacks. The server language is <configs>/core.cfg ServerLang, then s2sdk ServerLanguage, then en. Per-player mute, message language and volume live in the official permissions cookies under the Metamod cookie names (QuakeSounds.Muted / QuakeSounds.Lang / QuakeSounds.Volume). The !qs menu is a native s2sdk menu; when the optional hudmenu plugin is installed its per-player !menu choice (chat/centerhtml/wasd/hud) is honoured, otherwise the old HUD_LAYOUT fallback is used; per-player volume of the soundevents is applied with the dynamically resolved SosSetSoundEventParams message (no hardcoded id), exactly like the original. No exported methods.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.0
- Plugify dependencies: s2sdk (>=2.18.2), permissions (>=1.3.1, optional), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `quake_sounds-1.2.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/quake_sounds`.

## License
GPL-3.0, see LICENSE.

