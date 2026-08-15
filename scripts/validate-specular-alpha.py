#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
material_h = (root / 'src/linux_qt/assets/MaterialSystem.hpp').read_text()
material_cpp = (root / 'src/linux_qt/assets/MaterialSystem.cpp').read_text()
viewport = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
tests = (root / 'src/linux_qt/tests/asset_system_tests.cpp').read_text()

checks = {
    'material alpha-mask state': all(x in material_h for x in (
        'envMapMaskFromBaseAlpha', 'envMapMaskFromNormalAlpha', 'invertPhongMask')),
    'Source VMT mask parsing': all(x in material_cpp for x in (
        '$basealphaenvmapmask', '$normalmapalphaenvmapmask', '$invertphongmask')),
    'all GLSL profiles declare mask mode': viewport.count('uniform int uSpecularMaskMode;') == 4,
    'all GLSL profiles apply scalar mask': viewport.count('float environmentAmount = strength * specularMask *') == 4,
    'base and normal alpha are retained': viewport.count('float baseAlphaMask =') == 4 and
                                          viewport.count('float normalAlphaMask =') == 4,
    'direct Phong remains independently masked': viewport.count('* phongMask;') == 4,
    'VertexLitGeneric skin default': 'QStringLiteral("VertexLitGeneric")' in viewport,
    'renderer uploads mask controls': 'setUniformValue("uSpecularMaskMode"' in viewport and
                                       'setUniformValue("uInvertSpecularMask"' in viewport,
    'regression fixture covers normal alpha/inversion': '$normalmapalphaenvmapmask 1 $invertphongmask 1' in tests,
    'material stores authored envmap cubemaps': all(x in material_h for x in (
        'std::string envMap;', 'CubeImage envMapCube;', 'envMapUsesMapCubemap', 'hasEnvMapCube')),
    'cubemap VTF decoder exists': 'decodeVtfCubemap' in material_h and
                                   'MaterialSystem::decodeVtfCubemap' in material_cpp,
    'env_cubemap is the explicit map token': 'loweredEnv == "env_cubemap"' in material_cpp,
    'explicit envmap is loaded before fallback': 'Envmap VTF not found:' in material_cpp and
                                                  'ensureMaterialEnvironmentCubeMap' in viewport,
    'renderer selects authored cubemap first': 'if (authoredEnvironment) selectedEnvironment = authoredEnvironment;' in viewport,
    'renderer retains skybox fallback': 'GLuint selectedEnvironment = environmentCubeMap_;' in viewport,
    'cubemap cache is released': 'materialEnvironmentCubeMaps_.clear();' in viewport,
    'regression fixture covers explicit and missing envmaps': 'custom_envmap' in tests and
                                                               'missing_envmap' in tests and
                                                               'decodeVtfCubemap(makeCubemapVtf())' in tests,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    print('Specular alpha-mask validation failed:')
    for name in failed:
        print(' -', name)
    raise SystemExit(1)
print('Source alpha-masked specular and authored envmap selection validation passed.')
