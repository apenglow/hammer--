#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
hardware = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
document = (root / 'src/linux_qt/app/MapDocumentWidget.cpp').read_text()
checks = {
    'six Hammer sky faces': 'suffixes{{"bk", "lf", "ft", "rt", "up", "dn"}}' in hardware,
    'cubemap construction': 'ensureEnvironmentCubeMap' in hardware and
                            'GL_TEXTURE_CUBE_MAP_POSITIVE_X' in hardware and
                            'GL_TEXTURE_CUBE_MAP_NEGATIVE_Z' in hardware,
    'Hammer face orientation resampling': 'sampleHammerSkyDirection' in hardware and
                                           '// BK / +Y' in hardware and
                                           '// LF / -X' in hardware and
                                           '// FT / -Y' in hardware and
                                           '// RT / +X' in hardware,
    'cube edge filtering': 'GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE' in hardware and
                           'GL_LINEAR_MIPMAP_LINEAR' in hardware,
    'shader variants sample one refractively bent cube ray':
        hardware.count('uniform samplerCube uEnvironmentMap;') == 4 and
        hardware.count('texture(uEnvironmentMap, environmentDirection)') == 2 and
        hardware.count('textureCube(uEnvironmentMap, environmentDirection)') == 2 and
        hardware.count('refractiveBend = clamp(') == 4,
    'envmap and water use sky': hardware.count('uHasEnvironmentMap') >= 12 and
                                'fakeReflection' not in hardware and
                                'environmentColor = mix(vec3(0.08' not in hardware,
    'cache lifecycle': 'environmentSkyName_' in hardware and
                       'glDeleteTextures(1, &environmentCubeMap_)' in hardware,
    'model browser inherits map sky': 'scene_->skyName = skyName_' in document and
                                      'scene_ ? scene_->skyName : std::string{}' in document,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    for name in failed: print('FAIL:', name)
    raise SystemExit(1)
print('Current-map skybox cubemap construction, orientation, shader sampling, water/envmap use, cache lifecycle, and model-browser propagation validated.')
