// quake_sounds plugin tests — no engine: src/plugin.cpp is compiled into this TU (its state and
// handlers live in an anonymous namespace) and every __s2sdk_* / __permissions_* binding slot is a
// recording fake.
//
// Covered:
//   * startup (PITFALLS A1): OnPluginStart is Ok even with missing config/bindings, every
//     registration happens once (map listeners, eight game events, commands, manifest callback,
//     user-message hook), no client/entity/schema call is made before the first map (B1), the
//     once-per-second reconcile owns no s2sdk timer (C9/B4);
//   * the map gate (B1/B2): no client call in OnPluginStart, between OnMapEnd and OnMapStart, or
//     after OnMapEnd; a plugin loaded into a running map opens the gate on its first update, an
//     OnMapEnd seen before that keeps it shut;
//   * kills/streaks: first blood, headshot_once, weapon and streak priority, selfkill (world
//     handle 0), teamkill, knife, reset_on_death, reset_on_round_start, warmup gate and
//     ignore_world_damage; "userid"/"attacker" are read with GetEventPlayerSlot (GetEventInt is
//     never used), the event is always left Continue;
//   * bots and slot reuse: bot kills are not sent to bots, ignore_bots skips both sides, a slot
//     reused by another player never inherits the previous streak/mute/volume (with and without
//     a player_disconnect event), a changed bot handle resets a bot's streak;
//   * cookies (permissions): LoadUser + GetCookie restore mute/language/volume, a reused slot is
//     reset while LoadUser is in flight, a failed/unavailable permissions plugin keeps the
//     session defaults;
//   * sounds/resources: EmitSound per listener with the per-player volume, signed (negative but
//     valid) CEntityHandle values reach EmitSound, client "play" for sounds/... paths, the
//     manifest precaches exactly the production 23-resource list, GE_SosSetSoundEventParams
//     carries the packed volume for the right guid/mask, and the user-message hook is removed;
//   * native menus/commands: !qs opens the toggle+volume menu, console qs is refused, mm_qs works
//     from the client console, !lang writes the cookie, mm_quakesounds is server-console-only,
//     menu selection is deferred out of the handler (no cookie/action inside it), disabled volume
//     items, ExitBack returns to the parent menu, a map end destroys menus, command ownership is
//     teardown-correct (RemoveCommand vs RemoveCommandListener);
//   * config/phrases (HEAD direct <configs>/quake_sounds paths first, raw only as fallback) and
//     the core.cfg -> s2sdk -> en language chain;
//     malformed/missing files keep the defaults; the C4 countdown reads m_flC4Blow from a signed
//     handle and follows game time; the warmup field is read at its schema offset only with the
//     gate open;
//   * teardown (A7/C11): OnPluginEnd removes commands/events/listeners/hooks/menus and drops the
//     state, a reload registers everything once again, and no s2sdk call happens after the end.
//
// Run by ./test.sh (build/tests/quake_sounds_plugin_tests); no pisexcompat and no engine are
// involved. This is not a game-server test: it only drives recording fakes.

#include "../src/plugin.cpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what)
{
	++g_checks;
	if (!condition)
	{
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what)
{
	Check(actual == expected,
		  what + " (got " + std::to_string(actual) + ", want " + std::to_string(expected) + ")");
}

void CheckEqStr(const std::string& actual, const std::string& expected, const std::string& what)
{
	Check(actual == expected, what + " (got \"" + actual + "\", want \"" + expected + "\")");
}

void Section(const char* name) { std::printf("== %s\n", name); }

// ==========================================================================================
// config fixture: <configs>/quake_sounds/..., <configs>/core.cfg and a throwaway cwd
// ==========================================================================================

std::string g_baseDir;
std::string g_configsDir;
std::string g_dataDir;
std::string g_cwdDir;
std::filesystem::path g_originalCwd;

std::string ConfigPath() { return g_configsDir + "/quake_sounds/quake_sounds.ini"; }
std::string PhrasesPath() { return g_configsDir + "/quake_sounds/translations/quake_sounds.phrases.txt"; }
std::string CoreConfigPath() { return g_configsDir + "/core.cfg"; }
std::string RawConfigPath() { return "addons/configs/quake_sounds.ini"; }
std::string RawPhrasesPath() { return "addons/translations/quake_sounds.phrases.txt"; }

std::string ReadTextFile(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) return {};
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

void WriteTextFile(const std::string& path, std::string_view content)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << content;
}

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
{
	std::size_t pos = 0;
	while (!from.empty() && (pos = text.find(from, pos)) != std::string::npos)
	{
		text.replace(pos, from.size(), to);
		pos += to.size();
	}
	return text;
}

std::string ReplaceKeyValue(std::string text, const std::string& key, const std::string& value)
{
	const std::string needle = "\"" + key + "\"";
	const std::size_t keyPos = text.find(needle);
	if (keyPos == std::string::npos) return text;
	std::size_t open = text.find('"', keyPos + needle.size());
	if (open == std::string::npos) return text;
	std::size_t close = text.find('"', open + 1);
	if (close == std::string::npos) return text;
	text.replace(open + 1, close - open - 1, value);
	return text;
}

std::string ShippedConfig() { return ReadTextFile(QUAKE_SOUNDS_TEST_CONFIG); }
std::string ShippedPhrases() { return ReadTextFile(QUAKE_SOUNDS_TEST_PHRASES); }

void SetConfigText(const std::string& text) { WriteTextFile(ConfigPath(), text); }
void SetRawConfigText(const std::string& text) { WriteTextFile(RawConfigPath(), text); }
void RemoveRawConfig() { std::error_code error; std::filesystem::remove(RawConfigPath(), error); }
void SetRawPhrasesText(const std::string& text) { WriteTextFile(RawPhrasesPath(), text); }
void RemoveRawPhrases() { std::error_code error; std::filesystem::remove(RawPhrasesPath(), error); }

void RestoreConfigFiles()
{
	SetConfigText(ShippedConfig());
	WriteTextFile(PhrasesPath(), ShippedPhrases());
	WriteTextFile(CoreConfigPath(), "\"Core\"\n{\n    \"ServerLang\"    \"russian\"\n}\n");
	RemoveRawConfig();
	RemoveRawPhrases();
}

// ==========================================================================================
// fake clients
// ==========================================================================================

struct FakeClient
{
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool bot = false;
	bool alive = false;
	int team = 0;
	uint64_t steam = 0;
	std::string name;
	bool hasPawn = true;
	int32_t controllerHandle = 0;
};

FakeClient g_clients[64];
int g_clientCalls = 0;
int g_entityCalls = 0;
int g_schemaCalls = 0;
int g_timerCalls = 0;

void ResetCallCounters()
{
	g_clientCalls = 0;
	g_entityCalls = 0;
	g_schemaCalls = 0;
	g_timerCalls = 0;
}

void* ControllerPtr(int slot) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + slot)); }
void* PawnPtr(int slot) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000 + slot)); }

int32_t ControllerHandle(int slot) { return static_cast<int32_t>(0x40000000u | static_cast<uint32_t>(slot)); }
int32_t PawnHandleOf(int slot) { return static_cast<int32_t>(0xC0000000u | static_cast<uint32_t>(slot)); }

void AddHuman(int slot, uint64_t steam, int team, const std::string& name, bool alive = true)
{
	FakeClient& c = g_clients[slot];
	c = FakeClient{};
	c.connected = true;
	c.inGame = true;
	c.bot = false;
	c.alive = alive;
	c.team = team;
	c.steam = steam;
	c.name = name;
	c.hasPawn = true;
	c.controllerHandle = ControllerHandle(slot);
}

void AddBot(int slot, int team, const std::string& name, bool alive = true)
{
	FakeClient& c = g_clients[slot];
	c = FakeClient{};
	c.connected = true;
	c.inGame = true;
	c.bot = true;
	c.alive = alive;
	c.team = team;
	c.steam = 0;
	c.name = name;
	c.hasPawn = true;
	c.controllerHandle = ControllerHandle(slot);
}

void RemoveClient(int slot) { g_clients[slot] = FakeClient{}; }
void SetControllerSerial(int slot, int32_t handle) { g_clients[slot].controllerHandle = handle; }

plg::string FakeGetClientName(int32_t slot)
{
	if (slot < 0 || slot >= 64) return plg::string();
	return plg::string(g_clients[slot].name.c_str());
}

bool FakeIsClientConnected(int32_t slot)
{
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].connected;
}

bool FakeIsClientInGame(int32_t slot)
{
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].inGame;
}

bool FakeIsClientSourceTV(int32_t slot)
{
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].sourceTV;
}

bool FakeIsClientAlive(int32_t slot)
{
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].alive;
}

bool FakeIsFakeClient(int32_t slot)
{
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].bot;
}

s2sdk::CSTeam FakeGetClientTeam(int32_t slot)
{
	++g_clientCalls;
	if (slot < 0 || slot >= 64) return s2sdk::CSTeam::None;
	return static_cast<s2sdk::CSTeam>(g_clients[slot].team);
}

uint64_t FakeGetClientSteamID64(int32_t slot)
{
	++g_clientCalls;
	if (slot < 0 || slot >= 64) return 0;
	return g_clients[slot].steam;
}

void* FakeGetClientPawn(int32_t slot)
{
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !g_clients[slot].hasPawn) return nullptr;
	return PawnPtr(slot);
}

void* FakePlayerSlotToEntPointer(int32_t slot)
{
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !g_clients[slot].connected) return nullptr;
	return ControllerPtr(slot);
}

int32_t FakePlayerSlotToEntHandle(int32_t slot)
{
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !g_clients[slot].connected) return -1;
	return g_clients[slot].controllerHandle;
}

int32_t FakeEntPointerToEntHandle(void* ptr)
{
	++g_entityCalls;
	if (!ptr) return -1;
	const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
	if (value >= 0x20000 && value < 0x20000 + 64)
		return PawnHandleOf(static_cast<int>(value - 0x20000));
	if (value >= 0x10000 && value < 0x10000 + 64)
		return ControllerHandle(static_cast<int>(value - 0x10000));
	return -1;
}

int32_t FakeEntPointerToEntIndex(void* ptr)
{
	++g_entityCalls;
	if (!ptr) return -1;
	const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
	if (value >= 0x20000 && value < 0x20000 + 64) return static_cast<int32_t>(value - 0x20000) + 1;
	if (value >= 0x10000 && value < 0x10000 + 64) return static_cast<int32_t>(value - 0x10000) + 1;
	return -1;
}

int32_t FakeEntIndexToEntHandle(int32_t index)
{
	++g_entityCalls;
	if (index == 0) return 0; // the world
	if (index >= 1 && index <= 64) return PawnHandleOf(index - 1);
	return -1;
}

int32_t FakeEntHandleToEntIndex(int32_t handle)
{
	++g_entityCalls;
	if (handle == 0) return 0;
	const uint32_t value = static_cast<uint32_t>(handle);
	if ((value & 0xC0000000u) == 0xC0000000u) return static_cast<int32_t>(value & 0x7FFFFFFFu);
	return -1;
}

bool FakeIsValidEntHandle(int32_t handle)
{
	++g_entityCalls;
	// PITFALLS C1: 0 is the world (valid for EmitSound), -1 is INVALID_EHANDLE_INDEX; every other
	// value is a valid CEntityHandle even when it reads as a negative int.
	return handle != -1;
}

bool FakeIsValidEntPointer(void* ptr)
{
	++g_entityCalls;
	return ptr != nullptr;
}

// ==========================================================================================
// fake events
// ==========================================================================================

struct FakeEvent
{
	std::map<std::string, int32_t> slots;
	std::map<std::string, std::string> strings;
	std::map<std::string, bool> bools;
};

std::vector<std::string> g_eventSlotReads;
std::vector<std::string> g_eventIntReads;

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key)
{
	if (!event) return -1;
	g_eventSlotReads.push_back(ToStd(key));
	const FakeEvent* e = static_cast<const FakeEvent*>(event);
	const auto it = e->slots.find(ToStd(key));
	return it == e->slots.end() ? -1 : it->second;
}

std::string FakeGetEventStringImpl(void* event, const plg::string& key)
{
	if (!event) return {};
	const FakeEvent* e = static_cast<const FakeEvent*>(event);
	const auto it = e->strings.find(ToStd(key));
	return it == e->strings.end() ? std::string() : it->second;
}

plg::string FakeGetEventString(void* event, const plg::string& key)
{
	return plg::string(FakeGetEventStringImpl(event, key).c_str());
}

bool FakeGetEventBool(void* event, const plg::string& key)
{
	if (!event) return false;
	const FakeEvent* e = static_cast<const FakeEvent*>(event);
	const auto it = e->bools.find(ToStd(key));
	return it != e->bools.end() && it->second;
}

int32_t FakeGetEventInt(void* /*event*/, const plg::string& key)
{
	// A decoy: the plugin must never read a player field with GetEventInt (the raw user id is not
	// a slot). Any call is recorded and fails the corresponding assertion.
	g_eventIntReads.push_back(ToStd(key));
	return 999;
}

// ==========================================================================================
// fake sound / output / resources
// ==========================================================================================

struct EmittedSound
{
	int32_t handle = 0;
	std::string sound;
	int32_t pitch = 0;
	float volume = 0.0f;
	float delay = 0.0f;
};
std::vector<EmittedSound> g_emitted;
std::vector<std::pair<int32_t, std::string>> g_clientCommands;
std::vector<std::pair<int32_t, std::string>> g_chat;
std::vector<std::pair<int32_t, std::string>> g_center;
std::vector<std::pair<int32_t, std::string>> g_alert;
std::vector<std::tuple<int32_t, std::string, int32_t>> g_html;
std::vector<std::string> g_precached;
std::vector<std::string> g_serverPrints;

void FakeEmitSound(int32_t handle, const plg::string& sound, int32_t pitch, float volume, float delay)
{
	g_emitted.push_back(EmittedSound{handle, ToStd(sound), pitch, volume, delay});
}

void FakeClientCommand(int32_t slot, const plg::string& command)
{
	g_clientCommands.emplace_back(slot, ToStd(command));
}

void FakePrintToChat(int32_t slot, const plg::string& message)
{
	g_chat.emplace_back(slot, ToStd(message));
}

void FakePrintCenterText(int32_t slot, const plg::string& message)
{
	g_center.emplace_back(slot, ToStd(message));
}

void FakePrintAlertText(int32_t slot, const plg::string& message)
{
	g_alert.emplace_back(slot, ToStd(message));
}

void FakePrintCentreHtml(int32_t slot, const plg::string& message, int32_t duration)
{
	g_html.emplace_back(slot, ToStd(message), duration);
}

void FakePrintToServer(const plg::string& message) { g_serverPrints.push_back(ToStd(message)); }

void FakePrecache(const plg::string& resource) { g_precached.push_back(ToStd(resource)); }

s2sdk::TimerId FakeCreateTimer(double, s2sdk::TimerCallback, s2sdk::TimerFlag,
							   const plg::vector<plg::any>&)
{
	++g_timerCalls;
	return 1;
}

// ==========================================================================================
// fake map / manifest / server language
// ==========================================================================================

std::string g_mapName;
s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;
bool g_throwMapEndRegister = false;
s2sdk::OnBuildGameSessionManifestCallback g_manifestCallback = nullptr;
int g_manifestRegisters = 0;
int g_manifestUnregisters = 0;
std::string g_sdkLanguage = "english";

plg::string FakeGetCurrentMap() { return plg::string(g_mapName.c_str()); }
plg::string FakeGetServerLanguage() { return plg::string(g_sdkLanguage.c_str()); }

void FakeMapStartRegister(s2sdk::OnMapStartCallback cb)
{
	g_mapStartCallback = cb;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback)
{
	g_mapStartCallback = nullptr;
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback cb)
{
	if (g_throwMapEndRegister) throw std::runtime_error("fake OnMapEnd register failure");
	g_mapEndCallback = cb;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback)
{
	g_mapEndCallback = nullptr;
	++g_mapEndUnregisters;
}

void FakeManifestRegister(s2sdk::OnBuildGameSessionManifestCallback cb)
{
	g_manifestCallback = cb;
	++g_manifestRegisters;
}

void FakeManifestUnregister(s2sdk::OnBuildGameSessionManifestCallback)
{
	g_manifestCallback = nullptr;
	++g_manifestUnregisters;
}

// ==========================================================================================
// fake game events (HookEvent/UnhookEvent)
// ==========================================================================================

struct FakeEventHook
{
	s2sdk::EventCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};
std::map<std::string, FakeEventHook> g_eventHooks;
std::set<std::string> g_rejectedEvents;
int g_hookCalls = 0;
int g_unhookCalls = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback cb, s2sdk::HookMode mode)
{
	g_eventHooks[ToStd(name)] = FakeEventHook{cb, mode};
	++g_hookCalls;
	if (g_rejectedEvents.count(ToStd(name))) return s2sdk::EventHookError::InvalidEvent;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback cb, s2sdk::HookMode mode)
{
	const auto it = g_eventHooks.find(ToStd(name));
	if (it == g_eventHooks.end() || it->second.callback != cb || it->second.mode != mode) return s2sdk::EventHookError::InvalidEvent;
	g_eventHooks.erase(it);
	++g_unhookCalls;
	return s2sdk::EventHookError::Okay;
}

s2sdk::ResultType FireGameEvent(const std::string& name, FakeEvent* event)
{
	const auto it = g_eventHooks.find(name);
	if (it == g_eventHooks.end() || it->second.callback == nullptr)
	{
		Check(false, "event '" + name + "' is not hooked");
		return s2sdk::ResultType::Continue;
	}
	return it->second.callback(plg::string(name.c_str()), event, false);
}

FakeEvent MakeDeathEvent(int victim, int attacker, const std::string& weapon, bool headshot, int assister = -1)
{
	FakeEvent event;
	event.slots["userid"] = victim;
	event.slots["attacker"] = attacker;
	if (assister >= 0) event.slots["assister"] = assister;
	event.strings["weapon"] = weapon;
	event.bools["headshot"] = headshot;
	return event;
}

// ==========================================================================================
// fake console commands
// ==========================================================================================

struct FakeCommand
{
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
};
std::map<std::string, FakeCommand> g_fakeCommands;
std::map<std::string, bool> g_commandCreated;
std::vector<std::string> g_removedCommands;
std::vector<std::string> g_removedListeners;
int g_addConsoleCommandCalls = 0;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string&, s2sdk::ConVarFlag flags,
						   s2sdk::ConCommandCallback callback, s2sdk::HookMode mode)
{
	++g_addConsoleCommandCalls;
	g_fakeCommands[ToStd(name)] = FakeCommand{callback, mode, flags};
	const auto it = g_commandCreated.find(ToStd(name));
	return it == g_commandCreated.end() ? true : it->second;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback)
{
	const auto it = g_fakeCommands.find(ToStd(name));
	if (it == g_fakeCommands.end() || it->second.callback != callback) return false;
	g_fakeCommands.erase(it);
	g_removedCommands.push_back(ToStd(name));
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback, s2sdk::HookMode mode)
{
	const auto it = g_fakeCommands.find(ToStd(name));
	if (it == g_fakeCommands.end() || it->second.callback != callback || it->second.mode != mode) return false;
	g_fakeCommands.erase(it);
	g_removedListeners.push_back(ToStd(name));
	return true;
}

s2sdk::ResultType RunCommand(const std::string& name, int32_t caller, s2sdk::ConCommandContext context,
							 std::vector<std::string> args)
{
	const auto it = g_fakeCommands.find(name);
	if (it == g_fakeCommands.end() || it->second.callback == nullptr)
	{
		Check(false, "command '" + name + "' is not registered");
		return s2sdk::ResultType::Continue;
	}
	plg::vector<plg::string> plgArgs;
	plgArgs.resize(args.size());
	for (std::size_t i = 0; i < args.size(); ++i) plgArgs[i] = ToPlg(args[i]);
	return it->second.callback(caller, context, plgArgs);
}

// ==========================================================================================
// fake menus
// ==========================================================================================

struct FakeMenuItem
{
	std::string info;
	std::string display;
	s2sdk::MenuItemStyle style = s2sdk::MenuItemStyle::Default;
};

struct FakeMenu
{
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<FakeMenuItem> items;
	bool exit = false;
	bool exitBack = false;
	int32_t displaySlot = -1;
};

std::map<s2sdk::MenuId, FakeMenu> g_fakeMenus;
std::map<int32_t, s2sdk::MenuId> g_clientMenus;
s2sdk::MenuId g_nextMenuId = 0;
std::vector<s2sdk::MenuId> g_createdMenus;
std::vector<s2sdk::MenuId> g_destroyedMenus;
std::set<std::string> g_registeredMenuTypes = {"chat", "console", "centerhtml", "button", "hud", "wasd"};
std::map<int32_t, std::string> g_menuTypeChoices;
int g_hudmenuCalls = 0;
bool g_cancelClientMenuFails = false;
bool g_cancelClientMenuLeavesState = false;
bool g_setMenuTypeFails = false;
int g_backendCloses = 0;
bool g_afterUnload = false;
int g_handlerCallsAfterUnload = 0;

void CallFakeMenuHandler(const FakeMenu& menu, s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot,
						 int32_t param)
{
	if (menu.handler == nullptr) return;
	if (g_afterUnload) ++g_handlerCallsAfterUnload;
	menu.handler(id, action, slot, param);
}

int BoundMenuCount()
{
	int count = 0;
	for (const auto& [id, menu] : g_fakeMenus)
		if (menu.handler != nullptr) ++count;
	return count;
}

plg::string FakeGetClientMenuType(int32_t slot)
{
	++g_hudmenuCalls;
	const auto it = g_menuTypeChoices.find(slot);
	return it == g_menuTypeChoices.end() ? plg::string() : plg::string(it->second.c_str());
}

bool FakeIsMenuTypeRegistered(const plg::string& name)
{
	return g_registeredMenuTypes.count(ToStd(name)) != 0;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& type)
{
	if (g_setMenuTypeFails) return false;
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) return false;
	it->second.type = ToStd(type);
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id)
{
	const auto it = g_fakeMenus.find(id);
	return it == g_fakeMenus.end() ? plg::string() : plg::string(it->second.type.c_str());
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler, const plg::string& type)
{
	const s2sdk::MenuId id = g_nextMenuId++;
	FakeMenu menu;
	menu.title = ToStd(title);
	menu.type = ToStd(type);
	menu.handler = handler;
	g_fakeMenus[id] = std::move(menu);
	g_createdMenus.push_back(id);
	return id;
}

bool FakeDestroyMenu(s2sdk::MenuId id)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) return false;
	for (auto client = g_clientMenus.begin(); client != g_clientMenus.end();)
	{
		if (client->second == id)
		{
			if (it->second.type != "chat" && it->second.type != "console") ++g_backendCloses;
			CallFakeMenuHandler(it->second, id, s2sdk::MenuAction::Cancel, client->first,
								static_cast<int32_t>(s2sdk::MenuCancelReason::Destroyed));
			CallFakeMenuHandler(it->second, id, s2sdk::MenuAction::End, client->first, 0);
			client = g_clientMenus.erase(client);
		}
		else
		{
			++client;
		}
	}
	g_fakeMenus.erase(it);
	g_destroyedMenus.push_back(id);
	return true;
}

bool FakeIsValidMenu(s2sdk::MenuId id) { return g_fakeMenus.count(id) != 0; }

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle style)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) return -1;
	it->second.items.push_back(FakeMenuItem{ToStd(info), ToStd(display), style});
	return static_cast<int32_t>(it->second.items.size()) - 1;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) return false;
	it->second.exit = enabled;
	return true;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) return false;
	it->second.exitBack = enabled;
	return true;
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) return false;
	const auto current = g_clientMenus.find(slot);
	if (current != g_clientMenus.end())
	{
		const auto old = g_fakeMenus.find(current->second);
		if (old != g_fakeMenus.end())
		{
			CallFakeMenuHandler(old->second, current->second, s2sdk::MenuAction::Cancel, slot,
								static_cast<int32_t>(s2sdk::MenuCancelReason::Interrupted));
			CallFakeMenuHandler(old->second, current->second, s2sdk::MenuAction::End, slot, 0);
		}
		g_clientMenus.erase(current);
	}
	g_clientMenus[slot] = id;
	it->second.displaySlot = slot;
	CallFakeMenuHandler(it->second, id, s2sdk::MenuAction::Start, slot, 0);
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot)
{
	const auto it = g_clientMenus.find(slot);
	return it == g_clientMenus.end() ? 0 : it->second;
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason)
{
	if (g_cancelClientMenuFails) return false;        // simulates an s2sdk contract violation
	if (g_cancelClientMenuLeavesState) return true;    // "true" but state.id was not cleared
	const auto it = g_clientMenus.find(slot);
	if (it == g_clientMenus.end()) return false;
	const s2sdk::MenuId id = it->second;
	g_clientMenus.erase(it);
	const auto menu = g_fakeMenus.find(id);
	if (menu != g_fakeMenus.end())
	{
		CallFakeMenuHandler(menu->second, id, s2sdk::MenuAction::Cancel, slot,
							static_cast<int32_t>(reason));
		CallFakeMenuHandler(menu->second, id, s2sdk::MenuAction::End, slot, 0);
	}
	return true;
}

plg::string FakeGetMenuItemInfoText(s2sdk::MenuId id, int32_t index)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end() || index < 0 || index >= static_cast<int32_t>(it->second.items.size()))
		return plg::string();
	return plg::string(it->second.items[static_cast<std::size_t>(index)].info.c_str());
}

plg::string FakeGetMenuItemDisplay(s2sdk::MenuId id, int32_t index)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end() || index < 0 || index >= static_cast<int32_t>(it->second.items.size()))
		return plg::string();
	return plg::string(it->second.items[static_cast<std::size_t>(index)].display.c_str());
}

s2sdk::MenuItemStyle FakeGetMenuItemStyle(s2sdk::MenuId id, int32_t index)
{
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end() || index < 0 || index >= static_cast<int32_t>(it->second.items.size()))
		return s2sdk::MenuItemStyle::Disabled;
	return it->second.items[static_cast<std::size_t>(index)].style;
}

const FakeMenu* MenuOfSlot(int32_t slot)
{
	const auto it = g_clientMenus.find(slot);
	if (it == g_clientMenus.end()) return nullptr;
	const auto menu = g_fakeMenus.find(it->second);
	return menu == g_fakeMenus.end() ? nullptr : &menu->second;
}

int32_t ItemIndexByInfo(const FakeMenu& menu, const std::string& info)
{
	for (std::size_t i = 0; i < menu.items.size(); ++i)
		if (menu.items[i].info == info) return static_cast<int32_t>(i);
	return -1;
}

bool SelectMenuItem(int32_t slot, const std::string& info)
{
	const auto it = g_clientMenus.find(slot);
	if (it == g_clientMenus.end()) return false;
	const s2sdk::MenuId id = it->second;
	const auto menu = g_fakeMenus.find(id);
	if (menu == g_fakeMenus.end() || menu->second.handler == nullptr) return false;
	const int32_t index = ItemIndexByInfo(menu->second, info);
	if (index < 0) return false;
	// s2sdk's closeOnSelect: close the display, then Select + End.
	g_clientMenus.erase(it);
	CallFakeMenuHandler(menu->second, id, s2sdk::MenuAction::Select, slot, index);
	CallFakeMenuHandler(menu->second, id, s2sdk::MenuAction::End, slot, 0);
	return true;
}

bool ExitBackMenu(int32_t slot) { return FakeCancelClientMenu(slot, s2sdk::MenuCancelReason::ExitBack); }
bool ExitMenu(int32_t slot) { return FakeCancelClientMenu(slot, s2sdk::MenuCancelReason::Exit); }

// ==========================================================================================
// fake schema / game rules / bomb
// ==========================================================================================

void* const kRulesPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(0x30000));
int32_t g_warmupOffset = 100;
int32_t g_blowOffset = 200;
bool g_warmup = false;
bool g_bombEntityPresent = false;
int32_t g_bombHandle = -123456; // a valid CEntityHandle that reads as a negative int
float g_bombBlow = 0.0f;
float g_gameTime = 0.0f;
int32_t g_lastFloatHandle = 0;

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName)
{
	++g_schemaCalls;
	if (ToStd(className) == "CCSGameRules" && ToStd(memberName) == "m_bWarmupPeriod") return g_warmupOffset;
	if (ToStd(className) == "CPlantedC4" && ToStd(memberName) == "m_flC4Blow") return g_blowOffset;
	return -1;
}

int64_t FakeGetEntData2(void* entity, int32_t offset, int32_t size)
{
	++g_schemaCalls;
	if (entity == kRulesPtr && offset == g_warmupOffset && size == 1) return g_warmup ? 1 : 0;
	return 0;
}

double FakeGetEntDataFloat(int32_t handle, int32_t offset, int32_t size)
{
	++g_schemaCalls;
	g_lastFloatHandle = handle;
	if (handle == g_bombHandle && offset == g_blowOffset && size == 4) return g_bombBlow;
	return 0.0;
}

void* FakeGetGameRules()
{
	++g_schemaCalls;
	return kRulesPtr;
}

int32_t FakeFindEntityByClassname(int32_t, const plg::string& classname)
{
	++g_entityCalls;
	if (ToStd(classname) == "planted_c4" && g_bombEntityPresent) return g_bombHandle;
	return -1;
}

float FakeGetGameTime()
{
	++g_schemaCalls;
	return g_gameTime;
}

// ==========================================================================================
// fake user messages (the per-player soundevent volume)
// ==========================================================================================

struct FakeUserMessage
{
	int16_t id = 0;
	std::string name;
	bool serializable = true;
	bool hasGuid = false;
	int32_t guid = -1;
	uint32_t hash = 0;
	uint64_t mask = 0;
	std::map<std::string, int32_t> ints;
	std::map<std::string, std::string> strings;
};

struct SentUserMessage
{
	int16_t id = 0;
	int32_t guid = -1;
	uint64_t mask = 0;
	std::string packed;
};

std::map<int16_t, s2sdk::UserMessageCallback> g_userMessageHooks;
s2sdk::UserMessageCallback g_userMessageCallback = nullptr;
int16_t g_userMessageHookId = -1;
int g_userMessageUnhooks = 0;
int g_userMessageHooksCalls = 0;
int g_userMessageCreateCalls = 0;
int g_userMessageNameCalls = 0;
int g_pbReadCalls = 0;
int g_pbSetCalls = 0;
int g_userMessageSendCalls = 0;
std::vector<SentUserMessage> g_userMessageSends;
bool g_messageLookupEnabled = true;
int16_t g_sosStartId = 208;
int16_t g_sosSetId = 210;
int16_t g_sosStopId = 209;
bool g_createFromIdNullSerializable = false;

int16_t FakeUserMessageFindMessageIdByName(const plg::string& name)
{
	// The real s2sdk registry does a partial-name lookup and matches CMsgSos... for a "Sos..."
	// query; the old GE_ spellings are not in it. This fake accepts exactly that shape.
	const std::string n = ToStd(name);
	if (!g_messageLookupEnabled) return 0;
	if (n.rfind("GE_", 0) == 0) return 0;
	if (n.find("SosStartSoundEvent") != std::string::npos) return g_sosStartId;
	if (n.find("SosSetSoundEventParams") != std::string::npos) return g_sosSetId;
	if (n.find("SosStopSoundEvent") != std::string::npos) return g_sosStopId;
	return 0;
}

void* FakeUserMessageCreateFromId(int16_t id)
{
	++g_userMessageCreateCalls;
	auto* message = new FakeUserMessage();
	message->id = id;
	message->serializable = !g_createFromIdNullSerializable;
	if (id == 208) message->name = "CMsgSosStartSoundEvent";
	else if (id == 209) message->name = "CMsgSosStopSoundEvent";
	else if (id == 210) message->name = "CMsgSosSetSoundEventParams";
	else message->name = "CMsgUnknown";
	return message;
}

void FakeUserMessageDestroy(void* message) { delete static_cast<FakeUserMessage*>(message); }

void FakeUserMessageSend(void* message)
{
	++g_userMessageSendCalls;
	auto* m = static_cast<FakeUserMessage*>(message);
	if (!m) return;
	SentUserMessage send;
	send.id = m->id;
	const auto guid = m->ints.find("soundevent_guid");
	if (guid != m->ints.end()) send.guid = guid->second;
	send.mask = m->mask;
	const auto packed = m->strings.find("packed_params");
	if (packed != m->strings.end()) send.packed = packed->second;
	g_userMessageSends.push_back(send);
}

int16_t FakeUserMessageGetMessageID(void* message)
{
	return message ? static_cast<FakeUserMessage*>(message)->id : 0;
}

plg::string FakeUserMessageGetMessageName(void* message)
{
	++g_userMessageNameCalls;
	if (!message) return plg::string();
	auto* m = static_cast<FakeUserMessage*>(message);
	return m->serializable ? plg::string(m->name.c_str()) : plg::string();
}

void* FakeUserMessageGetSerializableMessage(void* message)
{
	if (!message) return nullptr;
	auto* m = static_cast<FakeUserMessage*>(message);
	return m->serializable ? message : nullptr;
}

bool FakeUserMessageHasField(void* message, const plg::string& field)
{
	auto* m = static_cast<FakeUserMessage*>(message);
	if (!m) return false;
	return ToStd(field) == "soundevent_guid" && m->hasGuid;
}

uint64_t FakeUserMessageGetRecipientMask(void* message)
{
	return message ? static_cast<FakeUserMessage*>(message)->mask : 0;
}

void FakeUserMessageSetRecipientMask(void* message, uint64_t mask)
{
	if (message) static_cast<FakeUserMessage*>(message)->mask = mask;
}

int32_t FakePbReadInt32(void* message, const plg::string& field, int32_t)
{
	++g_pbReadCalls;
	auto* m = static_cast<FakeUserMessage*>(message);
	if (!m) return 0;
	if (ToStd(field) == "soundevent_guid") return m->guid;
	const auto it = m->ints.find(ToStd(field));
	return it == m->ints.end() ? 0 : it->second;
}

uint32_t FakePbReadUInt32(void* message, const plg::string& field, int32_t)
{
	++g_pbReadCalls;
	auto* m = static_cast<FakeUserMessage*>(message);
	if (!m) return 0;
	if (ToStd(field) == "soundevent_hash") return m->hash;
	return 0;
}

bool FakePbSetInt32(void* message, const plg::string& field, int32_t value)
{
	++g_pbSetCalls;
	if (message) static_cast<FakeUserMessage*>(message)->ints[ToStd(field)] = value;
	return true;
}

bool FakePbSetString(void* message, const plg::string& field, const plg::string& value)
{
	++g_pbSetCalls;
	if (message) static_cast<FakeUserMessage*>(message)->strings[ToStd(field)] = ToStd(value);
	return true;
}

bool FakeHookUserMessage(int16_t id, s2sdk::UserMessageCallback callback, s2sdk::HookMode mode)
{
	if (g_afterUnload) ++g_handlerCallsAfterUnload;
	++g_userMessageHooksCalls;
	if (mode != s2sdk::HookMode::Post) return false;
	g_userMessageHooks[id] = callback;
	g_userMessageCallback = callback;
	g_userMessageHookId = id;
	return true;
}

bool FakeUnhookUserMessage(int16_t id, s2sdk::UserMessageCallback callback, s2sdk::HookMode)
{
	const auto it = g_userMessageHooks.find(id);
	if (it == g_userMessageHooks.end() || it->second != callback) return false;
	g_userMessageHooks.erase(it);
	g_userMessageCallback = nullptr;
	g_userMessageHookId = -1;
	++g_userMessageUnhooks;
	return true;
}

// ==========================================================================================
// fake permissions (cookies)
// ==========================================================================================

std::map<std::pair<uint64_t, std::string>, plg::any> g_cookieStore;
std::vector<std::tuple<uint64_t, std::string, std::string>> g_cookieWrites;
bool g_loadUserFails = false;
int g_loadUserCalls = 0;
std::vector<std::pair<uint64_t, std::string>> g_loadUserLog;

permissions::Status FakeLoadUser(int64_t, uint64_t steam, const plg::string& name, bool, bool)
{
	++g_loadUserCalls;
	g_loadUserLog.emplace_back(steam, ToStd(name));
	if (g_loadUserFails) return permissions::Status::StorageError;
	return permissions::Status::Success;
}

permissions::Status FakeGetCookie(uint64_t steam, const plg::string& name, plg::any& out)
{
	const auto it = g_cookieStore.find({steam, ToStd(name)});
	if (it == g_cookieStore.end()) return permissions::Status::CookieNotFound;
	out = it->second;
	return permissions::Status::Success;
}

permissions::Status FakeSetCookie(int64_t, uint64_t steam, const plg::string& name, const plg::any& value, bool)
{
	const std::string key = ToStd(name);
	g_cookieStore[{steam, key}] = value;
	if (plg::holds_alternative<plg::string>(value))
		g_cookieWrites.emplace_back(steam, key, ToStd(plg::get<plg::string>(value)));
	else
		g_cookieWrites.emplace_back(steam, key, std::string());
	return permissions::Status::Success;
}

void SetCookieValue(uint64_t steam, const std::string& name, const std::string& value)
{
	g_cookieStore[{steam, name}] = plg::any(plg::string(value.c_str()));
}

bool CookieWasWritten(uint64_t steam, const std::string& name, const std::string& value)
{
	for (const auto& [s, n, v] : g_cookieWrites)
		if (s == steam && n == name && v == value) return true;
	return false;
}

// ==========================================================================================
// fake log / plugin id
// ==========================================================================================

struct LogEntry
{
	std::string text;
	plg::Severity severity = plg::Severity::Info;
};
std::vector<LogEntry> g_logs;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&)
{
	g_logs.push_back(LogEntry{std::string(message), severity});
}

bool LoggedWith(plg::Severity severity, const std::string& text)
{
	for (const LogEntry& entry : g_logs)
		if (entry.severity == severity && entry.text.find(text) != std::string::npos) return true;
	return false;
}

plg::uuid FakeGetPluginId(plg::hndl) { return 77; }

plg::string FakeGetConfigsDir() { return plg::string(g_configsDir.c_str()); }
plg::string FakeGetDataDir() { return plg::string(g_dataDir.c_str()); }

// ==========================================================================================
// installation / reset
// ==========================================================================================

bool g_pluginStarted = false;

void InstallFakes()
{
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::plugin::GetId = &FakeGetPluginId;
	plg::plugin::handle = reinterpret_cast<plg::hndl>(static_cast<uintptr_t>(1));

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientTeam = &FakeGetClientTeam;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_EntIndexToEntHandle = &FakeEntIndexToEntHandle;
	__s2sdk_EntHandleToEntIndex = &FakeEntHandleToEntIndex;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;

	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;
	__s2sdk_OnBuildGameSessionManifest_Register = &FakeManifestRegister;
	__s2sdk_OnBuildGameSessionManifest_Unregister = &FakeManifestUnregister;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventString = &FakeGetEventString;
	__s2sdk_GetEventBool = &FakeGetEventBool;
	__s2sdk_GetEventInt = &FakeGetEventInt;

	__s2sdk_EmitSound = &FakeEmitSound;
	__s2sdk_ClientCommand = &FakeClientCommand;
	__s2sdk_Precache = &FakePrecache;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintCenterText = &FakePrintCenterText;
	__s2sdk_PrintAlertText = &FakePrintAlertText;
	__s2sdk_PrintCentreHtml = &FakePrintCentreHtml;
	__s2sdk_PrintToServer = &FakePrintToServer;
	__s2sdk_GetGameTime = &FakeGetGameTime;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_GetMenuItemInfoText = &FakeGetMenuItemInfoText;
	__s2sdk_GetMenuItemDisplay = &FakeGetMenuItemDisplay;
	__s2sdk_GetMenuItemStyle = &FakeGetMenuItemStyle;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;

	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetEntData2 = &FakeGetEntData2;
	__s2sdk_GetEntDataFloat = &FakeGetEntDataFloat;
	__s2sdk_GetGameRules = &FakeGetGameRules;
	__s2sdk_FindEntityByClassname = &FakeFindEntityByClassname;

	__s2sdk_CreateTimer = &FakeCreateTimer;

	__s2sdk_UserMessageFindMessageIdByName = &FakeUserMessageFindMessageIdByName;
	__s2sdk_UserMessageCreateFromId = &FakeUserMessageCreateFromId;
	__s2sdk_UserMessageSend = &FakeUserMessageSend;
	__s2sdk_UserMessageDestroy = &FakeUserMessageDestroy;
	__s2sdk_UserMessageGetMessageID = &FakeUserMessageGetMessageID;
	__s2sdk_UserMessageGetMessageName = &FakeUserMessageGetMessageName;
	__s2sdk_UserMessageGetSerializableMessage = &FakeUserMessageGetSerializableMessage;
	__s2sdk_UserMessageHasField = &FakeUserMessageHasField;
	__s2sdk_UserMessageGetRecipientMask = &FakeUserMessageGetRecipientMask;
	__s2sdk_UserMessageSetRecipientMask = &FakeUserMessageSetRecipientMask;
	__s2sdk_PbReadInt32 = &FakePbReadInt32;
	__s2sdk_PbReadUInt32 = &FakePbReadUInt32;
	__s2sdk_PbSetInt32 = &FakePbSetInt32;
	__s2sdk_PbSetString = &FakePbSetString;
	__s2sdk_HookUserMessage = &FakeHookUserMessage;
	__s2sdk_UnhookUserMessage = &FakeUnhookUserMessage;

	__permissions_LoadUser = &FakeLoadUser;
	__permissions_GetCookie = &FakeGetCookie;
	__permissions_SetCookie = &FakeSetCookie;
}

void ClearFakeState()
{
	for (FakeClient& client : g_clients) client = FakeClient{};
	g_emitted.clear();
	g_clientCommands.clear();
	g_chat.clear();
	g_center.clear();
	g_alert.clear();
	g_html.clear();
	g_precached.clear();
	g_serverPrints.clear();
	g_eventSlotReads.clear();
	g_eventIntReads.clear();
	g_eventHooks.clear();
	g_rejectedEvents.clear();
	g_hookCalls = 0;
	g_unhookCalls = 0;
	g_mapName.clear();
	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;
	g_throwMapEndRegister = false;
	g_manifestCallback = nullptr;
	g_manifestRegisters = 0;
	g_manifestUnregisters = 0;
	g_sdkLanguage = "english";
	g_fakeCommands.clear();
	g_commandCreated.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();
	g_addConsoleCommandCalls = 0;
	g_fakeMenus.clear();
	g_clientMenus.clear();
	g_nextMenuId = 0;
	g_createdMenus.clear();
	g_destroyedMenus.clear();
	g_registeredMenuTypes = {"chat", "console", "centerhtml", "button", "hud", "wasd"};
	g_menuTypeChoices.clear();
	g_hudmenuCalls = 0;
	g_cancelClientMenuFails = false;
	g_cancelClientMenuLeavesState = false;
	g_setMenuTypeFails = false;
	g_backendCloses = 0;
	g_afterUnload = false;
	g_handlerCallsAfterUnload = 0;
	g_userMessageHooks.clear();
	g_userMessageCallback = nullptr;
	g_userMessageHookId = -1;
	g_userMessageUnhooks = 0;
	g_userMessageHooksCalls = 0;
	g_userMessageCreateCalls = 0;
	g_userMessageNameCalls = 0;
	g_pbReadCalls = 0;
	g_pbSetCalls = 0;
	g_userMessageSendCalls = 0;
	g_userMessageSends.clear();
	g_messageLookupEnabled = true;
	g_sosStartId = 208;
	g_sosSetId = 210;
	g_sosStopId = 209;
	g_createFromIdNullSerializable = false;
	g_cookieStore.clear();
	g_cookieWrites.clear();
	g_loadUserFails = false;
	g_loadUserCalls = 0;
	g_loadUserLog.clear();
	g_logs.clear();
	g_warmup = false;
	g_bombEntityPresent = false;
	g_bombBlow = 0.0f;
	g_gameTime = 0.0f;
	g_lastFloatHandle = 0;
	g_warmupOffset = 100;
	g_blowOffset = 200;
	ResetCallCounters();
}

void ResetWorld()
{
	ClearFakeState();
	InstallFakes();
	std::filesystem::current_path(g_cwdDir);
	RestoreConfigFiles();
	g_pluginStarted = false;
}

plg::PluginResult StartPlugin()
{
	g_afterUnload = false;
	const plg::PluginResult result = g_quakeSoundsPlugin.OnPluginStart();
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart is Ok");
	g_pluginStarted = true;
	return result;
}

plg::PluginResult StopPlugin()
{
	const plg::PluginResult result = g_quakeSoundsPlugin.OnPluginEnd();
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;
	g_afterUnload = true;
	return result;
}

void Update(int64_t milliseconds = 100)
{
	const plg::PluginResult result =
		g_quakeSoundsPlugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginUpdate is Ok");
}

void RunUpdateForCookies() { Update(1100); }

void FireMapStart()
{
	if (g_mapStartCallback == nullptr)
	{
		Check(false, "the OnMapStart listener is not registered");
		return;
	}
	g_mapStartCallback();
}

void FireMapEnd()
{
	if (g_mapEndCallback == nullptr)
	{
		Check(false, "the OnMapEnd listener is not registered");
		return;
	}
	g_mapEndCallback();
}

void OpenMap()
{
	g_mapName = "de_dust2";
	FireMapStart();
}

void FireManifest()
{
	if (g_manifestCallback == nullptr)
	{
		Check(false, "the manifest callback is not registered");
		return;
	}
	g_manifestCallback();
}

const EmittedSound* LastSound()
{
	return g_emitted.empty() ? nullptr : &g_emitted.back();
}

std::string LastSoundName()
{
	return g_emitted.empty() ? std::string() : g_emitted.back().sound;
}

int SoundsNamed(const std::string& name)
{
	int count = 0;
	for (const EmittedSound& sound : g_emitted)
		if (sound.sound == name) ++count;
	return count;
}

std::string ConfigWithChat(bool center = false)
{
	std::string text = ShippedConfig();
	text = ReplaceKeyValue(text, "enable_chat_message", "1");
	if (center) text = ReplaceKeyValue(text, "enable_center_message", "1");
	return text;
}

std::string ConfigFileOnlySounds()
{
	std::string text = ShippedConfig();
	// every soundevent name becomes a client-side sounds/... path; the volume menu has no record
	// left, so !qs takes the direct toggle branch of the original
	text = ReplaceAll(text, "Alko.", "sounds/quake7/");
	return text;
}

// ==========================================================================================
// tests
// ==========================================================================================

void TestStartupAndParity()
{
	Section("startup: registrations, config/phrases, precache, no client call before the map");
	ResetWorld();
	StartPlugin();

	Check(g_Cfg.iDefaultVolume == 30, "default_volume 0.3 loaded");
	Check(g_mapEntries.size() == 28, "28 sound records loaded");
	Check(g_Cfg.vPrecache.size() == 23, "23 precache resources loaded");
	CheckEqStr(g_sServerLang, "ru", "core.cfg ServerLang russian -> ru");
	CheckEqStr(Phrase("Menu_Title"), "Quake-звуки", "Russian phrases loaded");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1, "map listeners registered once");
	Check(g_hookCalls == 8, "eight game events hooked");
	bool allHooksPre = true;
	for (const auto& [name, hook] : g_eventHooks)
		if (hook.mode != s2sdk::HookMode::Pre) allHooksPre = false;
	Check(allHooksPre, "every game event is a Pre hook that answers Continue");
	Check(g_fakeCommands.size() == 4, "mm_qs, mm_quakesounds, qs and lang registered");
	Check(g_fakeCommands.count("qs") == 1 && g_fakeCommands.count("lang") == 1, "trigger-less chat names");
	Check(g_manifestRegisters == 1, "manifest callback registered");
	CheckEqInt(g_userMessageHookId, -1, "no sound-event hook before the first live map");
	CheckEqInt(g_userMessageHooksCalls, 0, "no HookUserMessage call before the first live map");
	CheckEqInt(g_userMessageCreateCalls, 0, "no UserMessageCreateFromId call before the first live map");
	CheckEqInt(g_userMessageNameCalls, 0, "no GetMessageName call before the first live map");
	Check(g_timerCalls == 0, "the plugin creates no s2sdk timer");
	Check(g_mapReady == false, "the gate is shut at start");
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "no client/entity/schema call in OnPluginStart (PITFALLS B1)");
	Check(g_precached.empty(), "no precache before the manifest callback");

	// no map yet: OnPluginUpdate may not touch a client either
	ResetCallCounters();
	Update();
	Check(g_mapReady == false, "GetCurrentMap() == \"\" keeps the gate shut");
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "no client/entity/schema call before the first map");

	// the manifest callback precaches exactly the production resource list
	FireManifest();
	CheckEqInt((int64_t)g_precached.size(), 23, "manifest precaches 23 resources");
	if (g_precached.size() == 23)
	{
		CheckEqStr(g_precached.front(), "soundevents/soundevents_alko.vsndevts",
				   "first precache resource");
		CheckEqStr(g_precached.back(), "sounds/timrc4/1sec.vsnd", "last precache resource");
		Check(std::equal(g_precached.begin(), g_precached.end(), g_Cfg.vPrecache.begin()),
			  "precache order matches the config");
	}
	Check(g_clientCalls == 0 && g_entityCalls == 0, "precache makes no client/entity call");

	// the SOS ids are resolved lazily on the first live map (no hardcoded fallback) and only then
	// is the start message hooked
	OpenMap();
	CheckEqInt(g_userMessageHookId, 208, "the resolved SosStartSoundEvent id is hooked on the live map");
	CheckEqInt(g_userMessageHooksCalls, 1, "exactly one HookUserMessage call on the first map");
	CheckEqInt(g_userMessageCreateCalls, 0, "id resolution does not create a user message");
	CheckEqInt(g_userMessageNameCalls, 0, "id resolution does not read a created message name");

	StopPlugin();
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1, "map listeners unregistered");
	Check(g_manifestUnregisters == 1, "manifest callback unregistered");
	Check(g_unhookCalls == 8, "eight events unhooked");
	Check(g_fakeCommands.empty(), "all commands removed");
	Check(g_userMessageUnhooks == 1, "user-message hook removed");
}

void TestMapGate()
{
	Section("map gate B1/B2: mid-map load, map end and the boot changelevel window");
	ResetWorld();
	StartPlugin();
	Update();
	Check(!g_mapReady, "no map yet -> gate shut");

	g_mapName = "de_dust2";
	Update();
	Check(g_mapReady, "a plugin loaded into a running map latches readiness on its first update");

	ResetCallCounters();
	FireMapEnd();
	Check(!g_mapReady, "OnMapEnd closes the gate");
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "OnMapEnd only clears state (PITFALLS B2)");
	Update();
	Check(!g_mapReady, "updates between maps do not re-open the gate");
	Check(g_clientCalls == 0, "no client query between maps");

	// a player_death arriving in the changelevel window is ignored
	AddHuman(1, 111, 2, "Alice");
	ResetCallCounters();
	FakeEvent death = MakeDeathEvent(2, 1, "ak47", false);
	Check(FireGameEvent("player_death", &death) == s2sdk::ResultType::Continue, "death answers Continue");
	Check(g_clientCalls == 0 && g_mapKills.empty(), "no kill is processed while the gate is shut");

	ResetCallCounters();
	FireMapStart();
	Check(g_mapReady, "OnMapStart opens the gate again");
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "OnMapStart only clears state (no client/entity/schema call)");
	StopPlugin();

	// a fresh plugin instance loaded into a running map with an OnMapEnd already seen before its
	// first update must stay shut (the boot-time changelevel of PITFALLS B3)
	ResetWorld();
	g_mapName = "de_dust2";
	StartPlugin();
	FireMapEnd(); // the host still signals the old map end
	Update();
	Check(!g_mapReady, "an OnMapEnd before the first update keeps the gate shut");
	StopPlugin();
}

void TestKillsAndStreaks()
{
	Section("kills: first blood, headshot_once, weapon/streak priority, signed pawn handles");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	AddHuman(3, 333, 3, "Carol");

	ResetCallCounters();
	g_eventSlotReads.clear();
	g_eventIntReads.clear();
	FakeEvent first = MakeDeathEvent(2, 1, "ak47", true, 3);
	Check(FireGameEvent("player_death", &first) == s2sdk::ResultType::Continue, "death answers Continue");
	CheckEqInt((int64_t)g_mapKills[1], 1, "first kill counted");
	CheckEqStr(LastSoundName(), "Alko.Quake7.Firstblood", "first blood beats headshot/weapon/streak");
	Check(LastSound() != nullptr && LastSound()->sound == "Alko.Quake7.Firstblood",
		  "the last emitted record is the first blood sound");
	Check(g_emitted.size() == 1, "scope attacker -> only Alice hears the sound");
	Check(g_emitted.back().handle == PawnHandleOf(1), "sound emitted on the killer's signed pawn handle");
	Check(g_emitted.back().volume > 0.299f && g_emitted.back().volume < 0.301f,
		  "30% default volume applied");
	Check(std::find(g_eventSlotReads.begin(), g_eventSlotReads.end(), "userid") != g_eventSlotReads.end(),
		  "userid read with GetEventPlayerSlot");
	Check(std::find(g_eventSlotReads.begin(), g_eventSlotReads.end(), "attacker") != g_eventSlotReads.end(),
		  "attacker read with GetEventPlayerSlot");
	Check(g_eventIntReads.empty(), "no player field read with GetEventInt");
	Check(EventPlayerSlot(&first, "assister") == 3,
		  "assister would be read through the same GetEventPlayerSlot helper (not used by the kill logic)");
	Check(g_chat.empty() && g_center.empty() && g_alert.empty(), "messages are off in the production config");

	// second kill: headshot_once suppresses headshot after the first kill, the streak "2" plays
	AddHuman(2, 222, 3, "Bob", /*alive=*/true);
	g_emitted.clear();
	FakeEvent second = MakeDeathEvent(3, 1, "ak47", true);
	FireGameEvent("player_death", &second);
	CheckEqInt((int64_t)g_mapKills[1], 2, "second kill counted");
	CheckEqStr(LastSoundName(), "Alko.Quake7.Doublekill", "streak record after headshot_once");
	Check(g_emitted.size() == 1, "one emission for the attacker scope");

	// third kill: the "3" record has _filter all, so every listener hears it once
	AddHuman(3, 333, 3, "Carol", true);
	g_emitted.clear();
	FakeEvent third = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &third);
	CheckEqInt((int64_t)g_mapKills[1], 3, "third kill counted");
	CheckEqInt(SoundsNamed("Alko.Quake7.Triplekill"), 3, "Triplekill goes to all three listeners");
	Check(g_chat.empty(), "chat messages still off");

	// a weapons record wins over the streak when the priority puts it first
	g_emitted.clear();
	g_mapKills[1] = 1;
	AddHuman(2, 222, 3, "Bob", true);
	FakeEvent grenade = MakeDeathEvent(2, 1, "hegrenade", false);
	FireGameEvent("player_death", &grenade);
	CheckEqStr(LastSoundName(), "Alko.Quake7.Perfect", "weapon_hegrenade record played");

	// round_start resets the streaks
	FakeEvent roundStart;
	FireGameEvent("round_start", &roundStart);
	Check(g_mapKills.empty(), "round_start clears the streaks (reset_kills_on_round_start 1)");
	StopPlugin();
}

void TestSpecialsAndWorld()
{
	Section("specials: selfkill on worldent 0, teamkill, knife, world damage, round resets");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	AddHuman(3, 333, 2, "Carol");
	AddHuman(4, 444, 3, "Dave");

	// selfkill: the record plays from the world entity (handle 0) to the suicide himself
	g_emitted.clear();
	FakeEvent self = MakeDeathEvent(2, 2, "world", false);
	FireGameEvent("player_death", &self);
	CheckEqStr(LastSoundName(), "Alko.Quake7.Perfect", "selfkill record");
	CheckEqInt(g_emitted.size(), 1, "selfkill -> one recipient (scope attacker)");
	Check(g_emitted.back().handle == 0, "selfkill is emitted from the world handle 0");
	Check(g_mapKills.count(2) == 0, "count_self_kills 0 does not create a streak entry");

	// teamkill: attacker and victim on the same team, the record plays
	g_emitted.clear();
	FakeEvent team = MakeDeathEvent(3, 1, "ak47", false);
	FireGameEvent("player_death", &team);
	CheckEqStr(LastSoundName(), "Alko.Quake7.Teamkiller", "teamkill record");
	Check(g_mapKills.count(1) == 0, "count_team_kills 0 does not count the team kill");

	// knife kill (a prior counted streak so first blood does not preempt the knife record)
	g_emitted.clear();
	g_mapKills[1] = 2;
	FakeEvent knife = MakeDeathEvent(4, 1, "knife_karambit", false);
	FireGameEvent("player_death", &knife);
	CheckEqStr(LastSoundName(), "Alko.Quake7.Humiliation", "knife record");
	CheckEqInt((int64_t)g_mapKills[1], 3, "the knife kill was counted (different teams)");

	// world damage: attacker -1 -> ignored, no state change and no sound
	g_emitted.clear();
	FakeEvent world = MakeDeathEvent(2, -1, "world", false);
	FireGameEvent("player_death", &world);
	Check(g_emitted.empty(), "a world death plays nothing");

	// reset_kills_on_death drops the victim's streak
	g_mapKills[2] = 5;
	FakeEvent death = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &death);
	CheckEqInt((int64_t)g_mapKills[2], 0, "the victim's streak is reset on death");

	// reset_kills_on_round_start can be disabled
	std::string config = ShippedConfig();
	config = ReplaceKeyValue(config, "reset_kills_on_round_start", "0");
	SetConfigText(config);
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console, {"mm_quakesounds", "reload"});
	Check(g_Cfg.bResetOnRoundStart == false, "the reloaded config keeps reset_kills_on_round_start 0");
	g_mapKills[1] = 4;
	FakeEvent roundStart;
	FireGameEvent("round_start", &roundStart);
	CheckEqInt((int64_t)g_mapKills[1], 4, "round_start does not clear the streak when disabled");
	StopPlugin();
}

void TestBotsAndSlotReuse()
{
	Section("bots: their kills count but they never listen; ignore_bots skips both sides");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddBot(4, 2, "BOT Alice");
	AddHuman(1, 111, 3, "Bob");

	// ignore_bots 0: a bot kill is counted, first blood is "played" but no listener receives it
	FakeEvent botKill = MakeDeathEvent(1, 4, "ak47", false);
	FireGameEvent("player_death", &botKill);
	CheckEqInt((int64_t)g_mapKills[4], 1, "a bot kill is counted with ignore_bots 0");
	Check(g_emitted.empty(), "a bot is never a listener, so nothing was emitted");
	Check(g_chat.empty(), "a bot is never sent a chat line");

	// a bot victim is counted for the human attacker
	AddHuman(1, 111, 3, "Bob", true);
	g_emitted.clear();
	FakeEvent humanKillsBot = MakeDeathEvent(4, 1, "ak47", true);
	FireGameEvent("player_death", &humanKillsBot);
	CheckEqInt((int64_t)g_mapKills[1], 1, "the human kill of a bot is counted");
	CheckEqStr(LastSoundName(), "Alko.Quake7.Headshot", "the human attacker hears his own event");

	// ignore_bots 1: neither side is processed
	std::string config = ShippedConfig();
	config = ReplaceKeyValue(config, "ignore_bots", "1");
	SetConfigText(config);
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console, {"mm_quakesounds", "reload"});
	Check(g_Cfg.bIgnoreBots, "the reloaded config enables ignore_bots");
	g_mapKills.clear();
	g_emitted.clear();
	FakeEvent botKill2 = MakeDeathEvent(1, 4, "ak47", false);
	FireGameEvent("player_death", &botKill2);
	Check(g_mapKills.empty() && g_emitted.empty(), "ignore_bots skips a bot attacker");
	FakeEvent botVictim = MakeDeathEvent(4, 1, "ak47", false);
	FireGameEvent("player_death", &botVictim);
	Check(g_mapKills.empty() && g_emitted.empty(), "ignore_bots skips a bot victim");
	StopPlugin();

	Section("slot reuse: a new player never inherits the previous streak/state");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	AddHuman(3, 333, 3, "Carol");

	// Alice builds a streak of two
	FakeEvent a1 = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &a1);
	AddHuman(2, 222, 3, "Bob", true);
	FakeEvent a2 = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &a2);
	CheckEqInt((int64_t)g_mapKills[1], 2, "Alice has a streak of two");

	// the slot is reused by Daisy without a player_disconnect event: the identity check resets it
	AddHuman(1, 444, 2, "Daisy");
	g_emitted.clear();
	AddHuman(2, 222, 3, "Bob", true);
	FakeEvent d1 = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &d1);
	CheckEqInt((int64_t)g_mapKills[1], 1, "the reused slot starts at one, not three");
	CheckEqStr(LastSoundName(), "Alko.Quake7.Firstblood", "the new player gets first blood again");

	// and the cookie identity is reset as well (the old mute/volume do not leak)
	g_bMuted[1] = true;
	g_iVolume[1] = 10;
	AddHuman(1, 555, 2, "Eve");
	RunUpdateForCookies();
	Check(!g_bMuted[1], "the reused slot is not muted");
	CheckEqInt(g_iVolume[1], g_Cfg.iDefaultVolume, "the reused slot gets the default volume");

	// a player_disconnect event clears the streak immediately
	FireGameEvent("player_death", &a1); // Alice's slot is Eve now; harmless
	g_mapKills[1] = 7;
	FakeEvent disconnect;
	disconnect.slots["userid"] = 1;
	FireGameEvent("player_disconnect", &disconnect);
	RemoveClient(2);
	Check(g_mapKills.count(1) == 0, "player_disconnect clears the slot's streak");
	Check(!g_bMuted[1], "player_disconnect resets the mute switch");
	StopPlugin();
}


// ==========================================================================================
// more tests
// ==========================================================================================

void TestCookiesAndAnnounce()
{
	Section("cookies: LoadUser + volume/language, slot reuse, failures");
	ResetWorld();
	SetConfigText(ConfigWithChat(false));
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	AddHuman(3, 333, 1, "Spec");
	SetCookieValue(111, "QuakeSounds.Volume", "40");
	SetCookieValue(111, "QuakeSounds.Lang", "en");
	RunUpdateForCookies();
	Check(g_loadUserCalls >= 1, "the reconcile called permissions::LoadUser");
	CheckEqInt(g_iVolume[1], 40, "the volume cookie was applied");
	CheckEqStr(g_sLang[1], "en", "the language cookie was applied");
	Check(!g_bMuted[1], "the muted cookie is absent -> not muted");

	g_emitted.clear();
	g_chat.clear();
	FakeEvent death = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &death);
	CheckEqInt((int64_t)g_emitted.size(), 1, "scope attacker -> one listener");
	Check(g_emitted.back().volume > 0.399f && g_emitted.back().volume < 0.401f,
		  "40% of the listener volume applied");
	CheckEqInt((int64_t)g_chat.size(), 1, "the English chat message was sent once");
	if (!g_chat.empty())
	{
		Check(g_chat[0].second.find("Достижение:") != std::string::npos,
			  "the server-language Chat_Msg_Player template is used (the original only localises the record message)");
		Check(g_chat[0].second.find("First Blood") != std::string::npos,
			  "the English record message used");
		Check(g_chat[0].second.rfind("[QuakeSounds] \x01", 0) == 0,
			  "the chat text is colourised (cs2-menus {PREFIX}/{DEFAULT} bytes)");
	}

	// scope all: the observer gets the "other player" phrase with the killer's name
	SetConfigText(ReplaceKeyValue(ConfigWithChat(false), "sound_hearable_by", "all"));
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console, {"mm_quakesounds", "reload"});
	g_mapKills.clear();
	g_emitted.clear();
	g_chat.clear();
	AddHuman(2, 222, 3, "Bob", true);
	FireGameEvent("player_death", &death);
	CheckEqInt((int64_t)g_emitted.size(), 3, "sound_hearable_by all -> every non-bot listener");
	CheckEqInt((int64_t)g_chat.size(), 3, "every listener gets a chat message");
	bool observerSawAlice = false;
	for (const auto& [slot, text] : g_chat)
		if (slot == 3 && text.find("Alice") != std::string::npos &&
			text.find("Первая кровь") != std::string::npos)
			observerSawAlice = true;
	Check(observerSawAlice, "the observer got Chat_Msg_Other with the killer's name");

	// a muted player hears nothing (attacker scope)
	AddHuman(1, 112, 2, "Alice2");
	SetCookieValue(112, "QuakeSounds.Muted", "1");
	RunUpdateForCookies();
	Check(g_bMuted[1], "the muted cookie was applied");
	SetConfigText(ConfigWithChat(false)); // back to sound_hearable_by attacker
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console, {"mm_quakesounds", "reload"});
	g_mapKills.clear();
	g_emitted.clear();
	g_chat.clear();
	AddHuman(2, 222, 3, "Bob", true);
	FireGameEvent("player_death", &death);
	Check(g_emitted.empty() && g_chat.empty(), "a muted attacker gets neither sound nor message");

	// slot reuse: no cookies for the new player -> the defaults, not the previous occupant's
	AddHuman(1, 999, 2, "Zoe");
	RunUpdateForCookies();
	Check(!g_bMuted[1], "the reused slot is not muted");
	CheckEqInt(g_iVolume[1], g_Cfg.iDefaultVolume, "the reused slot has the default volume");
	Check(g_sLang[1].empty(), "the reused slot has no inherited language");

	// a failing LoadUser is retried and keeps the session defaults
	AddHuman(1, 888, 2, "Yuri");
	g_loadUserFails = true;
	const int before = g_loadUserCalls;
	RunUpdateForCookies();
	Check(g_loadUserCalls > before, "a failed LoadUser is retried");
	CheckEqInt(g_iVolume[1], g_Cfg.iDefaultVolume, "a failed load leaves the defaults");
	g_loadUserFails = false;

	// without the permissions plugin the reconcile is a no-op, the plugin keeps running
	__permissions_LoadUser = nullptr;
	const int beforeUnavailable = g_loadUserCalls;
	RunUpdateForCookies();
	CheckEqInt(g_loadUserCalls, beforeUnavailable, "no LoadUser call without the binding");
	Check(g_pluginStarted, "the plugin is still running");
	__permissions_LoadUser = &FakeLoadUser;

	StopPlugin();

	Section("messages: center default/alert/html and the per-player language");
	ResetWorld();
	SetConfigText(ReplaceKeyValue(ShippedConfig(), "enable_center_message", "1"));
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	FakeEvent death2 = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &death2);
	CheckEqInt((int64_t)g_center.size(), 1, "center_message_type default -> PrintCenterText");
	Check(g_center[0].second.find("First Blood") == std::string::npos,
		  "the Russian server language was used for the center text");
	Check(g_center[0].second.find("Первая кровь") != std::string::npos,
		  "Russian record message in the center text");

	SetConfigText(ReplaceKeyValue(ReplaceKeyValue(ShippedConfig(), "enable_center_message", "1"),
								  "center_message_type", "alert"));
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console, {"mm_quakesounds", "reload"});
	g_mapKills.clear();
	g_center.clear();
	g_alert.clear();
	FireGameEvent("player_death", &death2);
	CheckEqInt((int64_t)g_alert.size(), 1, "center_message_type alert -> PrintAlertText");
	Check(g_center.empty(), "no center print in alert mode");

	SetConfigText(ReplaceKeyValue(ReplaceKeyValue(ShippedConfig(), "enable_center_message", "1"),
								  "center_message_type", "html"));
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console, {"mm_quakesounds", "reload"});
	g_mapKills.clear();
	g_alert.clear();
	g_html.clear();
	FireGameEvent("player_death", &death2);
	CheckEqInt((int64_t)g_html.size(), 1, "center_message_type html -> PrintCentreHtml");
	if (!g_html.empty())
	{
		CheckEqInt(std::get<2>(g_html[0]), 3, "the html center message lasts 3 seconds");
		Check(std::get<1>(g_html[0]).find("Первая кровь") != std::string::npos,
			  "Russian record message in the html text");
	}
	StopPlugin();
}

void TestSoundVolumeMessage()
{
	Section("sounds: EmitSound, precache and the GE_SosSetSoundEventParams volume path");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	g_iVolume[1] = 30;

	FakeUserMessage start;
	start.id = 208;
	start.name = "CMsgSosStartSoundEvent";
	start.serializable = true;
	start.hasGuid = true;
	start.guid = 77;
	start.hash = qs::SoundEventHash("Alko.Quake7.Headshot");
	start.mask = 1ull << 1;
	Check(g_userMessageCallback != nullptr, "the user-message hook is installed");
	if (g_userMessageCallback != nullptr)
	{
		g_userMessageSends.clear();
		CheckEqInt((int64_t)g_userMessageCallback(&start), (int64_t)s2sdk::ResultType::Continue,
				   "the volume hook answers Continue");
		CheckEqInt((int64_t)g_userMessageSends.size(), 1, "one GE_SosSetSoundEventParams was sent");
		if (!g_userMessageSends.empty())
		{
			const SentUserMessage& send = g_userMessageSends[0];
			CheckEqInt(send.id, 210, "GE_SosSetSoundEventParams id");
			CheckEqInt(send.guid, 77, "the same soundevent_guid");
			CheckEqInt((int64_t)send.mask, (int64_t)(1ull << 1), "the recipient mask is the listener");
			Check(send.packed == qs::PackVolumeParam(0.3f),
				  "the packed volume parameter is the record volume * listener volume");
		}

		// somebody else's soundevent hash is ignored
		g_userMessageSends.clear();
		start.hash = qs::SoundEventHash("Some.Other.Sound");
		g_userMessageCallback(&start);
		Check(g_userMessageSends.empty(), "a foreign soundevent hash is ignored");

		// 100% of a 1.0 record is the soundevent's own volume -> no params message
		g_userMessageSends.clear();
		g_iVolume[1] = 100;
		start.hash = qs::SoundEventHash("Alko.Quake7.Headshot");
		g_userMessageCallback(&start);
		Check(g_userMessageSends.empty(), "volume 1.0 needs no SetParams message");

		// a message without soundevent_guid is not a start
		start.hasGuid = false;
		g_iVolume[1] = 30;
		g_userMessageSends.clear();
		g_userMessageCallback(&start);
		Check(g_userMessageSends.empty(), "a start without soundevent_guid is ignored");
		CheckEqInt((int64_t)g_userMessageCallback(nullptr), (int64_t)s2sdk::ResultType::Continue,
				   "a null user message is a no-op");
	}
	StopPlugin();
	CheckEqInt(g_userMessageUnhooks, 1, "the user-message hook is removed on unload");
}


void TestSoundVolumeSafety()
{
	Section("sounds: unresolved/invalid user messages never reach Create/Pb/Send/Hook");

	Section("sounds: registry unresolved -> no hook, no create/name/Pb/send, feature disabled");
	CheckEqInt(FakeUserMessageFindMessageIdByName(plg::string("SosStartSoundEvent")), 208,
			   "the fake registry resolves the real Sos... name");
	CheckEqInt(FakeUserMessageFindMessageIdByName(plg::string("GE_SosStartSoundEvent")), 0,
			   "the fake registry does not accept the old GE_ spelling");
	ResetWorld();
	g_messageLookupEnabled = false;
	StartPlugin();
	OpenMap();
	CheckEqInt(g_userMessageHookId, -1, "unresolved SOS ids install no hook");
	CheckEqInt(g_userMessageHooksCalls, 0, "no HookUserMessage call for an unresolved id");
	CheckEqInt(g_userMessageCreateCalls, 0, "no UserMessageCreateFromId for an unresolved id");
	CheckEqInt(g_userMessageNameCalls, 0, "no GetMessageName for an unresolved id");
	CheckEqInt(g_pbReadCalls, 0, "no PbRead for an unresolved id");
	CheckEqInt(g_pbSetCalls, 0, "no PbSet for an unresolved id");
	CheckEqInt(g_userMessageSendCalls, 0, "no Send for an unresolved id");
	Check(LoggedWith(plg::Severity::Error, "SosStartSoundEvent is not in"),
		  "the unresolved start id is logged");
	StopPlugin();

	Section("sounds: Set id unresolved -> start hook works, volume is a safe no-op");
	ResetWorld();
	g_sosSetId = 0;
	StartPlugin();
	OpenMap();
	CheckEqInt(g_userMessageHookId, 208, "the resolved start id is still hooked");
	Check(LoggedWith(plg::Severity::Error, "SosSetSoundEventParams is not in"),
		  "the unresolved set id is logged");
	FakeUserMessage start;
	start.id = 208;
	start.name = "CMsgSosStartSoundEvent";
	start.serializable = true;
	start.hasGuid = true;
	start.guid = 12;
	start.hash = qs::SoundEventHash("Alko.Quake7.Headshot");
	start.mask = 1ull << 1;
	g_userMessageSends.clear();
	g_userMessageCreateCalls = 0;
	g_pbSetCalls = 0;
	g_userMessageSendCalls = 0;
	g_userMessageCallback(&start);
	CheckEqInt(g_userMessageCreateCalls, 0, "no params message is created without the set id");
	CheckEqInt(g_pbSetCalls, 0, "no PbSet without the set id");
	CheckEqInt(g_userMessageSendCalls, 0, "no Send without the set id");
	StopPlugin();

	Section("sounds: incoming name/id are verified before any field read");
	ResetWorld();
	StartPlugin();
	OpenMap();
	CheckEqInt(g_userMessageHookId, 208, "the valid start id is hooked");
	FakeUserMessage message;
	message.id = 208;
	message.name = "CMsgSomethingElse";
	message.serializable = true;
	message.hasGuid = true;
	message.guid = 13;
	message.hash = qs::SoundEventHash("Alko.Quake7.Headshot");
	message.mask = 1ull << 1;
	g_pbReadCalls = 0;
	g_userMessageCreateCalls = 0;
	g_userMessageSendCalls = 0;
	g_userMessageCallback(&message);
	CheckEqInt(g_pbReadCalls, 0, "a mismatched incoming message name reaches no PbRead");
	CheckEqInt(g_userMessageCreateCalls, 0, "a mismatched incoming message name creates no params message");
	CheckEqInt(g_userMessageSendCalls, 0, "a mismatched incoming message name sends nothing");

	message.name = "CMsgSosStartSoundEvent";
	message.id = 999; // right name, stale/wrong id
	g_pbReadCalls = 0;
	g_userMessageCreateCalls = 0;
	g_userMessageSendCalls = 0;
	g_userMessageCallback(&message);
	CheckEqInt(g_pbReadCalls, 0, "a wrong incoming message id reaches no PbRead");
	CheckEqInt(g_userMessageCreateCalls, 0, "a wrong incoming message id creates no params message");
	CheckEqInt(g_userMessageSendCalls, 0, "a wrong incoming message id sends nothing");

	message.id = 208;
	g_userMessageCreateCalls = 0;
	g_pbSetCalls = 0;
	g_userMessageSendCalls = 0;
	g_createFromIdNullSerializable = true;
	g_userMessageCallback(&message);
	CheckEqInt(g_userMessageCreateCalls, 1, "the params object is created for the valid start");
	CheckEqInt(g_pbSetCalls, 0, "a null serializable params object is not written");
	CheckEqInt(g_userMessageSendCalls, 0, "a null serializable params object is not sent");
	g_createFromIdNullSerializable = false;

	Section("sounds: Stop id unresolved is a safe no-op");
	StopPlugin();
	ResetWorld();
	g_sosStopId = 0;
	StartPlugin();
	OpenMap();
	g_userMessageCreateCalls = 0;
	g_userMessageSendCalls = 0;
	sound_volume::Stop(1, 77);
	CheckEqInt(g_userMessageCreateCalls, 0, "Stop creates nothing without the stop id");
	CheckEqInt(g_userMessageSendCalls, 0, "Stop sends nothing without the stop id");
	StopPlugin();
}

void TestMenusAndCommands()
{
	Section("commands/menus: !qs, mm_qs, !lang, mm_quakesounds and deferred menu actions");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddBot(5, 2, "BOT");

	const s2sdk::ResultType qsResult =
		RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(qsResult == s2sdk::ResultType::Handled, "chat !qs is consumed");
	const FakeMenu* menu = MenuOfSlot(1);
	Check(menu != nullptr, "!qs opened a menu");
	if (menu != nullptr)
	{
		CheckEqInt((int64_t)menu->items.size(), 2, "the menu has toggle + volume items");
		Check(ItemIndexByInfo(*menu, "toggle") >= 0, "toggle item");
		Check(ItemIndexByInfo(*menu, "volume") >= 0, "volume item (soundevents exist)");
		CheckEqStr(menu->items[static_cast<std::size_t>(ItemIndexByInfo(*menu, "toggle"))].display,
				   "Выключить звуки", "Russian mute item");
		Check(menu->exit && !menu->exitBack, "the main menu has exit but no back button");
	}
	// s2sdk's first MenuId is 0; the plugin burns it and gets a usable id
	Check(g_createdMenus.size() >= 2 && g_createdMenus[0] == 0 && g_createdMenus[1] == 1,
		  "the first MenuId 0 was burned, the menu is id 1");

	// the chat spelling in the console context is refused
	const std::size_t createdBeforeConsole = g_createdMenus.size();
	Check(RunCommand("qs", 1, s2sdk::ConCommandContext::Console, {"qs"}) ==
			  s2sdk::ResultType::Continue,
		  "console qs is not handled");
	CheckEqInt((int64_t)g_createdMenus.size(), (int64_t)createdBeforeConsole,
			   "console qs opens no menu");

	// mm_qs is the client-console spelling
	Check(RunCommand("mm_qs", 1, s2sdk::ConCommandContext::Console, {"mm_qs"}) ==
			  s2sdk::ResultType::Handled,
		  "console mm_qs is handled");
	Check(MenuOfSlot(1) != nullptr, "mm_qs opened the menu");
	Check(RunCommand("mm_qs", 1, s2sdk::ConCommandContext::Chat, {"mm_qs"}) ==
			  s2sdk::ResultType::Continue,
		  "mm_qs is not a chat command");

	// a bot is refused by the settings command
	const std::size_t createdBeforeBot = g_createdMenus.size();
	RunCommand("qs", 5, s2sdk::ConCommandContext::Chat, {"qs"});
	CheckEqInt((int64_t)g_createdMenus.size(), (int64_t)createdBeforeBot, "bots get no menu");

	Section("menus: selection is deferred, then toggles the mute and writes the cookie");
	g_cookieWrites.clear();
	Check(SelectMenuItem(1, "toggle"), "toggle selected");
	Check(!g_bMuted[1], "no state change inside the menu handler");
	Check(!CookieWasWritten(111, "QuakeSounds.Muted", "1"),
		  "no cookie write inside the menu handler");
	Update();
	Check(g_bMuted[1], "the deferred action toggled the mute");
	Check(CookieWasWritten(111, "QuakeSounds.Muted", "1"), "the mute cookie was written");
	menu = MenuOfSlot(1);
	Check(menu != nullptr, "the menu was reopened after the toggle");
	if (menu != nullptr)
	{
		const int32_t idx = ItemIndexByInfo(*menu, "toggle");
		Check(idx >= 0 && menu->items[static_cast<std::size_t>(idx)].display == "Включить звуки",
			  "the reopened menu offers Unmute");
	}

	Section("menus: volume submenu, +10%, disabled limits and ExitBack");
	Check(SelectMenuItem(1, "volume"), "volume selected");
	Update();
	menu = MenuOfSlot(1);
	Check(menu != nullptr && menu->exitBack, "the volume menu has the back button");
	if (menu != nullptr)
	{
		CheckEqStr(menu->title, "Громкость: 30%", "the volume menu title");
		CheckEqInt((int64_t)menu->items.size(), 2, "up/down items");
	}
	Check(SelectMenuItem(1, "up"), "up selected");
	Update();
	CheckEqInt(g_iVolume[1], 40, "volume increased by 10%");
	Check(CookieWasWritten(111, "QuakeSounds.Volume", "40"), "the volume cookie was written");
	menu = MenuOfSlot(1);
	Check(menu != nullptr, "volume menu reopened");
	if (menu != nullptr) CheckEqStr(menu->title, "Громкость: 40%", "the new volume in the title");

	Check(ExitBackMenu(1), "ExitBack selected");
	Update();
	menu = MenuOfSlot(1);
	Check(menu != nullptr, "ExitBack returns to the main menu");
	if (menu != nullptr) CheckEqStr(menu->title, "Quake-звуки", "the parent menu title");

	Check(ExitMenu(1), "Exit selected");
	Update();
	Check(MenuOfSlot(1) == nullptr, "Exit closes the menu");

	// the up item is disabled at 100%
	g_iVolume[1] = 100;
	ShowVolumeMenu(1);
	menu = MenuOfSlot(1);
	if (menu != nullptr)
	{
		const int32_t up = ItemIndexByInfo(*menu, "up");
		Check(up >= 0 && menu->items[static_cast<std::size_t>(up)].style ==
							 s2sdk::MenuItemStyle::Disabled,
			  "the up item is disabled at 100%");
	}
	(void)ReleaseSlotMenu(1, /*mapReady=*/true);
	Update();

	Section("commands: !lang, mm_quakesounds and ownership");
	g_cookieWrites.clear();
	g_chat.clear();
	Check(RunCommand("lang", 1, s2sdk::ConCommandContext::Chat, {"lang", "en"}) ==
			  s2sdk::ResultType::Handled,
		  "!lang en is handled");
	CheckEqStr(g_sLang[1], "en", "the player's language is en");
	Check(CookieWasWritten(111, "QuakeSounds.Lang", "en"), "the language cookie was written");
	Check(!g_chat.empty(), "!lang answers in chat");
	g_chat.clear();
	RunCommand("lang", 1, s2sdk::ConCommandContext::Chat, {"lang"});
	Check(!g_chat.empty() && g_chat.back().second.find("Использование") != std::string::npos,
		  "!lang without an argument prints the usage phrase");
	g_chat.clear();
	RunCommand("lang", 1, s2sdk::ConCommandContext::Chat, {"lang", "x"});
	Check(!g_chat.empty(), "an invalid language prints the usage phrase");
	const std::string languageBefore = g_sLang[1];
	Check(RunCommand("lang", 1, s2sdk::ConCommandContext::Console, {"lang", "de"}) ==
			  s2sdk::ResultType::Continue,
		  "the console context is refused");
	CheckEqStr(g_sLang[1], languageBefore, "the console spelling changed nothing");

	g_serverPrints.clear();
	RunCommand("mm_quakesounds", 7, s2sdk::ConCommandContext::Console,
			   {"mm_quakesounds", "disable"});
	Check(g_Cfg.bEnabled, "a player cannot run the server command");
	RunCommand("mm_quakesounds", 0, s2sdk::ConCommandContext::Console,
			   {"mm_quakesounds", "disable"});
	Check(g_Cfg.bEnabled, "slot 0 is not the server console either");
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console,
			   {"mm_quakesounds", "disable"});
	Check(!g_Cfg.bEnabled, "the server console disables the plugin");
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console,
			   {"mm_quakesounds", "enable"});
	Check(g_Cfg.bEnabled, "the server console enables the plugin");
	g_serverPrints.clear();
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console,
			   {"mm_quakesounds", "nonsense"});
	Check(!g_serverPrints.empty() &&
			  g_serverPrints.back().find("usage: mm_quakesounds") != std::string::npos,
		  "the usage line goes to the server console");

	// reload picks up the new value but does not re-register commands
	SetConfigText(ReplaceKeyValue(ShippedConfig(), "default_volume", "0.5"));
	g_serverPrints.clear();
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console,
			   {"mm_quakesounds", "reload"});
	CheckEqInt(g_Cfg.iDefaultVolume, 50, "reload re-read the config");
	Check(!g_serverPrints.empty() &&
			  g_serverPrints.back().find("configuration reloaded") != std::string::npos,
		  "the reload line goes to the server console");
	CheckEqInt((int64_t)g_fakeCommands.size(), 4, "reload did not re-register commands");

	// command ownership: a pre-existing name is detached with RemoveCommandListener
	StopPlugin();
	ResetWorld();
	g_commandCreated["qs"] = false;
	StartPlugin();
	OpenMap();
	StopPlugin();
	Check(std::find(g_removedListeners.begin(), g_removedListeners.end(), "qs") !=
			  g_removedListeners.end(),
		  "qs was detached as a listener (pre-existing name)");
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "qs") ==
			  g_removedCommands.end(),
		  "qs was not removed as a command");
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "mm_qs") !=
			  g_removedCommands.end(),
		  "mm_qs (created) was removed as a command");
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "mm_quakesounds") !=
			  g_removedCommands.end(),
		  "mm_quakesounds (created) was removed as a command");
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "lang") !=
			  g_removedCommands.end(),
		  "lang (created) was removed as a command");
	Check(g_addConsoleCommandCalls >= 4, "the foreign-name start still called AddConsoleCommand");
	Check(g_fakeCommands.empty(), "no command callback is left after the foreign-name unload");

	// foreign ownership survives a reload: each start re-attaches the listener, each end removes it
	ResetWorld();
	g_commandCreated["qs"] = false;
	StartPlugin();
	CheckEqInt((int64_t)g_fakeCommands.size(), 4, "foreign-name commands registered on the first start");
	StopPlugin();
	StartPlugin();
	CheckEqInt((int64_t)g_fakeCommands.size(), 4, "foreign-name commands registered on the reload");
	StopPlugin();
	int qsListenerRemovals = 0;
	for (const std::string& name : g_removedListeners)
		if (name == "qs") ++qsListenerRemovals;
	CheckEqInt(qsListenerRemovals, 2, "the foreign qs listener was removed on both unloads");
	Check(g_fakeCommands.empty(), "no dangling command callback after the foreign-name reload");

	Section("menus: a map end destroys the open menu; a missing menu binding fails soft");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(MenuOfSlot(1) != nullptr, "menu open before the map end");
	StopPlugin();
	Check(g_fakeMenus.empty() && g_clientMenus.empty(), "unload visibly destroys the open menu");
	CheckEqInt(g_backendCloses, 0, "the live-map unload used the Disconnect path (no backend close)");
	Update();
	CheckEqInt(g_handlerCallsAfterUnload, 0, "the unloaded menu has no live callback");

	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(MenuOfSlot(1) != nullptr, "menu open before the map end");
	FireMapEnd();
	Check(g_clientMenus.empty(), "the map end cleared the client menu");
	Check(BoundMenuCount() == 0, "the displayed menu was destroyed on the map end");
	StopPlugin();

	// a destroy queued by a selection but not yet drained is freed on the map end as well
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(SelectMenuItem(1, "toggle"), "toggle selected before the map end");
	Check(!g_bMuted[1], "the deferred action did not run yet");
	FireMapEnd();
	Check(BoundMenuCount() == 0, "the queued menu destroy ran on the map end");
	Check(!g_bMuted[1], "the queued action was dropped with the map");
	StopPlugin();

	// ... and on unload without an intervening update
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(SelectMenuItem(1, "toggle"), "toggle selected before unload");
	StopPlugin();
	Check(BoundMenuCount() == 0 && g_fakeMenus.empty(), "the queued menu destroy ran on unload");

	ResetWorld();
	g_fakeMenus.clear();
	g_createdMenus.clear();
	__s2sdk_CreateMenu = nullptr;
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	Check(RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"}) ==
			  s2sdk::ResultType::Handled,
		  "a missing menu binding does not break the command");
	Check(LoggedWith(plg::Severity::Error, "menu bindings are unavailable"),
		  "the missing menu binding is logged");
	StopPlugin();

	Section("commands: a config without soundevents toggles the mute directly");
	ResetWorld();
	SetConfigText(ConfigFileOnlySounds());
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	g_chat.clear();
	g_createdMenus.clear();
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(g_bMuted[1], "direct mute toggle when no soundevent record exists");
	Check(BoundMenuCount() == 0, "no menu in the direct-toggle path");
	Check(!g_chat.empty() && g_chat.back().second.find("выключены") != std::string::npos,
		  "the Muted phrase was printed");
	Check(CookieWasWritten(111, "QuakeSounds.Muted", "1"),
		  "the direct toggle writes the mute cookie");
	StopPlugin();
}


void TestMenuTypeChoice()
{
	Section("menus: the optional hudmenu per-player !menu choice is honoured");
	struct Scenario
	{
		const char* choice;
		bool hudRegistered;
		bool wasdRegistered;
		bool withHudmenu;
		const char* expected;
	};
	const Scenario scenarios[] = {
		{"chat", true, true, true, "chat"},
		{"centerhtml", true, true, true, "centerhtml"},
		{"button", true, true, true, "button"},
		{"hud", true, true, true, "hud"},
		{"wasd", true, true, true, "wasd"},
		{"hud", false, true, true, "button"},   // backend gone -> WASD fallback
		{"wasd", true, false, true, "button"},  // backend gone -> WASD fallback
		{"garbage", true, true, true, "hud"},   // unknown answer -> HUD_LAYOUT default
		{"", true, true, true, "hud"},          // hudmenu installed but no per-player choice
		{"", true, true, false, "hud"},         // no hudmenu, hud backend registered
		{"", false, true, false, "button"},     // no hudmenu, no hud backend
	};
	for (const Scenario& scenario : scenarios)
	{
		ResetWorld();
		StartPlugin();
		OpenMap();
		AddHuman(1, 111, 2, "Alice");
		g_registeredMenuTypes = {"chat", "console", "centerhtml", "button", "wasd"};
		if (scenario.hudRegistered) g_registeredMenuTypes.insert("hud");
		else g_registeredMenuTypes.erase("hud");
		if (scenario.wasdRegistered) g_registeredMenuTypes.insert("wasd");
		else g_registeredMenuTypes.erase("wasd");
		g_menuTypeChoices[1] = scenario.choice;
		if (!scenario.withHudmenu) __hudmenu_GetClientMenuType = nullptr;
		RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
		const FakeMenu* menu = MenuOfSlot(1);
		Check(menu != nullptr, std::string("menu opened for choice '") + scenario.choice + "'");
		if (menu != nullptr)
			CheckEqStr(menu->type, scenario.expected,
					   std::string("menu type for choice '") + scenario.choice + "'");
		StopPlugin();
	}
}

void TestMenuIdentityAndFailSafe()
{
	Section("menus: the display-time steam id + controller serial binds every deferred click");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	SetControllerSerial(1, static_cast<int32_t>(0x40000001u));
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(MenuOfSlot(1) != nullptr, "menu open for Alice");
	Check(g_menus[1].identity.uSteam == 111, "the menu record stores Alice's steam id");
	CheckEqInt(g_menus[1].identity.iController, static_cast<int32_t>(0x40000001u),
			   "the menu record stores Alice's controller handle");

	// the same steam id on a new controller entity (reconnect/reuse) before the click
	SetControllerSerial(1, static_cast<int32_t>(0x50000001u));
	g_cookieWrites.clear();
	Check(SelectMenuItem(1, "toggle"), "the old menu can still be clicked");
	Update();
	Check(!g_bMuted[1], "the deferred toggle of the old occupant did not run");
	Check(!CookieWasWritten(111, "QuakeSounds.Muted", "1"), "no cookie was written for the reused slot");
	Check(MenuOfSlot(1) == nullptr, "no replacement menu was opened for the reused slot");

	// reuse after the click but before OnPluginUpdate drains the action
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	SetControllerSerial(1, static_cast<int32_t>(0x40000001u));
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(SelectMenuItem(1, "toggle"), "toggle selected for Alice");
	SetControllerSerial(1, static_cast<int32_t>(0x50000001u));
	Update();
	Check(!g_bMuted[1], "reuse before the update dropped the deferred action");
	Check(MenuOfSlot(1) == nullptr, "no menu was opened after the dropped action");

	// a different steam id is also rejected
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(SelectMenuItem(1, "toggle"), "toggle selected before the steam-id change");
	AddHuman(1, 222, 2, "Bob");
	Update();
	Check(!g_bMuted[1], "a different player in the slot did not inherit the click");
	Check(MenuOfSlot(1) == nullptr, "no menu for the different player");

	// the same identity still applies the action normally
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(SelectMenuItem(1, "toggle"), "toggle selected for the unchanged identity");
	Update();
	Check(g_bMuted[1], "the unchanged identity applies the click");

	StopPlugin();
	Check(g_handlerCallsAfterUnload == 0, "no menu handler runs after unload");

	Section("identity: per-player state survives a controller serial change on map load");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	SetControllerSerial(1, static_cast<int32_t>(0x40000001u));
	FakeEvent setupKill = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &setupKill); // records Alice's identity
	g_bMuted[1] = true;
	g_iVolume[1] = 40;
	g_sLang[1] = "en";
	FireMapEnd();
	FireMapStart();
	AddHuman(1, 111, 2, "Alice"); // same steam id, new controller entity after the map load
	AddHuman(2, 222, 3, "Bob");
	SetControllerSerial(1, static_cast<int32_t>(0x50000001u));
	FakeEvent kill = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &kill);
	Check(g_bMuted[1], "the mute switch survives the map-load controller serial change");
	CheckEqInt(g_iVolume[1], 40, "the volume survives the map-load controller serial change");
	CheckEqStr(g_sLang[1], "en", "the language survives the map-load controller serial change");
	StopPlugin();

	Section("menus: map-down contract violation -> no backend close, no callback after unload");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(MenuOfSlot(1) != nullptr, "menu displayed before the changelevel");
	g_backendCloses = 0;
	g_cancelClientMenuFails = true; // the manager keeps state.id after a false Cancel
	FireMapEnd();
	CheckEqInt(g_backendCloses, 0, "the map-down teardown never used a backend close");
	Check(BoundMenuCount() == 0, "the menu was destroyed through the builtin no-op-close type");
	StopPlugin();
	Update();
	CheckEqInt(g_handlerCallsAfterUnload, 0, "no handler callback after the unload");
	Check(g_fakeMenus.empty(), "no menu survives the unload");

	// CancelClientMenu answered true but the state was not cleared: the mandatory recheck must
	// still switch to the no-op-close type instead of trusting the answer.
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(MenuOfSlot(1) != nullptr, "menu displayed before the lying Cancel");
	g_backendCloses = 0;
	g_cancelClientMenuLeavesState = true;
	FireMapEnd();
	CheckEqInt(g_backendCloses, 0, "the recheck prevented a backend close despite Cancel=true");
	Check(BoundMenuCount() == 0, "the menu was destroyed after the recheck");
	StopPlugin();
	Update();
	CheckEqInt(g_handlerCallsAfterUnload, 0, "no callback after unload on the lying-Cancel path");

	Section("menus: A7_UNRESOLVED is reported when even the no-op close fails");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(MenuOfSlot(1) != nullptr, "menu displayed before the unresolved teardown");
	g_cancelClientMenuFails = true;
	g_setMenuTypeFails = true;
	g_backendCloses = 0;
	FireMapEnd();
	CheckEqInt(g_backendCloses, 0, "the unresolved map-down teardown did not call a backend close");
	Check(LoggedWith(plg::Severity::Warning, "A7_UNRESOLVED (parked)"),
		  "the unresolved menu was parked for the next map");
	Check(LoggedWith(plg::Severity::Warning, "changelevel window"),
		  "the parked menu mentions the changelevel window");
	StopPlugin();
	Check(LoggedWith(plg::Severity::Error, "A7_UNRESOLVED"),
		  "unload reports A7_UNRESOLVED instead of silently dropping the menu");
	CheckEqInt(g_backendCloses, 0, "no backend close on the unresolved unload either");
	CheckEqInt(g_handlerCallsAfterUnload, 0, "no handler callback ran after the unload returned");

	Section("menus: partial bindings disable menus instead of creating an unreleasable one");
	for (int missing = 0; missing < 5; ++missing)
	{
		ResetWorld();
		if (missing == 0) __s2sdk_SetMenuType = nullptr;
		else if (missing == 1) __s2sdk_GetMenuType = nullptr;
		else if (missing == 2) __s2sdk_CancelClientMenu = nullptr;
		else if (missing == 3) __s2sdk_GetClientMenu = nullptr;
		else __s2sdk_DestroyMenu = nullptr;
		StartPlugin();
		OpenMap();
		AddHuman(1, 111, 2, "Alice");
		RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
		Check(BoundMenuCount() == 0, "a partial menu binding creates no menu");
		Check(MenuOfSlot(1) == nullptr, "no menu is displayed with a partial binding set");
		StopPlugin();
		Check(g_handlerCallsAfterUnload == 0, "no callback survives the unload with partial bindings");
	}
}

void TestConfigPhrases()
{
	std::error_code error;
	Section("config: the direct <configs>/quake_sounds file wins over a raw cwd decoy");
	ResetWorld();
	SetRawConfigText("\"QuakeSounds\"\n{\n  \"global\" { \"default_volume\" \"0.9\" }\n"
					 "  \"sounds\" { \"2\" { \"_sound\" \"Alko.Test.Double\" } }\n}\n");
	SetRawPhrasesText("\"Phrases\"\n{\n  \"Menu_Title\" { \"ru\" \"RawTitle\" \"en\" \"RawTitle\" }\n"
					  "  \"Prefix\" { \"ru\" \"[Raw]\" \"en\" \"[Raw]\" }\n}\n");
	StartPlugin();
	CheckEqInt(g_Cfg.iDefaultVolume, 30, "the direct config default_volume won over the raw decoy");
	CheckEqInt((int64_t)g_mapEntries.size(), 28, "the direct config sound records won over the raw decoy");
	CheckEqStr(Phrase("Menu_Title"), "Quake-звуки", "the direct phrases won over the raw decoy");
	StopPlugin();

	Section("config: raw cwd fallback only when the direct files are absent");
	ResetWorld();
	SetRawConfigText("\"QuakeSounds\"\n{\n  \"global\" { \"default_volume\" \"0.9\" }\n"
					 "  \"sounds\" { \"2\" { \"_sound\" \"Alko.Test.Double\" } }\n}\n");
	SetRawPhrasesText("\"Phrases\"\n{\n  \"Menu_Title\" { \"ru\" \"RawTitle\" \"en\" \"RawTitle\" }\n"
					  "  \"Prefix\" { \"ru\" \"[Raw]\" \"en\" \"[Raw]\" }\n}\n");
	std::filesystem::remove(ConfigPath(), error);
	std::filesystem::remove(PhrasesPath(), error);
	StartPlugin();
	CheckEqInt(g_Cfg.iDefaultVolume, 90, "the raw fallback config is used when direct is absent");
	CheckEqInt((int64_t)g_mapEntries.size(), 1, "the raw fallback sound record is used when direct is absent");
	CheckEqStr(Phrase("Menu_Title"), "RawTitle", "the raw fallback phrases are used when direct is absent");
	StopPlugin();

	Section("config: a malformed direct file is not masked by a good raw fallback");
	ResetWorld();
	SetRawConfigText("\"QuakeSounds\"\n{\n  \"global\" { \"default_volume\" \"0.9\" }\n}\n");
	SetConfigText("\"QuakeSounds\" { \"global\" ");
	StartPlugin();
	CheckEqInt(g_Cfg.iDefaultVolume, 100, "a malformed direct config leaves the defaults");
	Check(g_mapEntries.empty(), "a malformed direct config leaves no records");
	Check(LoggedWith(plg::Severity::Error, "not valid KeyValues"), "the malformed direct config is reported");
	Check(LoggedWith(plg::Severity::Error, ConfigPath()), "the reported path is the direct file");
	StopPlugin();

	Section("language: core.cfg -> s2sdk -> en");
	ResetWorld();
	RemoveRawConfig();
	RemoveRawPhrases();
	g_sdkLanguage = "english";
	StartPlugin();
	CheckEqStr(g_sServerLang, "ru", "core.cfg ServerLang russian wins over s2sdk english");
	StopPlugin();

	ResetWorld();
	std::filesystem::remove(CoreConfigPath(), error);
	g_sdkLanguage = "english";
	StartPlugin();
	CheckEqStr(g_sServerLang, "en", "without core.cfg the s2sdk language is used");
	CheckEqStr(Phrase("Menu_Title"), "Quake Sounds", "the English phrase was selected");
	StopPlugin();

	ResetWorld();
	std::filesystem::remove(CoreConfigPath(), error);
	g_sdkLanguage = "";
	StartPlugin();
	CheckEqStr(g_sServerLang, "en", "without core.cfg and s2sdk the fallback is en");
	StopPlugin();

	Section("config/phrases: missing and malformed files keep the defaults");
	ResetWorld();
	std::filesystem::remove(ConfigPath(), error);
	std::filesystem::remove(PhrasesPath(), error);
	StartPlugin();
	CheckEqInt(g_Cfg.iDefaultVolume, 100, "missing config -> defaults");
	Check(g_mapEntries.empty(), "missing config -> no sound records");
	Check(LoggedWith(plg::Severity::Error, "failed to load"), "the missing config is logged");
	Check(!g_mapPhrases.empty() || Phrase("Menu_Mute") == "Выключить звуки",
		  "missing phrases -> the built-in table is used");
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	FakeEvent death = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &death);
	Check(g_emitted.empty(), "no record -> no sound");
	StopPlugin();

	ResetWorld();
	SetConfigText("\"QuakeSounds\" { \"global\" { \"default_volume\" ");
	StartPlugin();
	CheckEqInt(g_Cfg.iDefaultVolume, 100, "malformed config -> defaults");
	Check(LoggedWith(plg::Severity::Error, "not valid KeyValues"),
		  "the malformed config is reported");
	StopPlugin();

	Section("config: a reload re-reads the precache list; command names change on plugin reload");
	ResetWorld();
	StartPlugin();
	const std::size_t commandsBefore = g_fakeCommands.size();
	SetConfigText(ReplaceKeyValue(ShippedConfig(), "settings", "!qs,!quake"));
	RunCommand("mm_quakesounds", -1, s2sdk::ConCommandContext::Console, {"mm_quakesounds", "reload"});
	CheckEqInt((int64_t)g_fakeCommands.size(), (int64_t)commandsBefore,
			   "a reload does not change the registered commands");
	StopPlugin();
	StartPlugin();
	Check(g_fakeCommands.count("quake") == 1, "a plugin reload registers the new chat command");
	StopPlugin();

	ResetWorld();
	SetConfigText("\"QuakeSounds\"\n{\n  \"precache\"\n  {\n    \"soundevent_file\" \"events/one.vsndevts\"\n"
				  "    \"additional_files\" { \"f1\" \"sounds/one.vsnd\" }\n  }\n}\n");
	StartPlugin();
	g_precached.clear();
	FireManifest();
	CheckEqInt((int64_t)g_precached.size(), 2, "the reloaded/shipped precache list is used");
	CheckEqStr(g_precached.front(), "events/one.vsndevts", "custom precache first entry");
	StopPlugin();
}

void TestBombAndWarmup()
{
	Section("C4: planted_c4 m_flC4Blow on a signed handle, one beep per second");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	g_bombEntityPresent = true;
	g_bombHandle = -123456; // a valid CEntityHandle that reads negative
	g_gameTime = 100.0f;
	g_bombBlow = 103.4f;
	g_emitted.clear();
	FakeEvent planted;
	FireGameEvent("bomb_planted", &planted);
	Check(g_bBombActive, "bomb_planted arms the countdown");
	Update(0);
	CheckEqInt(SoundsNamed("Alko.Timer.4sec"), 2, "ceil(3.4) = 4 -> bomb_4 to both listeners");
	CheckEqInt(g_lastFloatHandle, g_bombHandle,
			   "the negative bomb handle was passed to the schema read");
	g_emitted.clear();
	Update(0);
	Check(g_emitted.empty(), "the same second does not beep twice");
	g_gameTime = 101.1f;
	Update(0);
	CheckEqInt(SoundsNamed("Alko.Timer.3sec"), 2, "ceil(2.3) = 3 -> bomb_3");
	g_emitted.clear();
	g_gameTime = 105.0f;
	Update(0);
	Check(g_emitted.empty() && !g_bBombActive, "the countdown stops when the blow time passed");
	Check(LoggedWith(plg::Severity::Info, "sound records"),
		  "the config was logged at load");

	// round_end stops a running countdown
	g_gameTime = 100.0f;
	g_bombBlow = 110.0f;
	FireGameEvent("bomb_planted", &planted);
	Check(g_bBombActive, "the countdown is armed again");
	FakeEvent roundEnd;
	FireGameEvent("round_end", &roundEnd);
	Check(!g_bBombActive, "round_end stops the countdown");

	Section("warmup: the schema field is read only with the gate open, and gates kills");
	ResetWorld();
	SetConfigText(ReplaceKeyValue(ShippedConfig(), "enabled_during_warmup", "0"));
	StartPlugin();
	ResetCallCounters();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	FakeEvent death = MakeDeathEvent(2, 1, "ak47", false);
	FireGameEvent("player_death", &death); // gate shut -> no schema read at all
	CheckEqInt(g_schemaCalls, 0, "no schema lookup before the first map");
	OpenMap();
	g_warmup = true;
	g_mapKills.clear();
	g_emitted.clear();
	FireGameEvent("player_death", &death);
	Check(g_mapKills.empty() && g_emitted.empty(), "a warmup death is ignored");
	Check(g_schemaCalls > 0, "the warmup field was read after the map opened");
	g_warmup = false;
	FireGameEvent("player_death", &death);
	CheckEqInt((int64_t)g_mapKills[1], 1, "a non-warmup death is processed");
	StopPlugin();
}

void TestMapEndUnload()
{
	Section("map end: no client/entity/schema call, state dropped, menus destroyed");
	ResetWorld();
	StartPlugin();
	OpenMap();
	AddHuman(1, 111, 2, "Alice");
	AddHuman(2, 222, 3, "Bob");
	g_bombEntityPresent = true;
	g_bombHandle = -123456;
	g_gameTime = 100.0f;
	g_bombBlow = 110.0f;
	FakeEvent planted;
	FireGameEvent("bomb_planted", &planted);
	RunCommand("qs", 1, s2sdk::ConCommandContext::Chat, {"qs"});
	Check(MenuOfSlot(1) != nullptr, "menu open before the map end");
	g_mapKills[1] = 5;

	ResetCallCounters();
	FireMapEnd();
	Check(!g_mapReady, "the gate is shut");
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "OnMapEnd made no client/entity/schema call");
	Check(g_mapKills.empty(), "the streaks were cleared");
	Check(!g_bBombActive, "the bomb countdown was stopped");
	Check(g_clientMenus.empty(), "the menu was cleared");
	Check(g_vActions.empty(), "the queued actions were dropped");

	// events during the changelevel window are no-ops
	ResetCallCounters();
	FakeEvent death = MakeDeathEvent(2, 1, "ak47", false);
	Check(FireGameEvent("player_death", &death) == s2sdk::ResultType::Continue, "death Continue");
	FakeEvent roundEnd;
	Check(FireGameEvent("round_end", &roundEnd) == s2sdk::ResultType::Continue, "round_end Continue");
	FakeEvent defused;
	Check(FireGameEvent("bomb_defused", &defused) == s2sdk::ResultType::Continue, "bomb Continue");
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "no call while the gate is shut");

	Section("unload: every registration is removed and nothing runs afterwards");
	ResetCallCounters();
	StopPlugin();
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1, "map listeners removed");
	Check(g_unhookCalls == 8, "events unhooked");
	Check(g_fakeCommands.empty(), "commands removed");
	Check(g_manifestUnregisters == 1, "manifest callback removed");
	CheckEqInt(g_userMessageUnhooks, 1, "user-message hook removed");
	Check(!g_bStarted, "the plugin is marked stopped");
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "unload made no client/entity/schema call");

	ResetCallCounters();
	Update();
	Check(g_clientCalls == 0 && g_entityCalls == 0 && g_schemaCalls == 0,
		  "OnPluginUpdate after OnPluginEnd is a no-op");

	Section("reload: a second start registers everything exactly once");
	ResetWorld();
	StartPlugin();
	OpenMap();
	StopPlugin();
	StartPlugin();
	OpenMap();
	CheckEqInt(g_mapStartRegisters, 2, "map listeners registered once per start");
	CheckEqInt(g_hookCalls, 16, "eight events per start, no duplicates");
	CheckEqInt((int64_t)g_fakeCommands.size(), 4, "four commands, no duplicates");
	CheckEqInt(g_manifestRegisters, 2, "manifest callback per start");
	CheckEqInt(g_userMessageHookId, 208, "user-message hook per start");
	CheckEqInt(g_timerCalls, 0, "still no s2sdk timer");
	StopPlugin();

	Section("fail soft: missing bindings only disable their feature");
	ResetWorld();
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	StartPlugin();
	Check(!g_mapListeners, "without map listeners the gate stays shut");
	Update();
	Check(!g_mapReady, "the gate stays shut without map listeners");
	StopPlugin();

	ResetWorld();
	g_throwMapEndRegister = true;
	StartPlugin();
	Check(!g_mapListeners, "a half-registered map-listener pair keeps the gate shut");
	Check(g_mapStartRegisters == 1 && g_mapStartUnregisters == 1,
		  "the already registered OnMapStart listener was removed again");
	CheckEqInt(g_mapEndRegisters, 0, "the OnMapEnd registration failed");
	Check(LoggedWith(plg::Severity::Error, "OnMapEnd listener"),
		  "the OnMapEnd registration failure is logged");
	StopPlugin();
	CheckEqInt(g_mapStartUnregisters, 1, "no double unregister on unload");

	ResetWorld();
	__s2sdk_HookEvent = nullptr;
	StartPlugin();
	Check(g_eventHooks.empty(), "no events without the HookEvent binding");
	Check(LoggedWith(plg::Severity::Error, "event bindings are unavailable"),
		  "the missing event binding is logged");
	StopPlugin();

	ResetWorld();
	__s2sdk_UnhookEvent = nullptr;
	StartPlugin();
	Check(g_eventHooks.empty(), "no events when UnhookEvent is missing");
	StopPlugin();

	ResetWorld();
	__s2sdk_UnhookUserMessage = nullptr;
	StartPlugin();
	Check(g_userMessageHookId == -1, "no user-message hook when UnhookUserMessage is missing");
	StopPlugin();

	// A7: AddConsoleCommand + RemoveCommand + RemoveCommandListener are all required together.
	// The ownership is unknown before Add (foreign name -> listener), so a single missing remover
	// must prevent registration instead of leaving a callback in the unloaded library.
	for (int missing = 0; missing < 3; ++missing)
	{
		ResetWorld();
		if (missing == 0) __s2sdk_AddConsoleCommand = nullptr;
		else if (missing == 1) __s2sdk_RemoveCommand = nullptr;
		else __s2sdk_RemoveCommandListener = nullptr;
		StartPlugin();
		CheckEqInt(g_addConsoleCommandCalls, 0, "no AddConsoleCommand call with a partial command binding set");
		Check(g_fakeCommands.empty(), "no command is registered with a partial command binding set");
		Check(LoggedWith(plg::Severity::Error, "command bindings are unavailable"),
			  "the partial command binding set is logged");
		StopPlugin();
		Check(g_fakeCommands.empty(), "no command callback is left with a partial command binding set");
	}
}

} // namespace

int main()
{
	if (!std::filesystem::exists(QUAKE_SOUNDS_TEST_CONFIG) ||
		!std::filesystem::exists(QUAKE_SOUNDS_TEST_PHRASES))
	{
		std::printf("FATAL: test config/phrases not found (%s, %s)\n",
					QUAKE_SOUNDS_TEST_CONFIG, QUAKE_SOUNDS_TEST_PHRASES);
		return 1;
	}
	const std::filesystem::path base =
		std::filesystem::temp_directory_path() /
		("quake_sounds_plugin_tests_" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code error;
	std::filesystem::remove_all(base, error);
	std::filesystem::create_directories(base / "configs/quake_sounds/translations", error);
	std::filesystem::create_directories(base / "data", error);
	std::filesystem::create_directories(base / "cwd/addons/configs", error);
	std::filesystem::create_directories(base / "cwd/addons/translations", error);
	g_baseDir = base.string();
	g_configsDir = (base / "configs").string();
	g_dataDir = (base / "data").string();
	g_cwdDir = (base / "cwd").string();
	g_originalCwd = std::filesystem::current_path(error);

	TestStartupAndParity();
	TestMapGate();
	TestKillsAndStreaks();
	TestSpecialsAndWorld();
	TestBotsAndSlotReuse();
	TestCookiesAndAnnounce();
	TestSoundVolumeMessage();
	TestSoundVolumeSafety();
	TestMenusAndCommands();
	TestMenuTypeChoice();
	TestMenuIdentityAndFailSafe();
	TestConfigPhrases();
	TestBombAndWarmup();
	TestMapEndUnload();

	std::filesystem::current_path(g_originalCwd, error);
	std::filesystem::remove_all(base, error);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
