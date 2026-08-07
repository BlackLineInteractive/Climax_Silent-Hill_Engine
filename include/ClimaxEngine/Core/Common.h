#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClimaxEngine/Core/Common.h  —  BACKWARD-COMPAT forwarding header
//
// Toolkit code that already includes Common.h continues to work unchanged.
// New code in climax-core / climax-game should include Types.h directly.
// New code in climax-render / toolkit should include Render/ViewerState.h.
// ─────────────────────────────────────────────────────────────────────────────

#include "ClimaxEngine/Core/Types.h"          // pure data, no GL
#include "ClimaxEngine/Render/ViewerState.h"  // GL globals, ViewerState
