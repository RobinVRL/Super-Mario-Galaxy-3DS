// SPDX-License-Identifier: CC0-1.0
#include "smg3ds/petari_bridge.h"

/*
 * Petari MR::isNearZero(f32, f32), isolated from MathUtil.cpp.
 *
 * Source provenance:
 *   Petari src/Game/Util/MathUtil.cpp at d38735a69037352c0eaffabadbf296495952e599
 *   Petari RMGE01 history at e8d8a89963f9a694d1e7bddad12fa3fe1f81451e
 *
 * Petari is CC0-1.0. The project-owned name deliberately avoids relying on
 * Metrowerks C++ mangling or claiming the original guest linker symbol.
 */
bool smg3ds_petari_mr_is_near_zero_f32_f32(float x, float tolerance)
{
    if (x < 0.0f)
        x = -x;

    if (x < tolerance)
        return true;

    return false;
}
