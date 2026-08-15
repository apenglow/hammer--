#pragma once

#include "GameFileSystem.hpp"
#include "VmfDocument.hpp"

#include <string>
#include <vector>

namespace hammer::collab {

// Wire-path safety for shared assets. The remote peer is untrusted (anyone
// who can reach the session port joins), so every path from the network must
// pass this before it is read or written: relative, forward slashes, no
// drive letters, no "." / ".." segments, and confined to the two asset roots
// the feature actually shares. The path is expected in normalized form
// (lowercase, forward slashes) as produced by collectCustomAssetPaths.
bool isSafeAssetPath(const std::string& path);

// Every file the map needs that resolves to loose "custom" content on this
// peer (GameFileSystem pathId "custom": gameinfo custom/* mounts and override
// mounts) — the files a collaborator cannot get from the stock game:
//   - each used material's VMT, the textures it references, and one level of
//     "include" patch VMTs
//   - each used model's .mdl and its companion files
// Paths come back normalized (lowercase, forward slashes) and deduplicated.
// VPK-sourced and base-game files are by definition not custom and excluded.
std::vector<std::string> collectCustomAssetPaths(const hammer::vmf::Document& document,
                                                 const hammer::assets::GameFileSystem& fs);

// The material/model names a document references, WITHOUT touching the file
// system. collectCustomAssetPaths is filesystem-bound (a missing file walks
// every search path, enumerating directories), which is far too expensive to
// run per edit; callers use this first to skip the scan entirely when an edit
// introduces no new asset reference.
std::vector<std::string> collectAssetReferences(const hammer::vmf::Document& document);

} // namespace hammer::collab
