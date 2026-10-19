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

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

#include "../deps/release/Detours/src/detours.h"

#include <cfloat>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <algorithm>
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
		InfoPrintf("Damage arcs: hook %s, on %i\n", nullptr != g_DamageIndicator_ShowDirection ? "OK" : "MISSING", g_PovDamageArcs ? 1 : 0);
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

void MirvPov_BeforeFrameStageNotify(int curStage) {
	SetScoreboardPatch(0 != g_HltvSpoofMode && nullptr != g_KillCamLastSwapped && 0 == g_KillCamSuspendFrames);

	if (0 < g_KillCamSuspendFrames) {
		if (SOURCESDK::CS2::FRAME_RENDER_PASS == curStage) {
			KillCam_OnSuspendedFrame();
			if (0 == --g_KillCamSuspendFrames) g_KillCamSeekTargetTick = -1;
		}
		return;
	}
	KillCam_Apply(false);
	if (g_PovSuppressBuyMenu && nullptr != g_KillCamLastSwapped) SuppressTargetBuyMenu();
	if (SOURCESDK::CS2::FRAME_RENDER_PASS == curStage) DamageReport_OnFrame();

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
		return false;
	}

	if (0 == strcmp(pszName, "spec_target_updated")) {
		FixSpecTargetEvent(pEvent);
		return false;
	}

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
		if (KillCam_Restore()) advancedfx::Message("mirv_pov: off.\n");
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

		if (0 == _stricmp(cmd, "damagearcs") && 3 <= argc) {
			if (nullptr == g_DamageIndicator_ShowDirection) {
				advancedfx::Warning("mirv_pov: damage direction hook not installed.\n");
				return;
			}
			g_PovDamageArcs = 0 != atoi(args->ArgV(2));
			advancedfx::Message("mirv_pov: damage direction arcs %s.\n", g_PovDamageArcs ? "on" : "off");
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
		"%s spectarget <0|1> -Make the demo UI's spectated player (and its markers) the POV player, also while dead (default 1).\n"
		"%s chatlog <0|1> - Log chat (SayText2) messages arriving at the chat HUD.\n"
		"%s hltvlog [0|1] - IsHLTV() caller logging on (resets) / off, without argument prints the callers.\n"
		"%s diag <0|1> - Diagnostic logging to console and %%TEMP%%\\mirv_pov_log.txt (default 0).\n"
		, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0
	);
}
