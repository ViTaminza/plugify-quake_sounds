// quake_sounds — Plugify C++ port of the Metamod:Source plugin ref/src/cs2-quake-sounds
// (itself a port of Kandru's CounterStrikeSharp "CS2 QuakeSounds"): announcer sounds for kill
// streaks, special kills, round events and the C4 countdown.
//
// Ported 1:1 (config keys, defaults and behaviour) from ref/src/cs2-quake-sounds, with the
// production config ref/prod-27016/addons/configs/quake_sounds.ini (the "fleet" variant, whose
// sound names come from workshop addon 3811020435 "Quake 7 Sounds").
//
// Deliberate differences (all forced by the framework, none change the config semantics):
//   * engine access is s2sdk/pisexcompat (no Metamod, no EntitySystem iteration): the C4 countdown
//     finds "planted_c4" with s2sdk::FindEntityByClassname and reads m_flC4Blow through the schema;
//   * ICookiesApi is replaced by the official permissions cookies (same names: QuakeSounds.Muted /
//     QuakeSounds.Lang / QuakeSounds.Volume, see README of pisexcompat §"player settings");
//     the user is loaded with permissions::LoadUser by a once-per-second reconcile, because the
//     permissions core knows nothing about a client until somebody asks for it;
//   * the "listener" emission mode is EmitSound on each listener's own pawn (s2sdk entity handles);
//     the per-player volume is applied through the GE_SosSetSoundEventParams message, exactly like
//     the original's SoundVolume::Set (src/sound_volume.cpp);
//   * the utils settings-menu hook (utils_settings_menu) is NOT ported: the Plugify menu API of
//     pisexcompat has no settings-menu extension point. The command behaviour of the original
//     (menu when settings_menu=1 or a volume sound exists, direct mute toggle otherwise) is kept.
//
// Everything runs on the game thread, no s2sdk call is made before a map exists (pisex::Poll and
// the StartupServer deferral take care of that) and no exception escapes into s2sdk.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <permissions/permissions.hpp>

#include <pisex/pisex.hpp>

#include "quake_core.hpp"
#include "sound_volume.hpp"

namespace {

const char* kLogTag = "[QuakeSounds]";

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

qs::Config g_Cfg;
std::map<std::string, qs::SoundEntry> g_mapEntries;
std::map<std::string, std::string> g_mapPhrases;
std::string g_sServerLang = "ru";
std::map<uint32_t, float> g_mapOwnEvents; // soundevent hash -> record volume (1.0 when unset)

bool g_bMuted[64] = {};
int g_iVolume[64] = {}; // percent; soundevents only
std::string g_sLang[64];
std::map<int, int> g_mapKills; // slot -> kill streak (the entry's existence is what first blood checks)

std::mt19937 g_Rng{std::random_device{}()};

std::deque<std::function<void()>> g_vNextFrame;
bool g_bReady = false;
int64_t g_iPluginId = 0;

// C4 countdown: the reference walks the entity list for "planted_c4" every frame while the bomb is
// planted; FindEntityByClassname does the same by classname.
bool g_bBombActive = false;
int g_iBombLast = INT32_MAX;
int g_iBombHandle = 0; // s2sdk handle of the planted C4 (0 = not resolved yet)

constexpr const char* kConfigFile = "quake_sounds.ini";
constexpr const char* kPhrasesFile = "quake_sounds.phrases.txt";

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

void LogMessage(const std::string& sText, plg::Severity eSeverity)
{
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + sText, eSeverity, plg::source_location::current());
}

void Log(const std::string& sText) { LogMessage(sText, plg::Severity::Info); }
void LogError(const std::string& sText) { LogMessage(sText, plg::Severity::Error); }

void Debug(const std::string& sText)
{
	if (g_Cfg.bDebug) Log("[DEBUG] " + sText);
}

void NextFrame(std::function<void()> fn)
{
	g_vNextFrame.push_back(std::move(fn));
}

std::string Phrase(const std::string& sKey)
{
	return qs::Phrase(g_mapPhrases, g_sServerLang, sKey);
}

std::string FilePath(const std::string& sName)
{
	return pisex::ConfigDir() + "/" + sName;
}

// s2sdk entity calls take handles; the pisex/s2sdk "*Index" helpers hand out 1-based indexes.
// EntIndexToEntHandle answers INVALID_EHANDLE_INDEX (-1) for a missing index and a valid handle can
// be a negative int (see qs::IsValidHandle), so only the raw value decides.
int EntityHandle(int iIndex)
{
	if (iIndex <= 0 || !__s2sdk_EntIndexToEntHandle) return 0;
	const int iHandle = s2sdk::EntIndexToEntHandle(iIndex);
	return qs::IsValidHandle(iHandle) ? iHandle : 0;
}

// The world entity is engine index 0 (the original emitted "from the world" with CEntityIndex(0));
// the returned value is a handle and is judged as one (qs::IsValidHandle), 0 = not resolved.
int WorldHandle()
{
	if (!__s2sdk_EntIndexToEntHandle) return 0;
	const int iHandle = s2sdk::EntIndexToEntHandle(0);
	return qs::IsValidHandle(iHandle) ? iHandle : 0;
}



// ---------------------------------------------------------------------------------------------
// Player queries (all of them go through pisexcompat, which never touches a client before a map)
// ---------------------------------------------------------------------------------------------

bool HasController(int iSlot)
{
	return iSlot >= 0 && iSlot < 64 && pisex::IsConnected(iSlot) && pisex::GetControllerEntity(iSlot) > 0;
}

// pisex::GetPlayerTeam reads the controller's m_iTeamNum: the team lives on the controller, so
// no pawn is needed.
int TeamOf(int iSlot)
{
	return HasController(iSlot) ? pisex::GetPlayerTeam(iSlot) : 0;
}

bool IsBot(int iSlot)
{
	return pisex::IsFakeClient(iSlot);
}

bool IsAliveSlot(int iSlot)
{
	return HasController(iSlot) && pisex::IsAlive(iSlot);
}

std::string NameOf(int iSlot)
{
	const char* sz = pisex::GetPlayerName(iSlot);
	return (sz && sz[0]) ? sz : "?";
}

int PawnOf(int iSlot)
{
	return HasController(iSlot) ? pisex::GetPawnEntity(iSlot) : 0;
}

// ---------------------------------------------------------------------------------------------
// Cookies (official permissions plugin, names identical to the Pisex ones)
// ---------------------------------------------------------------------------------------------

bool CookieRead(uint64_t uSteamID64, const char* szName, std::string& sOut)
{
	if (!uSteamID64 || !__permissions_GetCookie) return false;
	try
	{
		plg::any value;
		if (permissions::GetCookie(uSteamID64, plg::string(szName), value) != permissions::Status::Success) return false;
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
	if (!uSteamID64 || !__permissions_SetCookie) return;
	try
	{
		permissions::SetCookie(g_iPluginId, uSteamID64, plg::string(szName), plg::any(plg::string(sValue)),
							   /*dontBroadcast=*/false);
	}
	catch (...)
	{
	}
}

// The permissions core only knows a user after somebody asked for him (LoadUser -> OnUserRequest
// -> the storage plugin fills the record). Metamod's ICookiesApi did that implicitly.
void ResetPlayerState(int iSlot);
void LoadPlayer(int iSlot);
uint64_t g_uLoadedSteam[64] = {};
float g_flReconcileTimer = 0.0f;

void ReconcileCookies(float flDelta)
{
	if (!__permissions_LoadUser) return;
	g_flReconcileTimer += flDelta;
	if (g_flReconcileTimer < 1.0f) return;
	g_flReconcileTimer = 0.0f;

	for (int i = 0; i < 64; i++)
	{
		if (!HasController(i) || IsBot(i) || !pisex::IsAuthenticated(i))
		{
			if (g_uLoadedSteam[i])
			{
				g_uLoadedSteam[i] = 0;
				ResetPlayerState(i);
			}
			continue;
		}
		const uint64_t uSteam = pisex::GetSteamID64(i);
		if (!uSteam || g_uLoadedSteam[i] == uSteam) continue;
		const std::string sName = NameOf(i);
		if (permissions::LoadUser(g_iPluginId, uSteam, plg::string(sName), /*offline=*/false,
								  /*dontBroadcast=*/false) == permissions::Status::Success)
		{
			g_uLoadedSteam[i] = uSteam;
			LoadPlayer(i);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Player state (the reference's ResetPlayer/LoadPlayer/ToggleMute)
// ---------------------------------------------------------------------------------------------

void ResetPlayerState(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64) return;
	g_bMuted[iSlot] = false;
	g_iVolume[iSlot] = g_Cfg.iDefaultVolume;
	g_sLang[iSlot].clear();
}

void LoadPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64) return;
	ResetPlayerState(iSlot);
	const uint64_t uSteam = pisex::GetSteamID64(iSlot);
	if (!uSteam) return;

	std::string sValue;
	if (CookieRead(uSteam, "QuakeSounds.Muted", sValue)) g_bMuted[iSlot] = atoi(sValue.c_str()) != 0;
	if (CookieRead(uSteam, "QuakeSounds.Lang", sValue)) g_sLang[iSlot] = sValue;
	if (CookieRead(uSteam, "QuakeSounds.Volume", sValue))
		g_iVolume[iSlot] = std::clamp(atoi(sValue.c_str()), 0, 100);
}

void ToggleMute(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64) return;
	g_bMuted[iSlot] = !g_bMuted[iSlot];
	CookieWrite(pisex::GetSteamID64(iSlot), "QuakeSounds.Muted", g_bMuted[iSlot] ? "1" : "0");
	pisex::g_pUtils->PrintToChat(iSlot, "%s", Phrase(g_bMuted[iSlot] ? "Muted" : "Unmuted").c_str());
}

// ---------------------------------------------------------------------------------------------
// Config / phrases
// ---------------------------------------------------------------------------------------------

void LoadPhrases()
{
	g_mapPhrases.clear();
	const char* szLang = pisex::g_pUtils->GetLanguage();
	g_sServerLang = qs::NormalizeLang((szLang && szLang[0]) ? szLang : "ru");
	if (g_sServerLang.empty()) g_sServerLang = "ru";

	bool ok = false;
	const std::string sText = pisex::ReadFile(pisex::TranslationsDir() + "/" + kPhrasesFile, &ok);
	if (!ok) return;
	qs::ParsePhrasesText(sText, g_sServerLang, g_mapPhrases);
}

void LoadConfig()
{
	bool ok = false;
	const std::string sText = pisex::ReadFile(FilePath(kConfigFile), &ok);
	if (!ok || !qs::ParseConfigText(sText, g_Cfg, g_mapEntries))
	{
		LogError(std::string("failed to read ") + FilePath(kConfigFile) + " - keeping the defaults");
		return;
	}

	g_mapOwnEvents.clear();
	for (const auto& [sKey, e] : g_mapEntries)
		for (const auto& sSound : e.vSounds)
			if (!qs::IsFilePathSound(sSound)) g_mapOwnEvents[qs::SoundEventHash(sSound)] = qs::EntryVolume(e);

	Log(std::to_string(g_mapEntries.size()) + " sound records, " + std::to_string(g_Cfg.vPrecache.size()) + " precache files");
}

// The s2sdk resource manifest is rebuilt on every map load (s2sdk clears its precache set at map
// end), so the list is added again from the manifest callback - the AddPrecache of the original.
// A callback of s2sdk: nothing may unwind out of here.
void OnBuildManifest()
{
	try
	{
		if (!__s2sdk_Precache) return;
		for (const auto& s : g_Cfg.vPrecache) s2sdk::Precache(plg::string(s));
	}
	catch (...)
	{
	}
}

// ---------------------------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------------------------

bool IsListener(int iSlot)
{
	// bots hear nothing; ignore_bots only decides whether their kills count
	if (!HasController(iSlot) || IsBot(iSlot)) return false;
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
	for (int i = 0; i < 64; i++)
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
		for (int i : vTo)
			if (__s2sdk_ClientCommand) s2sdk::ClientCommand(i, plg::string("play " + sSound));
		return;
	}

	const float flVol = flVolume >= 0.0f ? flVolume : 1.0f;

	if (sPlayOn == "listener")
	{
		// "listener": every listener hears it on himself (the announcer is equally loud everywhere)
		for (int i : vTo)
		{
			const float flPlayer = qs::ListenerVolume(flVol, g_iVolume[i]);
			if (flPlayer <= 0.0f) continue;
			const int iHandle = EntityHandle(PawnOf(i));
			if (!qs::IsValidHandle(iHandle) || !__s2sdk_EmitSound) continue;
			s2sdk::EmitSound(iHandle, plg::string(sSound), 100, flPlayer, 0.0f);
		}
		Debug("emit " + sSound + " on listeners (" + std::to_string(vTo.size()) + " players)");
		return;
	}

	// "player": from the killer's pawn; "world" (or no source): from the world entity
	const bool bWorld = (sPlayOn == "world") || iSource < 0;
	const int iEntIndex = bWorld ? 0 : PawnOf(iSource);
	Debug("emit " + sSound + " on ent " + std::to_string(iEntIndex) + " to " + std::to_string(vTo.size()) + " players");
	for (int i : vTo)
	{
		const float flPlayer = qs::ListenerVolume(flVol, g_iVolume[i]);
		if (flPlayer <= 0.0f) continue;
		// every listener has his own volume: one emission per player, like the original
		const int iHandle = bWorld ? WorldHandle() : EntityHandle(iEntIndex);
		if (!qs::IsValidHandle(iHandle) || !__s2sdk_EmitSound) continue;
		s2sdk::EmitSound(iHandle, plg::string(sSound), 100, flPlayer, 0.0f);
	}
}

void Announce(int iSource, const qs::SoundEntry& e, const std::vector<int>& vTo)
{
	if ((!g_Cfg.bCenter && !g_Cfg.bChat) || e.mapMessages.empty()) return;
	for (int i : vTo)
	{
		if (IsBot(i)) continue;
		const std::string sMsg = qs::ResolveMessage(e, g_sLang[i], g_sServerLang);
		if (sMsg.empty()) continue;
		const bool bSelf = i == iSource;
		const std::string sSource = iSource >= 0 ? NameOf(iSource) : "";
		if (g_Cfg.bCenter)
		{
			const std::string s = qs::Replace(qs::Replace(Phrase(bSelf ? "Center_Msg_Player" : "Center_Msg_Other"),
														  "{player}", sSource), "{message}", sMsg);
			if (g_Cfg.sCenterType == "alert") pisex::g_pUtils->PrintToAlert(i, "%s", s.c_str());
			else if (g_Cfg.sCenterType == "html") pisex::g_pUtils->PrintToCenterHtml(i, 3, "%s", s.c_str());
			else pisex::g_pUtils->PrintToCenter(i, "%s", s.c_str());
		}
		if (g_Cfg.bChat)
		{
			const std::string s = qs::Replace(qs::Replace(Phrase(bSelf ? "Chat_Msg_Player" : "Chat_Msg_Other"),
														  "{player}", sSource), "{message}", sMsg);
			pisex::g_pUtils->PrintToChat(i, "%s", s.c_str());
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
	for (int i = 0; i < 64; i++)
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

void OnPlayerDeath(pisex::IGameEvent* pEvent)
{
	if (!pEvent) return;
	if (!g_Cfg.bWarmup && __s2sdk_GetGameRules && __s2sdk_GetEntData2)
	{
		// the game rules object is not an entity: the field is read at its schema offset of the
		// rules pointer (m_bWarmupPeriod is one byte). Offset-based instead of the typed
		// GetEntSchema2, which dereferences the cached CSchemaType of the class (the map-start
		// schema poison of FortniteEmotes Signature D).
		static int s_iWarmupOffset = -2; // -2 = not resolved yet, -1 = the field was not found
		if (s_iWarmupOffset == -2) s_iWarmupOffset = pisex::SchemaOffset("CCSGameRules", "m_bWarmupPeriod");
		void* pRules = s2sdk::GetGameRules();
		if (pRules && s_iWarmupOffset >= 0)
		{
			try
			{
				if (s2sdk::GetEntData2(pRules, s_iWarmupOffset, /*size=*/1) != 0) return;
			}
			catch (...)
			{
			}
		}
	}

	const int iVictim = pEvent->GetPlayerSlot("userid");
	if (!HasController(iVictim) || (IsBot(iVictim) && g_Cfg.bIgnoreBots)) return;
	auto itVictim = g_mapKills.find(iVictim);
	if (itVictim != g_mapKills.end() && g_Cfg.bResetOnDeath) itVictim->second = 0;

	const int iAttacker = pEvent->GetPlayerSlot("attacker");
	if (!HasController(iAttacker) || (IsBot(iAttacker) && g_Cfg.bIgnoreBots)) return;

	bool bCount = true;
	if (iAttacker == iVictim) bCount = g_Cfg.bCountSelf;
	else if (TeamOf(iAttacker) == TeamOf(iVictim)) bCount = g_Cfg.bCountTeam;
	if (bCount) g_mapKills[iAttacker]++;

	const std::string sWeapon = qs::Lower(pEvent->GetString("weapon", ""));
	const bool bHeadshot = pEvent->GetBool("headshot");
	const int iKills = g_mapKills.count(iAttacker) ? g_mapKills[iAttacker] : 0;
	Debug("death: " + NameOf(iAttacker) + " -> " + NameOf(iVictim) + " weapon=" + sWeapon
		  + " hs=" + (bHeadshot ? "1" : "0") + " streak=" + std::to_string(iKills));

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

// ---------------------------------------------------------------------------------------------
// C4 countdown (the reference's BombFrame)
// ---------------------------------------------------------------------------------------------

void BombStop()
{
	g_bBombActive = false;
	g_iBombHandle = 0;
}

// Handle of the planted C4. The reference kept a CHandle and only walked the entity list while it
// was dead (BombFind); this does the same, so the list is not walked on every frame.
bool BombAlive()
{
	if (qs::IsValidHandle(g_iBombHandle) && __s2sdk_IsValidEntHandle && s2sdk::IsValidEntHandle(g_iBombHandle)) return true;

	g_iBombHandle = 0;
	if (!__s2sdk_FindEntityByClassname) return false;
	// FindEntityByClassname returns a HANDLE (INVALID_EHANDLE_INDEX = -1 when nothing matches)
	const int iHandle = s2sdk::FindEntityByClassname(-1, "planted_c4");
	if (!qs::IsValidHandle(iHandle)) return false;
	if (__s2sdk_IsValidEntHandle && !s2sdk::IsValidEntHandle(iHandle)) return false;
	g_iBombHandle = iHandle;
	return true;
}

void BombFrame()
{
	if (!g_bBombActive) return;
	if (!BombAlive())
	{
		BombStop();
		return;
	}
	pisex::GlobalVars* pGlobals = pisex::GetCGlobalVars();
	if (!pGlobals || !__s2sdk_GetEntDataFloat2) return;
	// m_flC4Blow is a GameTime_t (a float wrapper): the absolute game time of the explosion. The
	// offset is resolved once - a name-based read re-hashes the class and the field on every frame
	// the bomb is ticking (the warmup read of OnPlayerDeath resolves its offset the same way, and
	// plugins/round_end_sounds reads its GameTime_t the same way).
	static int s_iBlowOffset = -2; // -2 = not resolved yet, -1 = the field was not found
	if (s_iBlowOffset == -2) s_iBlowOffset = pisex::SchemaOffset("CPlantedC4", "m_flC4Blow");
	if (s_iBlowOffset < 0) return;
	void* pBomb = pisex::EntIndexToEntPointer(pisex::EntHandleToEntIndex(g_iBombHandle));
	if (!pBomb) return;
	float flBlow = 0.0f;
	try { flBlow = (float)s2sdk::GetEntDataFloat2(pBomb, s_iBlowOffset, /*size=*/4); }
	catch (...) { return; }
	const float flLeft = flBlow - pGlobals->curtime;
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

bool AnyVolumeSoundCached()
{
	return !g_mapOwnEvents.empty();
}

void ShowMenu(int iSlot);

void ShowVolumeMenu(int iSlot)
{
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, qs::Replace(Phrase("Volume_Title"), "{volume}", std::to_string(g_iVolume[iSlot])).c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "up", Phrase("Volume_Up").c_str(), g_iVolume[iSlot] >= 100 ? pisex::ITEM_DISABLED : pisex::ITEM_DEFAULT);
	pisex::g_pMenus->AddItemMenu(hMenu, "down", Phrase("Volume_Down").c_str(), g_iVolume[iSlot] <= 0 ? pisex::ITEM_DISABLED : pisex::ITEM_DEFAULT);
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "back") { NextFrame([iSlot]() { ShowMenu(iSlot); }); return; }
		if (sBack != "up" && sBack != "down") return;
		NextFrame([iSlot, sBack]() {
			g_iVolume[iSlot] = std::clamp(g_iVolume[iSlot] + (sBack == "up" ? 10 : -10), 0, 100);
			CookieWrite(pisex::GetSteamID64(iSlot), "QuakeSounds.Volume", std::to_string(g_iVolume[iSlot]));
			ShowVolumeMenu(iSlot);
		});
	});
	if (!IsBot(iSlot)) pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowMenu(int iSlot)
{
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, Phrase("Menu_Title").c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "toggle", Phrase(g_bMuted[iSlot] ? "Menu_Unmute" : "Menu_Mute").c_str());
	if (AnyVolumeSoundCached())
		pisex::g_pMenus->AddItemMenu(hMenu, "volume", qs::Replace(Phrase("Menu_Volume"), "{volume}", std::to_string(g_iVolume[iSlot])).c_str());
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "volume") { NextFrame([iSlot]() { ShowVolumeMenu(iSlot); }); return; }
		if (sBack != "toggle") return;
		NextFrame([iSlot]() {
			ToggleMute(iSlot);
			ShowMenu(iSlot);
		});
	});
	if (!IsBot(iSlot)) pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void OnSettingsCommand(int iSlot)
{
	if (!HasController(iSlot) || IsBot(iSlot)) return;
	// no menu-free path when a volume sound exists: !qs opens the menu even with settings_menu 0
	if (g_Cfg.bSettingsMenu || AnyVolumeSoundCached()) ShowMenu(iSlot);
	else ToggleMute(iSlot);
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

std::vector<std::string> SplitArgs(const char* szContent)
{
	std::vector<std::string> v;
	std::string sCur;
	for (const char* p = szContent ? szContent : ""; *p; p++)
	{
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		{
			if (!sCur.empty()) { v.push_back(sCur); sCur.clear(); }
			continue;
		}
		if (*p != '"') sCur += *p;
	}
	if (!sCur.empty()) v.push_back(sCur);
	return v;
}

bool ValidLang(const std::string& s)
{
	if (s.size() < 2 || s.size() > 8) return false;
	for (char c : s)
		if (!isalpha((unsigned char)c) && c != '-') return false;
	return true;
}

void RegisterCommands()
{
	std::vector<std::string> vConsole = {"mm_qs"};
	pisex::g_pUtils->RegCommand(vConsole, g_Cfg.vCommands, [](int iSlot, const char*) -> bool {
		OnSettingsCommand(iSlot);
		return true;
	});
	if (!g_Cfg.vLangCommands.empty())
	{
		pisex::g_pUtils->RegCommand({}, g_Cfg.vLangCommands, [](int iSlot, const char* szContent) -> bool {
			if (!HasController(iSlot) || IsBot(iSlot)) return true;
			auto args = SplitArgs(szContent);
			// chat dispatch hands back the registered spelling first; the original stripped it too
			if (!args.empty() && (args[0][0] == '!' || args[0][0] == '/' || args[0][0] == '.')) args.erase(args.begin());
			if (args.empty() || !ValidLang(args[0]))
			{
				pisex::g_pUtils->PrintToChat(iSlot, "%s", Phrase("Lang_Usage").c_str());
				return true;
			}
			g_sLang[iSlot] = qs::Lower(args[0]);
			CookieWrite(pisex::GetSteamID64(iSlot), "QuakeSounds.Lang", g_sLang[iSlot]);
			pisex::g_pUtils->PrintToChat(iSlot, "%s", qs::Replace(Phrase("Lang_Set"), "{lang}", g_sLang[iSlot]).c_str());
			return true;
		});
	}
	// mm_quakesounds reload|enable|disable - server console only
	pisex::g_pUtils->RegCommand({"mm_quakesounds"}, {}, [](int iSlot, const char* szContent) -> bool {
		if (iSlot != -1) return true;
		const auto args = SplitArgs(szContent);
		const std::string sSub = args.size() > 1 ? qs::Lower(args[1]) : "";
		if (sSub == "reload")
		{
			LoadConfig();
			LoadPhrases();
			Log("configuration reloaded: " + std::to_string(g_mapEntries.size()) + " sounds (chat commands change after a plugin reload)");
		}
		else if (sSub == "enable" || sSub == "disable")
		{
			g_Cfg.bEnabled = sSub == "enable";
			Log(g_Cfg.bEnabled ? "enabled (until reload/restart)" : "disabled (until reload/restart)");
		}
		else
			Log("usage: mm_quakesounds reload|enable|disable");
		return true;
	});
}

// ---------------------------------------------------------------------------------------------
// Sound event volume (the PostEventPost of the original)
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnSoundEventPost(void* pUserMessage)
{
	// a callback of s2sdk (it runs under the user-message manager's lock): nothing may unwind out
	// of here
	try
	{
		int iGuid = -1;
		uint32_t uHash = 0;
		if (!sound_volume::ParseStart(pUserMessage, iGuid, uHash)) return s2sdk::ResultType::Continue;
		auto it = g_mapOwnEvents.find(uHash);
		if (it == g_mapOwnEvents.end()) return s2sdk::ResultType::Continue;

		const float flEntry = it->second;
		const uint64_t uMask = sound_volume::RecipientMask(pUserMessage);
		// the sound start is sent a tick after the EmitSound of the same frame; the volume comes
		// from the receiver's own setting
		for (int i = 0; i < 64; i++)
		{
			if (!(uMask & (1ull << i))) continue;
			const float flVolume = qs::ListenerVolume(flEntry, g_iVolume[i]);
			if (flVolume >= 1.0f) continue; // 1.0 is the soundevent's own volume: nothing to do
			Debug("volume " + std::to_string(flVolume) + " -> slot " + std::to_string(i) + " (guid "
				  + std::to_string(iGuid) + ")");
			sound_volume::Set(i, iGuid, flVolume);
		}
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

void HookEvents()
{
	// The reference's MapStartHook: entities of the previous map are gone, the streaks start over.
	// pisexcompat's StartupServer runs once per *server* start, so the per-map part lives here.
	pisex::RegisterMapStartCallback([](const char*) {
		g_mapKills.clear();
		BombStop();
	});
	pisex::g_pUtils->HookEvent("player_death", [](const char*, pisex::IGameEvent* pEvent, bool) { OnPlayerDeath(pEvent); });
	pisex::g_pUtils->HookEvent("player_disconnect", [](const char*, pisex::IGameEvent* pEvent, bool) {
		const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
		if (iSlot < 0 || iSlot >= 64) return;
		g_mapKills.erase(iSlot);
		ResetPlayerState(iSlot);
		g_uLoadedSteam[iSlot] = 0;
	});
	pisex::g_pUtils->HookEvent("round_start", [](const char*, pisex::IGameEvent*, bool) {
		if (g_Cfg.bResetOnRoundStart) g_mapKills.clear();
		BombStop();
		PlayGlobal("round_start");
	});
	pisex::g_pUtils->HookEvent("round_end", [](const char*, pisex::IGameEvent*, bool) {
		BombStop();
		PlayGlobal("round_end");
	});
	pisex::g_pUtils->HookEvent("round_freeze_end", [](const char*, pisex::IGameEvent*, bool) { PlayGlobal("round_freeze_end"); });
	pisex::g_pUtils->HookEvent("bomb_planted", [](const char*, pisex::IGameEvent*, bool) {
		PlayGlobal("bomb_planted");
		g_bBombActive = true;
		g_iBombLast = INT32_MAX;
		g_iBombHandle = 0; // look the entity up in the frame, like the original reset its CHandle
	});
	pisex::g_pUtils->HookEvent("bomb_defused", [](const char*, pisex::IGameEvent*, bool) {
		BombStop();
		PlayGlobal("bomb_defused");
	});
	pisex::g_pUtils->HookEvent("bomb_exploded", [](const char*, pisex::IGameEvent*, bool) {
		BombStop();
		PlayGlobal("bomb_exploded");
	});
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------------------------

class QuakeSoundsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override
	{
		try
		{
			pisex::Init("quake_sounds");
			g_iPluginId = static_cast<int64_t>(GetId());

			LoadConfig();
			LoadPhrases();
			for (int i = 0; i < 64; i++) g_iVolume[i] = g_Cfg.iDefaultVolume;

			if (!sound_volume::Init())
				LogError("soundevent volume control is unavailable (s2sdk user messages missing)");

			// Rebuilt on every map load, so the list is added from the manifest callback.
			if (__s2sdk_OnBuildGameSessionManifest_Register)
				s2sdk::OnBuildGameSessionManifest_Register(&OnBuildManifest);

			if (sound_volume::StartMessageId() > 0 && __s2sdk_HookUserMessage)
			{
				if (!s2sdk::HookUserMessage((int16_t)sound_volume::StartMessageId(), &OnSoundEventPost, s2sdk::HookMode::Post))
					LogError("hooking GE_SosStartSoundEvent failed - player volumes will not apply");
			}

			HookEvents();
			RegisterCommands();

			g_bReady = true;
			Log("loaded: " + std::to_string(g_mapEntries.size()) + " sounds, precache files: "
				+ std::to_string(g_Cfg.vPrecache.size()));
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed: that stops every plugify plugin
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override
	{
		try
		{
			pisex::Poll();
			if (!g_bReady) return {};

			while (!g_vNextFrame.empty())
			{
				auto fn = std::move(g_vNextFrame.front());
				g_vNextFrame.pop_front();
				fn();
			}

			ReconcileCookies((float)dt.count() / 1000.0f);
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
			g_bReady = false;
			g_vNextFrame.clear();
			if (sound_volume::StartMessageId() > 0 && __s2sdk_UnhookUserMessage)
				s2sdk::UnhookUserMessage((int16_t)sound_volume::StartMessageId(), &OnSoundEventPost, s2sdk::HookMode::Post);
			if (__s2sdk_OnBuildGameSessionManifest_Unregister)
				s2sdk::OnBuildGameSessionManifest_Unregister(&OnBuildManifest);
			pisex::Shutdown();
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
