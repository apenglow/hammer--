#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
material = (root / "src/linux_qt/assets/MaterialSystem.cpp").read_text()
tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()

required = {
    "variables[target.name] = {};": 1,
    "else invalidateResult(target);": 1,
    '"YellowLevel" { "resultVar" "$yellow" }': 1,
    '"Equals" { "srcVar1" "$yellow" "resultVar" "$color2" }': 1,
    "assert(!heavyPlayerMaterial->color2Active": 1,
}
for token, minimum in required.items():
    count = material.count(token) + tests.count(token)
    if count < minimum:
        raise SystemExit(f"missing runtime-proxy fallback token: {token}")

print("TF2 runtime-proxy fallback validation passed")
