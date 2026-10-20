#include "stdafx.h"

#include "MirvPov.h"

#include "ClientEntitySystem.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/AfxDetours.h"
#include "../shared/binutils.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/convar.h"
#include "../deps/release/prop/cs2/sdk_src/public/icvar.h"

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

#include "../deps/release/Detours/src/detours.h"

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <algorithm>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

/*
	mirv_pov: watch a CS2 demo as if you were the player (fake POV), by making the player the
	client's local player controller. See doc/notes_cs2/mirv_pov.md (usage) and
	doc/notes_cs2/mirv_pov_spike.md (how it was found / what each hook is for).

	client.dll (CS2 1.41.9.1 RVAs only as orientation):

	CBasePlayerController local player setter (0x933de0), only caller is the
	controller's data update (vtable of CBasePlayerController sub-object, index 11):

	  this = controller, edx = split screen slot (0)
	  ebx = engine->vtable[0x110](&tmp, slot)        // engine's local player slot
	  if (g_KillCamObsMode != 0) {                   // set by CCSUsrMsg_KillCam handler
	      ebx = g_KillCamTarget - 1;                 // killcam target becomes local player!
	  }
	  if (ebx == entindex(this) - 1) {
	      g_LocalPlayerControllers[slot] = this;     // read by GetSplitScreenPlayer(0)
	      this->m_bIsLocalPlayerController = true;
	      ... notifications ...
	  }

	The setter never clears m_bIsLocalPlayerController of the previous local
	controller, so we do that ourselves.

	On top of that swap the client is told it's not HLTV (CEngineClient::IsHLTV), and a few
	local-player-only paths are fixed up: demo seeks, crosshair, buy menu blur, scoreboard,
	prediction and the death card.
*/

namespace {

typedef void (__fastcall * SetAsLocalPlayerController_t)(void * pController, int nSlot);
typedef int * (__fastcall * EngineGetLocalPlayer_t)(void * pEngine, int * pOut, int nSlot);
typedef void * (__fastcall * DemoPlayer_SkipToTick_t)(void * This, int nTick, unsigned int nFlags);
typedef bool (__fastcall * EngineClient_IsHLTV_t)(void * This);
typedef void (__fastcall * ApplyOwnCrosshair_t)(void * pSettings);
typedef bool (__fastcall * ApplyCrosshairCode_t)(void * pSettings, const char * pszCode);
typedef const char * (__fastcall * GetCrosshairCode_t)(void * pController);
typedef void (__fastcall * HudChatDelegate_SayText2_t)(void * This, void * pMsg);
typedef void (__fastcall * SetPredictable_t)(void * pEntity, bool bPredictable);
typedef void * (__fastcall * GetSplitScreenPlayerPawn_t)(int nSlot);
typedef void * (__fastcall * FindHudElement_t)(const char * pszName);
typedef void (__fastcall * DeathPanel_SetPendingDamage_t)(void * pPanel, int a, int b, int c, int d, int e, int f);

// The death card is shown by the CCSUsrMsg_SendLastKillerDamageToClient handler, which calls
// CCSGO_HudDeathPanel's "set pending damage + show". That message isn't in GOTV demos, so we
// call it ourselves after the POV target's player_death (damage numbers 0 for now).
FindHudElement_t g_FindHudElement = nullptr;
DeathPanel_SetPendingDamage_t g_DeathPanel_SetPendingDamage = nullptr;

// Death card: the death panel's player_death handler only shows if
// event->GetPlayerPawn("userid") == GetSplitScreenPlayerPawn(0). The getter resolves the
// controller's m_hPawn, which at that point is already the observer pawn, so for the swapped
// target we return its m_hPlayerPawn at that one call site.
// Same for the panel's Show (bails unless the local pawn is a player pawn) and its per frame
// update (reads the death time for the slide-in).
GetSplitScreenPlayerPawn_t g_Org_GetSplitScreenPlayerPawn = nullptr;
enum DeathPanelSite_e { DeathPanelSite_Event = 0, DeathPanelSite_Show, DeathPanelSite_Think, DeathPanelSite_Count };
size_t g_DeathPanelPawnRetAddrs[DeathPanelSite_Count] = {};
bool g_PovDeathCard = true;

// Damage direction arcs: the CCSUsrMsg_Damage handler (CCSGO_HudDamageIndicator) checks the local
// pawn is alive and calls ShowDirection(indicator, &inflictorWorldPos, localPawn), which raises the
// arc intensities. The message isn't in GOTV demos, so we call it on the target's player_hurt, with
// the attacker's position as the inflictor.
typedef void (__fastcall * DamageIndicator_ShowDirection_t)(void * pIndicator, const float * pInflictorPos, void * pPawn);
DamageIndicator_ShowDirection_t g_DamageIndicator_ShowDirection = nullptr;
bool g_PovDamageArcs = true;

// Scope blur: the scope overlay's blur comes from CCSPlayer_CameraServices::m_vClientScopeInaccuracy
// (client only, interpolated), set from the active weapon's inaccuracy and spread by an update
// function. C_CSPlayerPawn calls it every frame for non-local pawns (vtable[146]) and for the local
// pawn from another path (vtable[321]) that doesn't run in demos, so in POV it froze at the swap.
// We call it for the target ourselves.
typedef void (__fastcall * UpdateScopeInaccuracy_t)(void * pPawn);
UpdateScopeInaccuracy_t g_UpdateScopeInaccuracy = nullptr;
int g_UpdateScopeInaccuracyCameraServicesOfs = 0; // checked against the schema on first use (not resolved yet at hook time)
bool g_PovScopeBlur = true;

// Chat lines from player-only messages (CCSUsrMsg_RadioText, CUserMessageTextMsg) that GOTV demos
// don't have: grenade radio ("[CT] Name﹫Place: Flashbang!") from teammates' weapon_fire, and the
// kill reward (" +$300 for neutralizing an enemy with the AK-47") from the target's kills plus their
// money change. Built from the same localization tokens and printed through the same chat print
// function (filter {0, 1}, client = player slot or -1) as the native handlers.
typedef void (__cdecl * ChatPrintf_t)(void * pChat, unsigned char * pFilter, int nClient, const char * pszFormat, ...);
typedef const char * (__fastcall * Localize_Find_t)(void * pLocalize, const char * pszToken);
ChatPrintf_t g_ChatPrintf = nullptr;
void ** g_ppLocalize = nullptr;
int g_LocalizeFindIndex = -1;
bool g_PovChatRadio = true;
bool g_PovChatMoney = true;

// The radio handler plays "HudChat.Message" after the line unless cl_clutch_mode is on:
// EmitSound(-1, &params) with params { name, ..., volume 1.0 (+0x14), -1 (+0x1c), pitch 100 (+0x28), flags (+0x2a) }.
typedef void (__fastcall * EmitSound_t)(int nEntity, void * pParams);
EmitSound_t g_EmitSound = nullptr;
unsigned char ** g_ppClutchModeCvar = nullptr; // cl_clutch_mode
ptrdiff_t g_OfsClutchModeValue = 0;

// Radio voice lines: GOTV gets every agent VO line (sas.ct_flashbang02, ...) as a sound event,
// players only get their own team's. CMsgSosStartSoundEvent from a player pawn without packed params
// (position) is agent VO (all other pawn sounds have a position). We detour the message's parse
// function and clear the sound event hash of enemy VO, so nothing plays. Parsing may not happen on
// the main thread, so it only reads g_MuteVoPawn, which the main thread fills each frame.
typedef const char * (__fastcall * SosStartSoundEvent_Parse_t)(void * pMsg, const char * ptr, void * pCtx);
SosStartSoundEvent_Parse_t g_Org_SosStartSoundEvent_Parse = nullptr;
ptrdiff_t g_OfsSosHash = 0;
ptrdiff_t g_OfsSosSourceEntity = 0;
const ptrdiff_t c_OfsSosHasBits = 0x10;
const unsigned int c_SosHasPackedParams = 1;
const unsigned int c_SosHasHash = 4;
const unsigned int c_SosHasSourceEntity = 0x20;
const int c_MuteVoMaxEntities = 16384;
volatile unsigned char g_MuteVoPawn[c_MuteVoMaxEntities] = {};
volatile bool g_MuteVoActive = false;
volatile long g_MuteVoCount = 0; // diagnostics
bool g_PovMuteEnemyRadio = true;

// C4 explosion screen shake (grenades have none, their shake amplitude is 0): the server sends
// CUserMessageShake to players on the ground: UTIL_ScreenShake(bomb, amplitude 3, frequency 150,
// duration 0.5, radius 10000, SHAKE_START), amplitude falling off linearly to 0 at the radius.
// Client side that ends in CViewEffects::Shake(ScreenShake_t *) (per split screen slot, vtable[4]).
typedef void (__fastcall * ViewEffects_Shake_t)(void * pViewEffects, void * pShake);
unsigned char * g_pViewEffects = nullptr; // slot 0
int g_ViewEffectsShakeIndex = -1;
bool g_PovBombShake = true;
const float c_BombShakeAmplitude = 3.0f;
const float c_BombShakeFrequency = 150.0f;
const float c_BombShakeDuration = 0.5f;
const float c_BombShakeRadius = 10000.0f;

// Ear ringing: the server plays Flashbang.Ring.* to that player only, together with a
// CUserMessageAudioParameter that triggers the matching "control.deafen*" mix (muffling): for
// blinded players by the distance between the flash and their eyes (< 100 Long, < 500 Medium,
// < 1000 Short), and Long / control.deafenHE for HE damage >= 30. The client's handler for that
// parameter type calls the sound system's vtable[0x1b8 / 8] with the hashed control name.
typedef void (__fastcall * SoundSystem_TriggerControl_t)(void * pSoundSystem, unsigned int nameHash);
void ** g_ppSoundSystem = nullptr;
int g_SoundSystemTriggerControlIndex = -1;
bool g_PovEarRinging = true;
float g_LastFlashPos[3] = {};
int g_LastFlashTick = -1;

// Spectator tools (x-ray: every player glows through walls in team colour, ...) are on when
//   force_spectator_only_tools || (local->m_bIsLocalPlayerController ? g_bIsHLTV : local->m_bIsHLTV) || <pawn / game mode checks>
// (local = GetSplitScreenPlayer(0)). Player glow isn't decided every frame but refreshed on events
// (player_death, spec_target_updated, ...). After a seek we make spec_target_updated fire while the
// swap is paused (GOTV again), so x-ray got latched on until the next death. So we return false
// whenever a POV target is set, also while paused. The original's inputs are kept for mirv_pov info.
typedef bool (__fastcall * SpectatorToolsEnabled_t)();
SpectatorToolsEnabled_t g_Org_SpectatorToolsEnabled = nullptr;
unsigned char ** g_ppForceSpectatorToolsCvar = nullptr;
ptrdiff_t g_OfsForceSpectatorToolsValue = 0;
ptrdiff_t g_OfsControllerIsHltv = 0; // CBasePlayerController::m_bIsHLTV
unsigned char * g_pCachedIsHltv = nullptr; // what FrameStageNotify cached from IsHLTV()
bool g_PovNoSpectatorTools = true;
volatile bool g_SpecToolsLastOriginal = false;
volatile long g_SpecToolsBlocked = 0;
bool __fastcall New_SpectatorToolsEnabled(); // after the killcam state

const char * __fastcall New_SosStartSoundEvent_Parse(void * pMsg, const char * ptr, void * pCtx) {
	const char * pResult = g_Org_SosStartSoundEvent_Parse(pMsg, ptr, pCtx);
	if (g_MuteVoActive && nullptr != pResult) {
		unsigned char * p = (unsigned char *)pMsg;
		unsigned int hasBits = *(unsigned int *)(p + c_OfsSosHasBits);
		if (0 == (hasBits & c_SosHasPackedParams) && 0 != (hasBits & c_SosHasSourceEntity)) {
			int source = *(int *)(p + g_OfsSosSourceEntity);
			if (0 <= source && source < c_MuteVoMaxEntities && g_MuteVoPawn[source]) {
				*(unsigned int *)(p + g_OfsSosHash) = 0;
				*(unsigned int *)(p + c_OfsSosHasBits) = hasBits & ~c_SosHasHash;
				InterlockedIncrement(&g_MuteVoCount);
			}
		}
	}
	return pResult;
}

// As local player the target's pawn (and its weapons / viewmodel) becomes predictable, so the
// client shows its own predicted copy of some values instead of the recorded ones. Without user
// input that copy freezes. C_BaseEntity::SetPredictable (vtable index 130) is detoured to keep
// those entities non-predictable while swapped. (Didn't fix the AWP scope blur, see notes.)
SetPredictable_t g_Org_SetPredictable = nullptr;
ptrdiff_t g_OfsPredictable = 0; // C_BaseEntity::m_bPredictable
bool g_PovNoPredict = true;

// Diagnostic: log CUserMessageSayText2 deliveries to the chat HUD (CHudChatDelegate vtable[12]).
HudChatDelegate_SayText2_t g_Org_HudChat_SayText2 = nullptr;
bool g_ChatLog = false;

struct ClientAddrs_t {
	bool Ok = false;
	SetAsLocalPlayerController_t SetAsLocalPlayerController = nullptr;
	void ** pLocalPlayerControllers = nullptr;
	int * pKillCamObsMode = nullptr;
	int * pKillCamTarget = nullptr;
	void ** ppEngine = nullptr;
	int EngineGetLocalPlayerVtableOfs = 0;
	ptrdiff_t OfsIsLocalPlayerControllerFromCode = 0;
	size_t ModuleBase = 0;
	size_t TextStart = 0;
	size_t TextEnd = 0;
	size_t FrameStageNotifyIsHltvRetAddr = 0; // CSource2Client::FrameStageNotify caches IsHLTV() for most of client.dll
	ApplyOwnCrosshair_t ApplyOwnCrosshair = nullptr; // trampoline after detouring
	ApplyCrosshairCode_t ApplyCrosshairCode = nullptr;
	GetCrosshairCode_t GetCrosshairCode = nullptr;
	// CCSObserver_ObserverServices vtable[47] fires "spec_target_updated" (which the demo UI
	// uses, e.g. for the kill / death markers) when m_hObserverTarget differs from this cached handle:
	ptrdiff_t OfsObserverLastTarget = 0;
	ptrdiff_t OfsObserverTargetFromCode = 0;
	// Scoreboard: "if (IsPlayingDemo() && !g_bIsHLTV_cached) skip" in the scoreboard update. While we
	// spoof IsHLTV that blocks TAB, so we NOP this one 6 byte je while the swap is active.
	unsigned char * ScoreboardHltvJe = nullptr;
	unsigned char ScoreboardHltvJeOrg[6] = {};
	bool ScoreboardPatched = false;
} g_Client;

// Use the target's crosshair code instead of the viewer's cl_crosshair* settings while swapped.
bool g_PovCrosshair = true;
// Diagnostic: result of the last target crosshair attempt.
enum PovCrosshairResult_e { PovXh_None = 0, PovXh_Applied, PovXh_NoController, PovXh_EmptyCode, PovXh_ApplyFailed, PovXh_Exception };
int g_PovCrosshairLastResult = PovXh_None;
// GetCrosshairCode returns "" if this byte is set (same flag also gates demo chat / scoreboard):
unsigned char * g_pCrosshairHideFlag = nullptr;

// The recorded C_CSPlayerPawn::m_bIsBuyMenuOpen makes the client blur the view (and hide
// e.g. the radar) for the local player, without the menu itself. Clear it while swapped.
bool g_PovSuppressBuyMenu = true;

// Round end damage report (under the team counter avatars). The client builds it from the local
// controller's CCSPlayerController_DamageServices::m_DamageList (CDamageRecord, 0x78 bytes each),
// which isn't in GOTV demos. We collect the same numbers from player_hurt / player_death and when
// the round is won (C_CSGameRules::m_iRoundWinStatus becomes non-0, there's no round_end event in
// demos) run the native reporter on them, by pointing the target's list at our records meanwhile.
typedef void (__fastcall * TeamCounter_DamageReport_t)(void * pTeamCounter);
struct DamageReportAddrs_t {
	TeamCounter_DamageReport_t Report = nullptr;
	FindHudElement_t FindHudElement = nullptr;
	void ** ppGameRules = nullptr;
	unsigned char ** ppUseNetvarsCvar = nullptr; // sv_dev_damage_use_netvars (reporter only runs if true)
	ptrdiff_t OfsCvarValue = 0;
	ptrdiff_t OfsRoundWinStatus = 0;
	ptrdiff_t OfsDamageServices = 0; // CCSPlayerController::m_pDamageServices
	ptrdiff_t OfsListCount = 0; // m_DamageList size
	ptrdiff_t OfsListMem = 0; // m_DamageList memory
} g_DmgReport;
bool g_PovDamageReport = true;

// CDamageRecord fields the reporter reads:
const int c_DmgRecordSize = 0x78;
const int c_DmgRecordAttacker = 0x38; // controller handle
const int c_DmgRecordVictim = 0x3c; // controller handle
const int c_DmgRecordHealthRemoved = 0x68; // float, m_flActualHealthRemoved
const int c_DmgRecordHits = 0x6c; // int
const int c_DmgRecordKillType = 0x75; // byte, see DmgKillType_e
// Index into the client's "HUD--killtype-*" table, 0 = not killed.
enum DmgKillType_e { DmgKillType_None = 0, DmgKillType_Default, DmgKillType_Headshot, DmgKillType_Blast, DmgKillType_Burn, DmgKillType_Slash, DmgKillType_Shock };

const int c_DmgMaxPlayers = 65; // controller entity indices 1..64
// Every player_hurt / player_death seen, tagged with its round, so after seeking back into a
// round that was played before the report is still complete. Replays are dropped by tick.
struct DmgEvent_t {
	int Tick;
	int Round; // see DamageReport_GetRound
	unsigned char Attacker; // controller entity index, 0 = none / world
	unsigned char Victim;
	bool Death;
	unsigned char KillType;
	int Health; // player_hurt: health left
	int DmgHealth;
};
std::vector<DmgEvent_t> g_DmgEvents;
struct DmgPair_t { float HealthRemoved; int Hits; unsigned char KillType; bool Used; };
DmgPair_t g_DmgPairs[c_DmgMaxPlayers][c_DmgMaxPlayers]; // [attacker][victim], built from g_DmgEvents for one round
const int c_DmgMaxRecords = 2 * c_DmgMaxPlayers;
alignas(16) unsigned char g_DmgRecords[c_DmgMaxRecords][c_DmgRecordSize];
int g_DmgLastWinStatus = -1; // -1 = unknown (after a reset, don't trigger on the first value)
// The win status can arrive before the round's last player_death was fired, so wait a little:
const int c_DmgPublishDelayFrames = 8;
int g_DmgPublishInFrames = 0;
int g_DmgLastResult = 0; // diagnostics, see DamageReport_Publish

// Without C_CSGameRules::m_totalRoundsPlayed all events are round 0 and we clear at round start.
bool DamageReport_HasRounds() {
	return 0 != g_clientDllOffsets.C_CSGameRules.m_totalRoundsPlayed;
}

// Seeks: don't trigger on the win status we land on.
void DamageReport_Reset(bool bClearEvents) {
	if (bClearEvents || !DamageReport_HasRounds()) g_DmgEvents.clear();
	g_DmgLastWinStatus = -1;
	g_DmgPublishInFrames = 0;
}

// demo UI spec target: the client fires "spec_target_updated" with the (real, GOTV) local player's
// observer target. After a seek we spec_player the target, but a dead player can't be spectated, so
// the demo UI shows (and puts markers for) someone else. We set the event's "target" to the
// target's pawn instead, with IGameEvent::SetEntity (vtable index taken from the client's sender).
typedef void (__fastcall * GameEvent_SetEntity_t)(void * pEvent, const SOURCESDK::CS2::CKV3MemberName * pKey, void * pEntity);
int g_GameEventSetEntityIndex = -1;
bool g_PovSpecTarget = true;

struct EngineAddrs_t {
	DemoPlayer_SkipToTick_t SkipToTick = nullptr; // trampoline after detouring
	EngineClient_IsHLTV_t IsHLTV = nullptr; // trampoline after detouring
} g_Engine;

// IsHLTV() spoofing while the killcam swap is active:
// 0 = off, 1 = all client.dll callers (default), 2 = only the FrameStageNotify cache (most of client.dll reads that).
int g_HltvSpoofMode = 1;
bool g_HltvLog = false;
const int c_HltvLogMax = 128;
struct HltvLogEntry_t { size_t RetAddr; unsigned int Count; bool LastResult; bool LastSpoofed; } g_HltvLogEntries[c_HltvLogMax];
volatile long g_HltvLogCount = 0;

// killcam method state
int g_KillCamTarget = 0;
int g_KillCamObsMode = 2; // OBS_MODE_IN_EYE
void * g_KillCamLastSwapped = nullptr;
bool g_KillCamFaulted = false;

// Demo seeks / level loads rebuild entities. Doing that while the target is the local
// controller crashes in local player only camera code (CCSPlayer_CameraServices,
// seen in client.dll+0xb30b36), so we restore before and re-apply afterwards.
// While paused we also make the real (GOTV) local player spectate the target with
// spec_player, so the demo UI (kill / death markers) follows the target too.
const int c_KillCamResumeDelayFrames = 64; // fallback if we can't tell when a seek is done
const int c_KillCamSpecFrames = 8; // pause before swapping, so spec_player can apply
const int c_KillCamSeekTailFrames = 4; // after the seek target tick was reached
int g_KillCamSuspendFrames = 0;
int g_KillCamSeekTargetTick = -1;

bool __fastcall New_SpectatorToolsEnabled() {
	bool bResult = g_Org_SpectatorToolsEnabled();
	g_SpecToolsLastOriginal = bResult;
	if (bResult && g_PovNoSpectatorTools && 0 != g_KillCamTarget) {
		InterlockedIncrement(&g_SpecToolsBlocked);
		return false;
	}
	return bResult;
}

int32_t ReadRel32(size_t addr) { return *(int32_t *)addr; }

bool BytesEqual(size_t addr, const unsigned char * bytes, size_t count) {
	return 0 == memcmp((const void *)addr, bytes, count);
}

CEntityInstance * GetEntity(int index) {
	if (nullptr == g_GetEntityFromIndex || nullptr == g_pEntityList || nullptr == *g_pEntityList) return nullptr;
	return (CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, index);
}

int GetEntityIndex(void * pEntity) {
	if (nullptr == pEntity) return -1;
	return ((CEntityInstance *)pEntity)->GetHandle().GetEntryIndex();
}

// Schema offsets are only available after client.dll was initialized, so this is resolved on use.
ptrdiff_t GetOfsIsLocalPlayerController() {
	ptrdiff_t ofsFromSchema = g_clientDllOffsets.CBasePlayerController.m_bIsLocalPlayerController;
	return 0 != ofsFromSchema ? ofsFromSchema : g_Client.OfsIsLocalPlayerControllerFromCode;
}

bool * IsLocalPlayerControllerPtr(void * pController) {
	return (bool *)((unsigned char *)pController + GetOfsIsLocalPlayerController());
}

const char * SafeName(const char * name) {
	return nullptr != name ? name : "(null)";
}

void * GetLocalPlayerController() {
	return g_Client.pLocalPlayerControllers[0];
}

// Must not contain objects with destructors (SEH).
int GetEngineLocalPlayerSlot() {
	__try {
		void * pEngine = *g_Client.ppEngine;
		if (nullptr == pEngine) return -2;
		EngineGetLocalPlayer_t fn = (EngineGetLocalPlayer_t)((*(void ***)pEngine)[g_Client.EngineGetLocalPlayerVtableOfs / sizeof(void *)]);
		int tmp = -1;
		return *fn(pEngine, &tmp, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -3;
	}
}

// Must not contain objects with destructors (SEH).
bool KillCam_Swap(void * pTarget) {
	__try {
		*g_Client.pKillCamObsMode = g_KillCamObsMode;
		*g_Client.pKillCamTarget = g_KillCamTarget;

		void * pPrev = GetLocalPlayerController();
		if (pPrev != pTarget) {
			g_Client.SetAsLocalPlayerController(pTarget, 0);
			if (nullptr != pPrev && pPrev != GetLocalPlayerController()) *IsLocalPlayerControllerPtr(pPrev) = false;
		}
		return GetLocalPlayerController() == pTarget;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_KillCamFaulted = true;
		return false;
	}
}

// Must not contain objects with destructors (SEH).
bool KillCam_Restore() {
	__try {
		*g_Client.pKillCamObsMode = 0;
		*g_Client.pKillCamTarget = 0;

		void * pPrev = GetLocalPlayerController();

		// Let the game's own check pick the real local controller again. Skip the previous
		// (swapped) one: it's still local, so it would look like a match.
		int highest = GetHighestEntityIndex();
		for (int i = 0; i <= highest; ++i) {
			CEntityInstance * pEnt = GetEntity(i);
			if (nullptr == pEnt || pEnt == pPrev || !pEnt->IsPlayerController()) continue;
			g_Client.SetAsLocalPlayerController(pEnt, 0);
			if (pEnt == GetLocalPlayerController()) break;
		}

		if (nullptr != pPrev && pPrev != GetLocalPlayerController()) *IsLocalPlayerControllerPtr(pPrev) = false;
		return pPrev != GetLocalPlayerController() || nullptr == pPrev;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_KillCamFaulted = true;
		return false;
	}
}

int GetDemoTick() {
	if (nullptr == g_pEngineToClient) return -1;
	SOURCESDK::CS2::IDemoFile * pDemoFile = g_pEngineToClient->GetDemoFile();
	return nullptr != pDemoFile ? pDemoFile->GetDemoTick() : -1;
}

void KillCam_Suspend(const char * reason, bool bRestore, int nFrames, int nSeekTargetTick = -1) {
	if (!g_Client.Ok || 0 == g_KillCamTarget) return;
	if (bRestore && nullptr != g_KillCamLastSwapped) KillCam_Restore();
	else {
		*g_Client.pKillCamObsMode = 0;
		*g_Client.pKillCamTarget = 0;
	}
	g_KillCamLastSwapped = nullptr;
	if (0 == g_KillCamSuspendFrames && nullptr != reason) advancedfx::Message("mirv_pov: %s, pausing swap.\n", reason);
	g_KillCamSuspendFrames = nFrames;
	g_KillCamSeekTargetTick = nSeekTargetTick;
}

// Make the game re-fire "spec_target_updated" for the real local player's observer target,
// so the demo UI picks up a target switched while we were swapped.
// Must not contain objects with destructors (SEH).
void InvalidateSpecTargetCache() {
	if (0 == g_Client.OfsObserverLastTarget
		|| 0 == g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices
		|| g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget != g_Client.OfsObserverTargetFromCode) return;
	__try {
		CEntityInstance * pController = (CEntityInstance *)GetLocalPlayerController();
		if (nullptr == pController) return;
		int pawnIndex = pController->GetPlayerPawnHandle().GetEntryIndex();
		if (pawnIndex < 0) return;
		CEntityInstance * pPawn = GetEntity(pawnIndex);
		if (nullptr == pPawn || !pPawn->IsPlayerPawn()) return;
		unsigned char * pServices = *(unsigned char **)((unsigned char *)pPawn + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
		if (nullptr == pServices) return;
		*(uint32_t *)(pServices + g_Client.OfsObserverLastTarget) = 0xffffffff; // invalid handle
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_Client.OfsObserverLastTarget = 0;
	}
}

// Called once per rendered frame while the swap is paused.
void KillCam_OnSuspendedFrame() {
	// A few frames before swapping (spec_player has applied by then):
	if (3 == g_KillCamSuspendFrames && 0 != g_KillCamTarget) InvalidateSpecTargetCache();

	if (0 != g_KillCamTarget && nullptr != g_pEngineToClient) {
		// The real (GOTV) local player is local again, so spec_player applies to it:
		char cmd[32];
		_snprintf_s(cmd, _TRUNCATE, "spec_player %i", g_KillCamTarget);
		g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
	}

	if (0 <= g_KillCamSeekTargetTick && c_KillCamSeekTailFrames < g_KillCamSuspendFrames) {
		int tick = GetDemoTick();
		if (0 <= tick && abs(tick - g_KillCamSeekTargetTick) <= 2) g_KillCamSuspendFrames = c_KillCamSeekTailFrames;
	}
}

void KillCam_Apply(bool bVerbose) {
	if (!g_Client.Ok || 0 == g_KillCamTarget || g_KillCamFaulted || 0 < g_KillCamSuspendFrames) return;

	CEntityInstance * pTarget = GetEntity(g_KillCamTarget);
	if (nullptr == pTarget || !pTarget->IsPlayerController()) {
		// Target not (yet) there, e.g. before the player connected: keep the game's state.
		if (bVerbose) advancedfx::Warning("mirv_pov: entity %i is not a player controller (yet).\n", g_KillCamTarget);
		if (*g_Client.pKillCamTarget == g_KillCamTarget) KillCam_Restore();
		g_KillCamLastSwapped = nullptr;
		return;
	}

	bool bResult = KillCam_Swap(pTarget);

	if (g_KillCamFaulted) {
		advancedfx::Warning("mirv_pov: exception during killcam swap, disabling killcam method until restart.\n");
		return;
	}

	if (bVerbose || g_KillCamLastSwapped != pTarget) {
		g_KillCamLastSwapped = pTarget;
		if (bResult) advancedfx::Message("mirv_pov: local player controller is now %i (%s).\n", g_KillCamTarget, SafeName(pTarget->GetSanitizedPlayerName()));
		else advancedfx::Warning("mirv_pov: game did not accept %i as local player controller.\n", g_KillCamTarget);
	}
}

void * __fastcall New_DemoPlayer_SkipToTick(void * This, int nTick, unsigned int nFlags) {
	DamageReport_Reset(false);
	if (0 != g_KillCamTarget) {
		// Same units as IDemoFile::GetDemoTick (flag 1 = relative). -2 = random tick, unknown target.
		int targetTick = -1;
		if (0 <= nTick || (nFlags & 1)) {
			targetTick = (nFlags & 1) ? GetDemoTick() + nTick : nTick;
			if (targetTick < 0) targetTick = 0;
		}
		KillCam_Suspend("demo seek", true, c_KillCamResumeDelayFrames, targetTick);
	}
	return g_Engine.SkipToTick(This, nTick, nFlags);
}

void HltvLog_Record(size_t retAddr, bool result, bool spoofed) {
	long count = g_HltvLogCount;
	for (long i = 0; i < count && i < c_HltvLogMax; ++i) {
		if (g_HltvLogEntries[i].RetAddr == retAddr) {
			++g_HltvLogEntries[i].Count;
			g_HltvLogEntries[i].LastResult = result;
			g_HltvLogEntries[i].LastSpoofed = spoofed;
			return;
		}
	}
	long index = InterlockedIncrement(&g_HltvLogCount) - 1;
	if (index < c_HltvLogMax) g_HltvLogEntries[index] = { retAddr, 1, result, spoofed };
}

bool __fastcall New_EngineClient_IsHLTV(void * This) {
	bool result = g_Engine.IsHLTV(This);

	size_t retAddr = (size_t)_ReturnAddress();
	bool bFromClient = g_Client.TextStart <= retAddr && retAddr < g_Client.TextEnd;
	bool bSpoof = false;

	if (result && bFromClient && 0 != g_HltvSpoofMode
		&& nullptr != g_KillCamLastSwapped && 0 == g_KillCamSuspendFrames) {
		bSpoof = 1 == g_HltvSpoofMode || retAddr == g_Client.FrameStageNotifyIsHltvRetAddr;
	}

	if (g_HltvLog && bFromClient) HltvLog_Record(retAddr, result, bSpoof);

	return bSpoof ? false : result;
}

void __fastcall New_HudChat_SayText2(void * This, void * pMsg) {
	if (g_ChatLog && nullptr != pMsg) {
		// CUserMessageSayText2: entityindex at +0x74, chat at +0x70 (see client.dll SayText2 handler).
		advancedfx::Message("mirv_pov chatlog: SayText2 entity %i chat %i, swapped %i, local controller %i\n",
			*(int *)((unsigned char *)pMsg + 0x74), *((unsigned char *)pMsg + 0x70) ? 1 : 0,
			nullptr != g_KillCamLastSwapped ? 1 : 0, GetEntityIndex(GetLocalPlayerController()));
	}
	g_Org_HudChat_SayText2(This, pMsg);
}

// Pawn entity index of the swapped target, -1 if none.
int GetSwappedTargetPawnIndex() {
	if (nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames) return -1;
	return ((CEntityInstance *)g_KillCamLastSwapped)->GetPlayerPawnHandle().GetEntryIndex();
}

// Must not contain objects with destructors (SEH).
bool IsTargetPawnOrOwned(void * pEntity, int pawnIndex) {
	if (nullptr == pEntity || pawnIndex < 0) return false;
	__try {
		if (GetEntityIndex(pEntity) == pawnIndex) return true;
		if (0 == g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity) return false;
		SOURCESDK::CS2::CBaseHandle owner(*(uint32_t *)((unsigned char *)pEntity + g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity));
		return owner.GetEntryIndex() == pawnIndex;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Must not contain objects with destructors (SEH).
void * GetSwappedTargetPlayerPawn() {
	if (0 == g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn || nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames) return nullptr;
	__try {
		SOURCESDK::CS2::CBaseHandle handle(*(uint32_t *)((unsigned char *)g_KillCamLastSwapped + g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn));
		int index = handle.GetEntryIndex();
		if (index < 0) return nullptr;
		CEntityInstance * pPawn = GetEntity(index);
		return nullptr != pPawn && pPawn->IsPlayerPawn() ? pPawn : nullptr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Diagnostic lines that should survive console spam: console + %TEMP%\mirv_pov_log.txt
// Only with mirv_pov_debug diag 1.
bool g_PovDiag = false;

void DiagLog(const char * fmt, ...) {
	if (!g_PovDiag) return;
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	advancedfx::Message("%s", buf);
	char path[MAX_PATH + 32] = {};
	DWORD len = GetTempPathA(MAX_PATH, path);
	if (0 == len || MAX_PATH <= len) return;
	strcat_s(path, "mirv_pov_log.txt");
	FILE * file = nullptr;
	if (0 == fopen_s(&file, path, "a") && nullptr != file) {
		fputs(buf, file);
		fclose(file);
	}
}

void * __fastcall New_GetSplitScreenPlayerPawn(int nSlot) {
	void * pResult = g_Org_GetSplitScreenPlayerPawn(nSlot);
	size_t retAddr = (size_t)_ReturnAddress();
	for (int site = 0; site < DeathPanelSite_Count; ++site) {
		if (0 == g_DeathPanelPawnRetAddrs[site] || retAddr != g_DeathPanelPawnRetAddrs[site]) continue;
		void * pPlayerPawn = GetSwappedTargetPlayerPawn();
		if (DeathPanelSite_Think != site) {
			DiagLog("mirv_pov: death panel %s: current pawn %i, target player pawn %i, swapped %i, fix %i\n",
				DeathPanelSite_Event == site ? "player_death" : "Show",
				GetEntityIndex(pResult), GetEntityIndex(pPlayerPawn), nullptr != g_KillCamLastSwapped ? 1 : 0, g_PovDeathCard ? 1 : 0);
		}
		if (g_PovDeathCard && nullptr != pPlayerPawn) return pPlayerPawn;
		break;
	}
	return pResult;
}

void __fastcall New_SetPredictable(void * pEntity, bool bPredictable) {
	if (bPredictable && g_PovNoPredict && IsTargetPawnOrOwned(pEntity, GetSwappedTargetPawnIndex())) bPredictable = false;
	g_Org_SetPredictable(pEntity, bPredictable);
}

// Called after the swap was (re-)applied: the game has already made them predictable by then.
// Must not contain objects with destructors (SEH).
void ClearTargetPredictable() {
	if (!g_PovNoPredict || nullptr == g_Org_SetPredictable || 0 == g_OfsPredictable) return;
	int pawnIndex = GetSwappedTargetPawnIndex();
	if (pawnIndex < 0) return;
	__try {
		int highest = GetHighestEntityIndex();
		for (int i = 0; i <= highest; ++i) {
			CEntityInstance * pEnt = GetEntity(i);
			if (nullptr == pEnt || !*((bool *)pEnt + g_OfsPredictable)) continue;
			if (IsTargetPawnOrOwned(pEnt, pawnIndex)) g_Org_SetPredictable(pEnt, false);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_PovNoPredict = false;
	}
}

void DamageReport_AddRecord(int & count, uint32_t attacker, uint32_t victim, const DmgPair_t & pair) {
	if (!pair.Used || c_DmgMaxRecords <= count) return;
	unsigned char * pRecord = g_DmgRecords[count++];
	memset(pRecord, 0, c_DmgRecordSize);
	*(uint32_t *)(pRecord + c_DmgRecordAttacker) = attacker;
	*(uint32_t *)(pRecord + c_DmgRecordVictim) = victim;
	*(float *)(pRecord + c_DmgRecordHealthRemoved) = pair.HealthRemoved;
	*(int *)(pRecord + c_DmgRecordHits) = pair.Hits;
	pRecord[c_DmgRecordKillType] = pair.KillType;
}

// Records of everything between target and the others, like the server's list.
int DamageReport_BuildRecords(int target) {
	if (target < 1 || c_DmgMaxPlayers <= target) return 0;
	CEntityInstance * pTarget = GetEntity(target);
	if (nullptr == pTarget) return 0;
	uint32_t targetHandle = (uint32_t)pTarget->GetHandle().ToInt();
	int count = 0;
	for (int other = 1; other < c_DmgMaxPlayers; ++other) {
		if (other == target) continue;
		CEntityInstance * pOther = GetEntity(other);
		if (nullptr == pOther || !pOther->IsPlayerController()) continue;
		uint32_t otherHandle = (uint32_t)pOther->GetHandle().ToInt();
		DamageReport_AddRecord(count, targetHandle, otherHandle, g_DmgPairs[target][other]);
		DamageReport_AddRecord(count, otherHandle, targetHandle, g_DmgPairs[other][target]);
	}
	return count;
}

// Must not contain objects with destructors (SEH).
// Returns the record count, 0 = nothing to show, -1 = failed, -2 = the demo has the list itself.
int DamageReport_CallReporter(void * pController, int count) {
	__try {
		void * pElement = g_DmgReport.FindHudElement("CCSGO_HudTeamCounter");
		unsigned char * pServices = *(unsigned char **)((unsigned char *)pController + g_DmgReport.OfsDamageServices);
		if (nullptr == pElement || nullptr == pServices) return -1;
		int * pCount = (int *)(pServices + g_DmgReport.OfsListCount);
		void ** ppMem = (void **)(pServices + g_DmgReport.OfsListMem);
		if (0 < *pCount) return -2; // e.g. a POV demo: the game shows it by itself
		int orgCount = *pCount;
		void * pOrgMem = *ppMem;
		*pCount = count;
		*ppMem = g_DmgRecords;
		__try {
			g_DmgReport.Report((unsigned char *)pElement - 0x20);
		}
		__finally {
			*pCount = orgCount;
			*ppMem = pOrgMem;
		}
		return count;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

// Must not contain objects with destructors (SEH).
bool DamageReport_GetRoundWinStatus(int & outStatus) {
	__try {
		unsigned char * pGameRules = (unsigned char *)*g_DmgReport.ppGameRules;
		if (nullptr == pGameRules) return false;
		outStatus = *(int *)(pGameRules + g_DmgReport.OfsRoundWinStatus);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Round an event belongs to: m_totalRoundsPlayed is incremented together with setting
// m_iRoundWinStatus, so after the round was won it's one too high. -1 = unknown.
// Must not contain objects with destructors (SEH).
int DamageReport_GetRound() {
	if (!DamageReport_HasRounds()) return 0;
	__try {
		unsigned char * pGameRules = (unsigned char *)*g_DmgReport.ppGameRules;
		if (nullptr == pGameRules) return -1;
		int played = *(int *)(pGameRules + g_clientDllOffsets.C_CSGameRules.m_totalRoundsPlayed);
		int winStatus = *(int *)(pGameRules + g_DmgReport.OfsRoundWinStatus);
		return 0 != winStatus ? played - 1 : played;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

bool DamageReport_IsValidPlayer(int index) { return 1 <= index && index < c_DmgMaxPlayers; }

// Fills g_DmgPairs from the events of that round, in tick order.
void DamageReport_BuildPairs(int round) {
	memset(g_DmgPairs, 0, sizeof(g_DmgPairs));
	std::vector<DmgEvent_t> events;
	for (const DmgEvent_t & ev : g_DmgEvents) if (ev.Round == round) events.push_back(ev);
	std::stable_sort(events.begin(), events.end(), [](const DmgEvent_t & a, const DmgEvent_t & b) { return a.Tick < b.Tick; });

	int health[c_DmgMaxPlayers]; // to get the actual health removed (dmg_health isn't capped at what was left)
	for (int i = 0; i < c_DmgMaxPlayers; ++i) health[i] = 100;

	for (const DmgEvent_t & ev : events) {
		int removed = 0;
		if (ev.Death) health[ev.Victim] = 0;
		else {
			removed = ev.DmgHealth;
			int fromHealth = health[ev.Victim] - ev.Health;
			if (0 <= fromHealth && fromHealth < removed) removed = fromHealth;
			if (removed < 0) removed = 0;
			health[ev.Victim] = ev.Health;
		}
		if (!DamageReport_IsValidPlayer(ev.Attacker) || ev.Attacker == ev.Victim) continue;
		DmgPair_t & pair = g_DmgPairs[ev.Attacker][ev.Victim];
		if (ev.Death) pair.KillType = ev.KillType;
		else if (0 < ev.DmgHealth) {
			pair.HealthRemoved += (float)removed;
			pair.Hits++;
		} else continue;
		pair.Used = true;
	}
}

void DamageReport_Publish() {
	g_DmgLastResult = 0;
	if (nullptr == g_DmgReport.Report || nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames) return;
	int round = DamageReport_GetRound();
	if (round < 0) return;
	DamageReport_BuildPairs(round);
	int count = DamageReport_BuildRecords(g_KillCamTarget);
	g_DmgLastResult = 0 < count ? DamageReport_CallReporter(g_KillCamLastSwapped, count) : 0;
	if (-1 == g_DmgLastResult) advancedfx::Warning("mirv_pov: showing the round end damage report failed.\n");
	else DiagLog("mirv_pov: round end damage report for %i, round %i: %i\n", g_KillCamTarget, round, g_DmgLastResult);
}

// Once per rendered frame (not while suspended).
void DamageReport_OnFrame() {
	if (nullptr == g_DmgReport.Report) return;
	int status = 0;
	if (!DamageReport_GetRoundWinStatus(status)) return;
	if (g_DmgLastWinStatus < 0) {
		g_DmgLastWinStatus = status;
	} else if (status != g_DmgLastWinStatus) {
		if (0 == g_DmgLastWinStatus) g_DmgPublishInFrames = c_DmgPublishDelayFrames; // round won
		else if (0 == status) { // next round
			if (!DamageReport_HasRounds()) g_DmgEvents.clear();
			g_DmgPublishInFrames = 0;
		}
		g_DmgLastWinStatus = status;
	}
	if (0 < g_DmgPublishInFrames && 0 == --g_DmgPublishInFrames && g_PovDamageReport) DamageReport_Publish();
}

unsigned char DamageReport_KillType(const char * pszWeapon, bool bHeadshot) {
	if (nullptr != pszWeapon) {
		if (0 == strncmp(pszWeapon, "weapon_", 7)) pszWeapon += 7;
		if (0 == strcmp(pszWeapon, "hegrenade") || 0 == strcmp(pszWeapon, "planted_c4")) return DmgKillType_Blast;
		if (0 == strcmp(pszWeapon, "inferno") || 0 == strcmp(pszWeapon, "molotov") || 0 == strcmp(pszWeapon, "incgrenade")) return DmgKillType_Burn;
		if (0 == strncmp(pszWeapon, "knife", 5) || 0 == strncmp(pszWeapon, "bayonet", 7)) return DmgKillType_Slash;
		if (0 == strcmp(pszWeapon, "taser")) return DmgKillType_Shock;
	}
	return bHeadshot ? DmgKillType_Headshot : DmgKillType_Default;
}

void DamageReport_AddEvent(int attacker, int victim, bool bDeath, unsigned char killType, int health, int dmgHealth) {
	if (!DamageReport_IsValidPlayer(victim)) return;
	DmgEvent_t ev;
	ev.Tick = GetDemoTick();
	ev.Round = DamageReport_GetRound();
	ev.Attacker = (unsigned char)(DamageReport_IsValidPlayer(attacker) ? attacker : 0);
	ev.Victim = (unsigned char)victim;
	ev.Death = bDeath;
	ev.KillType = killType;
	ev.Health = health;
	ev.DmgHealth = dmgHealth;
	if (ev.Round < 0) return;
	if (0 <= ev.Tick) {
		for (const DmgEvent_t & o : g_DmgEvents) {
			if (o.Tick == ev.Tick && o.Attacker == ev.Attacker && o.Victim == ev.Victim && o.Death == ev.Death
				&& o.Health == ev.Health && o.DmgHealth == ev.DmgHealth) return; // played again after seeking back
		}
	}
	g_DmgEvents.push_back(ev);
}

// Must not contain objects with destructors (SEH).
bool ApplyTargetCrosshair(void * pSettings) {
	__try {
		CEntityInstance * pController = GetEntity(g_KillCamTarget);
		if (nullptr == pController || !pController->IsPlayerController()) { g_PovCrosshairLastResult = PovXh_NoController; return false; }
		const char * pszCode = g_Client.GetCrosshairCode(pController);
		if (nullptr == pszCode || '\0' == *pszCode) { g_PovCrosshairLastResult = PovXh_EmptyCode; return false; }
		bool bResult = g_Client.ApplyCrosshairCode(pSettings, pszCode);
		g_PovCrosshairLastResult = bResult ? PovXh_Applied : PovXh_ApplyFailed;
		return bResult;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_PovCrosshair = false;
		g_PovCrosshairLastResult = PovXh_Exception;
		return false;
	}
}

// Called by the crosshair HUD when it's not showing an observed player's crosshair.
void __fastcall New_ApplyOwnCrosshair(void * pSettings) {
	if (g_PovCrosshair && nullptr != g_KillCamLastSwapped && 0 == g_KillCamSuspendFrames
		&& ApplyTargetCrosshair(pSettings)) return;

	g_Client.ApplyOwnCrosshair(pSettings);
}

void SetScoreboardPatch(bool bPatch) {
	if (nullptr == g_Client.ScoreboardHltvJe || bPatch == g_Client.ScoreboardPatched) return;
	DWORD oldProtect;
	if (!VirtualProtect(g_Client.ScoreboardHltvJe, sizeof(g_Client.ScoreboardHltvJeOrg), PAGE_EXECUTE_READWRITE, &oldProtect)) return;
	if (bPatch) memset(g_Client.ScoreboardHltvJe, 0x90, sizeof(g_Client.ScoreboardHltvJeOrg));
	else memcpy(g_Client.ScoreboardHltvJe, g_Client.ScoreboardHltvJeOrg, sizeof(g_Client.ScoreboardHltvJeOrg));
	VirtualProtect(g_Client.ScoreboardHltvJe, sizeof(g_Client.ScoreboardHltvJeOrg), oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), g_Client.ScoreboardHltvJe, sizeof(g_Client.ScoreboardHltvJeOrg));
	g_Client.ScoreboardPatched = bPatch;
}

// Must not contain objects with destructors (SEH).
void SuppressTargetBuyMenu() {
	ptrdiff_t ofs = g_clientDllOffsets.C_CSPlayerPawn.m_bIsBuyMenuOpen;
	if (0 == ofs) return;
	__try {
		CEntityInstance * pController = (CEntityInstance *)g_KillCamLastSwapped;
		int pawnIndex = pController->GetPlayerPawnHandle().GetEntryIndex();
		if (pawnIndex < 0) return;
		CEntityInstance * pPawn = GetEntity(pawnIndex);
		if (nullptr == pPawn || !pPawn->IsPlayerPawn()) return;
		bool * pIsBuyMenuOpen = (bool *)((unsigned char *)pPawn + ofs);
		if (*pIsBuyMenuOpen) *pIsBuyMenuOpen = false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_PovSuppressBuyMenu = false;
	}
}

void HltvLog_Print() {
	long count = g_HltvLogCount;
	if (count > c_HltvLogMax) count = c_HltvLogMax;
	advancedfx::Message("IsHLTV() callers (client.dll RVA : calls : last result : spoofed), %li entries:\n", count);
	for (long i = 0; i < count; ++i) {
		advancedfx::Message("  0x%llx : %u : %i : %i%s\n",
			(unsigned long long)(g_HltvLogEntries[i].RetAddr - g_Client.ModuleBase), g_HltvLogEntries[i].Count,
			g_HltvLogEntries[i].LastResult ? 1 : 0, g_HltvLogEntries[i].LastSpoofed ? 1 : 0,
			g_HltvLogEntries[i].RetAddr == g_Client.FrameStageNotifyIsHltvRetAddr ? "  <- FrameStageNotify cache" : "");
	}
}

// info output goes to the console and (if open) a text file, since the console can be
// flooded by game messages.
FILE * g_InfoFile = nullptr;

void InfoPrintf(const char * fmt, ...) {
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	advancedfx::Message("%s", buf);
	if (nullptr != g_InfoFile) fputs(buf, g_InfoFile);
}

void PrintInfo() {
	InfoPrintf("client.dll: %s", g_Client.Ok ? "OK" : "FAILED\n");
	if (g_Client.Ok) {
		InfoPrintf(
			" (setter +0x%llx, local controllers +0x%llx, killcam mode +0x%llx / target +0x%llx, m_bIsLocalPlayerController +0x%llx)\n",
			(unsigned long long)((size_t)g_Client.SetAsLocalPlayerController - g_Client.ModuleBase),
			(unsigned long long)((size_t)g_Client.pLocalPlayerControllers - g_Client.ModuleBase),
			(unsigned long long)((size_t)g_Client.pKillCamObsMode - g_Client.ModuleBase),
			(unsigned long long)((size_t)g_Client.pKillCamTarget - g_Client.ModuleBase),
			(unsigned long long)GetOfsIsLocalPlayerController());
		if (0 != g_clientDllOffsets.CBasePlayerController.m_bIsLocalPlayerController
			&& g_clientDllOffsets.CBasePlayerController.m_bIsLocalPlayerController != g_Client.OfsIsLocalPlayerControllerFromCode)
			InfoPrintf("WARNING: m_bIsLocalPlayerController schema offset differs from code (+0x%llx).\n", (unsigned long long)g_Client.OfsIsLocalPlayerControllerFromCode);
	}
	InfoPrintf("Target: %i%s%s | seek guard %s\n",
		g_KillCamTarget, g_KillCamFaulted ? " (FAULTED)" : "", 0 < g_KillCamSuspendFrames ? " (paused)" : "",
		nullptr != g_Engine.SkipToTick ? "OK" : "MISSING");
	InfoPrintf("IsHLTV hook: %s, spoof mode %i, logging %i | crosshair hook: %s, target crosshair %i\n",
		nullptr != g_Engine.IsHLTV ? "OK" : "MISSING", g_HltvSpoofMode, g_HltvLog ? 1 : 0,
		nullptr != g_Client.ApplyOwnCrosshair ? "OK" : "MISSING", g_PovCrosshair ? 1 : 0);
	InfoPrintf("Buy menu suppression: %i (m_bIsBuyMenuOpen +0x%llx) | scoreboard patch: %s, active %i | chat hook: %s\n", g_PovSuppressBuyMenu ? 1 : 0,
		(unsigned long long)g_clientDllOffsets.C_CSPlayerPawn.m_bIsBuyMenuOpen,
		nullptr != g_Client.ScoreboardHltvJe ? "OK" : "MISSING", g_Client.ScoreboardPatched ? 1 : 0,
		nullptr != g_Org_HudChat_SayText2 ? "OK" : "MISSING");

	{
		int winStatus = -1;
		int useNetvars = -1;
		if (nullptr != g_DmgReport.Report) {
			DamageReport_GetRoundWinStatus(winStatus);
			if (nullptr != *g_DmgReport.ppUseNetvarsCvar) useNetvars = *(*g_DmgReport.ppUseNetvarsCvar + g_DmgReport.OfsCvarValue) ? 1 : 0;
		}
		InfoPrintf("Scope blur update: %s, on %i\n", nullptr != g_UpdateScopeInaccuracy ? "OK" : "MISSING", g_PovScopeBlur ? 1 : 0);
		InfoPrintf("Damage arcs: hook %s, on %i | chat lines: print %s, localize %s, radio %i, kill reward %i (money +0x%llx/+0x%llx, place +0x%llx)\n",
			nullptr != g_DamageIndicator_ShowDirection ? "OK" : "MISSING", g_PovDamageArcs ? 1 : 0,
			nullptr != g_ChatPrintf ? "OK" : "MISSING", nullptr != g_ppLocalize ? "OK" : "MISSING", g_PovChatRadio ? 1 : 0, g_PovChatMoney ? 1 : 0,
			(unsigned long long)g_clientDllOffsets.CCSPlayerController.m_pInGameMoneyServices,
			(unsigned long long)g_clientDllOffsets.CCSPlayerController_InGameMoneyServices.m_iAccount,
			(unsigned long long)g_clientDllOffsets.C_CSPlayerPawn.m_szLastPlaceName);
		{
			int forceTools = -1, localIsLocal = -1, localIsHltv = -1, cachedHltv = -1;
			void * pLocal = GetLocalPlayerController();
			if (nullptr != g_ppForceSpectatorToolsCvar && nullptr != *g_ppForceSpectatorToolsCvar) forceTools = *(*g_ppForceSpectatorToolsCvar + g_OfsForceSpectatorToolsValue) ? 1 : 0;
			if (nullptr != pLocal) {
				localIsLocal = *IsLocalPlayerControllerPtr(pLocal) ? 1 : 0;
				if (0 != g_OfsControllerIsHltv) localIsHltv = *((unsigned char *)pLocal + g_OfsControllerIsHltv) ? 1 : 0;
			}
			if (nullptr != g_pCachedIsHltv) cachedHltv = *g_pCachedIsHltv ? 1 : 0;
			InfoPrintf("Spectator tools (x-ray): hook %s, blocking %i, original last %i, blocked so far %li | force_spectator_only_tools %i, local %i: m_bIsLocalPlayerController %i, m_bIsHLTV %i, cached IsHLTV %i\n",
				nullptr != g_Org_SpectatorToolsEnabled ? "OK" : "MISSING", g_PovNoSpectatorTools ? 1 : 0, g_SpecToolsLastOriginal ? 1 : 0, g_SpecToolsBlocked,
				forceTools, GetEntityIndex(pLocal), localIsLocal, localIsHltv, cachedHltv);
		}
		InfoPrintf("C4 shake: view effects %s, on %i (m_fFlags +0x%llx) | ear ringing: on %i, deafen control %s\n",
			nullptr != g_pViewEffects ? "OK" : "MISSING", g_PovBombShake ? 1 : 0, (unsigned long long)g_clientDllOffsets.C_BaseEntity.m_fFlags,
			g_PovEarRinging ? 1 : 0, nullptr != g_ppSoundSystem ? "OK" : "MISSING");
		InfoPrintf("Chat sound: %s | enemy radio mute: hook %s, on %i, active %i, muted so far %li (hash +0x%llx, source +0x%llx)\n",
			nullptr != g_EmitSound ? "OK" : "MISSING", nullptr != g_Org_SosStartSoundEvent_Parse ? "OK" : "MISSING",
			g_PovMuteEnemyRadio ? 1 : 0, g_MuteVoActive ? 1 : 0, g_MuteVoCount,
			(unsigned long long)g_OfsSosHash, (unsigned long long)g_OfsSosSourceEntity);
		InfoPrintf("Damage report: hook %s, on %i,sv_dev_damage_use_netvars %i, round win status %i, round %i, events %i, last result %i | spec target fix: %s, on %i\n",
			nullptr != g_DmgReport.Report ? "OK" : "MISSING", g_PovDamageReport ? 1 : 0, useNetvars, winStatus,
			nullptr != g_DmgReport.Report ? DamageReport_GetRound() : -1, (int)g_DmgEvents.size(), g_DmgLastResult,
			0 <= g_GameEventSetEntityIndex ? "OK" : "MISSING", g_PovSpecTarget ? 1 : 0);
	}

	if (!g_Client.Ok) return;

	InfoPrintf("Game: engine local slot=%i, killcam mode=%i target=%i, local controller=%i\n",
		GetEngineLocalPlayerSlot(), *g_Client.pKillCamObsMode, *g_Client.pKillCamTarget, GetEntityIndex(GetLocalPlayerController()));

	if (CEntityInstance * pLocal = (CEntityInstance *)GetLocalPlayerController()) {
		int pawnIndex = pLocal->GetPlayerPawnHandle().GetEntryIndex();
		CEntityInstance * pPawn = 0 <= pawnIndex ? GetEntity(pawnIndex) : nullptr;
		if (nullptr != pPawn && pPawn->IsPlayerPawn() && 0 != g_OfsPredictable)
			InfoPrintf("Prediction blocker: hook %s, blocking %i, local pawn m_bPredictable=%i (+0x%llx)\n",
				nullptr != g_Org_SetPredictable ? "OK" : "MISSING", g_PovNoPredict ? 1 : 0,
				*((bool *)pPawn + g_OfsPredictable) ? 1 : 0, (unsigned long long)g_OfsPredictable);
		if (nullptr != pPawn && pPawn->IsPlayerPawn())
			InfoPrintf("Local pawn %i: observer mode %i, observer target entity %i (spec_target_updated cache +0x%llx)\n",
				pawnIndex, pPawn->GetObserverMode(), pPawn->GetObserverTarget().GetEntryIndex(), (unsigned long long)g_Client.OfsObserverLastTarget);
	}

	{
		static const char * results[] = { "none yet", "applied target code", "no controller", "empty code (hidden or invalid)", "ApplyCrosshairCode failed", "exception" };
		InfoPrintf("Crosshair: hide flag %i, last target crosshair result: %s\n",
			nullptr != g_pCrosshairHideFlag ? (int)*g_pCrosshairHideFlag : -1,
			results[g_PovCrosshairLastResult < 6 ? g_PovCrosshairLastResult : 0]);
		int highestXh = GetHighestEntityIndex();
		for (int i = 0; i <= highestXh; ++i) {
			CEntityInstance * pEnt = GetEntity(i);
			if (nullptr == pEnt || !pEnt->IsPlayerController()) continue;
			const char * pszRaw = nullptr;
			if (0 != g_clientDllOffsets.CCSPlayerController.m_szCrosshairCodes)
				pszRaw = *(const char **)((unsigned char *)pEnt + g_clientDllOffsets.CCSPlayerController.m_szCrosshairCodes);
			const char * pszCode = nullptr != g_Client.GetCrosshairCode ? g_Client.GetCrosshairCode(pEnt) : nullptr;
			InfoPrintf("  %3i crosshair raw: \"%s\" GetCrosshairCode: \"%s\"\n", i, SafeName(pszRaw), SafeName(pszCode));
		}
	}

	InfoPrintf("Player controllers (index : name : m_bIsLocalPlayerController : pawn index):\n");
	int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance * pEnt = GetEntity(i);
		if (nullptr == pEnt || !pEnt->IsPlayerController()) continue;
		InfoPrintf("%3i : %s : %i : %i%s\n", i, SafeName(pEnt->GetSanitizedPlayerName()),
			*IsLocalPlayerControllerPtr(pEnt) ? 1 : 0,
			pEnt->GetPlayerPawnHandle().GetEntryIndex(),
			pEnt == GetLocalPlayerController() ? "  <- local" : "");
	}
}

} // namespace

void MirvPov_HookClientDll(HMODULE clientDll) {
	Afx::BinUtils::ImageSectionsReader sections(clientDll);
	if (sections.Eof()) return;
	Afx::BinUtils::MemRange textRange = sections.GetMemRange();

	g_Client.TextStart = textRange.Start;
	g_Client.TextEnd = textRange.End;

	// CSource2Client::FrameStageNotify: mov rcx, [engine]; mov rax, [rcx]; call [rax+IsHLTV]; add ebx, -3; mov [g_bIsHLTV], al
	{
		Afx::BinUtils::MemRange fsnResult = Afx::BinUtils::FindPatternString(textRange,
			"48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 83 C3 FD 88 05 ?? ?? ?? ??");
		if (!fsnResult.IsEmpty()) g_Client.FrameStageNotifyIsHltvRetAddr = fsnResult.Start + 0x10;
		else advancedfx::Warning("mirv_pov: FrameStageNotify IsHLTV cache site not found (hltv mode 2 won't work).\n");
	}

	// CCSObserver_ObserverServices vtable[47]: mov r8d, [rcx+lastTarget]; ... mov ecx, [rdi+m_hObserverTarget] at +0x6e
	{
		Afx::BinUtils::MemRange obsResult = Afx::BinUtils::FindPatternString(textRange,
			"48 89 6C 24 18 57 48 81 EC ?? ?? ?? ?? 44 8B 41 ?? 33 ED 48 8B 15");
		if (!obsResult.IsEmpty() && 0x8B == *(unsigned char *)(obsResult.Start + 0x6e) && 0x4F == *(unsigned char *)(obsResult.Start + 0x6f)) {
			g_Client.OfsObserverLastTarget = *(unsigned char *)(obsResult.Start + 0x10);
			g_Client.OfsObserverTargetFromCode = *(unsigned char *)(obsResult.Start + 0x70);
		} else advancedfx::Warning("mirv_pov: observer services spec_target_updated check not found.\n");
	}

	// Scoreboard update helper: push rbx; push rsi; ...; call [engine+IsPlayingDemo]; test al,al; je;
	// cmp byte ptr [g_bIsHLTV], 0; je <skip> (patched while swapped)
	{
		Afx::BinUtils::MemRange sbResult = Afx::BinUtils::FindPatternString(textRange,
			"53 56 48 83 EC 38 48 8B F1 48 8B DA 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 84 C0 74 ?? 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ??");
		if (!sbResult.IsEmpty()) {
			size_t cmpAddr = sbResult.Start + 0x20;
			// The cmp must reference the same cached flag that FrameStageNotify writes:
			size_t flagFromCmp = cmpAddr + 7 + ReadRel32(cmpAddr + 2);
			size_t flagFromFsn = 0 != g_Client.FrameStageNotifyIsHltvRetAddr
				? g_Client.FrameStageNotifyIsHltvRetAddr + 3 + 6 + ReadRel32(g_Client.FrameStageNotifyIsHltvRetAddr + 5) : 0;
			if (flagFromCmp == flagFromFsn) {
				g_Client.ScoreboardHltvJe = (unsigned char *)(sbResult.Start + 0x27);
				memcpy(g_Client.ScoreboardHltvJeOrg, g_Client.ScoreboardHltvJe, sizeof(g_Client.ScoreboardHltvJeOrg));
			} else advancedfx::Warning("mirv_pov: scoreboard HLTV check doesn't match cached flag.\n");
		} else advancedfx::Warning("mirv_pov: scoreboard HLTV check not found.\n");
	}

	// C_BaseEntity::SetPredictable(bool): cmp dl, [rcx+m_bPredictable]; je; mov [rcx+m_bPredictable], dl; ...
	{
		Afx::BinUtils::MemRange spResult = Afx::BinUtils::FindPatternString(textRange,
			"48 89 74 24 10 57 48 83 EC 20 0F B6 F2 48 8B F9 3A 91 ?? ?? ?? ?? 74 ?? 88 91 ?? ?? ?? ?? C6 81");
		if (!spResult.IsEmpty() && ReadRel32(spResult.Start + 0x12) == ReadRel32(spResult.Start + 0x1a)) {
			g_OfsPredictable = ReadRel32(spResult.Start + 0x12);
			g_Org_SetPredictable = (SetPredictable_t)spResult.Start;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)g_Org_SetPredictable, New_SetPredictable);
			if (NO_ERROR != DetourTransactionCommit()) {
				g_Org_SetPredictable = nullptr;
				advancedfx::Warning("mirv_pov: failed to detour SetPredictable.\n");
			}
		} else advancedfx::Warning("mirv_pov: SetPredictable not found.\n");
	}

	// GetSplitScreenPlayerPawn(int) and its call site in the death panel's player_death handler
	// ("xor ecx,ecx; mov [rbp+victimPawn],rax; call GetSplitScreenPlayerPawn; mov rcx,[r13]; mov edx,4; mov r8d,...").
	{
		Afx::BinUtils::MemRange getterResult = Afx::BinUtils::FindPatternString(textRange,
			"48 83 EC ?? 83 F9 FF 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 4C 8D 05 ?? ?? ?? ?? 33 D2 4D 8B 04 C0");
		Afx::BinUtils::MemRange siteResult = Afx::BinUtils::FindPatternString(textRange,
			"33 C9 48 89 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 8B 4D 00 BA 04 00 00 00 41 B8");
		if (!getterResult.IsEmpty() && !siteResult.IsEmpty()
			&& siteResult.Start + 9 + 5 + ReadRel32(siteResult.Start + 10) == getterResult.Start) {
			g_DeathPanelPawnRetAddrs[DeathPanelSite_Event] = siteResult.Start + 0x0e;

			// Show: mov byte ptr [..], 1; xor ecx, ecx; movss xmm0, [rax+30h]; movss [..], xmm0; call GetSplitScreenPlayerPawn
			Afx::BinUtils::MemRange showResult = Afx::BinUtils::FindPatternString(textRange,
				"C6 05 ?? ?? ?? ?? 01 33 C9 F3 0F 10 40 30 F3 0F 11 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B F0 48 85 C0");
			if (!showResult.IsEmpty() && showResult.Start + 0x16 + 5 + ReadRel32(showResult.Start + 0x17) == getterResult.Start)
				g_DeathPanelPawnRetAddrs[DeathPanelSite_Show] = showResult.Start + 0x1b;
			else advancedfx::Warning("mirv_pov: death panel Show pawn check not found.\n");

			// Per frame update: jle; xor ecx, ecx; call GetSplitScreenPlayerPawn; mov rdi, rax; ... [rdi+m_flDeathTime]
			Afx::BinUtils::MemRange thinkResult = Afx::BinUtils::FindPatternString(textRange,
				"0F 8E ?? ?? ?? ?? 33 C9 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 00 48 8B CF FF 90 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 57 10");
			if (!thinkResult.IsEmpty() && thinkResult.Start + 8 + 5 + ReadRel32(thinkResult.Start + 9) == getterResult.Start)
				g_DeathPanelPawnRetAddrs[DeathPanelSite_Think] = thinkResult.Start + 0x0d;
			else advancedfx::Warning("mirv_pov: death panel update pawn check not found.\n");

			g_Org_GetSplitScreenPlayerPawn = (GetSplitScreenPlayerPawn_t)getterResult.Start;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)g_Org_GetSplitScreenPlayerPawn, New_GetSplitScreenPlayerPawn);
			if (NO_ERROR != DetourTransactionCommit()) {
				g_Org_GetSplitScreenPlayerPawn = nullptr;
				for (int site = 0; site < DeathPanelSite_Count; ++site) g_DeathPanelPawnRetAddrs[site] = 0;
				advancedfx::Warning("mirv_pov: failed to detour GetSplitScreenPlayerPawn.\n");
			}
		} else advancedfx::Warning("mirv_pov: death panel pawn check not found.\n");
	}

	// CCSUsrMsg_SendLastKillerDamageToClient handler: reads 6 ints, FindHudElement("CCSGO_HudDeathPanel"), SetPendingDamage(...).
	{
		Afx::BinUtils::MemRange dmgResult = Afx::BinUtils::FindPatternString(textRange,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 8B 59 ?? 8B 79 ?? 8B 71 ?? 8B 69 ?? 44 8B 71 ?? 44 8B 79 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2 48 85 C0 48 8D 48 E0");
		if (!dmgResult.IsEmpty() && 0xE8 == *(unsigned char *)(dmgResult.Start + 0x60)) {
			g_FindHudElement = (FindHudElement_t)(dmgResult.Start + 0x33 + 5 + ReadRel32(dmgResult.Start + 0x34));
			g_DeathPanel_SetPendingDamage = (DeathPanel_SetPendingDamage_t)(dmgResult.Start + 0x60 + 5 + ReadRel32(dmgResult.Start + 0x61));
		} else advancedfx::Warning("mirv_pov: SendLastKillerDamageToClient handler not found (death card won't show).\n");
	}

	// CHudChatDelegate vtable[12] handles CUserMessageSayText2 (called through a "jmp [rax+0x60]" thunk).
	if (void ** vtable = (void **)Afx::BinUtils::FindClassVtable(clientDll, ".?AVCHudChatDelegate@@", 0, 0x0)) {
		AfxDetourPtr(&(vtable[12]), New_HudChat_SayText2, (PVOID *)&g_Org_HudChat_SayText2);
	} else advancedfx::Warning("mirv_pov: CHudChatDelegate vtable not found (chatlog won't work).\n");

	// Crosshair HUD: if (!flag || !ApplyObserverCrosshair(&settings, observedPawn)) ApplyOwnCrosshair(&settings);
	// ApplyObserverCrosshair (cl_show_observer_crosshair) ends with:
	//   ApplyCrosshairCode(settings, GetCrosshairCode(controller)) at +0x18e (GetCrosshairCode) / +0x199 (ApplyCrosshairCode).
	{
		Afx::BinUtils::MemRange xhResult = Afx::BinUtils::FindPatternString(textRange,
			"48 8B D6 48 8D 4F 60 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8D 4F 60 E8 ?? ?? ?? ??");
		if (!xhResult.IsEmpty()) {
			size_t applyObserver = xhResult.Start + 0x07 + 5 + ReadRel32(xhResult.Start + 0x08);
			size_t applyOwn = xhResult.Start + 0x14 + 5 + ReadRel32(xhResult.Start + 0x15);
			if (0xE8 == *(unsigned char *)(applyObserver + 0x18e) && 0xE8 == *(unsigned char *)(applyObserver + 0x199)) {
				g_Client.GetCrosshairCode = (GetCrosshairCode_t)(applyObserver + 0x18e + 5 + ReadRel32(applyObserver + 0x18f));
				g_Client.ApplyCrosshairCode = (ApplyCrosshairCode_t)(applyObserver + 0x199 + 5 + ReadRel32(applyObserver + 0x19a));
				g_Client.ApplyOwnCrosshair = (ApplyOwnCrosshair_t)applyOwn;
				// GetCrosshairCode: ... call <settings getter: lea rax, [obj]; ret> (+0x0d); cmp byte ptr [rax+flag], 0 (+0x12)
				size_t getCode = (size_t)g_Client.GetCrosshairCode;
				static const unsigned char cmpByteRax[] = { 0x80, 0xB8 };
				if (0xE8 == *(unsigned char *)(getCode + 0x0d) && BytesEqual(getCode + 0x12, cmpByteRax, sizeof(cmpByteRax))) {
					size_t settingsGetter = getCode + 0x0d + 5 + ReadRel32(getCode + 0x0e);
					static const unsigned char leaRax[] = { 0x48, 0x8D, 0x05 };
					if (BytesEqual(settingsGetter, leaRax, sizeof(leaRax)) && 0xC3 == *(unsigned char *)(settingsGetter + 7)) {
						size_t settings = settingsGetter + 7 + ReadRel32(settingsGetter + 3);
						g_pCrosshairHideFlag = (unsigned char *)(settings + ReadRel32(getCode + 0x14));
					}
				}
				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourAttach(&(PVOID &)g_Client.ApplyOwnCrosshair, New_ApplyOwnCrosshair);
				if (NO_ERROR != DetourTransactionCommit()) {
					g_Client.ApplyOwnCrosshair = nullptr;
					advancedfx::Warning("mirv_pov: failed to detour crosshair settings.\n");
				}
			} else advancedfx::Warning("mirv_pov: observer crosshair layout changed.\n");
		} else advancedfx::Warning("mirv_pov: crosshair HUD site not found.\n");
	}

	// Damage list change callback (CCSPlayerController_DamageServices::m_nSendUpdate):
	//   if (sv_dev_damage_use_netvars && changed && g_pGameRules && g_pGameRules->m_iRoundWinStatus)
	//     jmp DamageReport(FindHudElement("CCSGO_HudTeamCounter") - 0x20)
	// DamageReport: ... mov r12, [r13+m_pDamageServices] (+0x7c); cmp [r12+size], eax (+0x94);
	//   imul rbp, rax, 78h (+0xa3); add rbp, [r12+memory] (+0xa7)
	{
		Afx::BinUtils::MemRange cbResult = Afx::BinUtils::FindPatternString(textRange,
			"48 83 EC 28 48 8B 05 ?? ?? ?? ?? 80 78 ?? 00 74 ?? 41 8B 00 39 41 ?? 74 ?? 89 41 ?? 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? "
			"83 B8 ?? ?? ?? ?? 00 74 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2 48 85 C0 48 8D 48 E0 48 0F 44 CA 48 83 C4 28 E9");
		if (!cbResult.IsEmpty()) {
			size_t cb = cbResult.Start;
			size_t report = cb + 0x4e + 5 + ReadRel32(cb + 0x4f);
			static const unsigned char movServices[] = { 0x4D, 0x8B, 0xA5 };
			static const unsigned char cmpSize[] = { 0x41, 0x39, 0x44, 0x24 };
			static const unsigned char imulStride[] = { 0x48, 0x6B, 0xE8, (unsigned char)c_DmgRecordSize };
			static const unsigned char addMemory[] = { 0x49, 0x03, 0x6C, 0x24 };
			if (BytesEqual(report + 0x7c, movServices, sizeof(movServices)) && BytesEqual(report + 0x94, cmpSize, sizeof(cmpSize))
				&& BytesEqual(report + 0xa3, imulStride, sizeof(imulStride)) && BytesEqual(report + 0xa7, addMemory, sizeof(addMemory))) {
				g_DmgReport.ppUseNetvarsCvar = (unsigned char **)(cb + 0x04 + 7 + ReadRel32(cb + 0x07));
				g_DmgReport.OfsCvarValue = *(unsigned char *)(cb + 0x0d);
				g_DmgReport.ppGameRules = (void **)(cb + 0x1c + 7 + ReadRel32(cb + 0x1f));
				g_DmgReport.OfsRoundWinStatus = ReadRel32(cb + 0x2a);
				g_DmgReport.FindHudElement = (FindHudElement_t)(cb + 0x38 + 5 + ReadRel32(cb + 0x39));
				g_DmgReport.OfsDamageServices = ReadRel32(report + 0x7f);
				g_DmgReport.OfsListCount = *(unsigned char *)(report + 0x98);
				g_DmgReport.OfsListMem = *(unsigned char *)(report + 0xab);
				g_DmgReport.Report = (TeamCounter_DamageReport_t)report;
				DamageReport_Reset(true);
			} else advancedfx::Warning("mirv_pov: damage reporter layout changed (no round end damage report).\n");
		} else advancedfx::Warning("mirv_pov: damage list callback not found (no round end damage report).\n");
	}

	// CCSUsrMsg_Damage handler: if (msg.amount > 0) ShowDirection(this, &msg.inflictor_world_pos, pawn)
	{
		Afx::BinUtils::MemRange arcResult = Afx::BinUtils::FindPatternString(textRange,
			"83 7F 50 00 0F 8E ?? ?? ?? ?? 48 8B 47 48 48 8D 0D ?? ?? ?? ?? 48 85 C0 48 8D 54 24 ?? 4C 8B C3 48 0F 45 C8 "
			"F3 0F 10 41 18 F3 0F 10 49 1C F3 0F 11 44 24 ?? F3 0F 10 41 20 48 8B CD F3 0F 11 44 24 ?? F3 0F 11 4C 24 ?? E8");
		if (!arcResult.IsEmpty()) {
			g_DamageIndicator_ShowDirection = (DamageIndicator_ShowDirection_t)(arcResult.Start + 0x48 + 5 + ReadRel32(arcResult.Start + 0x49));
		} else advancedfx::Warning("mirv_pov: CCSUsrMsg_Damage handler not found (no damage direction arcs).\n");
	}

	// Scope inaccuracy update: weapon = GetActiveWeapon(pawn); cameraServices = pawn->m_pCameraServices;
	// ... weapon->GetInaccuracy(0, 0) (vtable 0xcf8) ...
	{
		Afx::BinUtils::MemRange sbResult = Afx::BinUtils::FindPatternString(textRange,
			"48 89 5C 24 10 57 48 83 EC 50 48 8B D9 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 74 24 60 48 8B B3 ?? ?? ?? ?? "
			"48 85 F6 0F 84 ?? ?? ?? ?? 48 8B 08 45 33 C0 0F 29 74 24 40 33 D2 0F 29 7C 24 30 44 0F 29 44 24 20 4C 8B 89 ?? ?? ?? ?? 48 8B C8 41 FF D1");
		if (!sbResult.IsEmpty()) {
			g_UpdateScopeInaccuracy = (UpdateScopeInaccuracy_t)sbResult.Start;
			g_UpdateScopeInaccuracyCameraServicesOfs = *(int *)(sbResult.Start + 0x26);
		} else advancedfx::Warning("mirv_pov: scope inaccuracy update not found (scope blur stays frozen in POV).\n");
	}

	// CCSUsrMsg_RadioText handler: ... ChatPrintf(this, &filter, client, "%s", text); then "HudChat.Message" if enabled,
	// and earlier: mov rcx, [g_pLocalize]; lea r15, [""]; mov rdx, r15; mov rax, [rcx]; mov r8, [rax+Find]
	{
		Afx::BinUtils::MemRange printResult = Afx::BinUtils::FindPatternString(textRange,
			"4C 8D 0D ?? ?? ?? ?? 4C 89 7C 24 20 44 8B C3 48 8D 95 ?? ?? ?? ?? 49 8B CD E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 40 38 70 58");
		Afx::BinUtils::MemRange locResult = Afx::BinUtils::FindPatternString(textRange,
			"48 8B 0D ?? ?? ?? ?? 4C 8D 3D ?? ?? ?? ?? 49 8B D7 48 8B 01 4C 8B 40 78 48 8B 85 ?? ?? ?? ?? 48 85 C0 48 0F 45 D0 41 FF D0");
		if (!printResult.IsEmpty() && !locResult.IsEmpty()
			&& 0 == strcmp((const char *)(printResult.Start + 7 + ReadRel32(printResult.Start + 3)), "%s")) {
			g_ChatPrintf = (ChatPrintf_t)(printResult.Start + 0x19 + 5 + ReadRel32(printResult.Start + 0x1a));
			g_ppLocalize = (void **)(locResult.Start + 7 + ReadRel32(locResult.Start + 3));
			g_LocalizeFindIndex = *(unsigned char *)(locResult.Start + 0x17) / 8;
		} else advancedfx::Warning("mirv_pov: RadioText handler not found (no radio / kill reward chat lines).\n");
		if (!printResult.IsEmpty()) {
			g_ppClutchModeCvar = (unsigned char **)(printResult.Start + 0x1e + 7 + ReadRel32(printResult.Start + 0x21));
			g_OfsClutchModeValue = *(unsigned char *)(printResult.Start + 0x28);
		}
	}

	// Chat sound helper: ... params = { "HudChat.Message", ... }; EmitSound(-1, &params)
	{
		Afx::BinUtils::MemRange sndResult = Afx::BinUtils::FindPatternString(textRange,
			"33 C0 48 C7 44 24 34 00 00 80 3F 48 89 44 24 40 48 8D 54 24 20 B8 64 00 00 00 C7 44 24 3C FF FF FF FF 66 89 44 24 48 "
			"B9 FF FF FF FF 0F B6 44 24 4A 24 E1 0C 01 88 44 24 4A 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 E8");
		if (!sndResult.IsEmpty() && 0 == strcmp((const char *)(sndResult.Start + 0x39 + 7 + ReadRel32(sndResult.Start + 0x3c)), "HudChat.Message")) {
			g_EmitSound = (EmitSound_t)(sndResult.Start + 0x45 + 5 + ReadRel32(sndResult.Start + 0x46));
		} else advancedfx::Warning("mirv_pov: chat sound not found (radio lines without sound).\n");
	}

	// shake_testpunch: slot = engine->GetActiveSlot(&tmp); g_ViewEffects[slot] (stride 0xa8)->Shake(&shake):
	//   movsxd rcx, [rax]; imul rax, rcx, 0A8h; lea rcx, [g_ViewEffects]; add rcx, rax; mov rax, [rcx]; call [rax+Shake]
	{
		Afx::BinUtils::MemRange veResult = Afx::BinUtils::FindPatternString(textRange,
			"48 8B 0D ?? ?? ?? ?? 48 8D 94 24 ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 8D 54 24 20 48 63 08 48 69 C1 A8 00 00 00 "
			"48 8D 0D ?? ?? ?? ?? 48 03 C8 48 8B 01 FF 50 ??");
		if (!veResult.IsEmpty()) {
			g_pViewEffects = (unsigned char *)(veResult.Start + 0x27 + 7 + ReadRel32(veResult.Start + 0x2a));
			g_ViewEffectsShakeIndex = *(unsigned char *)(veResult.Start + 0x36) / 8;
		} else advancedfx::Warning("mirv_pov: view effects not found (no C4 screen shake).\n");
	}

	// CUserMessageAudioParameter handler: by parameter_type (+0x48) token: ... the third one:
	//   cmp [token], eax; jne; mov rcx, [g_pSoundSystem]; mov edx, [msg+name_hash_code]; mov rax, [rcx]; jmp [rax+TriggerControl]
	{
		Afx::BinUtils::MemRange apResult = Afx::BinUtils::FindPatternString(textRange,
			"8B 41 48 48 8B D1 39 05 ?? ?? ?? ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 45 33 C9 F3 0F 10 52 50 8B 52 4C 48 8B 01 48 FF A0 ?? ?? ?? ?? "
			"39 05 ?? ?? ?? ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 44 8B 42 54 8B 52 4C 48 8B 01 48 FF A0 ?? ?? ?? ?? "
			"39 05 ?? ?? ?? ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 8B 52 4C 48 8B 01 48 FF A0");
		if (!apResult.IsEmpty() && 0x92e1d607 == *(unsigned int *)(apResult.Start + 0x4a + 6 + ReadRel32(apResult.Start + 0x4c))) {
			g_ppSoundSystem = (void **)(apResult.Start + 0x52 + 7 + ReadRel32(apResult.Start + 0x55));
			g_SoundSystemTriggerControlIndex = ReadRel32(apResult.Start + 0x62) / 8;
		} else advancedfx::Warning("mirv_pov: audio parameter handler not found (ear ringing without muffling).\n");
	}

	// Spectator tools check: push rbx; ...; mov rax, [force_spectator_only_tools]; cmp byte ptr [rax+value], 0; jne true;
	// local = GetSplitScreenPlayer(0); cmp byte ptr [local+m_bIsLocalPlayerController], 0; je; movzx eax, [g_bIsHLTV]; jmp; movzx eax, [local+m_bIsHLTV]
	{
		Afx::BinUtils::MemRange stResult = Afx::BinUtils::FindPatternString(textRange,
			"40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 80 78 ?? 00 0F 85 ?? ?? ?? ?? 33 C9 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? "
			"80 B8 ?? ?? ?? ?? 00 74 ?? 0F B6 05 ?? ?? ?? ?? EB ?? 0F B6 80");
		size_t flagFromFsn = 0 != g_Client.FrameStageNotifyIsHltvRetAddr
			? g_Client.FrameStageNotifyIsHltvRetAddr + 3 + 6 + ReadRel32(g_Client.FrameStageNotifyIsHltvRetAddr + 5) : 0;
		if (!stResult.IsEmpty() && stResult.Start + 0x2f + 7 + ReadRel32(stResult.Start + 0x32) == flagFromFsn) {
			g_ppForceSpectatorToolsCvar = (unsigned char **)(stResult.Start + 6 + 7 + ReadRel32(stResult.Start + 9));
			g_OfsForceSpectatorToolsValue = *(unsigned char *)(stResult.Start + 0x10);
			g_OfsControllerIsHltv = ReadRel32(stResult.Start + 0x3b);
			g_pCachedIsHltv = (unsigned char *)flagFromFsn;
			g_Org_SpectatorToolsEnabled = (SpectatorToolsEnabled_t)stResult.Start;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)g_Org_SpectatorToolsEnabled, New_SpectatorToolsEnabled);
			if (NO_ERROR != DetourTransactionCommit()) {
				g_Org_SpectatorToolsEnabled = nullptr;
				advancedfx::Warning("mirv_pov: failed to detour the spectator tools check.\n");
			}
		} else advancedfx::Warning("mirv_pov: spectator tools check not found (x-ray may show in POV).\n");
	}

	// CMsgSosStartSoundEvent: vtable[9] = _InternalParse, vtable[11] = _InternalSerialize, whose
	// "mov ecx, [rsi+hash]; mov byte ptr [rdx], 15h" / "movsxd rax, [rsi+source]; lea r8, [rdx+1]; mov byte ptr [rdx], 18h"
	// give the field offsets.
	if (void ** vtable = (void **)Afx::BinUtils::FindClassVtable(clientDll, ".?AVCMsgSosStartSoundEvent@@", 0, 0x0)) {
		size_t serialize = (size_t)vtable[11];
		Afx::BinUtils::MemRange serializeRange(serialize, serialize + 0x200);
		Afx::BinUtils::MemRange hashResult = Afx::BinUtils::FindPatternString(serializeRange, "8B 4E ?? C6 02 15");
		Afx::BinUtils::MemRange sourceResult = Afx::BinUtils::FindPatternString(serializeRange, "48 63 46 ?? 4C 8D 42 01 C6 02 18");
		static const unsigned char parsePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x41, 0x56, 0x41, 0x57 };
		if (!hashResult.IsEmpty() && !sourceResult.IsEmpty() && BytesEqual((size_t)vtable[9], parsePrologue, sizeof(parsePrologue))) {
			g_OfsSosHash = *(unsigned char *)(hashResult.Start + 2);
			g_OfsSosSourceEntity = *(unsigned char *)(sourceResult.Start + 3);
			g_Org_SosStartSoundEvent_Parse = (SosStartSoundEvent_Parse_t)vtable[9];
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)g_Org_SosStartSoundEvent_Parse, New_SosStartSoundEvent_Parse);
			if (NO_ERROR != DetourTransactionCommit()) {
				g_Org_SosStartSoundEvent_Parse = nullptr;
				advancedfx::Warning("mirv_pov: failed to detour CMsgSosStartSoundEvent parsing.\n");
			}
		} else advancedfx::Warning("mirv_pov: CMsgSosStartSoundEvent layout changed (enemy radio voice stays audible).\n");
	} else advancedfx::Warning("mirv_pov: CMsgSosStartSoundEvent not found (enemy radio voice stays audible).\n");

	// "spec_target_updated" sender (CCSObserver_ObserverServices): event = CreateEvent(..); SetPlayer("userid", pawn);
	// mov rdi, [rcx+SetEntity] (+0xbf); ...; SetEntity("target", GetObserverTarget())
	{
		Afx::BinUtils::MemRange specResult = Afx::BinUtils::FindPatternString(textRange,
			"48 89 5C 24 18 56 48 83 EC 30 48 8B D9 48 8D 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 45 33 C9 45 33 C0 48 8B 01 FF 50 30 48 8B F0 48 85 C0");
		static const unsigned char movRdi[] = { 0x48, 0x8B, 0xB9 };
		if (!specResult.IsEmpty()
			&& 0 == strcmp((const char *)(specResult.Start + 0x0d + 7 + ReadRel32(specResult.Start + 0x10)), "spec_target_updated")
			&& BytesEqual(specResult.Start + 0xbf, movRdi, sizeof(movRdi)) && 0 == ReadRel32(specResult.Start + 0xc2) % 8) {
			g_GameEventSetEntityIndex = ReadRel32(specResult.Start + 0xc2) / 8;
		} else advancedfx::Warning("mirv_pov: spec_target_updated sender not found (demo UI may show another spectated player after seeks).\n");
	}

	// See comment on top.
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(textRange,
		"40 55 53 57 41 56 41 57 48 8B EC 48 81 EC ?? ?? ?? ?? 48 63 FA 4C 8B F1 41 BF FF FF FF FF 83 FF FF ?? ?? "
		"48 8B 0D ?? ?? ?? ?? 48 8D 55 ?? 44 8B C7 48 8B 01 FF 90 ?? ?? ?? ?? 8B 18 EB ?? 41 8B DF "
		"83 3D ?? ?? ?? ?? 00 74 ?? 8B 05 ?? ?? ?? ?? 83 F8 FF 8D 58 FF");
	if (result.IsEmpty()) return;

	size_t setter = result.Start;

	// +0x87: lea rcx, [g_LocalPlayerControllers] / +0x8e: xor esi,esi / +0x90: mov [rcx+rdi*8], r14 / +0x94: mov byte ptr [r14+ofs], 1
	static const unsigned char leaRcx[] = { 0x48, 0x8D, 0x0D };
	static const unsigned char storeAndFlag[] = { 0x33, 0xF6, 0x4C, 0x89, 0x34, 0xF9, 0x41, 0xC6, 0x86 };
	if (!BytesEqual(setter + 0x87, leaRcx, sizeof(leaRcx)) || !BytesEqual(setter + 0x8e, storeAndFlag, sizeof(storeAndFlag)) || 0x01 != *(unsigned char *)(setter + 0x9b)) {
		advancedfx::Warning("mirv_pov: client.dll local player setter layout changed.\n");
		return;
	}

	g_Client.ModuleBase = (size_t)clientDll;
	g_Client.SetAsLocalPlayerController = (SetAsLocalPlayerController_t)setter;
	g_Client.ppEngine = (void **)(setter + 0x23 + 7 + ReadRel32(setter + 0x26));
	g_Client.EngineGetLocalPlayerVtableOfs = ReadRel32(setter + 0x36);
	g_Client.pKillCamObsMode = (int *)(setter + 0x41 + 7 + ReadRel32(setter + 0x43));
	g_Client.pKillCamTarget = (int *)(setter + 0x4a + 6 + ReadRel32(setter + 0x4c));
	g_Client.pLocalPlayerControllers = (void **)(setter + 0x87 + 7 + ReadRel32(setter + 0x8a));

	g_Client.OfsIsLocalPlayerControllerFromCode = ReadRel32(setter + 0x97);

	g_Client.Ok = true;
}

void MirvPov_HookEngine2Dll(HMODULE engine2Dll) {
	Afx::BinUtils::ImageSectionsReader sections(engine2Dll);
	if (sections.Eof()) return;
	Afx::BinUtils::MemRange textRange = sections.GetMemRange();

	// CDemoPlayer::SkipToTick(int tick, flags: 1 = relative, 2 = pause), CDemoPlayer vtable index 19,
	// references "CDemoPlayer::SkipToTick" / "SkipToTick %d". Used by demo_gototick and the demo UI.
	{
		Afx::BinUtils::MemRange skipResult = Afx::BinUtils::FindPatternString(textRange,
			"40 55 56 41 54 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B 2D ?? ?? ?? ?? 45 8B E0 41 D1 EC 44 8B FA 41 80 E4 01 48 8B F1 41 F6 C0 01");
		if (!skipResult.IsEmpty()) {
			g_Engine.SkipToTick = (DemoPlayer_SkipToTick_t)skipResult.Start;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)g_Engine.SkipToTick, New_DemoPlayer_SkipToTick);
			if (NO_ERROR != DetourTransactionCommit()) {
				g_Engine.SkipToTick = nullptr;
				advancedfx::Warning("mirv_pov: failed to detour CDemoPlayer::SkipToTick.\n");
			}
		} else advancedfx::Warning("mirv_pov: CDemoPlayer::SkipToTick not found, seeking with killcam swap active will crash.\n");
	}

	// CEngineClient::IsHLTV (vtable index 86): return m_pClient && (m_bIsHLTV || ...)
	{
		Afx::BinUtils::MemRange hltvResult = Afx::BinUtils::FindPatternString(textRange,
			"48 8B 05 ?? ?? ?? ?? 48 85 C0 74 15 80 B8 ?? ?? ?? ?? 00 75 09 83 B8 ?? ?? ?? ?? 00 7E 03 B0 01 C3 32 C0 C3");
		if (!hltvResult.IsEmpty()) {
			g_Engine.IsHLTV = (EngineClient_IsHLTV_t)hltvResult.Start;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID &)g_Engine.IsHLTV, New_EngineClient_IsHLTV);
			if (NO_ERROR != DetourTransactionCommit()) {
				g_Engine.IsHLTV = nullptr;
				advancedfx::Warning("mirv_pov: failed to detour CEngineClient::IsHLTV.\n");
			}
		} else advancedfx::Warning("mirv_pov: CEngineClient::IsHLTV not found.\n");
	}
}

static void ChatLines_OnFrame();
static void MuteRadio_OnFrame();

// Must not contain objects with destructors (SEH).
static void ScopeBlur_OnFrame() {
	if (!g_PovScopeBlur || nullptr == g_UpdateScopeInaccuracy || nullptr == g_KillCamLastSwapped) return;
	if (g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices != g_UpdateScopeInaccuracyCameraServicesOfs) {
		g_UpdateScopeInaccuracy = nullptr;
		advancedfx::Warning("mirv_pov: scope inaccuracy update doesn't match m_pCameraServices (scope blur stays frozen in POV).\n");
		return;
	}
	void * pPawn = GetSwappedTargetPlayerPawn();
	if (nullptr == pPawn) return;
	__try {
		g_UpdateScopeInaccuracy(pPawn);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_UpdateScopeInaccuracy = nullptr;
	}
}

void MirvPov_BeforeFrameStageNotify(int curStage) {
	SetScoreboardPatch(0 != g_HltvSpoofMode && nullptr != g_KillCamLastSwapped && 0 == g_KillCamSuspendFrames);

	if (0 < g_KillCamSuspendFrames) {
		g_MuteVoActive = false;
		if (SOURCESDK::CS2::FRAME_RENDER_PASS == curStage) {
			KillCam_OnSuspendedFrame();
			if (0 == --g_KillCamSuspendFrames) g_KillCamSeekTargetTick = -1;
		}
		return;
	}
	KillCam_Apply(false);
	if (g_PovSuppressBuyMenu && nullptr != g_KillCamLastSwapped) SuppressTargetBuyMenu();
	if (SOURCESDK::CS2::FRAME_RENDER_PASS == curStage) {
		DamageReport_OnFrame();
		ChatLines_OnFrame();
		MuteRadio_OnFrame();
		ScopeBlur_OnFrame();
	}

	// After a (re-)swap: undo the predictability the game just gave the target's entities.
	static void * s_pPredictClearedFor = nullptr;
	if (g_KillCamLastSwapped != s_pPredictClearedFor) {
		s_pPredictClearedFor = g_KillCamLastSwapped;
		if (nullptr != g_KillCamLastSwapped) ClearTargetPredictable();
	}
}

// From DeathMsg.cpp: the game's string hash used for game event keys.
typedef unsigned int (__fastcall * g_Original_hashString_t)(const char * string, unsigned int length, unsigned int lengthXorSeed);
extern g_Original_hashString_t g_Original_hashString;

// Must not contain objects with destructors (SEH).
static void * GetGameEventPlayerController(SOURCESDK::CS2::IGameEvent * pEvent, const char * pszKey) {
	if (nullptr == g_Original_hashString) return nullptr;
	__try {
		unsigned int len = (unsigned int)strlen(pszKey);
		SOURCESDK::CS2::CKV3MemberName key((int)g_Original_hashString(pszKey, len, len ^ 0x31415926), -1, pszKey);
		return pEvent->GetPlayerController(key);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Must not contain objects with destructors (SEH).
static int GetGameEventInt(SOURCESDK::CS2::IGameEvent * pEvent, const char * pszKey) {
	if (nullptr == g_Original_hashString) return 0;
	__try {
		unsigned int len = (unsigned int)strlen(pszKey);
		SOURCESDK::CS2::CKV3MemberName key((int)g_Original_hashString(pszKey, len, len ^ 0x31415926), -1, pszKey);
		return pEvent->GetInt(key);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

// Must not contain objects with destructors (SEH).
static float GetGameEventFloat(SOURCESDK::CS2::IGameEvent * pEvent, const char * pszKey) {
	if (nullptr == g_Original_hashString) return 0.0f;
	__try {
		unsigned int len = (unsigned int)strlen(pszKey);
		SOURCESDK::CS2::CKV3MemberName key((int)g_Original_hashString(pszKey, len, len ^ 0x31415926), -1, pszKey);
		return pEvent->GetFloat(key);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.0f;
	}
}

// Must not contain objects with destructors (SEH).
static bool GetGameEventBool(SOURCESDK::CS2::IGameEvent * pEvent, const char * pszKey) {
	if (nullptr == g_Original_hashString) return false;
	__try {
		unsigned int len = (unsigned int)strlen(pszKey);
		SOURCESDK::CS2::CKV3MemberName key((int)g_Original_hashString(pszKey, len, len ^ 0x31415926), -1, pszKey);
		return pEvent->GetBool(key);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Must not contain objects with destructors (SEH). Only valid while the event is.
static const char * GetGameEventString(SOURCESDK::CS2::IGameEvent * pEvent, const char * pszKey) {
	if (nullptr == g_Original_hashString) return nullptr;
	__try {
		unsigned int len = (unsigned int)strlen(pszKey);
		SOURCESDK::CS2::CKV3MemberName key((int)g_Original_hashString(pszKey, len, len ^ 0x31415926), -1, pszKey);
		return pEvent->GetString(key);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Collected all the time (not only while swapped), so switching players mid round works.
static void DamageReport_OnGameEvent(SOURCESDK::CS2::IGameEvent * pEvent, const char * pszName) {
	if (nullptr == g_DmgReport.Report) return;
	if (0 == strcmp(pszName, "begin_new_match")) {
		// Warmup has the same m_totalRoundsPlayed (0) as the first round.
		int tick = GetDemoTick();
		g_DmgEvents.erase(std::remove_if(g_DmgEvents.begin(), g_DmgEvents.end(),
			[tick](const DmgEvent_t & ev) { return ev.Tick < 0 || ev.Tick <= tick; }), g_DmgEvents.end());
		return;
	}
	bool bHurt = 0 == strcmp(pszName, "player_hurt");
	if (!bHurt && 0 != strcmp(pszName, "player_death")) return;
	int victim = GetEntityIndex(GetGameEventPlayerController(pEvent, "userid"));
	int attacker = GetEntityIndex(GetGameEventPlayerController(pEvent, "attacker"));
	if (bHurt) DamageReport_AddEvent(attacker, victim, false, DmgKillType_None, GetGameEventInt(pEvent, "health"), GetGameEventInt(pEvent, "dmg_health"));
	else DamageReport_AddEvent(attacker, victim, true, DamageReport_KillType(GetGameEventString(pEvent, "weapon"), GetGameEventBool(pEvent, "headshot")), 0, 0);
}

// The target's player pawn, also while dead (unlike GetSwappedTargetPlayerPawn).
// Must not contain objects with destructors (SEH).
static void * GetTargetPlayerPawn(int target) {
	if (0 == g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn || target < 1) return nullptr;
	__try {
		CEntityInstance * pController = GetEntity(target);
		if (nullptr == pController || !pController->IsPlayerController()) return nullptr;
		SOURCESDK::CS2::CBaseHandle handle(*(uint32_t *)((unsigned char *)pController + g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn));
		int index = handle.GetEntryIndex();
		if (index < 0) return nullptr;
		CEntityInstance * pPawn = GetEntity(index);
		return nullptr != pPawn && pPawn->IsPlayerPawn() ? pPawn : nullptr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Must not contain objects with destructors (SEH).
static void FixSpecTargetEvent(SOURCESDK::CS2::IGameEvent * pEvent) {
	if (!g_PovSpecTarget || 0 == g_KillCamTarget || g_GameEventSetEntityIndex < 0 || nullptr == g_Original_hashString) return;
	void * pPawn = GetTargetPlayerPawn(g_KillCamTarget);
	if (nullptr == pPawn) return;
	__try {
		const char * pszKey = "target";
		unsigned int len = (unsigned int)strlen(pszKey);
		SOURCESDK::CS2::CKV3MemberName key((int)g_Original_hashString(pszKey, len, len ^ 0x31415926), -1, pszKey);
		GameEvent_SetEntity_t setEntity = (GameEvent_SetEntity_t)(*(void ***)pEvent)[g_GameEventSetEntityIndex];
		setEntity(pEvent, &key, pPawn);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_GameEventSetEntityIndex = -1;
	}
}

// Must not contain objects with destructors (SEH).
static bool ShowDamageArc(void * pVictimPawn, const float * pInflictorPos) {
	FindHudElement_t findHudElement = nullptr != g_FindHudElement ? g_FindHudElement : g_DmgReport.FindHudElement;
	if (nullptr == findHudElement) return false;
	__try {
		void * pElement = findHudElement("CCSGO_HudDamageIndicator");
		if (nullptr == pElement) return false;
		g_DamageIndicator_ShowDirection((unsigned char *)pElement - 0x20, pInflictorPos, pVictimPawn);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static void DamageArcs_OnHurt(SOURCESDK::CS2::IGameEvent * pEvent) {
	if (!g_PovDamageArcs || nullptr == g_DamageIndicator_ShowDirection || nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames) return;
	if (GetGameEventPlayerController(pEvent, "userid") != g_KillCamLastSwapped) return;
	// Like the handler: only while alive, and only for damage.
	if (GetGameEventInt(pEvent, "health") <= 0 || GetGameEventInt(pEvent, "dmg_health") <= 0) return;
	int attacker = GetEntityIndex(GetGameEventPlayerController(pEvent, "attacker"));
	if (attacker < 1 || attacker == g_KillCamTarget) return; // world / self (e.g. fall damage): no direction
	void * pVictimPawn = GetSwappedTargetPlayerPawn();
	CEntityInstance * pAttackerPawn = (CEntityInstance *)GetTargetPlayerPawn(attacker);
	if (nullptr == pVictimPawn || nullptr == pAttackerPawn) return;
	float pos[3];
	pAttackerPawn->GetOrigin(pos[0], pos[1], pos[2]);
	if (FLT_MAX == pos[0]) return;
	if (!ShowDamageArc(pVictimPawn, pos)) advancedfx::Warning("mirv_pov: showing the damage direction failed.\n");
}

// Must not contain objects with destructors (SEH).
static const char * Localize(const char * pszToken) {
	if (nullptr == g_ppLocalize || g_LocalizeFindIndex < 0) return nullptr;
	__try {
		void * pLocalize = *g_ppLocalize;
		if (nullptr == pLocalize) return nullptr;
		return ((Localize_Find_t)(*(void ***)pLocalize)[g_LocalizeFindIndex])(pLocalize, pszToken);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

static const char * LocalizeOr(const char * pszToken, const char * pszFallback) {
	const char * pszResult = Localize(pszToken);
	return nullptr != pszResult ? pszResult : pszFallback;
}

// Replaces %s1 .. %s9 in pszFormat (like ILocalize::ConstructString).
static void FormatLocalized(char * pOut, size_t outSize, const char * pszFormat, const char * const * params, int count) {
	size_t len = 0;
	for (const char * p = pszFormat; '\0' != *p && len + 1 < outSize; ++p) {
		if ('%' == p[0] && 's' == p[1] && '1' <= p[2] && p[2] <= '9') {
			int index = p[2] - '1';
			const char * pszParam = index < count && nullptr != params[index] ? params[index] : "";
			for (; '\0' != *pszParam && len + 1 < outSize; ++pszParam) pOut[len++] = *pszParam;
			p += 2;
		} else pOut[len++] = *p;
	}
	pOut[len] = '\0';
}

// Must not contain objects with destructors (SEH).
static void PrintChat(int nClient, const char * pszText) {
	if (nullptr == g_ChatPrintf) return;
	__try {
		unsigned char filter[2] = { 0, 1 };
		g_ChatPrintf(nullptr, filter, nClient, "%s", pszText); // this is unused
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_ChatPrintf = nullptr;
	}
}

static const char * StripWeaponPrefix(const char * pszWeapon) {
	if (nullptr == pszWeapon) return "";
	return 0 == strncmp(pszWeapon, "weapon_", 7) ? pszWeapon + 7 : pszWeapon;
}

// Must not contain objects with destructors (SEH).
static bool GetPlaceName(void * pPawn, char * pOut, size_t outSize) {
	pOut[0] = '\0';
	if (nullptr == pPawn || 0 == g_clientDllOffsets.C_CSPlayerPawn.m_szLastPlaceName) return false;
	__try {
		const char * pszPlace = (const char *)pPawn + g_clientDllOffsets.C_CSPlayerPawn.m_szLastPlaceName;
		size_t i = 0;
		for (; i + 1 < outSize && i < 18 && '\0' != pszPlace[i]; ++i) pOut[i] = pszPlace[i];
		pOut[i] = '\0';
		return 0 < i;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Must not contain objects with destructors (SEH).
static bool EmitLocalSound(const char * pszSoundEvent) {
	if (nullptr == g_EmitSound) return false;
	__try {
		alignas(16) unsigned char params[0x40] = {};
		*(const char **)params = pszSoundEvent;
		*(float *)(params + 0x14) = 1.0f;
		*(int *)(params + 0x1c) = -1;
		*(unsigned short *)(params + 0x28) = 100;
		params[0x2a] = 1;
		g_EmitSound(-1, params);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_EmitSound = nullptr;
		return false;
	}
}

// Must not contain objects with destructors (SEH).
static void PlayChatSound() {
	__try {
		if (nullptr != g_ppClutchModeCvar && nullptr != *g_ppClutchModeCvar && 0 != *(*g_ppClutchModeCvar + g_OfsClutchModeValue)) return;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	EmitLocalSound("HudChat.Message");
}

// Must not contain objects with destructors (SEH).
static bool IsOnGround(void * pEntity) {
	if (0 == g_clientDllOffsets.C_BaseEntity.m_fFlags) return true;
	__try {
		return 0 != (*(unsigned int *)((unsigned char *)pEntity + g_clientDllOffsets.C_BaseEntity.m_fFlags) & 1); // FL_ONGROUND
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return true;
	}
}

// Must not contain objects with destructors (SEH).
static bool StartScreenShake(float amplitude, float frequency, float duration) {
	if (nullptr == g_pViewEffects || g_ViewEffectsShakeIndex < 0) return false;
	__try {
		struct { int Command; float Amplitude; float Frequency; float Duration; float Direction[3]; } shake = { 0 /* SHAKE_START */, amplitude, frequency, duration, { 0, 0, 0 } };
		((ViewEffects_Shake_t)(*(void ***)g_pViewEffects)[g_ViewEffectsShakeIndex])(g_pViewEffects, &shake);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_pViewEffects = nullptr;
		return false;
	}
}

static float Distance(const float a[3], const float b[3]) {
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static bool IsPovActive() {
	return nullptr != g_KillCamLastSwapped && 0 == g_KillCamSuspendFrames;
}

// Must not contain objects with destructors (SEH).
static bool GetPlantedC4Origin(float outOrigin[3]) {
	__try {
		int highest = GetHighestEntityIndex();
		for (int i = 65; i <= highest; ++i) {
			CEntityInstance * pEnt = GetEntity(i);
			if (nullptr == pEnt) continue;
			const char * pszClass = pEnt->GetClientClassName();
			if (nullptr == pszClass || 0 != strcmp(pszClass, "C_PlantedC4")) continue;
			pEnt->GetOrigin(outOrigin[0], outOrigin[1], outOrigin[2]);
			return FLT_MAX != outOrigin[0];
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

static void Effects_OnBombExploded() {
	if (!g_PovBombShake || !IsPovActive()) return;
	CEntityInstance * pPawn = (CEntityInstance *)GetSwappedTargetPlayerPawn();
	if (nullptr == pPawn || !IsOnGround(pPawn)) return; // no air shake
	float bomb[3];
	if (!GetPlantedC4Origin(bomb)) return;
	float origin[3];
	pPawn->GetOrigin(origin[0], origin[1], origin[2]);
	if (FLT_MAX == origin[0]) return;
	float distance = Distance(bomb, origin);
	if (c_BombShakeRadius < distance) return;
	float amplitude = c_BombShakeAmplitude * (1.0f - distance / c_BombShakeRadius);
	if (0.0f < amplitude) StartScreenShake(amplitude, c_BombShakeFrequency, c_BombShakeDuration);
}

// Must not contain objects with destructors (SEH).
static void TriggerSoundControl(const char * pszLowerCaseName) {
	if (nullptr == g_ppSoundSystem || g_SoundSystemTriggerControlIndex < 0 || nullptr == g_Original_hashString) return;
	__try {
		void * pSoundSystem = *g_ppSoundSystem;
		if (nullptr == pSoundSystem) return;
		unsigned int len = (unsigned int)strlen(pszLowerCaseName);
		unsigned int hash = g_Original_hashString(pszLowerCaseName, len, len ^ 0x31415926);
		((SoundSystem_TriggerControl_t)(*(void ***)pSoundSystem)[g_SoundSystemTriggerControlIndex])(pSoundSystem, hash);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		g_ppSoundSystem = nullptr;
	}
}

static void PlayEarRinging(const char * pszRing, const char * pszDeafenControl) {
	TriggerSoundControl(pszDeafenControl);
	EmitLocalSound(pszRing);
}

static void Effects_OnFlashDetonate(SOURCESDK::CS2::IGameEvent * pEvent) {
	g_LastFlashPos[0] = GetGameEventFloat(pEvent, "x");
	g_LastFlashPos[1] = GetGameEventFloat(pEvent, "y");
	g_LastFlashPos[2] = GetGameEventFloat(pEvent, "z");
	g_LastFlashTick = GetDemoTick();
}

// Flashbang deafen: only for blinded players, by the distance between the flash and the player's eyes.
static void Effects_OnPlayerBlind(SOURCESDK::CS2::IGameEvent * pEvent) {
	if (!g_PovEarRinging || !IsPovActive()) return;
	if (GetGameEventPlayerController(pEvent, "userid") != g_KillCamLastSwapped) return;
	CEntityInstance * pPawn = (CEntityInstance *)GetSwappedTargetPlayerPawn();
	if (nullptr == pPawn) return;
	float flash[3] = { FLT_MAX, 0, 0 };
	int flashEntity = GetGameEventInt(pEvent, "entityid");
	if (CEntityInstance * pFlash = 0 < flashEntity ? GetEntity(flashEntity) : nullptr) pFlash->GetOrigin(flash[0], flash[1], flash[2]);
	if (FLT_MAX == flash[0]) {
		if (g_LastFlashTick < 0 || 64 < abs(GetDemoTick() - g_LastFlashTick)) return;
		memcpy(flash, g_LastFlashPos, sizeof(flash));
	}
	float eye[3];
	pPawn->GetRenderEyeOrigin(eye);
	float distance = Distance(flash, eye);
	if (distance < 100.0f) PlayEarRinging("Flashbang.Ring.Long", "control.deafenlong");
	else if (distance < 500.0f) PlayEarRinging("Flashbang.Ring.Medium", "control.deafenmedium");
	else if (distance < 1000.0f) PlayEarRinging("Flashbang.Ring.Short", "control.deafenshort");
}

// HE deafen: blast damage of 30 or more.
static void Effects_OnHurt(SOURCESDK::CS2::IGameEvent * pEvent) {
	if (!g_PovEarRinging || !IsPovActive()) return;
	if (GetGameEventPlayerController(pEvent, "userid") != g_KillCamLastSwapped) return;
	if (0 != strcmp(StripWeaponPrefix(GetGameEventString(pEvent, "weapon")), "hegrenade")) return;
	if (GetGameEventInt(pEvent, "dmg_health") + GetGameEventInt(pEvent, "dmg_armor") < 30) return;
	PlayEarRinging("Flashbang.Ring.Long", "control.deafenhe");
}

// Grenade radio, sent by the server to the thrower's team.
static void ChatLines_OnWeaponFire(SOURCESDK::CS2::IGameEvent * pEvent) {
	if (!g_PovChatRadio || nullptr == g_ChatPrintf || nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames) return;
	static const struct { const char * Weapon; const char * Token; } radios[] = {
		{ "hegrenade", "#SFUI_TitlesTXT_Fire_in_the_hole" },
		{ "flashbang", "#SFUI_TitlesTXT_Flashbang_in_the_hole" },
		{ "smokegrenade", "#SFUI_TitlesTXT_Smoke_in_the_hole" },
		{ "molotov", "#SFUI_TitlesTXT_Molotov_in_the_hole" },
		{ "incgrenade", "#SFUI_TitlesTXT_Incendiary_in_the_hole" },
		{ "decoy", "#SFUI_TitlesTXT_Decoy_in_the_hole" },
	};
	const char * pszWeapon = StripWeaponPrefix(GetGameEventString(pEvent, "weapon"));
	const char * pszToken = nullptr;
	for (const auto & radio : radios) if (0 == strcmp(pszWeapon, radio.Weapon)) { pszToken = radio.Token; break; }
	if (nullptr == pszToken) return;

	CEntityInstance * pThrower = (CEntityInstance *)GetGameEventPlayerController(pEvent, "userid");
	if (nullptr == pThrower) return;
	int team = pThrower->GetTeam();
	if (team != ((CEntityInstance *)g_KillCamLastSwapped)->GetTeam()) return;
	int thrower = GetEntityIndex(pThrower);

	char prefixToken[40];
	_snprintf_s(prefixToken, _TRUNCATE, "#game_radio_team_prefix_%i", team);
	const char * pszPrefix = LocalizeOr(prefixToken, "");
	const char * pszName = SafeName(pThrower->GetSanitizedPlayerName());
	const char * pszText = LocalizeOr(pszToken, "Fire in the hole!");

	char place[32];
	char line[512];
	if (GetPlaceName(GetTargetPlayerPawn(thrower), place, sizeof(place))) {
		char placeToken[40];
		_snprintf_s(placeToken, _TRUNCATE, "#%s", place);
		const char * params[] = { pszName, LocalizeOr(placeToken, place), pszText, pszPrefix };
		FormatLocalized(line, sizeof(line), LocalizeOr("#Game_radio_location", " %s4%s1 @ %s2: %s3"), params, 4);
	} else {
		const char * params[] = { pszName, pszText, "", pszPrefix };
		FormatLocalized(line, sizeof(line), LocalizeOr("#Game_radio", " %s4%s1: %s2"), params, 4);
	}
	PrintChat(thrower - 1, line);
	PlayChatSound();
}

// Marks the pawns whose VO New_SosStartSoundEvent_Parse drops: the target's enemies.
static void MuteRadio_OnFrame() {
	static int s_marked[64];
	static int s_markedCount = 0;
	int marked[64];
	int markedCount = 0;
	bool bActive = g_PovMuteEnemyRadio && nullptr != g_Org_SosStartSoundEvent_Parse && nullptr != g_KillCamLastSwapped && 0 == g_KillCamSuspendFrames;
	if (bActive) {
		int team = ((CEntityInstance *)g_KillCamLastSwapped)->GetTeam();
		for (int i = 1; i <= 64 && markedCount < 64; ++i) {
			CEntityInstance * pController = GetEntity(i);
			if (nullptr == pController || !pController->IsPlayerController()) continue;
			int otherTeam = pController->GetTeam();
			if (otherTeam == team || (2 != otherTeam && 3 != otherTeam)) continue;
			int pawn = pController->GetPlayerPawnHandle().GetEntryIndex();
			if (pawn < 0 || c_MuteVoMaxEntities <= pawn) continue;
			g_MuteVoPawn[pawn] = 1;
			marked[markedCount++] = pawn;
		}
	}
	for (int i = 0; i < s_markedCount; ++i) {
		bool bStill = false;
		for (int j = 0; j < markedCount; ++j) if (marked[j] == s_marked[i]) { bStill = true; break; }
		if (!bStill) g_MuteVoPawn[s_marked[i]] = 0;
	}
	memcpy(s_marked, marked, sizeof(int) * markedCount);
	s_markedCount = markedCount;
	g_MuteVoActive = bActive;
}

// Money chat lines (CUserMessageTextMsg in the real game):
// - Personal awards (kill, team kill penalty, C4 plant / defuse): queued on the target's events,
//   printed when their money changes in the right direction (kills split if several share one
//   change; plant / defuse always the fixed amount).
// - Team income: printed right when the round ends (C_CSGameRules::m_iRoundWinStatus becomes the
//   winner), with the amount the server's CCSGameRules::AddTeamAccount gives for the round end
//   reason (cash_team_* convars; loss bonus from m_iNumConsecutive*Loses). The money itself comes
//   later and is absorbed without a line.
// - CTs' dead enemies award (cash_team_per_dead_enemy per dead T, at the server's pre restart):
//   printed when the next round starts.
// Like the server, no team income lines on the last round of a half or the match.
enum ChatAward_e { ChatAward_Kill = 0, ChatAward_TeamKill, ChatAward_Plant, ChatAward_Defuse };
struct ChatKill_t { int Tick; int Type; char Weapon[32]; };
const int c_ChatMaxKills = 8;
const int c_ChatKillTimeoutTicks = 32;
ChatKill_t g_ChatKills[c_ChatMaxKills];
int g_ChatKillCount = 0;
int g_ChatLastAccount = -1; // target's money last frame, -1 = unknown
void * g_ChatMoneyFor = nullptr;

// Round end reasons (CSRoundEndReason):
enum { RoundEnd_TargetBombed = 1, RoundEnd_BombDefused = 7, RoundEnd_CTsWin = 8, RoundEnd_TerroristsWin = 9,
	RoundEnd_HostagesRescued = 11, RoundEnd_TargetSaved = 12, RoundEnd_HostagesNotRescued = 13 };
struct ChatTeamIncome_t {
	bool Silent; // last round of a half / the match: the server prints no team income
	bool DeadTracking; // CT target: count dead Ts until the next round starts
	int DeadCount;
	int SwallowAmount; // team money still to come, absorbed without a line
	int SwallowDeadlineTick; // -1 = until the next round started
} g_ChatTeamIncome = { false, false, 0, 0, -1 };
int g_ChatLastWinStatus = -1; // -1 = unknown
const int c_ChatTeamIncomeTailTicks = 256; // after the next round started
const int c_ChatBombAward = 300; // cash_player_bomb_planted / cash_player_bomb_defused

static void ChatLines_ResetMoney() {
	g_ChatKillCount = 0;
	g_ChatLastAccount = -1;
	g_ChatTeamIncome.DeadTracking = false;
	g_ChatTeamIncome.SwallowAmount = 0;
	g_ChatTeamIncome.SwallowDeadlineTick = -1;
	g_ChatLastWinStatus = -1;
}

static void ChatLines_AddAward(int type, const char * pszWeapon) {
	if (c_ChatMaxKills <= g_ChatKillCount) return;
	ChatKill_t & award = g_ChatKills[g_ChatKillCount++];
	award.Tick = GetDemoTick();
	award.Type = type;
	strncpy_s(award.Weapon, nullptr != pszWeapon ? pszWeapon : "", _TRUNCATE);
}

static void ChatLines_OnDeath(SOURCESDK::CS2::IGameEvent * pEvent) {
	if (!g_PovChatMoney || nullptr == g_ChatPrintf || nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames) return;
	CEntityInstance * pAttacker = (CEntityInstance *)GetGameEventPlayerController(pEvent, "attacker");
	CEntityInstance * pVictim = (CEntityInstance *)GetGameEventPlayerController(pEvent, "userid");
	if (pAttacker != g_KillCamLastSwapped || nullptr == pVictim || pVictim == pAttacker) return;
	ChatLines_AddAward(pVictim->GetTeam() == pAttacker->GetTeam() ? ChatAward_TeamKill : ChatAward_Kill, StripWeaponPrefix(GetGameEventString(pEvent, "weapon")));
}

static void ChatLines_OnBombEvent(SOURCESDK::CS2::IGameEvent * pEvent, int type) {
	if (!g_PovChatMoney || nullptr == g_ChatPrintf || nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames) return;
	if (GetGameEventPlayerController(pEvent, "userid") != g_KillCamLastSwapped) return;
	ChatLines_AddAward(type, nullptr);
}

static const char * WeaponNameToken(const char * pszWeapon) {
	static const struct { const char * Weapon; const char * Token; } names[] = {
		{ "ak47", "#SFUI_WPNHUD_AK47" }, { "aug", "#SFUI_WPNHUD_Aug" }, { "awp", "#SFUI_WPNHUD_AWP" },
		{ "deagle", "#SFUI_WPNHUD_DesertEagle" }, { "elite", "#SFUI_WPNHUD_Elites" }, { "famas", "#SFUI_WPNHUD_Famas" },
		{ "fiveseven", "#SFUI_WPNHUD_FiveSeven" }, { "g3sg1", "#SFUI_WPNHUD_G3SG1" }, { "galilar", "#SFUI_WPNHUD_GalilAR" },
		{ "glock", "#SFUI_WPNHUD_Glock18" }, { "hkp2000", "#SFUI_WPNHUD_HKP2000" }, { "usp_silencer", "#SFUI_WPNHUD_USP_SILENCER" },
		{ "m249", "#SFUI_WPNHUD_M249" }, { "m4a1", "#SFUI_WPNHUD_M4A1" }, { "m4a1_silencer", "#SFUI_WPNHUD_M4_SILENCER" },
		{ "mac10", "#SFUI_WPNHUD_MAC10" }, { "mag7", "#SFUI_WPNHUD_Mag7" }, { "mp5sd", "#SFUI_WPNHUD_MP5SD" },
		{ "mp7", "#SFUI_WPNHUD_MP7" }, { "mp9", "#SFUI_WPNHUD_MP9" }, { "negev", "#SFUI_WPNHUD_Negev" },
		{ "nova", "#SFUI_WPNHUD_Nova" }, { "p250", "#SFUI_WPNHUD_P250" }, { "p90", "#SFUI_WPNHUD_P90" },
		{ "bizon", "#SFUI_WPNHUD_Bizon" }, { "revolver", "#SFUI_WPNHUD_REVOLVER" }, { "sawedoff", "#SFUI_WPNHUD_Sawedoff" },
		{ "scar20", "#SFUI_WPNHUD_SCAR20" }, { "sg556", "#SFUI_WPNHUD_SG556" }, { "ssg08", "#SFUI_WPNHUD_SSG08" },
		{ "taser", "#SFUI_WPNHUD_Taser" }, { "tec9", "#SFUI_WPNHUD_Tec9" }, { "ump45", "#SFUI_WPNHUD_UMP45" },
		{ "xm1014", "#SFUI_WPNHUD_xm1014" }, { "cz75a", "#SFUI_WPNHUD_CZ75" }, { "hegrenade", "#SFUI_WPNHUD_HE_Grenade" },
		{ "inferno", "#SFUI_WPNHUD_Molotov" }, { "molotov", "#SFUI_WPNHUD_Molotov" }, { "incgrenade", "#SFUI_WPNHUD_IncGrenade" },
		{ "flashbang", "#SFUI_WPNHUD_Flashbang" }, { "smokegrenade", "#SFUI_WPNHUD_SmokeGrenade" }, { "decoy", "#SFUI_WPNHUD_Decoy" },
	};
	for (const auto & name : names) if (0 == strcmp(pszWeapon, name.Weapon)) return name.Token;
	if (0 == strncmp(pszWeapon, "knife", 5) || 0 == strncmp(pszWeapon, "bayonet", 7)) return "#SFUI_WPNHUD_Knife";
	return nullptr;
}

static void ChatLines_PrintMoney(const char * pszToken, const char * pszFallback, int amount, const char * pszParam2 = nullptr) {
	char amountText[16];
	_snprintf_s(amountText, _TRUNCATE, "%i", amount < 0 ? -amount : amount);
	const char * params[] = { amountText, nullptr != pszParam2 ? pszParam2 : "" };
	char line[512];
	FormatLocalized(line, sizeof(line), LocalizeOr(pszToken, pszFallback), params, 2);
	PrintChat(-1, line);
}

static void ChatLines_PrintKill(const ChatKill_t & kill, int amount) {
	switch (kill.Type) {
	case ChatAward_TeamKill:
		ChatLines_PrintMoney("#Player_Cash_Award_Kill_Teammate", " -$%s1 penalty for killing a friendly", amount);
		return;
	case ChatAward_Plant:
		ChatLines_PrintMoney("#Player_Cash_Award_Bomb_Planted", " +$%s1 for planting the C4", amount);
		return;
	case ChatAward_Defuse:
		ChatLines_PrintMoney("#Player_Cash_Award_Bomb_Defused", " +$%s1 for defusing the C4", amount);
		return;
	}
	const char * pszWeaponToken = WeaponNameToken(kill.Weapon);
	const char * pszWeaponName = nullptr != pszWeaponToken ? Localize(pszWeaponToken) : nullptr;
	if (nullptr != pszWeaponName) ChatLines_PrintMoney("#Player_Cash_Award_Killed_Enemy", " +$%s1 for neutralizing an enemy with the %s2", amount, pszWeaponName);
	else ChatLines_PrintMoney("#Player_Cash_Award_Killed_Enemy_Generic", " +$%s1 for neutralizing an enemy", amount);
}

// Must not contain objects with destructors (SEH).
static int GetAccount(void * pController) {
	if (0 == g_clientDllOffsets.CCSPlayerController.m_pInGameMoneyServices || 0 == g_clientDllOffsets.CCSPlayerController_InGameMoneyServices.m_iAccount) return -1;
	__try {
		unsigned char * pServices = *(unsigned char **)((unsigned char *)pController + g_clientDllOffsets.CCSPlayerController.m_pInGameMoneyServices);
		if (nullptr == pServices) return -1;
		return *(int *)(pServices + g_clientDllOffsets.CCSPlayerController_InGameMoneyServices.m_iAccount);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

// Must not contain objects with destructors (SEH). Fields < 0 if unknown.
static bool GetRoundState(int & outWinStatus, int & outReason, int & outGamePhase) {
	outWinStatus = outReason = outGamePhase = -1;
	if (nullptr == g_DmgReport.ppGameRules) return false;
	__try {
		unsigned char * pGameRules = (unsigned char *)*g_DmgReport.ppGameRules;
		if (nullptr == pGameRules) return false;
		outWinStatus = *(int *)(pGameRules + g_DmgReport.OfsRoundWinStatus);
		if (0 != g_clientDllOffsets.C_CSGameRules.m_eRoundWinReason) outReason = *(int *)(pGameRules + g_clientDllOffsets.C_CSGameRules.m_eRoundWinReason);
		if (0 != g_clientDllOffsets.C_CSGameRules.m_gamePhase) outGamePhase = *(int *)(pGameRules + g_clientDllOffsets.C_CSGameRules.m_gamePhase);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Must not contain objects with destructors (SEH).
static bool IsHostageMap() {
	__try {
		int highest = GetHighestEntityIndex();
		for (int i = 65; i <= highest; ++i) {
			CEntityInstance * pEnt = GetEntity(i);
			if (nullptr == pEnt) continue;
			const char * pszClass = pEnt->GetClientClassName();
			if (nullptr != pszClass && 0 == strcmp(pszClass, "C_Hostage")) return true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

// Convar value (replicated ones like mp_maxrounds have the demo's value), or fallback.
// Must not contain objects with destructors (SEH).
static int GetCvarInt(const char * pszName, int fallback) {
	if (nullptr == SOURCESDK::CS2::g_pCVar) return fallback;
	__try {
		SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar(pszName, false);
		if (!handle.IsValid()) return fallback;
		SOURCESDK::CS2::Cvar_s * pCvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
		if (nullptr == pCvar) return fallback;
		switch (pCvar->m_eVarType) {
		case SOURCESDK::CS2::EConVarType_Bool: return pCvar->m_Value.m_bValue ? 1 : 0;
		case SOURCESDK::CS2::EConVarType_Int16: return pCvar->m_Value.m_i16Value;
		case SOURCESDK::CS2::EConVarType_UInt16: return pCvar->m_Value.m_u16Value;
		case SOURCESDK::CS2::EConVarType_Int32: return pCvar->m_Value.m_i32Value;
		case SOURCESDK::CS2::EConVarType_UInt32: return (int)pCvar->m_Value.m_u32Value;
		case SOURCESDK::CS2::EConVarType_Float32: return (int)pCvar->m_Value.m_flValue;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return fallback;
}

// Must not contain objects with destructors (SEH).
static int GetGameRulesInt(ptrdiff_t offset, int fallback) {
	if (0 == offset || nullptr == g_DmgReport.ppGameRules) return fallback;
	__try {
		unsigned char * pGameRules = (unsigned char *)*g_DmgReport.ppGameRules;
		if (nullptr == pGameRules) return fallback;
		return *(int *)(pGameRules + offset);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return fallback;
	}
}

// The winner's award for the round end reason (server defaults as fallback), 0 = none.
static int ChatLines_WinnerIncome(int reason, bool bTerrorist) {
	switch (reason) {
	case RoundEnd_TargetBombed: return GetCvarInt("cash_team_terrorist_win_bomb", 3500);
	case RoundEnd_BombDefused: return GetCvarInt("cash_team_win_by_defusing_bomb", 3250);
	case RoundEnd_CTsWin:
	case RoundEnd_TerroristsWin:
		if (IsHostageMap()) return bTerrorist ? GetCvarInt("cash_team_elimination_hostage_map_t", 1000) : GetCvarInt("cash_team_elimination_hostage_map_ct", 2000);
		return GetCvarInt("cash_team_elimination_bomb_map", 3250);
	case RoundEnd_TargetSaved: return GetCvarInt("cash_team_win_by_time_running_out_bomb", 3250);
	case RoundEnd_HostagesNotRescued: return GetCvarInt("cash_team_win_by_time_running_out_hostage", 3250);
	case RoundEnd_HostagesRescued: return GetCvarInt("cash_team_win_by_hostage_rescue", 3500);
	}
	return 0;
}

// Loss bonus like the server: cash_team_loser_bonus + cash_team_loser_bonus_consecutive_rounds *
// min(losses, mp_consecutive_loss_max), with the loss count from before this round. The client
// gets the already incremented count together with the round end (seen in a diag log), so - 1.
// (Teams start at 1, so losing the pistol round gives 1900.)
static int ChatLines_LoserBonus(bool bTerrorist) {
	int losses = GetGameRulesInt(bTerrorist ? g_clientDllOffsets.C_CSGameRules.m_iNumConsecutiveTerroristLoses : g_clientDllOffsets.C_CSGameRules.m_iNumConsecutiveCTLoses, 1) - 1;
	int cap = GetCvarInt("mp_consecutive_loss_max", 4);
	if (cap < losses) losses = cap;
	if (losses < 0) losses = 0;
	return GetCvarInt("cash_team_loser_bonus", 1400) + GetCvarInt("cash_team_loser_bonus_consecutive_rounds", 500) * losses;
}

// The server prints no team income on the last round of a half or the match (money resets).
static bool ChatLines_IsLastRoundOfHalfOrMatch(int gamePhase) {
	if (4 == gamePhase || 5 == gamePhase) return true; // halftime / match ended
	int played = GetGameRulesInt(g_clientDllOffsets.C_CSGameRules.m_totalRoundsPlayed, -1); // includes this round
	if (played <= 0) return false;
	int maxRounds = GetCvarInt("mp_maxrounds", 24);
	if (maxRounds <= 0) return false;
	if (played <= maxRounds) return played == maxRounds || (0 != GetCvarInt("mp_halftime", 1) && played == maxRounds / 2);
	int overtimeHalf = GetCvarInt("mp_overtime_maxrounds", 6) / 2;
	return 0 < overtimeHalf && 0 == (played - maxRounds) % overtimeHalf;
}

static void ChatLines_PrintTeamIncomeLines(int reason, bool bWon, bool bTerrorist, bool bAlive) {
	int swallow = 0;
	if (bWon) {
		int amount = ChatLines_WinnerIncome(reason, bTerrorist);
		swallow = amount;
		if (!g_ChatTeamIncome.Silent && 0 < amount) {
			switch (reason) {
			case RoundEnd_TargetBombed: ChatLines_PrintMoney("#Team_Cash_Award_T_Win_Bomb", " +$%s1 team income for detonating bomb", amount); break;
			case RoundEnd_BombDefused: ChatLines_PrintMoney("#Team_Cash_Award_Win_Defuse_Bomb", " +$%s1 team income for defusing the C4", amount); break;
			case RoundEnd_CTsWin:
			case RoundEnd_TerroristsWin:
				if (IsHostageMap()) ChatLines_PrintMoney("#Team_Cash_Award_Elim_Hostage", " +$%s1 team income for eliminating the enemy team", amount);
				else ChatLines_PrintMoney("#Team_Cash_Award_Elim_Bomb", " +$%s1 team income for eliminating the enemy team", amount);
				break;
			case RoundEnd_TargetSaved:
			case RoundEnd_HostagesNotRescued: ChatLines_PrintMoney("#Team_Cash_Award_Win_Time", " +$%s1 team income for running down the clock", amount); break;
			case RoundEnd_HostagesRescued: ChatLines_PrintMoney("#Team_Cash_Award_Win_Hostages_Rescue", " +$%s1 team income for rescuing the hostages", amount); break;
			}
		}
	} else if (bTerrorist && RoundEnd_TargetSaved == reason && bAlive) {
		// Ts still alive when the time ran out get nothing.
		if (!g_ChatTeamIncome.Silent) PrintChat(-1, LocalizeOr("#Team_Cash_Award_No_Income", " +$0 penalty for running out of time"));
	} else {
		int bonus = ChatLines_LoserBonus(bTerrorist);
		swallow = bonus;
		if (!g_ChatTeamIncome.Silent && 0 < bonus) ChatLines_PrintMoney("#Team_Cash_Award_Loser_Bonus", " +$%s1 team income for losing", bonus);
		if (bTerrorist && RoundEnd_BombDefused == reason) {
			int planted = GetCvarInt("cash_team_planted_bomb_but_defused", 600);
			swallow += planted;
			if (!g_ChatTeamIncome.Silent && 0 < planted) ChatLines_PrintMoney("#Team_Cash_Award_Planted_Bomb_But_Defused", " +$%s1 team income for planting the bomb", planted);
		}
	}
	g_ChatTeamIncome.SwallowAmount += swallow;
	g_ChatTeamIncome.SwallowDeadlineTick = -1;
	DiagLog("mirv_pov money: round end reason %i won %i T %i alive %i -> team income %i (losses CT %i T %i, silent %i)\n",
		reason, bWon ? 1 : 0, bTerrorist ? 1 : 0, bAlive ? 1 : 0, swallow,
		GetGameRulesInt(g_clientDllOffsets.C_CSGameRules.m_iNumConsecutiveCTLoses, -1),
		GetGameRulesInt(g_clientDllOffsets.C_CSGameRules.m_iNumConsecutiveTerroristLoses, -1), g_ChatTeamIncome.Silent ? 1 : 0);
}

// Must not contain objects with destructors (SEH).
static bool IsAlive(void * pPawn) {
	if (nullptr == pPawn) return false;
	__try {
		return 0 < ((CEntityInstance *)pPawn)->GetHealth();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static int ChatLines_CountDeadTerrorists() {
	int dead = 0;
	for (int i = 1; i <= 64; ++i) {
		CEntityInstance * pController = GetEntity(i);
		if (nullptr == pController || !pController->IsPlayerController() || 2 != pController->GetTeam()) continue;
		if (!IsAlive(GetTargetPlayerPawn(i))) ++dead;
	}
	return dead;
}

// Resolves the ICU style "{num_enemies}" and "{num_enemies, plural, one{..} other{..}}" in text.
static std::string ApplyNumEnemies(const std::string & text, int count) {
	std::string result;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t open = text.find("{num_enemies", pos);
		if (std::string::npos == open) break;
		result.append(text, pos, open - pos);
		// Find the matching close brace.
		int depth = 0;
		size_t close = open;
		for (; close < text.size(); ++close) {
			if ('{' == text[close]) ++depth;
			else if ('}' == text[close] && 0 == --depth) break;
		}
		if (text.size() <= close) {
			pos = open;
			break;
		}
		std::string inner = text.substr(open + 1, close - open - 1);
		if (inner == "num_enemies") result += std::to_string(count);
		else {
			const char * pszForm = 1 == count ? "one{" : "other{";
			size_t form = inner.find(pszForm);
			if (std::string::npos != form) {
				size_t start = form + strlen(pszForm);
				size_t end = inner.find('}', start);
				if (std::string::npos != end) result.append(inner, start, end - start);
			}
		}
		pos = close + 1;
	}
	result.append(text, pos, std::string::npos);
	return result;
}

static void ChatLines_PrintDeadEnemies(int amount, int count) {
	char amountText[16];
	_snprintf_s(amountText, _TRUNCATE, "%i", amount);
	const char * params[] = { amountText };
	char line[512];
	FormatLocalized(line, sizeof(line), LocalizeOr("#Team_Cash_Award_Dead_Terrorists:f",
		" +$%s1 team income for {num_enemies} eliminated {num_enemies, plural, one{terrorist} other{terrorists}}"), params, 1);
	PrintChat(-1, ApplyNumEnemies(line, count).c_str());
}

// Round end: team income lines. Next round start: CTs' dead enemies line.
static void ChatLines_OnRoundState(int tick) {
	int winStatus, reason, gamePhase;
	if (!GetRoundState(winStatus, reason, gamePhase)) return;
	if (g_ChatLastWinStatus < 0) {
		g_ChatLastWinStatus = winStatus;
		return;
	}
	if (0 != winStatus && g_ChatTeamIncome.DeadTracking) g_ChatTeamIncome.DeadCount = ChatLines_CountDeadTerrorists(); // until the server counts them (pre restart)
	if (winStatus == g_ChatLastWinStatus) return;
	if (0 == g_ChatLastWinStatus && (2 == winStatus || 3 == winStatus)) { // round won
		int team = ((CEntityInstance *)g_KillCamLastSwapped)->GetTeam();
		if (2 == team || 3 == team) {
			g_ChatTeamIncome.Silent = ChatLines_IsLastRoundOfHalfOrMatch(gamePhase);
			ChatLines_PrintTeamIncomeLines(reason, winStatus == team, 2 == team, IsAlive(GetSwappedTargetPlayerPawn()));
			g_ChatTeamIncome.DeadTracking = 3 == team;
			g_ChatTeamIncome.DeadCount = g_ChatTeamIncome.DeadTracking ? ChatLines_CountDeadTerrorists() : 0;
		}
	} else if (0 == winStatus) { // next round started
		if (g_ChatTeamIncome.DeadTracking) {
			g_ChatTeamIncome.DeadTracking = false;
			int amount = g_ChatTeamIncome.DeadCount * GetCvarInt("cash_team_per_dead_enemy", 50);
			if (0 < amount) {
				g_ChatTeamIncome.SwallowAmount += amount;
				if (!g_ChatTeamIncome.Silent) ChatLines_PrintDeadEnemies(amount, g_ChatTeamIncome.DeadCount);
			}
		}
		if (0 < g_ChatTeamIncome.SwallowAmount) g_ChatTeamIncome.SwallowDeadlineTick = tick + c_ChatTeamIncomeTailTicks;
	}
	g_ChatLastWinStatus = winStatus;
}

// Once per rendered frame (not while suspended).
static void ChatLines_OnFrame() {
	if (g_ChatMoneyFor != g_KillCamLastSwapped) {
		g_ChatMoneyFor = g_KillCamLastSwapped;
		ChatLines_ResetMoney();
	}
	if (nullptr == g_KillCamLastSwapped || !g_PovChatMoney || nullptr == g_ChatPrintf) return;
	static int s_lastTick = -1;
	int tick = GetDemoTick();
	if (tick < s_lastTick) ChatLines_ResetMoney(); // seeked back
	s_lastTick = tick;
	ChatLines_OnRoundState(tick);
	int account = GetAccount(g_KillCamLastSwapped);
	if (account < 0) return;
	if (g_PovDiag) {
		// When do money, round state and loss counters change relative to each other?
		static int s_last[5] = { -2, -2, -2, -2, -2 };
		int winStatus, reason, gamePhase;
		GetRoundState(winStatus, reason, gamePhase);
		int now[5] = { account, winStatus,
			GetGameRulesInt(g_clientDllOffsets.C_CSGameRules.m_iNumConsecutiveCTLoses, -1),
			GetGameRulesInt(g_clientDllOffsets.C_CSGameRules.m_iNumConsecutiveTerroristLoses, -1),
			GetGameRulesInt(g_clientDllOffsets.C_CSGameRules.m_totalRoundsPlayed, -1) };
		if (0 != memcmp(now, s_last, sizeof(now))) {
			DiagLog("mirv_pov money: tick %i account %i (%+i) | win status %i reason %i phase %i | losses CT %i T %i | rounds played %i | swallow %i\n",
				tick, account, -2 != s_last[0] ? account - s_last[0] : 0, winStatus, reason, gamePhase, now[2], now[3], now[4], g_ChatTeamIncome.SwallowAmount);
			memcpy(s_last, now, sizeof(now));
		}
	}
	if (0 <= g_ChatLastAccount && account != g_ChatLastAccount) {
		int delta = account - g_ChatLastAccount;
		int kills = 0, teamKills = 0, bombAwards = 0;
		for (int i = 0; i < g_ChatKillCount; ++i) {
			switch (g_ChatKills[i].Type) {
			case ChatAward_Kill: ++kills; break;
			case ChatAward_TeamKill: ++teamKills; break;
			default: ++bombAwards; break;
			}
		}
		int remainder = delta;
		if (0 < delta) {
			// C4 plant / defuse: fixed cash_player_bomb_planted / _defused, printed even if the
			// money is capped, like the server does.
			if (0 < bombAwards) {
				int bombSum = 0;
				int kept = 0;
				for (int i = 0; i < g_ChatKillCount; ++i) {
					int amount = -1;
					if (ChatAward_Plant == g_ChatKills[i].Type) amount = GetCvarInt("cash_player_bomb_planted", c_ChatBombAward);
					else if (ChatAward_Defuse == g_ChatKills[i].Type) amount = GetCvarInt("cash_player_bomb_defused", c_ChatBombAward);
					if (0 <= amount) {
						ChatLines_PrintKill(g_ChatKills[i], amount);
						bombSum += amount;
					} else g_ChatKills[kept++] = g_ChatKills[i];
				}
				g_ChatKillCount = kept;
				remainder = delta - bombSum;
				if (remainder < 0) remainder = 0;
			}
			// Team money we already printed at the round end:
			if (0 < remainder && 0 < g_ChatTeamIncome.SwallowAmount) {
				int take = remainder < g_ChatTeamIncome.SwallowAmount ? remainder : g_ChatTeamIncome.SwallowAmount;
				g_ChatTeamIncome.SwallowAmount -= take;
				remainder -= take;
			}
		}
		// Kill rewards add money, team kills cost it; anything else (buying) isn't for us.
		bool bPositive = 0 < remainder;
		int count = bPositive ? kills : (remainder < 0 ? teamKills : 0);
		if (0 < count) {
			int wanted = bPositive ? ChatAward_Kill : ChatAward_TeamKill;
			int kept = 0;
			for (int i = 0; i < g_ChatKillCount; ++i) {
				if (wanted == g_ChatKills[i].Type) ChatLines_PrintKill(g_ChatKills[i], remainder / count);
				else g_ChatKills[kept++] = g_ChatKills[i];
			}
			g_ChatKillCount = kept;
		}
	}
	g_ChatLastAccount = account;
	// No money change (e.g. at max money): no line.
	int kept = 0;
	for (int i = 0; i < g_ChatKillCount; ++i) {
		if (0 <= tick && g_ChatKills[i].Tick + c_ChatKillTimeoutTicks < tick) continue;
		g_ChatKills[kept++] = g_ChatKills[i];
	}
	g_ChatKillCount = kept;
	if (0 <= g_ChatTeamIncome.SwallowDeadlineTick && g_ChatTeamIncome.SwallowDeadlineTick < tick) {
		g_ChatTeamIncome.SwallowAmount = 0; // e.g. capped money
		g_ChatTeamIncome.SwallowDeadlineTick = -1;
	}
}

// Must not contain objects with destructors (SEH).
static bool ShowDeathCard() {
	__try {
		void * pElement = g_FindHudElement("CCSGO_HudDeathPanel");
		if (nullptr == pElement) return false;
		g_DeathPanel_SetPendingDamage((unsigned char *)pElement - 0x20, 0, 0, 0, 0, 0, 0);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Returns true if this is a player_death of the swapped target. Must not keep pEvent.
bool MirvPov_BeforeClientGameEvent(SOURCESDK::CS2::IGameEvent * pEvent) {
	if (nullptr == pEvent) return false;
	const char * pszName = pEvent->GetName();
	if (nullptr == pszName) return false;

	DamageReport_OnGameEvent(pEvent, pszName);

	if (0 == strcmp(pszName, "player_hurt")) {
		DamageArcs_OnHurt(pEvent);
		Effects_OnHurt(pEvent);
		return false;
	}

	if (0 == strcmp(pszName, "bomb_exploded")) {
		Effects_OnBombExploded();
		return false;
	}

	if (0 == strcmp(pszName, "flashbang_detonate")) {
		Effects_OnFlashDetonate(pEvent);
		return false;
	}

	if (0 == strcmp(pszName, "player_blind")) {
		Effects_OnPlayerBlind(pEvent);
		return false;
	}

	if (0 == strcmp(pszName, "spec_target_updated")) {
		FixSpecTargetEvent(pEvent);
		return false;
	}

	if (0 == strcmp(pszName, "weapon_fire")) {
		ChatLines_OnWeaponFire(pEvent);
		return false;
	}

	if (0 == strcmp(pszName, "bomb_planted")) {
		ChatLines_OnBombEvent(pEvent, ChatAward_Plant);
		return false;
	}

	if (0 == strcmp(pszName, "bomb_defused")) {
		ChatLines_OnBombEvent(pEvent, ChatAward_Defuse);
		return false;
	}

	if (0 == strcmp(pszName, "player_death")) ChatLines_OnDeath(pEvent);

	if (!g_PovDeathCard || nullptr == g_KillCamLastSwapped || 0 < g_KillCamSuspendFrames
		|| nullptr == g_FindHudElement || nullptr == g_DeathPanel_SetPendingDamage) return false;

	if (0 != strcmp(pszName, "player_death")) return false;

	return GetGameEventPlayerController(pEvent, "userid") == g_KillCamLastSwapped;
}

// After the native listeners (incl. the death panel's player_death handler) ran.
void MirvPov_AfterClientGameEvent(bool bBeforeResult) {
	if (!bBeforeResult || nullptr == g_KillCamLastSwapped) return;
	if (ShowDeathCard()) DiagLog("mirv_pov: POV target died, death card triggered.\n");
	else advancedfx::Warning("mirv_pov: POV target died, but triggering the death card failed.\n");
}

void MirvPov_OnLevelInitPreEntity() {
	// Entities are already gone here, so don't call into the game, only reset the override:
	KillCam_Suspend("level load", false, c_KillCamResumeDelayFrames);
	DamageReport_Reset(true);
}

static void MirvPov_Info() {
	char path[MAX_PATH + 32] = {};
	DWORD len = GetTempPathA(MAX_PATH, path);
	if (0 < len && len < MAX_PATH) strcat_s(path, "mirv_pov_info.txt");
	else path[0] = '\0';
	if ('\0' != path[0] && 0 != fopen_s(&g_InfoFile, path, "w")) g_InfoFile = nullptr;
	PrintInfo();
	if (nullptr != g_InfoFile) {
		fclose(g_InfoFile);
		g_InfoFile = nullptr;
		advancedfx::Message("mirv_pov: info also written to %s\n", path);
	}
}

static void MirvPov_List() {
	advancedfx::Message("Players (index : team : name):\n");
	int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance * pEnt = GetEntity(i);
		if (nullptr == pEnt || !pEnt->IsPlayerController()) continue;
		int team = pEnt->GetTeam();
		advancedfx::Message("%3i : %s : %s%s\n", i, 2 == team ? "T " : 3 == team ? "CT" : "--",
			SafeName(pEnt->GetSanitizedPlayerName()), i == g_KillCamTarget ? "  <- mirv_pov" : "");
	}
}

static void MirvPov_SetTarget(int target) {
	if (!g_Client.Ok) {
		advancedfx::Warning("mirv_pov: not supported by this CS2 version (client.dll addresses not found).\n");
		return;
	}
	if (0 < target) {
		CEntityInstance * pEnt = GetEntity(target);
		if (nullptr == pEnt || !pEnt->IsPlayerController()) {
			advancedfx::Warning("mirv_pov: %i is not a player, see mirv_pov list.\n", target);
			return;
		}
		// Restore first (if switching between targets), let spec_player apply, then swap:
		bool bWasSwapped = nullptr != g_KillCamLastSwapped;
		if (0 == g_KillCamTarget) g_KillCamTarget = target;
		KillCam_Suspend(nullptr, bWasSwapped, c_KillCamSpecFrames);
		g_KillCamTarget = target;
		advancedfx::Message("mirv_pov: switching to %s (%i).\n", SafeName(pEnt->GetSanitizedPlayerName()), target);
	} else {
		g_KillCamTarget = 0;
		g_KillCamLastSwapped = nullptr;
		g_KillCamSuspendFrames = 0;
		if (KillCam_Restore()) {
			InvalidateSpecTargetCache(); // spec_target_updated also refreshes player glow (spectator x-ray back on)
			advancedfx::Message("mirv_pov: off.\n");
		}
		else advancedfx::Warning("mirv_pov: restore failed, local player controller is %i.\n", GetEntityIndex(GetLocalPlayerController()));
	}
}

CON_COMMAND(mirv_pov, "Watch a demo from a player's own first person view, with their HUD (CS2).")
{
	int argc = args->ArgC();
	const char * arg0 = args->ArgV(0);

	if (2 <= argc) {
		const char * cmd = args->ArgV(1);
		if (0 == _stricmp(cmd, "list")) { MirvPov_List(); return; }
		if (0 == _stricmp(cmd, "info")) { MirvPov_Info(); return; }
		if ('0' <= cmd[0] && cmd[0] <= '9') { MirvPov_SetTarget(atoi(cmd)); return; }
	}

	advancedfx::Message(
		"%s <index> - Watch the demo as that player (index from %s list). Can switch directly between players.\n"
		"%s 0 - Back to normal demo spectating.\n"
		"%s list - List players with their index.\n"
		"%s info - Status and diagnostics (also written to %%TEMP%%\\mirv_pov_info.txt).\n"
		"Advanced switches: mirv_pov_debug\n"
		"Current: %i\n"
		, arg0, arg0, arg0, arg0, arg0, g_KillCamTarget
	);
}

CON_COMMAND(mirv_pov_debug, "Advanced / diagnostic switches for mirv_pov.")
{
	int argc = args->ArgC();
	const char * arg0 = args->ArgV(0);

	if (2 <= argc) {
		const char * cmd = args->ArgV(1);

		if (0 == _stricmp(cmd, "hltv") && 3 <= argc) {
			if (nullptr == g_Engine.IsHLTV) {
				advancedfx::Warning("mirv_pov: CEngineClient::IsHLTV hook not installed.\n");
				return;
			}
			g_HltvSpoofMode = atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: IsHLTV spoof mode %i (0 = off, 1 = all client callers, 2 = FrameStageNotify cache only).\n", g_HltvSpoofMode);
			return;
		}

		if (0 == _stricmp(cmd, "crosshair") && 3 <= argc) {
			if (nullptr == g_Client.ApplyOwnCrosshair) {
				advancedfx::Warning("mirv_pov: crosshair hook not installed.\n");
				return;
			}
			g_PovCrosshair = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: target crosshair %s.\n", g_PovCrosshair ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "buymenu") && 3 <= argc) {
			g_PovSuppressBuyMenu = 0 == atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: recorded buy menu state %s.\n", g_PovSuppressBuyMenu ? "suppressed (no blur)" : "kept");
			return;
		}

		if (0 == _stricmp(cmd, "predict") && 3 <= argc) {
			if (nullptr == g_Org_SetPredictable) {
				advancedfx::Warning("mirv_pov: SetPredictable hook not installed.\n");
				return;
			}
			g_PovNoPredict = 0 == atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: target prediction %s (takes effect on next switch).\n", g_PovNoPredict ? "blocked (recorded values)" : "allowed (game default)");
			return;
		}

		if (0 == _stricmp(cmd, "deathcard") && 3 <= argc) {
			if (nullptr == g_Org_GetSplitScreenPlayerPawn) {
				advancedfx::Warning("mirv_pov: death panel hook not installed.\n");
				return;
			}
			g_PovDeathCard = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: death card %s.\n", g_PovDeathCard ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "damagereport") && 3 <= argc) {
			if (nullptr == g_DmgReport.Report) {
				advancedfx::Warning("mirv_pov: damage report hook not installed.\n");
				return;
			}
			if (0 == _stricmp(args->ArgV(2), "show")) {
				DamageReport_Publish();
				advancedfx::Message("mirv_pov: damage report result %i (records, 0 = nothing collected / not in POV, -1 = failed, -2 = demo has its own).\n", g_DmgLastResult);
				return;
			}
			g_PovDamageReport = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: round end damage report %s.\n", g_PovDamageReport ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "scopeblur") && 3 <= argc) {
			if (nullptr == g_UpdateScopeInaccuracy) {
				advancedfx::Warning("mirv_pov: scope inaccuracy update not found.\n");
				return;
			}
			g_PovScopeBlur = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: scope blur update %s.\n", g_PovScopeBlur ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "damagearcs") && 3 <= argc) {
			if (nullptr == g_DamageIndicator_ShowDirection) {
				advancedfx::Warning("mirv_pov: damage direction hook not installed.\n");
				return;
			}
			g_PovDamageArcs = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: damage direction arcs %s.\n", g_PovDamageArcs ? "on" : "off");
			return;
		}

		if ((0 == _stricmp(cmd, "bombshake") || 0 == _stricmp(cmd, "earringing")) && 3 <= argc) {
			bool bOn = 0 != atoi(args->ArgV(2));
			if (0 == _stricmp(cmd, "bombshake")) g_PovBombShake = bOn;
			else g_PovEarRinging = bOn;
			advancedfx::Message("mirv_pov: %s %s.\n", 0 == _stricmp(cmd, "bombshake") ? "C4 explosion screen shake" : "flash / HE ear ringing", bOn ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "spectools") && 3 <= argc) {
			if (nullptr == g_Org_SpectatorToolsEnabled) {
				advancedfx::Warning("mirv_pov: spectator tools hook not installed.\n");
				return;
			}
			g_PovNoSpectatorTools = 0 == atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: spectator tools (x-ray) while in POV %s.\n", g_PovNoSpectatorTools ? "blocked" : "allowed");
			return;
		}

		if (0 == _stricmp(cmd, "enemyradio") && 3 <= argc) {
			if (nullptr == g_Org_SosStartSoundEvent_Parse) {
				advancedfx::Warning("mirv_pov: sound event hook not installed.\n");
				return;
			}
			g_PovMuteEnemyRadio = 0 == atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: enemy team radio voice %s.\n", g_PovMuteEnemyRadio ? "muted" : "audible");
			return;
		}

		if ((0 == _stricmp(cmd, "radio") || 0 == _stricmp(cmd, "killreward")) && 3 <= argc) {
			if (nullptr == g_ChatPrintf) {
				advancedfx::Warning("mirv_pov: chat print not found.\n");
				return;
			}
			bool bOn = 0 != atoi(args->ArgV(2));
			if (0 == _stricmp(cmd, "radio")) g_PovChatRadio = bOn;
			else g_PovChatMoney = bOn;
			advancedfx::Message("mirv_pov: %s chat lines %s.\n", 0 == _stricmp(cmd, "radio") ? "grenade radio" : "kill reward", bOn ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "spectarget") && 3 <= argc) {
			if (g_GameEventSetEntityIndex < 0) {
				advancedfx::Warning("mirv_pov: spec target fix not available.\n");
				return;
			}
			g_PovSpecTarget = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: demo UI spec target fix %s.\n", g_PovSpecTarget ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "chatlog") && 3 <= argc) {
			if (nullptr == g_Org_HudChat_SayText2) {
				advancedfx::Warning("mirv_pov: chat hook not installed.\n");
				return;
			}
			g_ChatLog = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: chat delivery logging %s.\n", g_ChatLog ? "on" : "off");
			return;
		}

		if (0 == _stricmp(cmd, "hltvlog")) {
			if (3 <= argc) {
				g_HltvLog = 0 != atoi(args->ArgV(2));
				if (g_HltvLog) g_HltvLogCount = 0;
				advancedfx::Message("mirv_pov: IsHLTV caller logging %s.\n", g_HltvLog ? "on (reset)" : "off");
			} else HltvLog_Print();
			return;
		}

		if (0 == _stricmp(cmd, "diag") && 3 <= argc) {
			g_PovDiag = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: diagnostic logging %s (console + %%TEMP%%\\mirv_pov_log.txt).\n", g_PovDiag ? "on" : "off");
			return;
		}
	}

	advancedfx::Message(
		"%s hltv <0|1|2> - Make the client think it's not HLTV while in POV: 0 off, 1 all client callers (default), 2 cached flag only.\n"
		"%s crosshair <0|1> - Use the player's crosshair code instead of your cl_crosshair* settings (default 1).\n"
		"%s buymenu <0|1> - 1 keeps the recorded buy menu state (view gets blurred), 0 suppresses it (default 0).\n"
		"%s predict <0|1> - 0 keeps the player's pawn / weapons non-predicted (default), 1 = game default.\n"
		"%s deathcard <0|1> - Show the \"killed you with\" death card (default 1).\n"
		"%s damagereport <0|1|show> - Round end damage report under the avatars (default 1), show = now, with what's collected so far.\n"
		"%s damagearcs <0|1> - Damage direction arcs when the player gets hit (default 1).\n"
		"%s scopeblur <0|1> - Update the scope blur from the player's weapon inaccuracy (default 1).\n"
		"%s radio <0|1> - Grenade radio chat lines of the player's team (default 1).\n"
		"%s killreward <0|1> - Kill reward chat lines (default 1).\n"
		"%s bombshake <0|1> - C4 explosion screen shake (default 1). Grenades don't shake the screen in CS2.\n"
		"%s earringing <0|1> - Ear ringing after flashbangs and HE hits (default 1).\n"
		"%s spectools <0|1> -1 allows spectator tools like x-ray while in POV (default 0).\n"
		"%s enemyradio <0|1> -1 keeps the enemy team's radio voice lines audible (default 0, muted like for a player).\n"
		"%s spectarget <0|1> - Make the demo UI's spectated player (and its markers) the POV player, also while dead (default 1).\n"
		"%s chatlog <0|1> - Log chat (SayText2) messages arriving at the chat HUD.\n"
		"%s hltvlog [0|1] - IsHLTV() caller logging on (resets) / off, without argument prints the callers.\n"
		"%s diag <0|1> - Diagnostic logging to console and %%TEMP%%\\mirv_pov_log.txt (default 0).\n"
		, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0
	);
}
