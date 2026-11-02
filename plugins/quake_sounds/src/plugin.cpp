// quake_sounds — Plugify C++ port of the Metamod:Source plugin cs2-quake-sounds
// (itself a port of Kandru's CounterStrikeSharp "CS2 QuakeSounds"): announcer sounds for kill
// streaks, special kills, round events and the C4 countdown.
//
// Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no s2guard and no other wrapper
// library. Every safety rule of handoff/weaponskins/PITFALLS.md lives in this file:
//
//   * B1/B2 map gate: no client/entity/schema call before the first map or between OnMapEnd and
//     the next OnMapStart. The OnMapStart/OnMapEnd listeners are registered in OnPluginStart;
//     OnMapStart opens the gate, OnMapEnd only clears state (no client query, PITFALLS B2) and the
//     first OnPluginUpdate of a plugin loaded into a running map opens it when GetCurrentMap()
//     proves that a map exists and no OnMapEnd was seen. Every client/entity/schema path starts
//     with the gate check.
//   * C2 client checks: 0 <= slot < 64, IsClientConnected + IsClientInGame + !IsClientSourceTV,
//     a non-null PlayerSlotToEntPointer and a valid PlayerSlotToEntHandle. GetClientSteamID64 is
//     read only after those checks (bots have none).
//   * C1 entity handles: a CEntityHandle is a uint32 view, so a negative value is a perfectly
//     valid handle once the serial counter passes 65535. The test is `!= -1 && != 0`, never
//     `> 0`; 0 is the world and is handled explicitly where the original emitted from worldent.
//   * C11 commands: "qs"/"lang" answer only the Chat context, "mm_qs" only Console and
//     "mm_quakesounds" only the server console (caller -1). Chat "!qs"/"/qs" reaches the same
//     callback without the trigger, so the registered name is the trigger-less spelling. The
//     ownership returned by AddConsoleCommand decides the teardown: RemoveCommand only for a
//     command this plugin created, RemoveCommandListener for a pre-existing name (C11/A7).
//   * A7 teardown: OnPluginEnd removes every command, unhooks all game events, unregisters the
//     map, manifest and user-message listeners, destroys every menu this plugin created and drops
//     the state, so `plg reload` leaves nothing pointing into the unloaded library. Menu teardown
//     follows the proven fail-safe: MenuId 0 is reserved before any real menu, all teardown
//     bindings (DestroyMenu/GetClientMenu/CancelClientMenu/SetMenuType/GetMenuType) are required
//     before any Create/DisplayMenu, a changelevel ends a display with CancelClientMenu(Disconnect)
//     and re-checks GetClientMenu; a surviving map-down display is switched to the builtin
//     no-op-close chat/console type (verified with GetMenuType) before DestroyMenu, and anything
//     that still cannot be released is parked/retried and finally reported as A7_UNRESOLVED.
//   * B4/C9: this plugin owns no s2sdk timer. The once-per-second cookie reconcile and the C4
//     countdown are deadline accumulators driven by OnPluginUpdate, so no action ever runs inside
//     s2sdk's timer mutex.
//   * A1: OnPluginStart never returns Failed; a missing binding or config only logs and disables
//     the part that needs it.
//
// Porting notes (the rest is parity with ref/src/cs2-quake-sounds):
//   * config and phrases keep the Metamod names/formats and are read by the plugin-local KV1
//     reader (src/kv1.cpp). The direct HEAD paths
//     <GetConfigsDir()>/quake_sounds/quake_sounds.ini and
//     <GetConfigsDir()>/quake_sounds/translations/quake_sounds.phrases.txt come first; the raw
//     "addons/configs/..." / "addons/translations/..." spellings and the data dir are only
//     fallbacks (see src/quake_config.hpp). A stale raw file cannot shadow the maintained Plugify
//     config.
//   * the server language is <configs>/core.cfg "ServerLang" -> s2sdk::GetServerLanguage -> "en"
//     (qs::PickLanguage), detected when the phrases are loaded.
//   * game events are Pre hooks removed with UnhookEvent; "userid" and "attacker" are read with
//     s2sdk::GetEventPlayerSlot (the engine's GetEventInt answers the raw user id, which is not a
//     slot). "weapon" is GetEventString and "headshot" GetEventBool.
//   * kill streaks are keyed by slot like the original, but tied to the slot identity (steam id +
//     controller handle, the handle carrying the entity serial; bots by handle): a
//     disconnected/replaced player can never inherit the previous occupant's streak/state. The
//     player_disconnect event and the cookie reconcile reset it. A same-steam reload of the
//     controller entity (map change) keeps the per-player settings.
//   * the menus honour the optional hudmenu plugin's per-player "!menu" choice (chat,
//     centerhtml, button, hud, wasd; hud/wasd only while registered) and fall back to the old
//     HUD_LAYOUT default ("hud" when registered, otherwise s2sdk "button"). Every deferred menu
//     action is bound to the steam id + controller handle captured when the menu was displayed,
//     not to whoever occupies the slot at click/update time.
//   * player mute/language/volume come from the official permissions cookies under the original
//     Metamod names (QuakeSounds.Muted / QuakeSounds.Lang / QuakeSounds.Volume); the user is
//     loaded with permissions::LoadUser from the once-per-second reconcile (the core knows nothing
//     about a client until somebody asks), exactly like the previous port.
//   * the utils settings-menu extension point is NOT ported (Plugify has no such hook). The
//     command behaviour of the original is kept: !qs opens the s2sdk menu when settings_menu is 1
//     or when a soundevent record exists, otherwise it toggles mute directly.
//   * per-player volume of the started soundevents is applied through GE_SosSetSoundEventParams,
//     exactly like the original's SoundVolume module (src/sound_volume.cpp).
//   * resources are precached from the s2sdk game-session manifest callback on every map load
//     (s2sdk clears its precache set at map end).
//   * chat output applies the cs2-menus colour tags itself (qs::Colorize) because the native
//     s2sdk PrintToChat sends the text untouched; center output uses PrintCenterText /
//     PrintAlertText / PrintCentreHtml (3 s) according to center_message_type.
//
// Everything runs on the game thread, no exception escapes into s2sdk.

#include <plg/any.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <hudmenu/hudmenu.hpp>
#include <permissions/permissions.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quake_config.hpp"
#include "quake_core.hpp"
#include "sound_volume.hpp"

namespace {

constexpr const char* kLogTag = "[QuakeSounds]";
constexpr int kMaxSlots = 64;
constexpr int kCenterHtmlSeconds = 3;
constexpr std::size_t kMaxQueuedActions = 256;
constexpr std::size_t kMaxQueuedMenuDestroys = 256;
constexpr double kReconcileIntervalSeconds = 1.0;

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

qs::Config g_Cfg;
std::map<std::string, qs::SoundEntry> g_mapEntries;
std::map<std::string, std::string> g_mapPhrases;
std::string g_sServerLang = "en";
std::map<uint32_t, float> g_mapOwnEvents; // soundevent hash -> record volume (1.0 when unset)

bool g_bMuted[kMaxSlots] = {};
int g_iVolume[kMaxSlots] = {}; // percent; soundevents only
std::string g_sLang[kMaxSlots];
std::map<int, int> g_mapKills; // slot -> kill streak (the entry's existence is what first blood checks)

// The identity of the player currently occupying a slot. It is the steam id *and* the controller
// handle (the handle carries the entity serial), so a slot reused by another player - even by a
// player with the same steam id but a new controller entity - never inherits the previous
// occupant's streak/mute/volume and can never apply a deferred menu click of that occupant.
struct PlayerIdentity
{
	uint64_t uSteam = 0;
	int32_t iController = 0;
	bool bValid = false;
};

bool SameIdentity(const PlayerIdentity& a, const PlayerIdentity& b)
{
	return a.bValid && b.bValid && a.uSteam == b.uSteam && a.iController == b.iController;
}

PlayerIdentity g_slotIdentity[kMaxSlots] = {};
uint64_t g_uLoadedSteam[kMaxSlots] = {};
float g_flReconcileSeconds = 0.0f;

std::mt19937 g_Rng{std::random_device{}()};

// Actions are always drained from OnPluginUpdate, never from an s2sdk callback (menu handlers run
// under the menu manager's lock and timer callbacks under the timer mutex). A queued action is
// bound to the slot identity captured when it was queued, so a click from the last frame of a map
// or from a player who left cannot hit the next occupant.
struct PendingAction
{
	int iSlot = -1;
	PlayerIdentity identity;
	std::function<void()> fn;
};
std::deque<PendingAction> g_vActions;

// Menus are created by this plugin and destroyed from OnPluginUpdate (outside the handler lock).
struct MenuSlot
{
	s2sdk::MenuId id = 0; // MenuId 0 is a valid id: `valid` tells a live menu from an empty slot
	bool valid = false;
	PlayerIdentity identity;
};
MenuSlot g_menus[kMaxSlots];
std::deque<s2sdk::MenuId> g_vMenuDestroy;
std::vector<s2sdk::MenuId> g_vParkedMenus; // map-down failures, retried while the plugin is loaded
s2sdk::MenuId g_reservedMenuId = 0;       // the dummy menu that burns s2sdk's id 0
bool g_hasReservedMenu = false;
bool g_bMenuUnavailableLogged = false;

// Map gate (PITFALLS B1/B2).
bool g_bStarted = false;     // OnPluginStart finished (OnPluginUpdate may run)
bool g_mapReady = false;     // client/entity/schema calls are safe right now
bool g_mapEndSeen = false;   // an OnMapEnd arrived since OnPluginStart
bool g_mapListeners = false;   // both map listeners are registered
bool g_mapStartAttempted = false; // the call was made: teardown must try to remove it
bool g_mapEndAttempted = false;
bool g_mapStartRegistered = false;
bool g_mapEndRegistered = false;

// C4 countdown (the reference's BombFrame).
bool g_bBombActive = false;
int g_iBombLast = INT32_MAX;
int32_t g_iBombHandle = -1; // CEntityHandle of the planted C4 (-1 = not resolved yet)
int32_t g_iBlowOffset = -1; // CPlantedC4::m_flC4Blow, resolved lazily with the gate open
int32_t g_iWarmupOffset = -1;
float g_flFallbackTime = 0.0f; // only when the host does not export GetGameTime (tests)

int64_t g_iPluginId = 0;
bool g_bManifestRegistered = false;
bool g_bSoundAvailable = false;
bool g_bSoundResolveAttempted = false;
bool g_bSoundStartMissingLogged = false;
bool g_bSoundSetMissingLogged = false;
int32_t g_iHookedStartId = -1;
int32_t g_iFailedStartHookId = -1;

// Command/menu/event registrations (see the teardown functions at the bottom).
struct CommandRegistration
{
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
	bool attempted = false;
	bool owned = false;
};
std::vector<CommandRegistration> g_commands;

struct EventRegistration
{
	const char* name = nullptr;
	s2sdk::EventCallback callback = nullptr;
	bool attempted = false;
	bool hooked = false;
};

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

std::string ToStd(const plg::string& s) { return std::string(s.data(), s.size()); }
plg::string ToPlg(std::string_view s) { return plg::string(s); }

void LogMessage(const std::string& sText, plg::Severity eSeverity)
{
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + sText, eSeverity, plg::source_location::current());
}

void Log(const std::string& sText) { LogMessage(sText, plg::Severity::Info); }
void LogWarning(const std::string& sText) { LogMessage(sText, plg::Severity::Warning); }
void LogError(const std::string& sText) { LogMessage(sText, plg::Severity::Error); }

void Debug(const std::string& sText)
{
	if (g_Cfg.bDebug) Log("[DEBUG] " + sText);
}

void ServerPrint(const std::string& sText)
{
	if (__s2sdk_PrintToServer == nullptr) return;
	try { s2sdk::PrintToServer(ToPlg(sText)); } catch (...) {}
}

s2sdk::ConVarFlag CombineFlags(s2sdk::ConVarFlag a, s2sdk::ConVarFlag b)
{
	return static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(a) | static_cast<int64_t>(b));
}

std::string Phrase(const std::string& sKey)
{
	return qs::Phrase(g_mapPhrases, g_sServerLang, sKey);
}

// ---------------------------------------------------------------------------------------------
// Map gate (PITFALLS B1/B2)
// ---------------------------------------------------------------------------------------------

void ResetPlayerState(int iSlot);
void ResetAllState();
void TeardownMenusOnMapEnd();
void TeardownMenusOnUnload();
void RetryParkedMenus();
void CloseMenuForSlot(int iSlot, bool bNotifyBackend);
void ShowMenu(int iSlot);
void ShowVolumeMenu(int iSlot);
void EnsureSoundHook();
void ToggleMute(int iSlot);
void BombStop();

void ResetMapState()
{
	// OnMapStart: the previous map's streaks and bomb are gone. The per-player mute/volume/
	// language survive a map change, exactly like the original (the players stay connected).
	g_mapKills.clear();
	BombStop();
	g_vActions.clear();
	g_vMenuDestroy.clear();
	g_flReconcileSeconds = 0.0f;
	g_flFallbackTime = 0.0f;
}

void OnMapStartGame()
{
	try
	{
		g_mapReady = true;
		g_mapEndSeen = false;
		ResetMapState();
		EnsureSoundHook(); // lazy first live map: ResolveIds + the start hook
	}
	catch (...)
	{
	}
}

void OnMapEndGame()
{
	try
	{
		// From here until the next OnMapStart s2sdk's client list is torn down and any client
		// query crashes. The gate is dropped first; only state is cleared afterwards. Menu
		// teardown uses the Disconnect path, which skips the client-facing backend close
		// window (CancelClientMenu(Disconnect) -> no backend close; a still-displayed menu gets
		// the builtin no-op-close type before DestroyMenu, or is parked / A7_UNRESOLVED).
		g_mapReady = false;
		g_mapEndSeen = true;
		TeardownMenusOnMapEnd();
		g_mapKills.clear();
		BombStop();
		g_vActions.clear();
		g_vMenuDestroy.clear();
		g_flReconcileSeconds = 0.0f;
	}
	catch (...)
	{
	}
}

void UnregisterMapListeners();

void RegisterMapListeners()
{
	if (g_mapListeners) return;
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapStart_Unregister == nullptr ||
		__s2sdk_OnMapEnd_Register == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr)
	{
		// Without the map listeners nothing could ever close the gate again, so it stays closed
		// (fail safe, never Failed - PITFALLS A1/B2).
		LogError("the s2sdk map listeners are unavailable - quake sounds stay off");
		return;
	}
	// Track each registration separately: if the second one fails, the first callback still has
	// to be removed on unload or it would point into the unloaded library.
	// Mark the attempt before the call: if s2sdk registered the callback and then threw, the
	// unregister below still has to remove it. Only a successful call counts as registered, so a
	// half-registered pair keeps the gate shut.
	g_mapStartAttempted = true;
	try
	{
		s2sdk::OnMapStart_Register(&OnMapStartGame);
		g_mapStartRegistered = true;
	}
	catch (...)
	{
		LogError("could not register the s2sdk OnMapStart listener - quake sounds stay off");
	}
	g_mapEndAttempted = true;
	try
	{
		s2sdk::OnMapEnd_Register(&OnMapEndGame);
		g_mapEndRegistered = true;
	}
	catch (...)
	{
		LogError("could not register the s2sdk OnMapEnd listener - quake sounds stay off");
	}
	g_mapListeners = g_mapStartRegistered && g_mapEndRegistered;
	if (!g_mapListeners)
	{
		// A half-registered pair cannot close the gate safely; remove what slipped through.
		UnregisterMapListeners();
	}
}

void UnregisterMapListeners()
{
	g_mapListeners = false;
	if (g_mapStartAttempted && __s2sdk_OnMapStart_Unregister != nullptr)
	{
		try { s2sdk::OnMapStart_Unregister(&OnMapStartGame); } catch (...) {}
	}
	g_mapStartAttempted = false;
	g_mapStartRegistered = false;
	if (g_mapEndAttempted && __s2sdk_OnMapEnd_Unregister != nullptr)
	{
		try { s2sdk::OnMapEnd_Unregister(&OnMapEndGame); } catch (...) {}
	}
	g_mapEndAttempted = false;
	g_mapEndRegistered = false;
}

// A plugin loaded (`plg reload`) into a running map never saw OnMapStart. GetCurrentMap() is the
// one map call that is safe before the first map (it returns "" when gpGlobals is null), so the
// first update that can prove a map exists opens the gate. After OnMapEnd only a real OnMapStart
// re-opens it (the host keeps pumping updates through a changelevel - PITFALLS B2/B3).
void PollMapGate()
{
	if (g_mapReady || g_mapEndSeen || !g_mapListeners) return;
	if (__s2sdk_GetCurrentMap == nullptr) return;
	try
	{
		if (!s2sdk::GetCurrentMap().empty())
		{
			g_mapReady = true;
			}
	}
	catch (...)
	{
	}
}

// ---------------------------------------------------------------------------------------------
// Client queries (all of them need the map gate)
// ---------------------------------------------------------------------------------------------

bool ValidSlot(int iSlot) { return iSlot >= 0 && iSlot < kMaxSlots; }

bool HasController(int iSlot)
{
	if (!g_mapReady || !ValidSlot(iSlot)) return false;
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr)
	{
		return false;
	}
	try
	{
		if (!s2sdk::IsClientConnected(iSlot) || !s2sdk::IsClientInGame(iSlot) ||
			s2sdk::IsClientSourceTV(iSlot))
		{
			return false;
		}
		if (__s2sdk_PlayerSlotToEntPointer != nullptr)
		{
			void* pController = s2sdk::PlayerSlotToEntPointer(iSlot);
			if (pController == nullptr) return false;
			if (__s2sdk_IsValidEntPointer != nullptr && !s2sdk::IsValidEntPointer(pController))
			{
				return false;
			}
		}
		if (__s2sdk_PlayerSlotToEntHandle != nullptr)
		{
			const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
			if (!qs::IsValidHandle(iHandle)) return false;
			if (__s2sdk_IsValidEntHandle != nullptr && !s2sdk::IsValidEntHandle(iHandle))
			{
				return false;
			}
		}
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool IsBotSlot(int iSlot)
{
	if (!HasController(iSlot)) return true;
	if (__s2sdk_IsFakeClient == nullptr) return false;
	try { return s2sdk::IsFakeClient(iSlot); } catch (...) { return true; }
}

int TeamOf(int iSlot)
{
	if (!HasController(iSlot) || __s2sdk_GetClientTeam == nullptr) return 0;
	try { return static_cast<int>(s2sdk::GetClientTeam(iSlot)); } catch (...) { return 0; }
}

bool IsAliveSlot(int iSlot)
{
	if (!HasController(iSlot) || __s2sdk_IsClientAlive == nullptr) return false;
	try { return s2sdk::IsClientAlive(iSlot); } catch (...) { return false; }
}

std::string NameOf(int iSlot)
{
	if (!HasController(iSlot) || __s2sdk_GetClientName == nullptr) return "?";
	try
	{
		const std::string name = ToStd(s2sdk::GetClientName(iSlot));
		// PITFALLS C2: "<unknown>" is the engine's placeholder, not a player name.
		if (name.empty() || name == "<unknown>") return "?";
		return name;
	}
	catch (...)
	{
		return "?";
	}
}

uint64_t ClientSteamID(int iSlot)
{
	if (!HasController(iSlot) || __s2sdk_GetClientSteamID64 == nullptr) return 0;
	try { return s2sdk::GetClientSteamID64(iSlot); } catch (...) { return 0; }
}

// The pawn's CEntityHandle, or 0 (= the world, as in the original's CEntityIndex(0) fallback)
// when the slot has no readable pawn/handle right now.
int32_t PawnHandle(int iSlot)
{
	if (!HasController(iSlot) || __s2sdk_GetClientPawn == nullptr) return 0;
	try
	{
		void* pPawn = s2sdk::GetClientPawn(iSlot);
		if (pPawn == nullptr) return 0;
		int32_t iHandle = -1;
		if (__s2sdk_EntPointerToEntHandle != nullptr)
		{
			iHandle = s2sdk::EntPointerToEntHandle(pPawn);
		}
		else if (__s2sdk_EntPointerToEntIndex != nullptr && __s2sdk_EntIndexToEntHandle != nullptr)
		{
			iHandle = s2sdk::EntIndexToEntHandle(s2sdk::EntPointerToEntIndex(pPawn));
		}
		return qs::IsValidHandle(iHandle) ? iHandle : 0;
	}
	catch (...)
	{
		return 0;
	}
}

// Human: steam id + controller handle. Bot: controller handle only (steam id 0). The controller
// handle is mandatory: it carries the entity serial, so the same steam id on a new controller
// entity (a reconnected/reused slot) is a different identity. Unknown identity never resets state.
PlayerIdentity SlotIdentity(int iSlot)
{
	PlayerIdentity identity;
	if (!HasController(iSlot)) return identity;
	if (__s2sdk_PlayerSlotToEntHandle == nullptr) return identity;
	int32_t iHandle = 0;
	try { iHandle = s2sdk::PlayerSlotToEntHandle(iSlot); } catch (...) { return identity; }
	if (!qs::IsValidHandle(iHandle)) return identity;
	identity.iController = iHandle;
	if (!IsBotSlot(iSlot))
	{
		const uint64_t uSteam = ClientSteamID(iSlot);
		if (!uSteam) return identity;
		identity.uSteam = uSteam;
	}
	identity.bValid = true;
	return identity;
}

// Called before a kill is counted and by the cookie reconcile: a slot whose identity changed
// starts from scratch (streak, mute, language and volume of the previous occupant are dropped).
void SyncSlotIdentity(int iSlot)
{
	const PlayerIdentity identity = SlotIdentity(iSlot);
	if (!identity.bValid) return;
	const PlayerIdentity previous = g_slotIdentity[iSlot];
	g_slotIdentity[iSlot] = identity; // refresh the controller serial even when the state is kept
	if (!previous.bValid) return;
	// For a human the per-slot state belongs to the steam id: a map change recreates the
	// controller entity (new serial) but must not wipe mute/volume/language. A different steam id
	// (or a human/bot switch) resets. Bots are identified by their controller handle.
	const bool bSamePerson = (previous.uSteam != 0 || identity.uSteam != 0)
								 ? previous.uSteam == identity.uSteam
								 : previous.iController == identity.iController;
	if (bSamePerson) return;
	g_mapKills.erase(iSlot);
	ResetPlayerState(iSlot);
	g_uLoadedSteam[iSlot] = 0;
}

// ---------------------------------------------------------------------------------------------
// Game event reads (the event pointer only lives inside the callback)
// ---------------------------------------------------------------------------------------------

int32_t EventPlayerSlot(void* pEvent, const char* szKey)
{
	if (pEvent == nullptr || szKey == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) return -1;
	try { return s2sdk::GetEventPlayerSlot(pEvent, ToPlg(szKey)); } catch (...) { return -1; }
}

std::string EventString(void* pEvent, const char* szKey)
{
	if (pEvent == nullptr || szKey == nullptr || __s2sdk_GetEventString == nullptr) return {};
	try { return ToStd(s2sdk::GetEventString(pEvent, ToPlg(szKey))); } catch (...) { return {}; }
}

bool EventBool(void* pEvent, const char* szKey)
{
	if (pEvent == nullptr || szKey == nullptr || __s2sdk_GetEventBool == nullptr) return false;
	try { return s2sdk::GetEventBool(pEvent, ToPlg(szKey)); } catch (...) { return false; }
}

// ---------------------------------------------------------------------------------------------
// Player state / config (the reference's ResetPlayer/LoadPlayer/ToggleMute)
// ---------------------------------------------------------------------------------------------

void ResetPlayerState(int iSlot)
{
	if (!ValidSlot(iSlot)) return;
	g_bMuted[iSlot] = false;
	g_iVolume[iSlot] = g_Cfg.iDefaultVolume;
	g_sLang[iSlot].clear();
}

std::string ConfigsRoot()
{
	return plg::GetConfigsDir != nullptr ? ToStd(plg::GetConfigsDir()) : std::string();
}

std::string DataRoot()
{
	return plg::GetDataDir != nullptr ? ToStd(plg::GetDataDir()) : std::string();
}

std::string SdkServerLanguage()
{
	if (__s2sdk_GetServerLanguage == nullptr) return {};
	try { return ToStd(s2sdk::GetServerLanguage()); } catch (...) { return {}; }
}

// <configs>/core.cfg "ServerLang" -> s2sdk ServerLanguage -> "en", exactly the order of the
// Pisex core the original read (see qs::PickLanguage).
std::string DetectServerLanguage()
{
	return qs::PickLanguage(qs::ReadCoreConfigLanguage(ConfigsRoot()), SdkServerLanguage());
}

void RebuildOwnEvents()
{
	g_mapOwnEvents.clear();
	for (const auto& [sKey, e] : g_mapEntries)
		for (const auto& sSound : e.vSounds)
			if (!qs::IsFilePathSound(sSound)) g_mapOwnEvents[qs::SoundEventHash(sSound)] = qs::EntryVolume(e);
}

void LoadConfig()
{
	// The original resets Config/entries before LoadFromFile and leaves the defaults behind when
	// the file cannot be read; a malformed file is reported, never silently masked.
	g_Cfg = qs::Config{};
	g_mapEntries.clear();
	g_mapOwnEvents.clear();

	const qs::ConfigLoad result = qs::LoadConfigFromDisk(ConfigsRoot(), DataRoot(), g_Cfg, g_mapEntries);
	if (!result.loaded)
	{
		LogError(result.error + " - keeping the defaults");
		return;
	}
	RebuildOwnEvents();
	Log("loaded " + result.path + ": " + std::to_string(g_mapEntries.size()) + " sound records, " +
		std::to_string(g_Cfg.vPrecache.size()) + " precache files");
}

void LoadPhrases()
{
	g_mapPhrases.clear();
	g_sServerLang = DetectServerLanguage();
	const qs::PhrasesLoad result = qs::LoadPhrasesFromDisk(ConfigsRoot(), g_sServerLang, g_mapPhrases);
	if (!result.loaded)
	{
		LogError("phrases: " + result.error + " - using the built-in phrases");
		return;
	}
	Log("phrases: " + result.path + " (" + g_sServerLang + ", " + std::to_string(g_mapPhrases.size()) + " entries)");
}

// ---------------------------------------------------------------------------------------------
// Cookies (official permissions plugin, names identical to the Pisex ones)
// ---------------------------------------------------------------------------------------------

bool CookieRead(uint64_t uSteamID64, const char* szName, std::string& sOut)
{
	if (!uSteamID64 || __permissions_GetCookie == nullptr) return false;
	try
	{
		plg::any value;
		if (permissions::GetCookie(uSteamID64, ToPlg(szName), value) != permissions::Status::Success) return false;
		if (plg::holds_alternative<plg::string>(value))
		{
			const plg::string& s = plg::get<plg::string>(value);
			sOut.assign(s.data(), s.size());
			return true;
		}
		if (plg::holds_alternative<int32_t>(value)) { sOut = std::to_string(plg::get<int32_t>(value)); return true; }
		if (plg::holds_alternative<int64_t>(value)) { sOut = std::to_string(plg::get<int64_t>(value)); return true; }
		if (plg::holds_alternative<bool>(value)) { sOut = plg::get<bool>(value) ? "1" : "0"; return true; }
		return false;
	}
	catch (...)
	{
		return false;
	}
}

void CookieWrite(uint64_t uSteamID64, const char* szName, const std::string& sValue)
{
	if (!uSteamID64 || __permissions_SetCookie == nullptr) return;
	try
	{
		permissions::SetCookie(g_iPluginId, uSteamID64, ToPlg(szName), plg::any(plg::string(sValue)),
							   /*dontBroadcast=*/false);
	}
	catch (...)
	{
	}
}

void LoadPlayer(int iSlot, uint64_t uSteamID64)
{
	if (!ValidSlot(iSlot)) return;
	ResetPlayerState(iSlot);
	if (!uSteamID64) return;

	std::string sValue;
	if (CookieRead(uSteamID64, "QuakeSounds.Muted", sValue)) g_bMuted[iSlot] = atoi(sValue.c_str()) != 0;
	if (CookieRead(uSteamID64, "QuakeSounds.Lang", sValue)) g_sLang[iSlot] = sValue;
	if (CookieRead(uSteamID64, "QuakeSounds.Volume", sValue))
		g_iVolume[iSlot] = std::clamp(atoi(sValue.c_str()), 0, 100);
}

// The permissions core only knows a user after somebody asked for him (LoadUser -> OnUserRequest
// -> the storage plugin fills the record). Metamod's ICookiesApi did that implicitly.
void ReconcileCookies(float flDelta)
{
	if (__permissions_LoadUser == nullptr) return;
	g_flReconcileSeconds += flDelta;
	if (g_flReconcileSeconds < static_cast<float>(kReconcileIntervalSeconds)) return;
	g_flReconcileSeconds = 0.0f;

	for (int i = 0; i < kMaxSlots; i++)
	{
		if (!HasController(i) || IsBotSlot(i))
		{
			if (g_uLoadedSteam[i])
			{
				g_uLoadedSteam[i] = 0;
				ResetPlayerState(i);
			}
			continue;
		}
		const uint64_t uSteam = ClientSteamID(i);
		if (!uSteam)
		{
			if (g_uLoadedSteam[i])
			{
				g_uLoadedSteam[i] = 0;
				ResetPlayerState(i);
			}
			continue;
		}
		if (g_uLoadedSteam[i] == uSteam) continue;

		SyncSlotIdentity(i);
		// A slot reused by another player must not keep the previous occupant's settings while
		// LoadUser is in flight (or failing).
		if (g_uLoadedSteam[i] != 0) ResetPlayerState(i);
		g_uLoadedSteam[i] = 0;

		const std::string sName = NameOf(i);
		try
		{
			if (permissions::LoadUser(g_iPluginId, uSteam, ToPlg(sName), /*offline=*/false,
									  /*dontBroadcast=*/false) == permissions::Status::Success)
			{
				g_uLoadedSteam[i] = uSteam;
				LoadPlayer(i, uSteam);
			}
		}
		catch (...)
		{
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------------------------

bool IsListener(int iSlot)
{
	// bots hear nothing; ignore_bots only decides whether their kills count
	if (!HasController(iSlot) || IsBotSlot(iSlot)) return false;
	return !g_bMuted[iSlot];
}

qs::Scope EntryScope(const qs::SoundEntry& e, bool bGlobal)
{
	return e.bHasScope ? e.eScope : (bGlobal ? qs::Scope::All : g_Cfg.eScope);
}

std::vector<int> Recipients(int iAttacker, int iVictim, const qs::SoundEntry& e, bool bGlobal = false)
{
	const qs::Scope eScope = EntryScope(e, bGlobal);
	// The two teams do not change while the listener list is built: looked up once instead of once
	// per slot.
	const int iAttackerTeam = TeamOf(iAttacker);
	const int iVictimTeam = TeamOf(iVictim);
	std::vector<int> v;
	for (int i = 0; i < kMaxSlots; i++)
		if (IsListener(i) && qs::MatchesScope(i, TeamOf(i), iAttacker, iAttackerTeam, iVictim, iVictimTeam, eScope))
			v.push_back(i);
	return v;
}

const std::string& PickSound(const qs::SoundEntry& e)
{
	if (e.vSounds.size() == 1) return e.vSounds[0];
	return e.vSounds[qs::PickSoundIndex(e.vSounds.size(), (uint32_t)g_Rng())];
}

void Emit(int iSource, const std::string& sSound, const std::vector<int>& vTo, const std::string& sPlayOn, float flVolume)
{
	if (vTo.empty()) return;

	// a sounds/... path is played by each client itself (no positioning), like the original
	if (qs::IsFilePathSound(sSound))
	{
		Debug("play " + sSound + " via client command to " + std::to_string(vTo.size()) + " players");
		if (__s2sdk_ClientCommand == nullptr) return;
		for (int i : vTo)
		{
			try { s2sdk::ClientCommand(i, ToPlg("play " + sSound)); } catch (...) {}
		}
		return;
	}

	const float flVol = flVolume >= 0.0f ? flVolume : 1.0f;
	if (__s2sdk_EmitSound == nullptr) return;

	if (sPlayOn == "listener")
	{
		// "listener": every listener hears it on himself (the announcer is equally loud everywhere)
		for (int i : vTo)
		{
			const float flPlayer = qs::ListenerVolume(flVol, g_iVolume[i]);
			if (flPlayer <= 0.0f) continue;
			// The original emitted from the world entity when the listener had no pawn (CEntityIndex(0)).
			const int32_t iHandle = PawnHandle(i);
			try { s2sdk::EmitSound(iHandle, ToPlg(sSound), 100, flPlayer, 0.0f); } catch (...) {}
		}
		Debug("emit " + sSound + " on listeners (" + std::to_string(vTo.size()) + " players)");
		return;
	}

	// "player": from the killer's pawn; "world" (or no source, or an unknown play mode): from the
	// world entity. The world's handle is 0 - a valid CEntityHandle even though qs::IsValidHandle
	// reserves 0 for "none"; the pawn helper falls back to 0 when no pawn is readable.
	const bool bWorld = !(sPlayOn == "player" && iSource >= 0);
	const int32_t iHandle = bWorld ? 0 : PawnHandle(iSource);
	Debug("emit " + sSound + " on ent " + std::to_string(iHandle) + " to " + std::to_string(vTo.size()) + " players");
	for (int i : vTo)
	{
		const float flPlayer = qs::ListenerVolume(flVol, g_iVolume[i]);
		if (flPlayer <= 0.0f) continue;
		// every listener has his own volume: one emission per player, like the original.
		try { s2sdk::EmitSound(iHandle, ToPlg(sSound), 100, flPlayer, 0.0f); } catch (...) {}
	}
}

void ChatToSlot(int iSlot, const std::string& sText)
{
	if (!g_mapReady || !HasController(iSlot) || __s2sdk_PrintToChat == nullptr) return;
	try { s2sdk::PrintToChat(iSlot, ToPlg(qs::Colorize(sText))); } catch (...) {}
}

void Announce(int iSource, const qs::SoundEntry& e, const std::vector<int>& vTo)
{
	if ((!g_Cfg.bCenter && !g_Cfg.bChat) || e.mapMessages.empty()) return;
	for (int i : vTo)
	{
		if (IsBotSlot(i)) continue;
		const std::string sMsg = qs::ResolveMessage(e, g_sLang[i], g_sServerLang);
		if (sMsg.empty()) continue;
		const bool bSelf = i == iSource;
		const std::string sSource = iSource >= 0 ? NameOf(iSource) : "";
		if (g_Cfg.bCenter)
		{
			const std::string s = qs::Replace(qs::Replace(Phrase(bSelf ? "Center_Msg_Player" : "Center_Msg_Other"),
														  "{player}", sSource), "{message}", sMsg);
			try
			{
				if (g_Cfg.sCenterType == "alert")
				{
					if (__s2sdk_PrintAlertText != nullptr) s2sdk::PrintAlertText(i, ToPlg(s));
				}
				else if (g_Cfg.sCenterType == "html")
				{
					if (__s2sdk_PrintCentreHtml != nullptr) s2sdk::PrintCentreHtml(i, ToPlg(s), kCenterHtmlSeconds);
				}
				else if (__s2sdk_PrintCenterText != nullptr)
				{
					s2sdk::PrintCenterText(i, ToPlg(s));
				}
			}
			catch (...)
			{
			}
		}
		if (g_Cfg.bChat)
		{
			const std::string s = qs::Replace(qs::Replace(Phrase(bSelf ? "Chat_Msg_Player" : "Chat_Msg_Other"),
														  "{player}", sSource), "{message}", sMsg);
			ChatToSlot(i, s);
		}
	}
}

// Plays the record with that key. true - the record exists (even when nobody would hear it: the
// priority chain treats that as "played", like the original).
bool Play(int iAttacker, int iVictim, const std::string& sKey, int iSource = -1, const std::string& sPlayOn = "")
{
	if (!g_Cfg.bEnabled) return false;
	auto it = g_mapEntries.find(sKey);
	if (it == g_mapEntries.end()) return false;
	const qs::SoundEntry& e = it->second;
	std::vector<int> vTo = Recipients(iAttacker, iVictim, e);
	if (vTo.empty()) return true;
	const int iFrom = iSource >= 0 ? iSource : iAttacker;
	Emit(iFrom, PickSound(e), vTo, sPlayOn.empty() ? g_Cfg.sPlayOn : sPlayOn, e.flVolume);
	Announce(iFrom, e, vTo);
	return true;
}

// Round and bomb records: once per listener, on himself or on the world.
void PlayGlobal(const std::string& sKey)
{
	if (!g_Cfg.bEnabled) return;
	auto it = g_mapEntries.find(sKey);
	if (it == g_mapEntries.end()) return;
	const qs::SoundEntry& e = it->second;
	std::vector<int> vTo = Recipients(-1, -1, e, true);
	if (vTo.empty()) return;
	const std::string& sSound = PickSound(e);
	if (g_Cfg.sPlayOn == "player" || g_Cfg.sPlayOn == "listener")
		Emit(-1, sSound, vTo, "listener", e.flVolume);
	else
		Emit(-1, sSound, vTo, "world", e.flVolume);
	Debug("global " + sKey + " -> " + std::to_string(vTo.size()) + " players");
}

// ---------------------------------------------------------------------------------------------
// Kill handling (the reference's OnPlayerDeath)
// ---------------------------------------------------------------------------------------------

bool TryLastManStanding(int iAttacker, int iVictim)
{
	const int iTeam = TeamOf(iVictim);
	if (iAttacker < 0 || (iTeam != 2 && iTeam != 3)) return false;
	int iLast = -1, nAlive = 0;
	for (int i = 0; i < kMaxSlots; i++)
	{
		if (i == iVictim || TeamOf(i) != iTeam || !IsAliveSlot(i)) continue;
		nAlive++;
		iLast = i;
	}
	return nAlive == 1 && Play(iAttacker, iLast, "lastmanstanding", iLast);
}

bool TryPlaySpecial(int iAttacker, int iVictim, const std::string& sWeapon, bool bHeadshot, int iKills)
{
	// like the original: a matching event without its own record falls through to the next one
	if (TryLastManStanding(iAttacker, iVictim)) return true;
	if (iAttacker == iVictim && Play(iAttacker, iVictim, "selfkill", -1, "world")) return true;
	if (iAttacker != iVictim && TeamOf(iAttacker) == TeamOf(iVictim) && Play(iAttacker, iVictim, "teamkill")) return true;
	if (qs::IsFirstBlood(g_mapKills, iAttacker) && Play(iAttacker, iVictim, "firstblood")) return true;
	if (sWeapon.find("knife") != std::string::npos && Play(iAttacker, iVictim, "knifekill")) return true;
	if (bHeadshot && (!g_Cfg.bHeadshotOnce || iKills == 1) && Play(iAttacker, iVictim, "headshot")) return true;
	return false;
}

bool WarmupActive()
{
	if (g_iWarmupOffset < 0 && __s2sdk_GetSchemaOffset != nullptr)
	{
		try
		{
			const int32_t iOffset = s2sdk::GetSchemaOffset(ToPlg("CCSGameRules"), ToPlg("m_bWarmupPeriod"));
			if (iOffset >= 0) g_iWarmupOffset = iOffset;
		}
		catch (...)
		{
		}
	}
	if (g_iWarmupOffset < 0 || __s2sdk_GetGameRules == nullptr || __s2sdk_GetEntData2 == nullptr) return false;
	try
	{
		// The game-rules object is not an entity: the field is read at its schema offset of the
		// rules pointer (m_bWarmupPeriod is one byte). Offset-based instead of the typed
		// GetEntSchema2, which dereferences the cached CSchemaType of the class (PITFALLS C6/C8).
		void* pRules = s2sdk::GetGameRules();
		if (pRules == nullptr) return false;
		return s2sdk::GetEntData2(pRules, g_iWarmupOffset, /*size=*/1) != 0;
	}
	catch (...)
	{
		return false;
	}
}

void OnPlayerDeathEvent(void* pEvent)
{
	if (pEvent == nullptr || !g_mapReady) return;
	if (!g_Cfg.bWarmup && WarmupActive()) return;

	const int iVictim = EventPlayerSlot(pEvent, "userid");
	if (!HasController(iVictim) || (IsBotSlot(iVictim) && g_Cfg.bIgnoreBots)) return;
	SyncSlotIdentity(iVictim);
	auto itVictim = g_mapKills.find(iVictim);
	if (itVictim != g_mapKills.end() && g_Cfg.bResetOnDeath) itVictim->second = 0;

	const int iAttacker = EventPlayerSlot(pEvent, "attacker");
	if (!HasController(iAttacker) || (IsBotSlot(iAttacker) && g_Cfg.bIgnoreBots)) return;
	SyncSlotIdentity(iAttacker);

	bool bCount = true;
	if (iAttacker == iVictim) bCount = g_Cfg.bCountSelf;
	else if (TeamOf(iAttacker) == TeamOf(iVictim)) bCount = g_Cfg.bCountTeam;
	if (bCount) g_mapKills[iAttacker]++;

	const std::string sWeapon = qs::Lower(EventString(pEvent, "weapon"));
	const bool bHeadshot = EventBool(pEvent, "headshot");
	const int iKills = g_mapKills.count(iAttacker) ? g_mapKills[iAttacker] : 0;
	Debug("death: " + NameOf(iAttacker) + " -> " + NameOf(iVictim) + " weapon=" + sWeapon +
		  " hs=" + (bHeadshot ? "1" : "0") + " streak=" + std::to_string(iKills));

	for (qs::Kind eKind : g_Cfg.vPriority)
	{
		bool bPlayed = false;
		switch (eKind)
		{
		case qs::Kind::Special:
			bPlayed = TryPlaySpecial(iAttacker, iVictim, sWeapon, bHeadshot, iKills);
			break;
		case qs::Kind::Weapons:
			bPlayed = Play(iAttacker, iVictim, qs::WeaponSoundKey(sWeapon));
			break;
		case qs::Kind::KillStreak:
			if (!(sWeapon == "world" && g_Cfg.bIgnoreWorld)) bPlayed = Play(iAttacker, iVictim, std::to_string(iKills));
			break;
		}
		if (bPlayed) return;
	}
}

void OnPlayerDisconnectEvent(void* pEvent)
{
	const int iSlot = EventPlayerSlot(pEvent, "userid");
	if (!ValidSlot(iSlot)) return;
	g_mapKills.erase(iSlot);
	ResetPlayerState(iSlot);
	g_uLoadedSteam[iSlot] = 0;
	g_slotIdentity[iSlot] = PlayerIdentity{};
	if (g_mapReady) CloseMenuForSlot(iSlot, /*bNotifyBackend=*/false);
}

// ---------------------------------------------------------------------------------------------
// C4 countdown (the reference's BombFrame)
// ---------------------------------------------------------------------------------------------

void BombStop()
{
	g_bBombActive = false;
	g_iBombHandle = -1;
}

// Handle of the planted C4. The reference kept a CHandle and only walked the entity list while it
// was dead (BombFind); this does the same, so the list is not walked on every frame.
bool BombAlive()
{
	if (qs::IsValidHandle(g_iBombHandle))
	{
		if (__s2sdk_IsValidEntHandle == nullptr) return true;
		try { if (s2sdk::IsValidEntHandle(g_iBombHandle)) return true; } catch (...) {}
	}
	g_iBombHandle = -1;
	if (__s2sdk_FindEntityByClassname == nullptr) return false;
	try
	{
		// FindEntityByClassname returns a HANDLE (INVALID_EHANDLE_INDEX = -1 when nothing matches)
		const int32_t iHandle = s2sdk::FindEntityByClassname(-1, ToPlg("planted_c4"));
		if (!qs::IsValidHandle(iHandle)) return false;
		if (__s2sdk_IsValidEntHandle != nullptr && !s2sdk::IsValidEntHandle(iHandle)) return false;
		g_iBombHandle = iHandle;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

double GameTime()
{
	if (g_mapReady && __s2sdk_GetGameTime != nullptr)
	{
		try { return static_cast<double>(s2sdk::GetGameTime()); } catch (...) {}
	}
	return static_cast<double>(g_flFallbackTime);
}

void BombFrame()
{
	if (!g_bBombActive) return;
	// A changelevel with the bomb still ticking fires no round_end, so the flag can survive into
	// the window where the old map's entity system is gone: the entity lookups below must not run
	// there (the reference guarded its own BombFind with g_pGameEntitySystem for this).
	if (!g_mapReady)
	{
		BombStop();
		return;
	}
	if (!BombAlive())
	{
		BombStop();
		return;
	}
	if (g_iBlowOffset < 0 && __s2sdk_GetSchemaOffset != nullptr)
	{
		try
		{
			const int32_t iOffset = s2sdk::GetSchemaOffset(ToPlg("CPlantedC4"), ToPlg("m_flC4Blow"));
			if (iOffset >= 0) g_iBlowOffset = iOffset;
		}
		catch (...)
		{
		}
	}
	if (g_iBlowOffset < 0 || __s2sdk_GetEntDataFloat == nullptr) return;
	float flBlow = 0.0f;
	try { flBlow = static_cast<float>(s2sdk::GetEntDataFloat(g_iBombHandle, g_iBlowOffset, /*size=*/4)); }
	catch (...) { return; }
	const float flLeft = flBlow - static_cast<float>(GameTime());
	if (flLeft <= 0.0f)
	{
		BombStop();
		return;
	}
	const int iLeft = (int)ceilf(flLeft);
	if (iLeft == g_iBombLast) return;
	g_iBombLast = iLeft;
	PlayGlobal(qs::BombKey(iLeft));
}

// ---------------------------------------------------------------------------------------------
// Menus (the reference's ShowMenu / ShowVolumeMenu / OnSettingsCommand)
// ---------------------------------------------------------------------------------------------

bool AnyVolumeSoundCached() { return !g_mapOwnEvents.empty(); }

bool MenuTypeRegistered(const char* szName)
{
	if (__s2sdk_IsMenuTypeRegistered == nullptr) return false;
	try { return s2sdk::IsMenuTypeRegistered(ToPlg(szName)); } catch (...) { return false; }
}

// The player's own !menu choice comes from the optional hudmenu plugin (the only source of the
// per-player menu type the old wrapper honoured). Without hudmenu - or with an unknown/stale
// answer - the old HUD_LAYOUT fallback is used: hudmenu's "hud" backend when it is registered,
// s2sdk's built-in WASD "button" type otherwise. "hud"/"wasd" answers are only accepted while the
// matching backend is really registered (pisexcompat's ResolveS2sdkMenuTypeName behaviour).
std::string ResolveMenuType(int iSlot)
{
	std::string sChoice;
	if (__hudmenu_GetClientMenuType != nullptr)
	{
		try { sChoice = ToStd(hudmenu::GetClientMenuType(iSlot)); } catch (...) { sChoice.clear(); }
	}
	if (sChoice == "chat" || sChoice == "centerhtml" || sChoice == "button") return sChoice;
	if (sChoice == "hud") return MenuTypeRegistered("hud") ? "hud" : "button";
	if (sChoice == "wasd") return MenuTypeRegistered("wasd") ? "wasd" : "button";
	return MenuTypeRegistered("hud") ? "hud" : "button";
}

// PITFALLS A7/B2: every menu call (including the dummy id0 reservation and the no-op-close type
// switch used between maps) must have its teardown counterpart. Without all of these a menu created
// here could not be released safely and would keep a handler pointing into the unloaded library.
bool MenuBindingsReady()
{
	return __s2sdk_CreateMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
		   __s2sdk_AddMenuItem != nullptr && __s2sdk_SetMenuExitButton != nullptr &&
		   __s2sdk_SetMenuExitBackButton != nullptr && __s2sdk_DisplayMenu != nullptr &&
		   __s2sdk_GetMenuItemInfoText != nullptr && __s2sdk_GetClientMenu != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_SetMenuType != nullptr &&
		   __s2sdk_GetMenuType != nullptr;
}

void LogMenuBindingsUnavailableOnce()
{
	if (g_bMenuUnavailableLogged) return;
	g_bMenuUnavailableLogged = true;
	LogError("the s2sdk menu bindings are unavailable (Create/Destroy/GetClientMenu/CancelClientMenu/"
			 "SetMenuType/GetMenuType are required) - the quake sounds menu is disabled");
}

// The slot that currently displays `id`, -1 when nobody does. s2sdk treats client state id 0 as
// "no menu", so the reserved dummy id0 can never be displayed.
int FindDisplaySlot(s2sdk::MenuId id)
{
	if (id == 0 || __s2sdk_GetClientMenu == nullptr) return -1;
	for (int iSlot = 0; iSlot < kMaxSlots; iSlot++)
	{
		try
		{
			if (s2sdk::GetClientMenu(iSlot) == id) return iSlot;
		}
		catch (...)
		{
		}
	}
	return -1;
}

// Switches a menu to a builtin whose close() is a no-op ("chat"/"console"), so a DestroyMenu that
// still has to end a display cannot reach the client layer. Verified through GetMenuType.
bool MakeMenuCloseNoOp(s2sdk::MenuId id)
{
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) return false;
	for (const char* szType : {"chat", "console"})
	{
		if (__s2sdk_IsMenuTypeRegistered != nullptr && !MenuTypeRegistered(szType)) continue;
		try
		{
			if (!s2sdk::SetMenuType(id, ToPlg(szType))) continue;
			if (ToStd(s2sdk::GetMenuType(id)) == szType) return true;
		}
		catch (...)
		{
		}
	}
	return false;
}

void ParkMenu(s2sdk::MenuId id)
{
	if (id == 0) return;
	for (const s2sdk::MenuId queued : g_vParkedMenus)
		if (queued == id) return;
	if (g_vParkedMenus.size() >= 64)
	{
		LogError("A7_UNRESOLVED: too many parked menus, cannot track menu id " + std::to_string(id));
		return;
	}
	g_vParkedMenus.push_back(id);
}

// Ends every display of `id` and destroys the menu; the caller must not be inside a menu handler.
// mapReady == false is the changelevel window, where a backend close must not touch a client.
// Returns true when the id is gone from s2sdk's menu table.
bool DestroyMenuChecked(s2sdk::MenuId id, bool mapReady)
{
	if (__s2sdk_DestroyMenu == nullptr) return false;
	if (id == 0)
	{
		// The reserved dummy id: the manager's client state 0 means "no menu", so it cannot be
		// displayed and the plain destroy is safe even between maps.
		bool bDestroyed = false;
		try { bDestroyed = s2sdk::DestroyMenu(id); } catch (...) { return false; }
		if (__s2sdk_IsValidMenu != nullptr)
		{
			try { return !s2sdk::IsValidMenu(id); } catch (...) {}
		}
		return bDestroyed;
	}
	if (FindDisplaySlot(id) >= 0)
	{
		bool bCancelled = false;
		if (__s2sdk_CancelClientMenu != nullptr)
		{
			const int iSlot = FindDisplaySlot(id);
			try { bCancelled = s2sdk::CancelClientMenu(iSlot, s2sdk::MenuCancelReason::Disconnect); }
			catch (...) { bCancelled = false; }
		}
		(void)bCancelled;
		// Re-query the manager even when CancelClientMenu answered true: a display that is still
		// there in the map-down window must not reach the backend close. The no-op-close type
		// switch ("chat"/"console") is the only safe path then; a DestroyMenu with a
		// hud/centerhtml/button close would touch the torn-down client layer.
		if (!mapReady && FindDisplaySlot(id) >= 0 && !MakeMenuCloseNoOp(id)) return false;
	}
	bool bDestroyed = false;
	try { bDestroyed = s2sdk::DestroyMenu(id); } catch (...) { return false; }
	if (__s2sdk_IsValidMenu != nullptr)
	{
		try { return !s2sdk::IsValidMenu(id); } catch (...) {}
	}
	return bDestroyed;
}

// Every menu id this plugin still owns: live slots, ids queued by a handler and parked ones. The
// bookkeeping is cleared; the caller destroys the ids outside the menu handler.
std::vector<s2sdk::MenuId> CollectOwnedMenuIds()
{
	std::vector<s2sdk::MenuId> ids;
	for (MenuSlot& slot : g_menus)
	{
		if (slot.valid && slot.id != 0) ids.push_back(slot.id);
		slot = MenuSlot{};
	}
	for (const s2sdk::MenuId id : g_vMenuDestroy)
		if (id != 0) ids.push_back(id);
	g_vMenuDestroy.clear();
	for (const s2sdk::MenuId id : g_vParkedMenus)
		if (id != 0) ids.push_back(id);
	g_vParkedMenus.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

// OnMapEnd: the client layer is being torn down, so mapReady=false; nothing is destroyed through a
// backend close. Ids that even the no-op-close path cannot release are parked and retried when the
// next map is up (and on unload as A7_UNRESOLVED).
void TeardownMenusOnMapEnd()
{
	g_vActions.clear();
	for (const s2sdk::MenuId id : CollectOwnedMenuIds())
	{
		if (DestroyMenuChecked(id, /*mapReady=*/false)) continue;
		ParkMenu(id);
		LogWarning("A7_UNRESOLVED (parked): could not close menu " + std::to_string(id) +
				   " in the changelevel window - retried when the next map is up");
	}
}

void RetryParkedMenus()
{
	if (g_vParkedMenus.empty() || !g_mapReady) return;
	std::vector<s2sdk::MenuId> ids = std::move(g_vParkedMenus);
	g_vParkedMenus.clear();
	for (const s2sdk::MenuId id : ids)
	{
		if (DestroyMenuChecked(id, /*mapReady=*/true)) continue;
		ParkMenu(id);
		LogError("A7_UNRESOLVED (parked): could not destroy menu " + std::to_string(id) +
				 " with a live map - keeping it for unload");
	}
}

// OnPluginEnd (A7): every menu this plugin created is released here, whether the map is up or in the
// changelevel window. With the map up a backend close runs on live clients; with the map down the
// no-op-close type switch guarantees that no client call happens. An id that cannot be released
// even then is reported as A7_UNRESOLVED instead of being silently dropped.
void TeardownMenusOnUnload()
{
	g_vActions.clear();
	const bool mapReady = g_mapReady;
	std::vector<s2sdk::MenuId> ids = CollectOwnedMenuIds();
	if (g_hasReservedMenu)
	{
		ids.push_back(g_reservedMenuId);
		g_hasReservedMenu = false;
		g_reservedMenuId = 0;
	}
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	std::vector<s2sdk::MenuId> unresolved;
	for (const s2sdk::MenuId id : ids)
		if (!DestroyMenuChecked(id, mapReady)) unresolved.push_back(id);
	g_vMenuDestroy.clear();
	g_vParkedMenus.clear();
	if (!unresolved.empty())
	{
		std::string list;
		for (const s2sdk::MenuId id : unresolved)
		{
			if (!list.empty()) list += ", ";
			list += std::to_string(id);
		}
		LogError("A7_UNRESOLVED: s2sdk refused to release menu id(s) " + list +
				 " - their handler points into this library and cannot be removed by the plugin API. "
				 "This is an s2sdk MenuManager contract violation; do not deploy until it is understood");
	}
}

// s2sdk menu id 0 is a real id (the first CreateMenu returns it) *and* the manager's "no menu"
// sentinel (client state 0), so a dummy menu burns it at start. The dummy is held until unload;
// every real menu therefore gets a non-zero id. The reservation itself is only made when every
// teardown binding (Destroy/GetClientMenu/CancelClientMenu/SetMenuType/GetMenuType) is present.
bool ReserveMenuIdZero()
{
	if (g_hasReservedMenu) return true;
	if (!MenuBindingsReady()) return false;
	try
	{
		g_reservedMenuId = s2sdk::CreateMenu(plg::string(), nullptr, plg::string());
		g_hasReservedMenu = true;
		if (g_reservedMenuId != 0)
			Debug("s2sdk menu id 0 belongs to another plugin; this plugin's menus start at id " +
				  std::to_string(g_reservedMenuId));
		return true;
	}
	catch (...)
	{
		g_hasReservedMenu = false;
		g_reservedMenuId = 0;
		return false;
	}
}

void QueueMenuDestroy(s2sdk::MenuId id)
{
	if (id == 0 || g_vMenuDestroy.size() >= kMaxQueuedMenuDestroys) return;
	g_vMenuDestroy.push_back(id);
}

void DrainMenuDestroys()
{
	// Between maps the queue is left for OnMapEnd/OnPluginEnd: CollectOwnedMenuIds() takes it
	// together with the live menus. Dropping entries here would leak their handlers.
	if (!g_mapReady) return;
	while (!g_vMenuDestroy.empty())
	{
		const s2sdk::MenuId id = g_vMenuDestroy.front();
		g_vMenuDestroy.pop_front();
		if (__s2sdk_IsValidMenu != nullptr)
		{
			try { if (!s2sdk::IsValidMenu(id)) continue; } catch (...) { continue; }
		}
		if (DestroyMenuChecked(id, /*mapReady=*/true)) continue;
		ParkMenu(id);
		LogError("could not destroy menu " + std::to_string(id) + " - parked for retry");
	}
}

void QueueAction(int iSlot, const PlayerIdentity& identity, std::function<void()> fn)
{
	if (!fn || !identity.bValid || g_vActions.size() >= kMaxQueuedActions) return;
	g_vActions.push_back(PendingAction{iSlot, identity, std::move(fn)});
}

bool CanRunAction(const PendingAction& action)
{
	if (!g_mapReady || !ValidSlot(action.iSlot)) return false;
	if (!HasController(action.iSlot) || IsBotSlot(action.iSlot)) return false;
	// The identity captured when the menu was displayed (steam id + controller handle serial), not
	// the slot's current occupant: a reused slot - even with the same steam id on a new controller
	// entity - must never apply the old player's click.
	return SameIdentity(SlotIdentity(action.iSlot), action.identity);
}

void DrainActions()
{
	while (!g_vActions.empty())
	{
		PendingAction action = std::move(g_vActions.front());
		g_vActions.pop_front();
		if (!CanRunAction(action)) continue;
		try { action.fn(); }
		catch (const std::exception& ex) { LogError(std::string("queued action failed: ") + ex.what()); }
		catch (...) { LogError("queued action failed with an unknown exception"); }
	}
}

void HandleMenuSelection(int iSlot, const std::string& sInfo)
{
	if (sInfo == "toggle")
	{
		ToggleMute(iSlot);
		ShowMenu(iSlot);
		return;
	}
	if (sInfo == "volume")
	{
		ShowVolumeMenu(iSlot);
		return;
	}
	if (sInfo != "up" && sInfo != "down") return;
	g_iVolume[iSlot] = std::clamp(g_iVolume[iSlot] + (sInfo == "up" ? 10 : -10), 0, 100);
	CookieWrite(ClientSteamID(iSlot), "QuakeSounds.Volume", std::to_string(g_iVolume[iSlot]));
	ShowVolumeMenu(iSlot);
}

// Runs under the menu manager's lock: it may only read menu state and queue work. No client,
// entity, schema, cookie or display call happens here (PITFALLS C10). The display-time identity is
// copied into the queued action before the slot record is cleared.
void OnMenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t iSlot, int32_t iParam)
{
	try
	{
		if (!ValidSlot(iSlot)) return;
		MenuSlot& slot = g_menus[iSlot];
		if (!slot.valid || slot.id != id) return;
		const PlayerIdentity identity = slot.identity;
		switch (action)
		{
		case s2sdk::MenuAction::Start:
			return;
		case s2sdk::MenuAction::Select:
		{
			std::string sInfo;
			if (__s2sdk_GetMenuItemInfoText != nullptr)
			{
				try { sInfo = ToStd(s2sdk::GetMenuItemInfoText(id, iParam)); } catch (...) {}
			}
			slot.valid = false;
			slot.identity = PlayerIdentity{};
			QueueMenuDestroy(id);
			if (!sInfo.empty())
				QueueAction(iSlot, identity, [iSlot, sInfo]() { HandleMenuSelection(iSlot, sInfo); });
			return;
		}
		case s2sdk::MenuAction::Cancel:
			slot.valid = false;
			slot.identity = PlayerIdentity{};
			QueueMenuDestroy(id);
			if (iParam == static_cast<int32_t>(s2sdk::MenuCancelReason::ExitBack))
				QueueAction(iSlot, identity, [iSlot]() { ShowMenu(iSlot); });
			return;
		case s2sdk::MenuAction::End:
			slot.valid = false;
			slot.identity = PlayerIdentity{};
			QueueMenuDestroy(id);
			return;
		}
	}
	catch (...)
	{
	}
}

// Replaces a slot's menu. Safe destruction first; when that fails (A7 contract violation) the new
// menu is not created, so two displays/handlers are never mixed.
bool ReleaseSlotMenu(int iSlot, bool mapReady)
{
	MenuSlot& slot = g_menus[iSlot];
	if (!slot.valid) return true;
	const s2sdk::MenuId id = slot.id;
	slot.valid = false;
	slot.identity = PlayerIdentity{};
	if (DestroyMenuChecked(id, mapReady)) return true;
	ParkMenu(id);
	LogError("could not release menu " + std::to_string(id) + " on slot " + std::to_string(iSlot) +
			 " - not opening a replacement");
	return false;
}

void CloseMenuForSlot(int iSlot, bool bNotifyBackend)
{
	if (!ValidSlot(iSlot)) return;
	// A disconnect is the map-down-safe case: CancelClientMenu(Disconnect) skips the backend close.
	(void)ReleaseSlotMenu(iSlot, bNotifyBackend);
}

void DisplayMenuChecked(s2sdk::MenuId id, int iSlot, const PlayerIdentity& identity)
{
	MenuSlot& slot = g_menus[iSlot];
	slot.id = id;
	slot.valid = true;
	slot.identity = identity;
	bool bShown = false;
	try { bShown = s2sdk::DisplayMenu(id, iSlot, /*time=*/0); } catch (...) { bShown = false; }
	if (bShown) return;
	slot.valid = false;
	slot.identity = PlayerIdentity{};
	if (!DestroyMenuChecked(id, g_mapReady))
	{
		ParkMenu(id);
		LogError("could not destroy menu " + std::to_string(id) + " after a failed display");
		return;
	}
	LogError("could not display the quake sounds menu on slot " + std::to_string(iSlot));
}

void ShowVolumeMenu(int iSlot)
{
	if (!g_mapReady || !HasController(iSlot) || IsBotSlot(iSlot)) return;
	if (!MenuBindingsReady())
	{
		LogMenuBindingsUnavailableOnce();
		return;
	}
	if (!ReserveMenuIdZero())
	{
		LogError("could not reserve s2sdk menu id 0 - the quake sounds menu is disabled");
		return;
	}
	const PlayerIdentity identity = SlotIdentity(iSlot);
	if (!identity.bValid) return;
	if (!ReleaseSlotMenu(iSlot, g_mapReady)) return;
	try
	{
		const s2sdk::MenuId id = s2sdk::CreateMenu(
			ToPlg(qs::Replace(Phrase("Volume_Title"), "{volume}", std::to_string(g_iVolume[iSlot]))),
			&OnMenuHandler, ToPlg(ResolveMenuType(iSlot)));
		if (id == 0)
		{
			// Only the dummy this plugin reserved may be destroyed here; an id 0 owned by another
			// plugin is never touched.
			if (g_hasReservedMenu && g_reservedMenuId == 0) (void)DestroyMenuChecked(0, /*mapReady=*/false);
			g_hasReservedMenu = false;
			g_reservedMenuId = 0;
			LogError("s2sdk returned menu id 0 for the volume menu - not displayed");
			return;
		}
		if (s2sdk::AddMenuItem(id, ToPlg("up"), ToPlg(Phrase("Volume_Up")),
							   g_iVolume[iSlot] >= 100 ? s2sdk::MenuItemStyle::Disabled
													   : s2sdk::MenuItemStyle::Default) < 0 ||
			s2sdk::AddMenuItem(id, ToPlg("down"), ToPlg(Phrase("Volume_Down")),
							   g_iVolume[iSlot] <= 0 ? s2sdk::MenuItemStyle::Disabled
													 : s2sdk::MenuItemStyle::Default) < 0)
		{
			if (!DestroyMenuChecked(id, g_mapReady)) ParkMenu(id);
			return;
		}
		s2sdk::SetMenuExitButton(id, true);
		s2sdk::SetMenuExitBackButton(id, true);
		DisplayMenuChecked(id, iSlot, identity);
	}
	catch (const std::exception& ex)
	{
		LogError(std::string("could not open the volume menu: ") + ex.what());
	}
	catch (...)
	{
		LogError("could not open the volume menu");
	}
}

void ShowMenu(int iSlot)
{
	if (!g_mapReady || !HasController(iSlot) || IsBotSlot(iSlot)) return;
	if (!MenuBindingsReady())
	{
		LogMenuBindingsUnavailableOnce();
		return;
	}
	if (!ReserveMenuIdZero())
	{
		LogError("could not reserve s2sdk menu id 0 - the quake sounds menu is disabled");
		return;
	}
	const PlayerIdentity identity = SlotIdentity(iSlot);
	if (!identity.bValid) return;
	if (!ReleaseSlotMenu(iSlot, g_mapReady)) return;
	try
	{
		const s2sdk::MenuId id =
			s2sdk::CreateMenu(ToPlg(Phrase("Menu_Title")), &OnMenuHandler, ToPlg(ResolveMenuType(iSlot)));
		if (id == 0)
		{
			// Only the dummy this plugin reserved may be destroyed here; an id 0 owned by another
			// plugin is never touched.
			if (g_hasReservedMenu && g_reservedMenuId == 0) (void)DestroyMenuChecked(0, /*mapReady=*/false);
			g_hasReservedMenu = false;
			g_reservedMenuId = 0;
			LogError("s2sdk returned menu id 0 for the quake sounds menu - not displayed");
			return;
		}
		if (s2sdk::AddMenuItem(id, ToPlg("toggle"),
							   ToPlg(Phrase(g_bMuted[iSlot] ? "Menu_Unmute" : "Menu_Mute")),
							   s2sdk::MenuItemStyle::Default) < 0)
		{
			if (!DestroyMenuChecked(id, g_mapReady)) ParkMenu(id);
			return;
		}
		if (AnyVolumeSoundCached())
		{
			if (s2sdk::AddMenuItem(id, ToPlg("volume"),
								   ToPlg(qs::Replace(Phrase("Menu_Volume"), "{volume}",
													 std::to_string(g_iVolume[iSlot]))),
								   s2sdk::MenuItemStyle::Default) < 0)
			{
				if (!DestroyMenuChecked(id, g_mapReady)) ParkMenu(id);
				return;
			}
		}
		s2sdk::SetMenuExitButton(id, true);
		s2sdk::SetMenuExitBackButton(id, false);
		DisplayMenuChecked(id, iSlot, identity);
	}
	catch (const std::exception& ex)
	{
		LogError(std::string("could not open the quake sounds menu: ") + ex.what());
	}
	catch (...)
	{
		LogError("could not open the quake sounds menu");
	}
}

void ToggleMute(int iSlot)
{
	if (!g_mapReady || !HasController(iSlot) || IsBotSlot(iSlot)) return;
	g_bMuted[iSlot] = !g_bMuted[iSlot];
	CookieWrite(ClientSteamID(iSlot), "QuakeSounds.Muted", g_bMuted[iSlot] ? "1" : "0");
	ChatToSlot(iSlot, Phrase(g_bMuted[iSlot] ? "Muted" : "Unmuted"));
}

void OnSettingsCommand(int iSlot)
{
	if (!g_mapReady || !HasController(iSlot) || IsBotSlot(iSlot)) return;
	SyncSlotIdentity(iSlot);
	// no menu-free path when a volume sound exists: !qs opens the menu even with settings_menu 0
	if (g_Cfg.bSettingsMenu || AnyVolumeSoundCached()) ShowMenu(iSlot);
	else ToggleMute(iSlot);
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

bool ValidLang(const std::string& s)
{
	if (s.size() < 2 || s.size() > 8) return false;
	for (char c : s)
		if (!isalpha((unsigned char)c) && c != '-') return false;
	return true;
}

std::string TriggerlessName(std::string s)
{
	s = qs::Trim(s);
	if (!s.empty() && (s.front() == '!' || s.front() == '/' || s.front() == '.')) s.erase(s.begin());
	return qs::Trim(s);
}

s2sdk::ResultType OnSettingsChatCommand(int32_t iCaller, s2sdk::ConCommandContext eContext,
										const plg::vector<plg::string>&)
{
	if (eContext != s2sdk::ConCommandContext::Chat) return s2sdk::ResultType::Continue;
	try { OnSettingsCommand(iCaller); } catch (...) {}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnSettingsConsoleCommand(int32_t iCaller, s2sdk::ConCommandContext eContext,
										   const plg::vector<plg::string>&)
{
	if (eContext != s2sdk::ConCommandContext::Console) return s2sdk::ResultType::Continue;
	try { OnSettingsCommand(iCaller); } catch (...) {}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnLangChatCommand(int32_t iCaller, s2sdk::ConCommandContext eContext,
									const plg::vector<plg::string>& vArgs)
{
	if (eContext != s2sdk::ConCommandContext::Chat) return s2sdk::ResultType::Continue;
	try
	{
		if (!g_mapReady || !HasController(iCaller) || IsBotSlot(iCaller)) return s2sdk::ResultType::Handled;
		if (vArgs.size() < 2 || !ValidLang(ToStd(vArgs[1])))
		{
			ChatToSlot(iCaller, Phrase("Lang_Usage"));
			return s2sdk::ResultType::Handled;
		}
		g_sLang[iCaller] = qs::Lower(ToStd(vArgs[1]));
		CookieWrite(ClientSteamID(iCaller), "QuakeSounds.Lang", g_sLang[iCaller]);
		ChatToSlot(iCaller, qs::Replace(Phrase("Lang_Set"), "{lang}", g_sLang[iCaller]));
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnServerCommand(int32_t iCaller, s2sdk::ConCommandContext,
								  const plg::vector<plg::string>& vArgs)
{
	// mm_quakesounds reload|enable|disable - server console only (caller -1, like the original).
	if (iCaller != -1) return s2sdk::ResultType::Handled;
	try
	{
		const std::string sSub = vArgs.size() > 1 ? qs::Lower(ToStd(vArgs[1])) : "";
		if (sSub == "reload")
		{
			LoadConfig();
			LoadPhrases();
			Log("configuration reloaded: " + std::to_string(g_mapEntries.size()) +
				" sounds (chat commands change after a plugin reload)");
			ServerPrint(std::string(kLogTag) + " configuration reloaded: " +
						std::to_string(g_mapEntries.size()) + " sounds");
		}
		else if (sSub == "enable" || sSub == "disable")
		{
			g_Cfg.bEnabled = sSub == "enable";
			Log(g_Cfg.bEnabled ? "enabled (until reload/restart)" : "disabled (until reload/restart)");
			ServerPrint(std::string(kLogTag) + (g_Cfg.bEnabled ? " enabled (until reload/restart)"
															   : " disabled (until reload/restart)"));
		}
		else
		{
			ServerPrint(std::string(kLogTag) + " usage: mm_quakesounds reload|enable|disable");
		}
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Handled;
}

void RegisterCommand(const std::string& sName, s2sdk::ConCommandCallback callback, s2sdk::ConVarFlag flags)
{
	if (sName.empty() || callback == nullptr) return;
	for (const CommandRegistration& existing : g_commands)
		if (existing.name == sName) return; // duplicate spelling in the config
	if (__s2sdk_AddConsoleCommand == nullptr || __s2sdk_RemoveCommand == nullptr ||
		__s2sdk_RemoveCommandListener == nullptr)
	{
		// The ownership is only known after AddConsoleCommand (true = created, false = a listener
		// on somebody else's name), so both removers are needed before the name is touched: a
		// missing listener remover would leave our callback in the unloaded library after a
		// foreign/pre-existing command (PITFALLS A7). Require the whole trio together.
		LogError("the s2sdk command bindings are unavailable (AddConsoleCommand/RemoveCommand/"
				 "RemoveCommandListener are required) - '" + sName + "' is not registered");
		return;
	}
	CommandRegistration registration;
	registration.name = sName;
	registration.callback = callback;
	registration.flags = flags;
	registration.mode = s2sdk::HookMode::Pre;
	registration.attempted = true;
	try
	{
		registration.owned = s2sdk::AddConsoleCommand(ToPlg(sName), ToPlg(""), flags, callback,
													  s2sdk::HookMode::Pre);
	}
	catch (...)
	{
		// s2sdk attaches the callback before it knows whether the name was free; the safe
		// assumption is "a listener of ours may be attached" -> tear down with RemoveCommandListener.
		registration.owned = false;
	}
	if (!registration.owned)
	{
		Debug("command '" + sName + "' already existed - only our listener was attached");
	}
	g_commands.push_back(std::move(registration));
}

void RegisterCommands()
{
	g_commands.clear();
	const s2sdk::ConVarFlag chatFlags =
		CombineFlags(s2sdk::ConVarFlag::Release, s2sdk::ConVarFlag::ClientCanExecute);

	RegisterCommand("mm_qs", &OnSettingsConsoleCommand, chatFlags);
	RegisterCommand("mm_quakesounds", &OnServerCommand, s2sdk::ConVarFlag::Release);
	for (const std::string& configured : g_Cfg.vCommands)
		RegisterCommand(TriggerlessName(configured), &OnSettingsChatCommand, chatFlags);
	for (const std::string& configured : g_Cfg.vLangCommands)
		RegisterCommand(TriggerlessName(configured), &OnLangChatCommand, chatFlags);
}

void RemoveCommands()
{
	for (CommandRegistration& command : g_commands)
	{
		if (!command.attempted) continue;
		command.attempted = false;
		if (command.owned)
		{
			// This plugin created the command: RemoveCommand detaches the listener and the command.
			if (__s2sdk_RemoveCommand != nullptr)
			{
				try { s2sdk::RemoveCommand(ToPlg(command.name), command.callback); } catch (...) {}
			}
			else if (__s2sdk_RemoveCommandListener != nullptr)
			{
				// An old s2sdk without RemoveCommand: at least detach our callback.
				try
				{
					s2sdk::RemoveCommandListener(ToPlg(command.name), command.callback, command.mode);
				}
				catch (...)
				{
				}
			}
		}
		else if (__s2sdk_RemoveCommandListener != nullptr)
		{
			// The name already existed (engine, another plugin or s2sdk's own table after a
			// reload): only the listener is ours. RemoveCommand would unregister somebody else's
			// command (PITFALLS C11).
			try
			{
				s2sdk::RemoveCommandListener(ToPlg(command.name), command.callback, command.mode);
			}
			catch (...)
			{
			}
		}
		command.owned = false;
	}
	g_commands.clear();
}

// ---------------------------------------------------------------------------------------------
// Game events
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnPlayerDeathHook(const plg::string&, void* pEvent, bool)
{
	try { OnPlayerDeathEvent(pEvent); } catch (...) {}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerDisconnectHook(const plg::string&, void* pEvent, bool)
{
	try { OnPlayerDisconnectEvent(pEvent); } catch (...) {}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundStartHook(const plg::string&, void*, bool)
{
	try
	{
		if (g_Cfg.bResetOnRoundStart) g_mapKills.clear();
		BombStop();
		PlayGlobal("round_start");
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundEndHook(const plg::string&, void*, bool)
{
	try
	{
		BombStop();
		PlayGlobal("round_end");
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundFreezeEndHook(const plg::string&, void*, bool)
{
	try { PlayGlobal("round_freeze_end"); } catch (...) {}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnBombPlantedHook(const plg::string&, void*, bool)
{
	try
	{
		PlayGlobal("bomb_planted");
		g_bBombActive = true;
		g_iBombLast = INT32_MAX;
		g_iBombHandle = -1; // look the entity up in the frame, like the original reset its CHandle
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnBombDefusedHook(const plg::string&, void*, bool)
{
	try
	{
		BombStop();
		PlayGlobal("bomb_defused");
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnBombExplodedHook(const plg::string&, void*, bool)
{
	try
	{
		BombStop();
		PlayGlobal("bomb_exploded");
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

EventRegistration g_events[] = {
	{"player_death", &OnPlayerDeathHook, false},
	{"player_disconnect", &OnPlayerDisconnectHook, false},
	{"round_start", &OnRoundStartHook, false},
	{"round_end", &OnRoundEndHook, false},
	{"round_freeze_end", &OnRoundFreezeEndHook, false},
	{"bomb_planted", &OnBombPlantedHook, false},
	{"bomb_defused", &OnBombDefusedHook, false},
	{"bomb_exploded", &OnBombExplodedHook, false},
};

void RegisterEvents()
{
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr)
	{
		// A hook that could not be removed in OnPluginEnd would point into the unloaded library.
		LogError("the s2sdk event bindings are unavailable - player/round events are not hooked");
		return;
	}
	for (EventRegistration& event : g_events)
	{
		event.hooked = false;
		event.attempted = true; // a throwing call may still have left the callback attached
		try
		{
			event.hooked = s2sdk::HookEvent(ToPlg(event.name), event.callback, s2sdk::HookMode::Pre) ==
						   s2sdk::EventHookError::Okay;
		}
		catch (...)
		{
			event.hooked = false;
		}
		if (!event.hooked) LogError(std::string("hooking '") + event.name + "' failed - that event is disabled");
	}
}

void RemoveEvents()
{
	if (__s2sdk_UnhookEvent == nullptr) return;
	for (EventRegistration& event : g_events)
	{
		if (!event.attempted) continue;
		event.attempted = false;
		event.hooked = false;
		try { s2sdk::UnhookEvent(ToPlg(event.name), event.callback, s2sdk::HookMode::Pre); } catch (...) {}
	}
}

// ---------------------------------------------------------------------------------------------
// Precache manifest and user-message volume hook
// ---------------------------------------------------------------------------------------------

// A callback of s2sdk: nothing may unwind out of here. s2sdk clears its precache set at map end,
// so the list is added again on every manifest build (the AddPrecache of the original).
void OnBuildManifest()
{
	try
	{
		if (__s2sdk_Precache == nullptr) return;
		for (const std::string& s : g_Cfg.vPrecache) s2sdk::Precache(ToPlg(s));
	}
	catch (...)
	{
	}
}

void RegisterManifest()
{
	if (g_bManifestRegistered) return;
	if (__s2sdk_OnBuildGameSessionManifest_Register == nullptr ||
		__s2sdk_OnBuildGameSessionManifest_Unregister == nullptr)
	{
		LogError("the s2sdk manifest bindings are unavailable - sounds are not precached");
		return;
	}
	g_bManifestRegistered = true; // attempted: Unregister is safe even if the call threw
	try
	{
		s2sdk::OnBuildGameSessionManifest_Register(&OnBuildManifest);
	}
	catch (...)
	{
		LogError("could not register the game-session manifest callback - sounds are not precached");
	}
}

void UnregisterManifest()
{
	if (!g_bManifestRegistered) return;
	g_bManifestRegistered = false;
	if (__s2sdk_OnBuildGameSessionManifest_Unregister != nullptr)
	{
		try { s2sdk::OnBuildGameSessionManifest_Unregister(&OnBuildManifest); } catch (...) {}
	}
}

// s2sdk's user-message hook is the PostEventAbstract equivalent the original used: the per-player
// volume of a started soundevent is sent as GE_SosSetSoundEventParams (src/sound_volume.cpp).
s2sdk::ResultType OnSoundEventPost(void* pUserMessage)
{
	try
	{
		// A callback of s2sdk (it runs under the user-message manager's lock): nothing may unwind
		// out of here and no game action is taken.
		if (!g_mapReady || pUserMessage == nullptr) return s2sdk::ResultType::Continue;
		int iGuid = -1;
		uint32_t uHash = 0;
		if (!sound_volume::ParseStart(pUserMessage, iGuid, uHash)) return s2sdk::ResultType::Continue;
		auto it = g_mapOwnEvents.find(uHash);
		if (it == g_mapOwnEvents.end()) return s2sdk::ResultType::Continue;

		const float flEntry = it->second;
		const uint64_t uMask = sound_volume::RecipientMask(pUserMessage);
		// the sound start is sent a tick after the EmitSound of the same frame; the volume comes
		// from the receiver's own setting
		for (int i = 0; i < kMaxSlots; i++)
		{
			if (!(uMask & (1ull << i))) continue;
			const float flVolume = qs::ListenerVolume(flEntry, g_iVolume[i]);
			if (flVolume >= 1.0f) continue; // 1.0 is the soundevent's own volume: nothing to do
			Debug("volume " + std::to_string(flVolume) + " -> slot " + std::to_string(i) + " (guid " +
				  std::to_string(iGuid) + ")");
			sound_volume::Set(i, iGuid, flVolume);
		}
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

// Lazily resolves the SOS ids on the first live map and hooks the start message once. No hardcoded
// fallback id is ever used: no registry hit -> no hook, no CreateFromId, no Pb*/Send. Start/Set/Stop
// are checked separately; the hook only needs Start, while a missing Set makes the per-player
// volume a no-op and a missing Stop makes the stop a no-op.
void EnsureSoundHook()
{
	if (!g_bSoundAvailable || !g_mapReady || __s2sdk_HookUserMessage == nullptr ||
		__s2sdk_UnhookUserMessage == nullptr)
	{
		return;
	}

	if (!g_bSoundResolveAttempted)
	{
		g_bSoundResolveAttempted = true;
		sound_volume::ResolveIds();
		if (sound_volume::StartMessageId() <= 0 && !g_bSoundStartMissingLogged)
		{
			g_bSoundStartMissingLogged = true;
			LogError("SosStartSoundEvent is not in the s2sdk user-message registry - the per-player "
					 "soundevent hook is disabled (no fallback id is used)");
		}
		if (sound_volume::SetMessageId() <= 0 && !g_bSoundSetMissingLogged)
		{
			g_bSoundSetMissingLogged = true;
			LogError("SosSetSoundEventParams is not in the s2sdk user-message registry - the "
					 "per-player volume is disabled (no fallback id is used)");
		}
	}

	const int iWant = sound_volume::StartMessageId();
	if (iWant <= 0) return;
	if (iWant == g_iHookedStartId || iWant == g_iFailedStartHookId) return;

	if (g_iHookedStartId > 0)
	{
		try { s2sdk::UnhookUserMessage((int16_t)g_iHookedStartId, &OnSoundEventPost, s2sdk::HookMode::Post); }
		catch (...) {}
		g_iHookedStartId = -1;
	}
	try
	{
		if (s2sdk::HookUserMessage((int16_t)iWant, &OnSoundEventPost, s2sdk::HookMode::Post))
		{
			g_iHookedStartId = iWant;
			g_iFailedStartHookId = -1;
		}
		else
		{
			g_iFailedStartHookId = iWant;
			LogError("hooking SosStartSoundEvent failed - the per-player volume stays off");
		}
	}
	catch (...)
	{
		// A throwing hook call may have attached the callback before throwing: remove it now.
		try { s2sdk::UnhookUserMessage((int16_t)iWant, &OnSoundEventPost, s2sdk::HookMode::Post); }
		catch (...) {}
		g_iFailedStartHookId = iWant;
		LogError("hooking SosStartSoundEvent failed - the per-player volume stays off");
	}
}

void UnhookSoundVolume()
{
	if (g_iHookedStartId > 0 && __s2sdk_UnhookUserMessage != nullptr)
	{
		try { s2sdk::UnhookUserMessage((int16_t)g_iHookedStartId, &OnSoundEventPost, s2sdk::HookMode::Post); }
		catch (...) {}
	}
	g_iHookedStartId = -1;
	g_iFailedStartHookId = -1;
	sound_volume::Reset();
	g_bSoundAvailable = false;
	g_bSoundResolveAttempted = false;
}

// ---------------------------------------------------------------------------------------------
// State reset
// ---------------------------------------------------------------------------------------------

void ResetAllState()
{
	for (int i = 0; i < kMaxSlots; i++)
	{
		ResetPlayerState(i);
		g_slotIdentity[i] = PlayerIdentity{};
		g_uLoadedSteam[i] = 0;
		g_menus[i] = MenuSlot{};
	}
	g_mapKills.clear();
	g_vActions.clear();
	g_vMenuDestroy.clear();
	g_vParkedMenus.clear();
	g_hasReservedMenu = false;
	g_reservedMenuId = 0;
	g_flReconcileSeconds = 0.0f;
	g_flFallbackTime = 0.0f;
	g_bBombActive = false;
	g_iBombLast = INT32_MAX;
	g_iBombHandle = -1;
	g_iBlowOffset = -1;
	g_iWarmupOffset = -1;
	g_bMenuUnavailableLogged = false;
	g_bStarted = false;
	g_mapReady = false;
	g_mapEndSeen = false;
	g_mapListeners = false;
	g_mapStartAttempted = false;
	g_mapEndAttempted = false;
	g_mapStartRegistered = false;
	g_mapEndRegistered = false;
	g_bManifestRegistered = false;
	g_bSoundAvailable = false;
	g_bSoundResolveAttempted = false;
	g_bSoundStartMissingLogged = false;
	g_bSoundSetMissingLogged = false;
	g_iHookedStartId = -1;
	g_iFailedStartHookId = -1;
	sound_volume::Reset();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------------------------

class QuakeSoundsPlugin final : public plg::Plugin
{
public:
	plg::PluginResult OnPluginStart() override
	{
		try
		{
			if (plg::plugin::GetId != nullptr && plg::plugin::handle != nullptr)
			{
				try { g_iPluginId = static_cast<int64_t>(plg::plugin::GetId(plg::plugin::handle)); }
				catch (...) { g_iPluginId = 0; }
			}
			else
			{
				g_iPluginId = 0;
			}

			LoadConfig();
			LoadPhrases();
			ResetAllState();
			// ResetAllState uses the loaded config's default volume for every slot; players are
			// loaded from their cookies by the reconcile as soon as the map is up.
			for (int i = 0; i < kMaxSlots; i++) g_iVolume[i] = g_Cfg.iDefaultVolume;

			g_bSoundAvailable = sound_volume::Init();
			g_bSoundResolveAttempted = false;
			g_iHookedStartId = -1;
			g_iFailedStartHookId = -1;
			if (!g_bSoundAvailable)
				LogError("soundevent volume control is unavailable (s2sdk user messages missing)");

			// Rebuilt on every map load, so the list is added from the manifest callback.
			RegisterManifest();

			RegisterMapListeners();
			RegisterEvents();
			RegisterCommands();

			// The dummy menu burns s2sdk's MenuId 0 before any real menu can be created; it is
			// only made when every teardown binding (Destroy/GetClientMenu/CancelClientMenu/
			// SetMenuType/GetMenuType) is present.
			if (!MenuBindingsReady()) LogMenuBindingsUnavailableOnce();
			else if (!ReserveMenuIdZero()) LogError("could not reserve s2sdk menu id 0 - the menu is disabled");

			g_bStarted = true;
			Log("loaded: " + std::to_string(g_mapEntries.size()) + " sound records, " +
				std::to_string(g_Cfg.vPrecache.size()) + " precache files, language " + g_sServerLang);
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override
	{
		if (!g_bStarted) return {};
		try
		{
			PollMapGate();

			const int64_t iElapsed = dt.count() > 0 ? static_cast<int64_t>(dt.count()) : 0;
			if (g_mapReady) g_flFallbackTime += static_cast<float>(iElapsed) / 1000.0f;

			// Deferred work first: the menu handler only queues, so the menu manager's lock is
			// never re-entered and no s2sdk timer mutex is involved (this plugin owns no timers).
			DrainMenuDestroys();
			RetryParkedMenus();
			DrainActions();
			if (!g_mapReady) return {};

			EnsureSoundHook();
			ReconcileCookies(static_cast<float>(iElapsed) / 1000.0f);
			BombFrame();
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("update failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("update failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override
	{
		try
		{
			g_bStarted = false;

			// Menus first: their handler/destroy path belongs to this library. On a live map a
			// backend close is safe; between maps the checked teardown switches to the builtin
			// no-op-close type and reports A7_UNRESOLVED if s2sdk refuses to release an id.
			TeardownMenusOnUnload();
			g_vActions.clear();
			g_vMenuDestroy.clear();

			RemoveCommands();
			RemoveEvents();
			UnregisterManifest();
			UnhookSoundVolume();
			UnregisterMapListeners();

			ResetAllState();
			Log("unloaded");
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("OnPluginEnd failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

QuakeSoundsPlugin g_quakeSoundsPlugin;

PLUGIFY_PLUGIN(PLUGIN_API, &g_quakeSoundsPlugin)
