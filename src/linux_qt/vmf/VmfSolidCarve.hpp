#pragma once

// Convex-solid subtraction, the geometry half of the Carve tool.
//
// Hammer's carve (CMapSolid::Carve, hammer/mapsolid.cpp) clips the target
// against every plane of the carver in side order, unconditionally. Planes
// that do not even cut the target still get a clip call, the arbitrary side
// order produces long thin fragments, and targets that merely touch the
// carver get rebuilt (and their textures scrambled) for nothing. This port
// deviates deliberately:
//
//   * Targets that do not overlap the carver's volume are left byte-identical
//     untouched — a face-touching neighbour classifies Front of the shared
//     plane and is rejected before any geometry is rebuilt.
//   * Only planes that actually cut the shrinking remainder produce pieces,
//     so the fragment count is the minimum this plane-at-a-time construction
//     can give, not the carver's side count.
//   * The cutting order is chosen greedily by volume: the plane whose outside
//     chunk is biggest cuts first. Big box-like pieces are split off early
//     and the inevitable small pieces cluster near the carved hole instead of
//     running the full length of the target as slivers.

#include "VmfDocument.hpp"
#include "VmfSolidClip.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace hammer::vmf {

// Volume of a convex VMF solid, from its rebuilt face polygons. Returns 0.0
// for degenerate solids.
double solidVolume(const Block& solid);

// Subtracts the carver's convex volume from the target.
//   std::nullopt   -> the volumes do not overlap; leave the target alone.
//   empty vector   -> the target lies entirely inside the carver; delete it.
//   otherwise      -> the convex pieces that replace the target. Every piece
//                     and its sides carry freshly allocated ids; uncut faces
//                     keep their original texturing (see clipSolid).
std::optional<std::vector<Block>> carveSolid(const Block& target, const Block& carver,
                                             const std::function<int()>& allocateId);

} // namespace hammer::vmf
