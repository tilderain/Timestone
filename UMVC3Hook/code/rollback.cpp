// Top-level rollback hook initialization.
#include "../pch.h"

#include "rollback.h"
#include "rollback_effect.h"
#include "rollback_resim.h"
#include "rollback_rng.h"
#include "rollback_shot.h"

void Rollback_InitHooks()
{
    Rollback_InitReSimulationHooks();
    Rollback_InitRngHooks();
    Effect_InitHooks();
    Shot_InitHooks();
}
