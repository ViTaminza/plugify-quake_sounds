// Tests for plugins/quake_sounds/src/quake_core.hpp (the engine-independent core of the ported
// cs2-quake-sounds).
//
//   * config parsing against a real quake_sounds.ini of the original (sound names from workshop
//     addon 3811020435),
//   * phrase parsing against the original's quake_sounds.phrases.txt,
//   * the kill logic (first blood, scope filters), the sound bookkeeping (soundevent hash,
//     packed GE_SosSetSoundEventParams volume parameter) and the small string helpers.
//
// No engine is involved: every entry of the core is a pure function of its inputs.

#include <plg/plugin.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "quake_core.hpp"

// The plugin gets these from PLUGIFY_PLUGIN; pisexcompat references them, the test leaves them
// null (which is also the state the library has to survive).
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

#ifndef QS_TEST_DATA_DIR
#define QS_TEST_DATA_DIR "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name)
{
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),             \
						g_section.data(), __FILE__, __LINE__, #expr);                              \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_ = (actual);                                                           \
		const std::string e_ = (expected);                                                         \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_.c_str(), e_.c_str());                                                   \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                            \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_, e_);                                                                   \
		}                                                                                          \
	} while (false)

std::string ReadTestFile(const std::string& sRelative)
{
	std::ifstream file(std::string(QS_TEST_DATA_DIR) + "/" + sRelative, std::ios::binary);
	if (!file) {
		std::printf("FATAL: cannot read %s\n", sRelative.c_str());
		++g_failures;
		return {};
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

using qs::Config;
using qs::Kind;
using qs::Scope;
using qs::SoundEntry;

void TestConfig()
{
	Section("config: the production quake_sounds.ini");
	Config cfg;
	std::map<std::string, SoundEntry> entries;
	CHECK(qs::ParseConfigText(ReadTestFile("configs/quake_sounds.ini"), cfg, entries));

	CHECK(cfg.bEnabled);
	CHECK(!cfg.bDebug);
	CHECK(cfg.bWarmup);
	CHECK_EQ_STR(cfg.sPlayOn, "player");
	CHECK_EQ_INT(cfg.iDefaultVolume, 30); // "default_volume" 0.3
	CHECK(cfg.eScope == Scope::Attacker); // "sound_hearable_by" attacker
	CHECK(!cfg.bIgnoreBots);
	CHECK(cfg.bIgnoreWorld); // ignore_world_damage 1
	CHECK(!cfg.bCountSelf);
	CHECK(!cfg.bCountTeam);
	CHECK(cfg.bResetOnDeath);      // reset_kills_on_death 1
	CHECK(cfg.bResetOnRoundStart); // reset_kills_on_round_start 1
	CHECK(cfg.bHeadshotOnce);      // headshot_once 1
	CHECK(!cfg.bCenter);           // enable_center_message 0
	CHECK(!cfg.bChat);             // enable_chat_message 0
	CHECK_EQ_STR(cfg.sCenterType, "default");
	CHECK(!cfg.bSettingsMenu); // settings_menu 0
	CHECK(cfg.bUtilsSettings); // utils_settings_menu 1
	CHECK_EQ_INT(cfg.vCommands.size(), 1);
	CHECK_EQ_STR(cfg.vCommands[0], "!qs");
	CHECK_EQ_INT(cfg.vLangCommands.size(), 1);
	CHECK_EQ_STR(cfg.vLangCommands[0], "!lang");

	// precache: the soundevents file plus the 22 additional_files entries
	CHECK_EQ_INT(cfg.vPrecache.size(), 23);
	CHECK_EQ_STR(cfg.vPrecache[0], "soundevents/soundevents_alko.vsndevts");
	CHECK_EQ_STR(cfg.vPrecache[1], "sounds/quake7/dominating.vsnd");
	CHECK_EQ_STR(cfg.vPrecache[22], "sounds/timrc4/1sec.vsnd");

	// sound_priorities: special_events 1 < weapons 2 < kill_streak 3
	CHECK_EQ_INT(cfg.vPriority.size(), 3);
	CHECK(cfg.vPriority[0] == Kind::Special);
	CHECK(cfg.vPriority[1] == Kind::Weapons);
	CHECK(cfg.vPriority[2] == Kind::KillStreak);

	Section("config: sound records");
	CHECK_EQ_INT(entries.size(), 28); // 2..14 (13), firstblood, knifekill, teamkill, selfkill,
									  // headshot (5), weapon_hegrenade, round_freeze_end,
									  // bomb_30/20/10/5/4/3/2/1 (8)
	for (int i = 2; i <= 14; i++)
	{
		auto it = entries.find(std::to_string(i));
		CHECK(it != entries.end());
		if (it == entries.end()) continue;
		CHECK(!it->second.vSounds.empty());
	}
	// only the record "3" of the config carries a _filter
	CHECK(!entries["2"].bHasScope);
	CHECK(!entries["4"].bHasScope);
	const SoundEntry& triple = entries["3"];
	CHECK(triple.bHasScope && triple.eScope == Scope::All); // "_filter" "all"
	CHECK_EQ_STR(triple.mapMessages.at("ru"), "Тройное убийство");
	CHECK_EQ_STR(triple.mapMessages.at("en"), "Triple Kill");
	CHECK_EQ_STR(triple.vSounds[0], "Alko.Quake7.Triplekill");

	const SoundEntry& hs = entries["headshot"];
	CHECK_EQ_STR(hs.vSounds[0], "Alko.Quake7.Headshot");
	CHECK(hs.flVolume < 0.0f); // no _volume -> the soundevent's own volume

	CHECK(entries.find("weapon_hegrenade") != entries.end());
	CHECK(entries.find("round_freeze_end") != entries.end());
	CHECK(entries.find("bomb_1") != entries.end());
	CHECK(entries.find("bomb_30") != entries.end());
	// the record without _sound is skipped, exactly like the original ("lastmanstanding" is
	// commented out in the config)
	CHECK(entries.find("lastmanstanding") == entries.end());

	Section("config: _volume and _filter parsing (in-memory)");
	Config cfg2;
	std::map<std::string, SoundEntry> entries2;
	const char* kText = R"(
"QuakeSounds"
{
	"global" { "sound_hearable_by" "victim_team" "default_volume" "1.0" }
	"sounds"
	{
		"x" { "_sound" "Quake.Foo,Quake.Bar" "_volume" "0.5" "_filter" "involved" "en" "X" }
		"y" { "_sound" "" }
	}
}
)";
	CHECK(qs::ParseConfigText(kText, cfg2, entries2));
	CHECK(cfg2.eScope == Scope::VictimTeam);
	CHECK_EQ_INT(cfg2.iDefaultVolume, 100);
	CHECK_EQ_INT(entries2.size(), 1);
	CHECK_EQ_INT(entries2["x"].vSounds.size(), 2);
	CHECK_EQ_STR(entries2["x"].vSounds[1], "Quake.Bar");
	CHECK(entries2["x"].flVolume > 0.49f && entries2["x"].flVolume < 0.51f);
	CHECK(entries2["x"].eScope == Scope::Involved);
	CHECK_EQ_STR(entries2["x"].mapMessages.at("en"), "X");
}

void TestPhrases()
{
	Section("phrases: the production file");
	std::map<std::string, std::string> ru;
	std::map<std::string, std::string> en;
	CHECK(qs::ParsePhrasesText(ReadTestFile("translations/quake_sounds.phrases.txt"), "ru", ru));
	CHECK(qs::ParsePhrasesText(ReadTestFile("translations/quake_sounds.phrases.txt"), "en", en));
	CHECK(!ru.empty());
	CHECK(!en.empty());

	// prefix expansion of every other key
	const std::string sMuted = qs::Phrase(ru, "ru", "Muted");
	CHECK(sMuted.find(qs::Phrase(ru, "ru", "Prefix")) != std::string::npos);
	CHECK(sMuted.find("{PREFIX}") == std::string::npos);
	CHECK_EQ_STR(qs::Phrase(ru, "ru", "Menu_Title"), "Quake-звуки");
	CHECK_EQ_STR(qs::Phrase(en, "en", "Menu_Title"), "Quake Sounds");

	// an unknown key falls back to the key itself, a missing store to the built-in table
	CHECK_EQ_STR(qs::Phrase(ru, "ru", "No_Such_Key"), "No_Such_Key");
	std::map<std::string, std::string> empty;
	CHECK_EQ_STR(qs::Phrase(empty, "ru", "Menu_Mute"), "Выключить звуки");
	CHECK_EQ_STR(qs::Phrase(empty, "en", "Menu_Mute"), "Mute Sounds");

	Section("phrases: message resolution");
	const SoundEntry entry = [] {
		SoundEntry e;
		e.mapMessages = {{"ru", "Первая кровь"}, {"en", "First Blood"}};
		return e;
	}();
	CHECK_EQ_STR(qs::ResolveMessage(entry, "", "ru"), "Первая кровь");
	CHECK_EQ_STR(qs::ResolveMessage(entry, "en", "ru"), "First Blood");
	CHECK_EQ_STR(qs::ResolveMessage(entry, "ru-RU", "en"), "Первая кровь");
	CHECK_EQ_STR(qs::ResolveMessage(entry, "de", "en"), "First Blood");
	// neither language matches: the alphabetically first key of the message map ("en" < "ru"),
	// exactly like the original's std::map begin()
	CHECK_EQ_STR(qs::ResolveMessage(entry, "de", ""), "First Blood");
	CHECK_EQ_STR(qs::NormalizeLang("ru-RU"), "ru");
	CHECK_EQ_STR(qs::NormalizeLang(" EN "), "en");
}

void TestKillLogic()
{
	Section("kill logic");
	std::map<int, int> kills;
	CHECK(!qs::IsFirstBlood(kills, 3));
	kills[3] = 1;
	CHECK(qs::IsFirstBlood(kills, 3));
	kills[4] = 1;
	CHECK(!qs::IsFirstBlood(kills, 3)); // somebody else killed too
	kills.erase(4);
	kills[3] = 2;
	CHECK(!qs::IsFirstBlood(kills, 3)); // his second kill
	CHECK(!qs::IsFirstBlood(kills, 7));

	// the Matches() switch of the original
	//                     listenerTeam  attacker  attackerTeam  victim  victimTeam
	CHECK(qs::MatchesScope(5, 2, 5, 2, 6, 3, Scope::All));
	CHECK(qs::MatchesScope(7, 2, 5, 2, 6, 3, Scope::AttackerTeam));
	CHECK(!qs::MatchesScope(7, 3, 5, 2, 6, 3, Scope::AttackerTeam));
	CHECK(qs::MatchesScope(7, 3, 5, 2, 6, 3, Scope::VictimTeam));
	CHECK(!qs::MatchesScope(7, 2, 5, 2, 6, 3, Scope::VictimTeam));
	CHECK(qs::MatchesScope(5, 2, 5, 2, 6, 3, Scope::Involved));
	CHECK(qs::MatchesScope(6, 3, 5, 2, 6, 3, Scope::Involved));
	CHECK(!qs::MatchesScope(7, 2, 5, 2, 6, 3, Scope::Involved));
	CHECK(qs::MatchesScope(5, 2, 5, 2, 6, 3, Scope::Attacker));
	CHECK(qs::MatchesScope(6, 3, 5, 2, 6, 3, Scope::Victim));
	CHECK(qs::MatchesScope(7, 1, 5, 2, 6, 3, Scope::Spectator));
	CHECK(!qs::MatchesScope(7, 2, 5, 2, 6, 3, Scope::Spectator));
	// global records (round/bomb) have no attacker/victim: attacker_team matches nobody
	CHECK(!qs::MatchesScope(7, 2, -1, 0, -1, 0, Scope::AttackerTeam));
	CHECK(qs::MatchesScope(7, 2, -1, 0, -1, 0, Scope::All));

	Section("sound bookkeeping");
	CHECK(qs::IsFilePathSound("sounds/quake7/dominating.vsnd"));
	CHECK(qs::IsFilePathSound("Sounds/Quake7/Dominating.VSND"));
	CHECK(!qs::IsFilePathSound("Alko.Quake7.Doublekill"));

	std::map<std::string, SoundEntry> entries;
	SoundEntry e1;
	e1.sKey = "a";
	e1.vSounds = {"sounds/x.vsnd"};
	entries["a"] = e1;
	CHECK(!qs::AnyVolumeSound(entries));
	SoundEntry e2;
	e2.sKey = "b";
	e2.vSounds = {"Alko.Track.10"};
	entries["b"] = e2;
	CHECK(qs::AnyVolumeSound(entries));

	CHECK_EQ_STR(qs::WeaponSoundKey("deagle"), "weapon_deagle");
	CHECK_EQ_STR(qs::WeaponSoundKey("weapon_ak47"), "weapon_ak47");
	CHECK_EQ_STR(qs::WeaponSoundKey("world"), "weapon_world");

	// the C4 countdown key of a whole number of remaining seconds (the reference's "bomb_" + iLeft)
	CHECK_EQ_STR(qs::BombKey(3), "bomb_3");
	CHECK_EQ_STR(qs::BombKey(30), "bomb_30");
	CHECK_EQ_STR(qs::BombKey(0), "bomb_0");

	CHECK(qs::ListenerVolume(0.5f, 30) > 0.14f && qs::ListenerVolume(0.5f, 30) < 0.16f);
	CHECK(qs::ListenerVolume(1.0f, 30) > 0.29f && qs::ListenerVolume(1.0f, 30) < 0.31f);
	CHECK(qs::ListenerVolume(1.0f, 0) == 0.0f);
	CHECK(qs::PickSoundIndex(3, 0) == 0);
	CHECK(qs::PickSoundIndex(3, 2) == 2);
	CHECK(qs::PickSoundIndex(3, 4) == 1);
	CHECK(qs::PickSoundIndex(3, 12345) == (size_t)(12345u % 3u));
}

void TestHashes()
{
	Section("soundevent hash and the packed volume parameter");

	// MurmurHash2 (seed 0x53524332 = 'CS2R'), lower-cased input; the engine hashes soundevent
	// names the same way (reference: SoundVolume::EventHash).
	const uint32_t uHashA = qs::SoundEventHash("Alko.Quake7.Doublekill");
	const uint32_t uHashB = qs::SoundEventHash("alko.quake7.doublekill");
	CHECK_EQ_INT(uHashA, uHashB); // case-insensitive
	CHECK(uHashA != qs::SoundEventHash("Alko.Quake7.Triplekill"));
	CHECK_EQ_INT(qs::SoundEventHash(""), qs::SoundEventHash("")); // stable

	// Pinned values (printed below): a change here means the volume of every soundevent would be
	// wrong in game.
	std::printf("   hash(alko.quake7.doublekill) = 0x%08X\n", uHashA);
	std::printf("   hash(alko.track.10)         = 0x%08X\n", qs::SoundEventHash("Alko.Track.10"));
	std::printf("   hash(volume, param seed)    = 0x%08X\n",
				(uint32_t)(unsigned char)qs::PackVolumeParam(0.5f)[0]
					| ((uint32_t)(unsigned char)qs::PackVolumeParam(0.5f)[1] << 8)
					| ((uint32_t)(unsigned char)qs::PackVolumeParam(0.5f)[2] << 16)
					| ((uint32_t)(unsigned char)qs::PackVolumeParam(0.5f)[3] << 24));

	// The packed parameter is hash("volume", 0xB5D9F700) | type 4 (float) | size 4 | 0 | value.
	const std::string sPacked = qs::PackVolumeParam(0.25f);
	CHECK_EQ_INT(sPacked.size(), 11);
	CHECK_EQ_INT((unsigned char)sPacked[4], 4); // type: float
	CHECK_EQ_INT((unsigned char)sPacked[5], 4); // size
	CHECK_EQ_INT((unsigned char)sPacked[6], 0);
	// MurmurHash2("volume", 0xB5D9F700) with its tail - the value the reference sound_volume.h
	// computes (and round_end_sounds sends); without the tail the client ignored the volume.
	uint32_t uPackedHash = 0;
	std::memcpy(&uPackedHash, sPacked.data(), 4);
	CHECK_EQ_INT(uPackedHash, 0xBD6054E9u);
	float flValue = 0.0f;
	std::memcpy(&flValue, sPacked.data() + 7, 4);
	CHECK(flValue > 0.24f && flValue < 0.26f);
	// stable for the same value, different for another one
	CHECK(qs::PackVolumeParam(1.0f) == qs::PackVolumeParam(1.0f));
	CHECK(qs::PackVolumeParam(1.0f) != qs::PackVolumeParam(0.5f));

	Section("string helpers");
	CHECK_EQ_STR(qs::Lower("AbC"), "abc");
	CHECK_EQ_STR(qs::Trim("  a b \r\n"), "a b");
	CHECK_EQ_STR(qs::Replace("x{y}x{y}", "{y}", "Z"), "xZxZ");
	const std::vector<std::string> v = qs::SplitList(" a , b ,, c ");
	CHECK_EQ_INT(v.size(), 3);
	CHECK_EQ_STR(v[0], "a");
	CHECK_EQ_STR(v[2], "c");
}

void TestScopeParsing()
{
	Section("scope / list parsing");
	CHECK(qs::ParseScope("all") == Scope::All);
	CHECK(qs::ParseScope("attacker_team") == Scope::AttackerTeam);
	CHECK(qs::ParseScope("VICTIM_TEAM") == Scope::VictimTeam);
	CHECK(qs::ParseScope(" involved ") == Scope::Involved);
	CHECK(qs::ParseScope("attacker") == Scope::Attacker);
	CHECK(qs::ParseScope("victim") == Scope::Victim);
	CHECK(qs::ParseScope("spectator") == Scope::Spectator);
	CHECK(qs::ParseScope("garbage") == Scope::All); // unknown -> all, like the original
	CHECK(qs::SplitList("").empty());
}

void TestHandles()
{
	Section("entity handle validity");
	// a CEntityHandle is a uint32 view (entity index in the low 15 bits, serial above): the pawn
	// handle 0xC912805D reads as the int32 -921534371 (index 93, serial 102949) and is VALID;
	// the sign means nothing
	CHECK(qs::IsValidHandle(-921534371));
	CHECK(qs::IsValidHandle(93));
	CHECK(!qs::IsValidHandle(0));  // index 0 serial 0 = the world, and the "none" of the helpers
	CHECK(!qs::IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

} // namespace

int main()
{
	TestConfig();
	TestPhrases();
	TestKillLogic();
	TestHashes();
	TestScopeParsing();
	TestHandles();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
