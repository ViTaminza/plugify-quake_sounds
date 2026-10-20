# quake_sounds — Quake announcer sounds (Plugify port)

Port of the Metamod:Source plugin `cs2-quake-sounds` (itself a port of Kandru's
CounterStrikeSharp "CS2 QuakeSounds"). Announcer sounds for kill streaks, special kills
(first blood, headshot, knife, team/self kill, last man standing), round events and the C4
countdown, with a per-player mute switch, message language and volume.

| | |
|---|---|
| original | `cs2-quake-sounds` (997 + 665 LOC of C++) |
| config | `configs/quake_sounds/quake_sounds.ini` — the original's configuration file |
| phrases | `configs/quake_sounds/translations/quake_sounds.phrases.txt` |
| commands | chat `!qs` (menu), `!lang <code>`; console `mm_qs`, `mm_quakesounds reload\|enable\|disable` |
| cookies | official permissions plugin, names of the Metamod original: `QuakeSounds.Muted`, `QuakeSounds.Lang`, `QuakeSounds.Volume` |
| workshop addons | **3811020435** "Quake 7 Sounds" (`Alko.Quake7.*`, `Alko.Timer.*` soundevents, `sounds/quake7/*.vsnd`, `sounds/timrc4/*.vsnd`). Add it to `csgo/cfg/plugify/multi_addon_manager.cfg` (`s2_extra_addons`). |
| tests | `test/quake_core_tests.cpp` (build/tests/quake_sounds_tests): config + phrase parsing, the kill logic, sound hash/packed volume parameter |

## What is ported

* every config key, default and behaviour of the reference (scope filters including the per-record
  `_filter`, the special/weapon/streak priority chain, `headshot_once`, `ignore_bots`,
  `ignore_world_damage`, `count_self_kills`, `count_team_kills`, kill resets, warmup gate, play
  modes `player` / `listener` / `world`, per-record `_volume`, the `play_on_entity` default);
* the menus (`!qs`: mute/unmute, volume in 10 % steps, volume menu only when a soundevent is in
  the config), the `!lang` per-player message language, the server commands;
* the C4 countdown driven by the real explosion time (`CCSGameRules`/`planted_c4` `m_flC4Blow` minus
  `curtime`), so a duel-extended timer keeps the beeps in sync;
* per-player volume of the soundevents through `GE_SosSetSoundEventParams`, i.e. the
  `SoundVolume` namespace of the original: s2sdk hooks `IGameEventSystem::PostEventAbstract`
  (`HookUserMessage(208, …, HookMode::Post)`) and the plugin answers each recipient with a
  `GE_SosSetSoundEventParams` (210) carrying the `volume` parameter (`src/sound_volume.cpp`);
* precaching through the s2sdk game-session manifest (`OnBuildGameSessionManifest` + `Precache`),
  re-added on every map load because s2sdk clears its precache set at map end.

## Deliberate differences

1. **Cookies** are read/written through the official permissions plugin under the *same names* the
   Metamod plugin used, keyed by steamid64. The user is requested with `permissions::LoadUser`
   from a once-per-second reconcile (the permissions core knows nothing about a client until
   somebody asks); without the permissions plugin the switches work per session only.
2. **The utils settings-menu toggle** (`utils_settings_menu`, a switch inside the cs2-menus
   settings menu) is not ported: the Plugify menu layer (pisexcompat) has no settings-menu
   extension point. `!qs` behaves exactly as before (menu when `settings_menu 1` or a soundevent
   record exists, direct mute toggle otherwise).
3. The s2sdk sounds are emitted per recipient (`EmitSound` per listener) exactly like the original's
   `IPlayersApi::EmitSound`, so every listener gets his own volume.

## Not ported

* nothing — every config key of the original's file is honoured. (`debug` only controls the
  plugin's debug log lines, as in the original.)

## Needs an in-game check

* the `Alko.Quake7.*` soundevent volume (the SOS parameter byte layout is pinned by the test, but
  only an in-game check shows whether the engine applies it as the Kandru plugin's did);
* `bomb_*` records: the countdown reads `m_flC4Blow` of `planted_c4` through the schema — verify
  the beeps on a real plant.
