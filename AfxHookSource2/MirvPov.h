#pragma once

// mirv_pov (CS2): watch a demo from a player's own POV by making that player the
// client's local player controller, see doc/notes_cs2/mirv_pov.md.

#include <Windows.h>

void MirvPov_HookClientDll(HMODULE clientDll);

void MirvPov_HookEngine2Dll(HMODULE engine2Dll);

void MirvPov_BeforeFrameStageNotify(int curStage);

void MirvPov_OnLevelInitPreEntity();

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

// Around CGameEventManager::FireEventClientSide. The event is freed by the original call,
// so it may only be inspected in Before; After gets what Before returned.
bool MirvPov_BeforeClientGameEvent(SOURCESDK::CS2::IGameEvent * pEvent);
void MirvPov_AfterClientGameEvent(bool bBeforeResult);
