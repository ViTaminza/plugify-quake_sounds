# quake_sounds (Plugify)

Quake announcer sounds (kill streaks, first blood, headshots, knife/team/self kills, round events, C4 countdown), ported from the Metamod:Source plugin cs2-quake-sounds (itself a port of Kandru's CounterStrikeSharp CS2 QuakeSounds). Reads configs/quake_sounds/quake_sounds.ini (the live server's fleet config, sound names from workshop addon 3811020435 'Quake 7 Sounds') and configs/quake_sounds/translations/quake_sounds.phrases.txt; per-player mute, message language and volume live in the official permissions cookies under the Metamod cookie names (QuakeSounds.Muted / QuakeSounds.Lang / QuakeSounds.Volume). Per-player volume of the soundevents is applied with GE_SosSetSoundEventParams, exactly like the original. No exported methods.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, permissions (optional), hudmenu (optional)

## Installation
Download `quake_sounds-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/quake_sounds`.

## License
GPL-3.0, see LICENSE.

