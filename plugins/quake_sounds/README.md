# quake_sounds — Quake announcer sounds (Plugify port)

Port of the Metamod:Source plugin `cs2-quake-sounds` (itself a port of Kandru's
CounterStrikeSharp "CS2 QuakeSounds"). Announcer sounds for kill streaks, special kills
(first blood, headshot, knife, team/self kill, last man standing), round events and the C4
countdown, with a per-player mute switch, message language and volume.

The plugin is written directly on the native **s2sdk** API: no `pisexcompat`, no `s2guard` and no
other wrapper library (docs/PLUGIN_STANDARD.md §7). The production KeyValues files are read by the
plugin-local KV1 reader (`src/kv1.cpp`); every safety rule of `handoff/weaponskins/PITFALLS.md`
lives in `src/plugin.cpp` (B1/B2 map gate, C1 handles, C2 client checks, C11 commands, A1/A7
start/teardown, no s2sdk timer).

| | |
|---|---|
| original | `cs2-quake-sounds` (997 LOC of C++) |
| config | `quake_sounds.ini` — the original's configuration file |
| phrases | `translations/quake_sounds.phrases.txt` |
| commands | chat `!qs` (menu), `!lang <code>`; console `mm_qs`, `mm_quakesounds reload\|enable\|disable` |
| cookies | official permissions plugin, names of the Metamod original: `QuakeSounds.Muted`, `QuakeSounds.Lang`, `QuakeSounds.Volume` |
| menu type | optional hudmenu plugin: the player's own `!menu` choice (`chat` / `centerhtml` / `wasd` / `hud`), fallback to the old HUD_LAYOUT default (`hud` when registered, otherwise s2sdk `button`) |
| workshop addons | **3811020435** "Quake 7 Sounds" (`Alko.Quake7.*`, `Alko.Timer.*` soundevents, `sounds/quake7/*.vsnd`, `sounds/timrc4/*.vsnd`). Add it to `csgo/cfg/plugify/multi_addon_manager.cfg` (`s2_extra_addons`). |
| tests | `tests/quake_core_tests.cpp` (engine-free core) and `tests/quake_sounds_plugin_tests.cpp` (the real `src/plugin.cpp` against recording s2sdk fakes) |

## Config / phrase resolution

The direct HEAD paths win; the raw Metamod/addons and data spellings are only fallbacks:

1. `<GetConfigsDir()>/quake_sounds/quake_sounds.ini` (HEAD `FilePath()`);
2. `addons/configs/quake_sounds.ini` (raw cwd fallback);
3. `<GetConfigsDir()>/quake_sounds/translations/quake_sounds.ini`;
4. `<GetDataDir()>/quake_sounds/quake_sounds.ini`.

Phrases: `<GetConfigsDir()>/quake_sounds/translations/quake_sounds.phrases.txt` (HEAD
`TranslationsDir()`), then the raw `addons/translations/quake_sounds.phrases.txt` and the old
pisexcompat full-path probes. A stale raw file can therefore never silently shadow the maintained
Plugify config.

The first existing file is authoritative; a malformed file is reported and the defaults/phrases
stay in place, never silently replaced by a later candidate. The server language is
`<GetConfigsDir()>/core.cfg` `ServerLang`, then `s2sdk::GetServerLanguage()`, then `en`.

## What is ported

* every config key, default and behaviour of the reference (scope filters including the per-record
  `_filter`, the special/weapon/streak priority chain, `headshot_once`, `ignore_bots`,
  `ignore_world_damage`, `count_self_kills`, `count_team_kills`, kill resets, warmup gate, play
  modes `player` / `listener` / `world`, per-record `_volume`, the `play_on_entity` default);
* the menus (`!qs`: mute/unmute, volume in 10 % steps, volume menu only when a soundevent is in
  the config) as native s2sdk menus, the `!lang` per-player message language and the server
  commands; the per-player menu type is asked from the optional hudmenu plugin
  (`GetClientMenuType`, the old wrapper's `!menu` choice); without it the HUD_LAYOUT fallback is
  used (`hud` when the backend is registered, otherwise s2sdk `button`);
* the menu lifecycle is the same fail-safe as the other native ports: the dummy `MenuId 0` is
  reserved before any real menu, every menu creation requires the teardown bindings
  (`DestroyMenu`/`GetClientMenu`/`CancelClientMenu`/`SetMenuType`/`GetMenuType`), a changelevel
  ends a still-displayed menu with `CancelClientMenu(Disconnect)` (no backend close), then
  re-checks `GetClientMenu`; a display that survives that is switched to the builtin no-op-close
  `chat`/`console` type before `DestroyMenu`, and a menu that even then cannot be released is
  parked/retried and finally reported as `A7_UNRESOLVED` — never silently dropped;
* the C4 countdown driven by the real explosion time (`CPlantedC4::m_flC4Blow` minus game time), so
  a duel-extended timer keeps the beeps in sync;
* per-player volume of the soundevents through `GE_SosSetSoundEventParams`, i.e. the `SoundVolume`
  namespace of the original: on the first live map the plugin resolves `SosStartSoundEvent` /
  `SosSetSoundEventParams` / `SosStopSoundEvent` from the s2sdk user-message registry (no hardcoded
  fallback id), hooks the start message and answers each recipient with a params message carrying
  the `volume` parameter (`src/sound_volume.cpp`). The incoming message name and id are verified
  before any field read; an unresolved id disables the corresponding operation instead of touching
  a null/invalid user message;
* precaching through the s2sdk game-session manifest (`OnBuildGameSessionManifest` + `Precache`),
  re-added on every map load because s2sdk clears its precache set at map end;
* game events as `HookMode::Pre` hooks that always answer `Continue`; `player_death.userid` and
  `.attacker` are read with `s2sdk::GetEventPlayerSlot` (the engine's `GetEventInt` returns the raw
  user id, which is not a slot), `weapon` with `GetEventString`, `headshot` with `GetEventBool`;
* kill streaks keyed by slot **and tied to the slot identity** (steam id, or the controller handle
  for bots), so a slot reused by another player never inherits the previous streak/mute/volume;
* `player_disconnect` clears the slot state, `round_start`/`round_end` clear the bomb countdown.

## Deliberate differences

1. **Cookies** are read/written through the official permissions plugin under the *same names* the
   Metamod plugin used, keyed by steamid64. The user is requested with `permissions::LoadUser`
   from a once-per-second reconcile (the permissions core knows nothing about a client until
   somebody asks); without the permissions plugin the switches work per session only.
2. **The utils settings-menu toggle** (`utils_settings_menu`, a switch inside the cs2-menus
   settings menu) is not ported: Plugify has no settings-menu extension point. `!qs` behaves
   exactly as before (menu when `settings_menu 1` or a soundevent record exists, direct mute
   toggle otherwise).
3. The s2sdk sounds are emitted per recipient (`EmitSound` per listener) exactly like the original's
   `IPlayersApi::EmitSound`, so every listener gets his own volume.
4. The optional **hudmenu** plugin is an API dependency, not a wrapper: it is the source of the
   player's `!menu` choice. The plugin declares it optional and falls back as above; the standalone
   repository still has an empty `libs/` (only `sdk/include` is used).
5. Chat output applies the cs2-menus colour tags itself (`qs::Colorize`), because the native
   `s2sdk::PrintToChat` sends the text untouched; centre output uses `PrintCenterText`,
   `PrintAlertText` or `PrintCentreHtml` (3 s) according to `center_message_type`.
6. The plugin owns **no s2sdk timer**: the once-per-second cookie reconcile and the C4 countdown
   are deadline accumulators in `OnPluginUpdate`, so nothing runs inside s2sdk's timer mutex.

## Not ported

* nothing else — every config key of the original's file is honoured. (`debug` only controls the
  plugin's debug log lines, as in the original.)

## Build and tests

`./build.sh plugify-quake_sounds` (monorepo) or the standalone repository's `./build.sh` build the
shared library; `build/tests/quake_sounds_tests` and `build/tests/quake_sounds_plugin_tests` run
without an engine, the second one through recording `__s2sdk_*` / `__permissions_*` fakes. The
tests cover config/phrase/KV1/colour/language/path parity and the runtime kill/streak/world/bot/
slot-reuse, map-gate, precache, menu/command, cookie and map-end/unload scenarios.

## Needs an in-game check

* the `Alko.Quake7.*` soundevent volume (the SOS parameter byte layout is pinned by the test, but
  only an in-game check shows whether the engine applies it as the Kandru plugin's did);
* `bomb_*` records: the countdown reads `m_flC4Blow` of `planted_c4` through the schema — verify
  the beeps on a real plant.
