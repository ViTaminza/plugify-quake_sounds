// Tests for the engine-free core of plugins/quake_sounds (src/quake_core.*, src/quake_config.*,
// src/kv1.*).
//
//   * config parsing against a real quake_sounds.ini of the original (sound names from workshop
//     addon 3811020435), including the production-file parity check when ref/ is present,
//   * the KV1 grammar (comments, quoted/unquoted tokens, escapes, BOM, malformed input),
//   * phrase parsing against the original's quake_sounds.phrases.txt,
//   * the kill logic (first blood, scope filters), the sound bookkeeping (soundevent hash,
//     packed GE_SosSetSoundEventParams volume parameter) and the small string helpers,
//   * the chat colour tags, the server-language chain (core.cfg -> s2sdk -> en) and the
//     direct-plugin-path-first config/phrase candidate order (raw only as fallback).
//
// No engine, plugify or s2sdk is involved: every entry of the core is a pure function of its
// inputs, so this executable links quake_sounds_core alone.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "kv1.hpp"
#include "quake_config.hpp"
#include "quake_core.hpp"

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

std::string ReadFile(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) return {};
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

std::string ReadTestFile(const std::string& sRelative)
{
	const std::string path = std::string(QS_TEST_DATA_DIR) + "/" + sRelative;
	std::string text = ReadFile(path);
	if (text.empty()) {
		std::printf("FATAL: cannot read %s\n", path.c_str());
		++g_failures;
	}
	return text;
}

void WriteFile(const std::string& path, std::string_view content)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << content;
}

using qs::Config;
using qs::Kind;
using qs::Scope;
using qs::SoundEntry;

[[maybe_unused]] bool SameEntry(const SoundEntry& a, const SoundEntry& b)
{
	return a.sKey == b.sKey && a.vSounds == b.vSounds && a.mapMessages == b.mapMessages &&
		   a.flVolume == b.flVolume && a.bHasScope == b.bHasScope && a.eScope == b.eScope;
}

[[maybe_unused]] bool SameEntries(const std::map<std::string, SoundEntry>& a, const std::map<std::string, SoundEntry>& b)
{
	if (a.size() != b.size()) return false;
	for (auto itA = a.begin(), itB = b.begin(); itA != a.end(); ++itA, ++itB)
	{
		if (itA->first != itB->first || !SameEntry(itA->second, itB->second)) return false;
	}
	return true;
}

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

	// The whole resource list is pinned: a config edit that silently drops or adds a precache
	// resource is caught here (resource parity with the live Metamod config).
	const std::vector<std::string> expectedPrecache = {
		"soundevents/soundevents_alko.vsndevts",
		"sounds/quake7/dominating.vsnd", "sounds/quake7/firstblood.vsnd",
		"sounds/quake7/hattrick.vsnd", "sounds/quake7/headshot.vsnd",
		"sounds/quake7/humiliation.vsnd", "sounds/quake7/impressive.vsnd",
		"sounds/quake7/killingspree.vsnd", "sounds/quake7/multikill.vsnd",
		"sounds/quake7/perfect.vsnd", "sounds/quake7/play.vsnd",
		"sounds/quake7/rampage.vsnd", "sounds/quake7/teamkiller.vsnd",
		"sounds/quake7/triplekill.vsnd", "sounds/quake7/unstoppable.vsnd",
		"sounds/timrc4/30sec.vsnd", "sounds/timrc4/20sec.vsnd", "sounds/timrc4/10sec.vsnd",
		"sounds/timrc4/5sec.vsnd", "sounds/timrc4/4sec.vsnd", "sounds/timrc4/3sec.vsnd",
		"sounds/timrc4/2sec.vsnd", "sounds/timrc4/1sec.vsnd",
	};
	CHECK(cfg.vPrecache == expectedPrecache);

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

	Section("config: malformed text keeps the defaults");
	Config cfg3;
	std::map<std::string, SoundEntry> entries3;
	entries3["stale"].sKey = "stale";
	CHECK(!qs::ParseConfigText("\"QuakeSounds\" { \"global\" { \"enabled\" \"0\" }", cfg3, entries3));
	CHECK(cfg3.bEnabled); // Config{} default
	CHECK(entries3.empty());
}

uint64_t Fnv1a64(std::string_view text)
{
	// A small pinned checksum of the production bytes: unlike the optional ref/ comparison this
	// also runs in the standalone repository and catches any accidental edit of the shipped files.
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const unsigned char byte : text)
	{
		hash ^= byte;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

void TestConfigParity()
{
	Section("config/phrases: pinned byte parity of the production files");
	CHECK_EQ_INT(Fnv1a64(ReadTestFile("configs/quake_sounds.ini")), 0x1ae4de1207dd3bafull);
	CHECK_EQ_INT(Fnv1a64(ReadTestFile("translations/quake_sounds.phrases.txt")), 0x2a4b6a90981f1cbfull);

	Section("config/phrase parity with the production Metamod files");
#ifdef QS_PROD_CONFIG_PATH
	// The live server's addons/configs/quake_sounds.ini is the reference: the shipped test file
	// must be byte-identical (the same file is deployed and packaged).
	CHECK_EQ_STR(ReadTestFile("configs/quake_sounds.ini"), ReadFile(QS_PROD_CONFIG_PATH));
	Config live;
	std::map<std::string, SoundEntry> liveEntries;
	CHECK(qs::ParseConfigText(ReadFile(QS_PROD_CONFIG_PATH), live, liveEntries));
	Config shipped;
	std::map<std::string, SoundEntry> shippedEntries;
	CHECK(qs::ParseConfigText(ReadTestFile("configs/quake_sounds.ini"), shipped, shippedEntries));
	CHECK(shipped.vPrecache == live.vPrecache);
	CHECK(SameEntries(shippedEntries, liveEntries));
	CHECK_EQ_INT(shipped.iDefaultVolume, live.iDefaultVolume);
	CHECK_EQ_STR(shipped.sPlayOn, live.sPlayOn);
	CHECK(shipped.eScope == live.eScope);
#else
	std::printf("   (QS_PROD_CONFIG_PATH not set: standalone repository, parity skipped)\n");
#endif
#ifdef QS_PROD_PHRASES_PATH
	CHECK_EQ_STR(ReadTestFile("translations/quake_sounds.phrases.txt"), ReadFile(QS_PROD_PHRASES_PATH));
#else
	std::printf("   (QS_PROD_PHRASES_PATH not set: standalone repository, parity skipped)\n");
#endif
}

void TestKv1()
{
	Section("KV1: the production grammar");
	const char* kText = "\xEF\xBB\xBF" // UTF-8 BOM
						"// leading comment\n"
						"\"Root\"\n"
						"{\n"
						"  \"a\" \"1\" // trailing comment\n"
						"  unquoted value\n"
						"  /* block comment */ \"b\" { \"c\" \"x\\\"y\" }\n"
						"  \"d\" { }\n"
						"}\n";
	const std::optional<qs::KvNode> root = qs::ParseKv1(kText);
	CHECK(root.has_value());
	if (!root) return;
	// a single top-level block is the file's root (Valve's loader flattening)
	CHECK_EQ_STR(root->name, "Root");
	CHECK_EQ_INT(root->GetInt("a", 0), 1);
	CHECK_EQ_STR(std::string(root->GetString("unquoted", "")), "value");
	const qs::KvNode* b = root->Find("b");
	CHECK(b != nullptr && b->isBlock);
	if (b) CHECK_EQ_STR(std::string(b->GetString("c", "")), "x\"y");
	const qs::KvNode* d = root->Find("d");
	CHECK(d != nullptr && d->isBlock);
	CHECK_EQ_INT(d ? (int)d->children.size() : -1, 0);

	Section("KV1: getters use the Valve semantics");
	const char* kGetters = R"("R" { "i" "5x" "neg" "-3" "f" "1.5x" "empty" "" "text" "abc" })";
	const std::optional<qs::KvNode> g = qs::ParseKv1(kGetters);
	CHECK(g.has_value());
	if (g)
	{
		CHECK_EQ_INT(g->GetInt("i", 99), 5);   // atoi("5x")
		CHECK_EQ_INT(g->GetInt("neg", 0), -3);
		CHECK_EQ_INT(g->GetInt("text", 0), 0); // atoi("abc")
		CHECK_EQ_INT(g->GetInt("missing", 7), 7);
		CHECK(g->GetFloat("f", 0.0f) > 1.49f && g->GetFloat("f", 0.0f) < 1.51f);
		CHECK_EQ_INT(g->GetInt("empty", 12), 12); // empty value -> default
	}

	Section("KV1: malformed input is rejected as a whole");
	CHECK(!qs::ParseKv1("\"R\" { \"a\" \"unterminated }").has_value());
	CHECK(!qs::ParseKv1("\"R\" { \"a\" \"1\" /* unterminated").has_value());
	CHECK(!qs::ParseKv1("\"R\" { \"a\" \"1\"").has_value()); // missing '}'
	CHECK(!qs::ParseKv1("\"R\" { } }").has_value());         // stray top-level '}'

	// #base / #include / #define are skipped instead of followed
	const std::optional<qs::KvNode> includes = qs::ParseKv1(
		"\"R\"\n{\n  #include \"other.cfg\"\n  \"a\" \"1\"\n}\n");
	CHECK(includes.has_value());
	if (includes) CHECK_EQ_INT(includes->GetInt("a", 0), 1);
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

	Section("phrases: language fallback and message resolution");
	// the server language selects the file entry; a missing entry falls back to "en"
	CHECK_EQ_STR(qs::Phrase(ru, "en", "Menu_Title"), "Quake-звуки"); // map was parsed for ru only
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

	Section("phrases: language names and the core.cfg -> s2sdk -> en chain");
	CHECK_EQ_STR(qs::NormalizeLanguage("russian"), "ru");
	CHECK_EQ_STR(qs::NormalizeLanguage("English"), "en");
	CHECK_EQ_STR(qs::NormalizeLanguage("ukrainian"), "ua");
	CHECK_EQ_STR(qs::NormalizeLanguage("pt-br"), "pt-BR");
	CHECK_EQ_STR(qs::NormalizeLanguage("schinese"), "zh-CN");
	CHECK_EQ_STR(qs::NormalizeLanguage(" EN "), "en");
	CHECK_EQ_STR(qs::PickLanguage("russian", "english"), "ru");
	CHECK_EQ_STR(qs::PickLanguage("", "english"), "en");
	CHECK_EQ_STR(qs::PickLanguage("", ""), "en");
	CHECK_EQ_STR(qs::PickLanguage("german", "russian"), "de");

	// <configs>/core.cfg "ServerLang"
	const std::filesystem::path base =
		std::filesystem::temp_directory_path() /
		("quake_sounds_lang_" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code error;
	std::filesystem::remove_all(base, error);
	std::filesystem::create_directories(base, error);
	WriteFile((base / "core.cfg").string(), "\"Core\"\n{\n    \"ServerLang\"    \"russian\"\n}\n");
	CHECK_EQ_STR(qs::ReadCoreConfigLanguage(base.string()), "russian");
	CHECK_EQ_STR(qs::PickLanguage(qs::ReadCoreConfigLanguage(base.string()), "english"), "ru");
	std::filesystem::remove(base / "core.cfg", error);
	CHECK_EQ_STR(qs::ReadCoreConfigLanguage(base.string()), "");
	WriteFile((base / "core.cfg").string(), "\"Core\" { \"ServerLang\" ");
	CHECK_EQ_STR(qs::ReadCoreConfigLanguage(base.string()), ""); // malformed -> no language
	std::filesystem::remove_all(base, error);
}

void TestPaths()
{
	Section("paths: HEAD direct <configs>/quake_sounds paths first, raw only as fallback");
	const std::vector<std::string> configCandidates = qs::ConfigCandidates("/cfg", "/data");
	CHECK_EQ_INT(configCandidates.size(), 4);
	CHECK_EQ_STR(configCandidates[0], "/cfg/quake_sounds/quake_sounds.ini");
	CHECK_EQ_STR(configCandidates[1], "addons/configs/quake_sounds.ini");
	CHECK_EQ_STR(configCandidates[2], "/cfg/quake_sounds/translations/quake_sounds.ini");
	CHECK_EQ_STR(configCandidates[3], "/data/quake_sounds/quake_sounds.ini");

	const std::vector<std::string> fallback = qs::ConfigCandidates("", "");
	CHECK_EQ_INT(fallback.size(), 4);
	CHECK_EQ_STR(fallback[0], "quake_sounds/quake_sounds.ini");
	CHECK_EQ_STR(fallback[1], "addons/configs/quake_sounds.ini");
	CHECK_EQ_STR(fallback[2], "quake_sounds/translations/quake_sounds.ini");
	CHECK_EQ_STR(fallback[3], "quake_sounds/quake_sounds.ini");

	const std::vector<std::string> phrases = qs::PhraseCandidates("/cfg");
	CHECK_EQ_INT(phrases.size(), 4);
	CHECK_EQ_STR(phrases[0], "/cfg/quake_sounds/translations/quake_sounds.phrases.txt");
	CHECK_EQ_STR(phrases[1], "addons/translations/quake_sounds.phrases.txt");
	CHECK_EQ_STR(phrases[2],
				 "/cfg/quake_sounds/translations/addons/translations/quake_sounds.phrases.txt");
	CHECK_EQ_STR(phrases[3], "/cfg/quake_sounds/addons/translations/quake_sounds.phrases.txt");

	Section("paths: direct file wins, raw fallback only when direct is absent, malformed direct not masked");
	const std::filesystem::path base =
		std::filesystem::temp_directory_path() /
		("quake_sounds_paths_" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code error;
	std::filesystem::remove_all(base, error);
	std::filesystem::create_directories(base / "cfg/quake_sounds/translations", error);
	std::filesystem::create_directories(base / "data", error);
	std::filesystem::create_directories(base / "cwd/addons/configs", error);
	std::filesystem::create_directories(base / "cwd/addons/translations", error);

	const std::string shipped = ReadTestFile("configs/quake_sounds.ini");
	const std::string shippedPhrases = ReadTestFile("translations/quake_sounds.phrases.txt");
	const std::string directPath = (base / "cfg/quake_sounds/quake_sounds.ini").string();
	const std::string directPhrasesPath = (base / "cfg/quake_sounds/translations/quake_sounds.phrases.txt").string();
	WriteFile(directPath, shipped);
	WriteFile(directPhrasesPath, shippedPhrases);

	// HEAD path: the direct plugin file is authoritative.
	Config cfg;
	std::map<std::string, SoundEntry> entries;
	qs::ConfigLoad load = qs::LoadConfigFromDisk((base / "cfg").string(), (base / "data").string(), cfg, entries);
	CHECK(load.loaded);
	CHECK_EQ_STR(load.path, directPath);
	CHECK_EQ_INT(cfg.iDefaultVolume, 30);
	std::map<std::string, std::string> phrasesMap;
	qs::PhrasesLoad phraseLoad = qs::LoadPhrasesFromDisk((base / "cfg").string(), "ru", phrasesMap);
	CHECK(phraseLoad.loaded);
	CHECK_EQ_STR(phraseLoad.path, directPhrasesPath);
	CHECK_EQ_STR(phrasesMap["Menu_Title"], "Quake-звуки");

	// A different raw cwd decoy must NOT shadow the maintained direct Plugify files.
	const std::filesystem::path previous = std::filesystem::current_path(error);
	std::filesystem::current_path(base / "cwd", error);
	WriteFile((base / "cwd/addons/configs/quake_sounds.ini").string(),
			  "\"QuakeSounds\"\n{\n  \"global\" { \"default_volume\" \"0.9\" }\n}\n");
	WriteFile((base / "cwd/addons/translations/quake_sounds.phrases.txt").string(),
			  "\"Phrases\"\n{\n  \"Menu_Title\" { \"ru\" \"RawTitle\" \"en\" \"RawTitle\" }\n}\n");
	Config directCfg;
	std::map<std::string, SoundEntry> directEntries;
	load = qs::LoadConfigFromDisk((base / "cfg").string(), (base / "data").string(), directCfg, directEntries);
	CHECK(load.loaded && load.path == directPath);
	CHECK_EQ_INT(directCfg.iDefaultVolume, 30); // not the raw decoy's 90
	std::map<std::string, std::string> directPhrases;
	qs::PhrasesLoad directPhraseLoad =
		qs::LoadPhrasesFromDisk((base / "cfg").string(), "ru", directPhrases);
	CHECK(directPhraseLoad.loaded && directPhraseLoad.path == directPhrasesPath);
	CHECK_EQ_STR(directPhrases["Menu_Title"], "Quake-звуки"); // not RawTitle

	// Without the direct files the raw cwd spelling is the fallback (hybrid installation).
	std::filesystem::remove(directPath, error);
	std::filesystem::remove(directPhrasesPath, error);
	Config rawCfg;
	std::map<std::string, SoundEntry> rawEntries;
	load = qs::LoadConfigFromDisk((base / "cfg").string(), (base / "data").string(), rawCfg, rawEntries);
	CHECK(load.loaded);
	CHECK_EQ_STR(load.path, "addons/configs/quake_sounds.ini");
	CHECK_EQ_INT(rawCfg.iDefaultVolume, 90);
	std::map<std::string, std::string> rawPhrases;
	qs::PhrasesLoad rawLoad = qs::LoadPhrasesFromDisk((base / "cfg").string(), "ru", rawPhrases);
	CHECK(rawLoad.loaded);
	CHECK_EQ_STR(rawLoad.path, "addons/translations/quake_sounds.phrases.txt");
	CHECK_EQ_STR(rawPhrases["Menu_Title"], "RawTitle");

	// A malformed direct file is reported, never masked by a good raw fallback.
	WriteFile(directPath, "\"QuakeSounds\" { \"global\" ");
	Config brokenCfg;
	std::map<std::string, SoundEntry> brokenEntries;
	load = qs::LoadConfigFromDisk((base / "cfg").string(), (base / "data").string(), brokenCfg, brokenEntries);
	CHECK(!load.loaded);
	CHECK(load.error.find(directPath) != std::string::npos);
	CHECK_EQ_INT(brokenCfg.iDefaultVolume, 100); // defaults, not the raw fallback's 90
	CHECK(brokenEntries.empty());

	std::filesystem::current_path(previous, error);
	std::filesystem::remove_all(base, error);
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
}

void TestColorize()
{
	Section("chat colour tags (the cs2-menus bytes)");
	CHECK_EQ_STR(qs::Colorize("{DEFAULT}a{RED}b{GREEN}c"),
				 std::string("\x01" "a" "\x02" "b" "\x04" "c"));
	CHECK_EQ_STR(qs::Colorize("{WHITE}{LIME}{LIGHTGREEN}{DARKRED}{GRAY}"),
				 std::string("\x01\x05\x06\x07\x08"));
	CHECK_EQ_STR(qs::Colorize("{LIGHTOLIVE}{OLIVE}{LIGHTBLUE}{BLUE}{PURPLE}{LIGHTRED}{GRAYBLUE}"),
				 std::string("\x09\x10\x0B\x0C\x0E\x0F\x0A"));
	// a line break is U+2029, which the CS2 chat renderer shows as a new line
	CHECK_EQ_STR(qs::Colorize("a\\nb"), std::string("a\xe2\x80\xa9" "b"));
	// unknown tags and the prefix placeholder are left for Phrase()/the caller
	CHECK_EQ_STR(qs::Colorize("{PREFIX} {UNKNOWN}"), "{PREFIX} {UNKNOWN}");
}

void TestStrings()
{
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
	if (!std::filesystem::exists(std::string(QS_TEST_DATA_DIR) + "/configs/quake_sounds.ini") ||
		!std::filesystem::exists(std::string(QS_TEST_DATA_DIR) +
								 "/translations/quake_sounds.phrases.txt"))
	{
		std::printf("FATAL: test data not found under %s\n", QS_TEST_DATA_DIR);
		return 1;
	}
	TestConfig();
	TestConfigParity();
	TestKv1();
	TestPhrases();
	TestPaths();
	TestKillLogic();
	TestHashes();
	TestColorize();
	TestStrings();
	TestScopeParsing();
	TestHandles();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
