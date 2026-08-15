#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
material_hpp = (root / 'src/linux_qt/assets/MaterialSystem.hpp').read_text()
material_cpp = (root / 'src/linux_qt/assets/MaterialSystem.cpp').read_text()
hardware = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
main_cpp = (root / 'src/linux_qt/app/MainWindow.cpp').read_text()
main_hpp = (root / 'src/linux_qt/app/MainWindow.hpp').read_text()
view_hpp = (root / 'src/linux_qt/app/MapViewWidget.hpp').read_text()
document_cpp = (root / 'src/linux_qt/app/MapDocumentWidget.cpp').read_text()
tests = (root / 'src/linux_qt/tests/asset_system_tests.cpp').read_text()

checks = {
    'Source Phong metadata': all(token in material_hpp for token in (
        'phongFresnelRanges', 'phongTint', 'phongTintDefined', 'envMapTint', 'phongAlbedoTint',
        'phongExponentOverride', 'phongExponentTexture', 'phongExponentImage',
        'selfIllumTint', 'selfIllumMask', 'rimLightExponent', 'rimLightBoost',
        'rimMaskFromExponentAlpha')),
    'Source Phong VMT keys parsed': all(token in material_cpp for token in (
        '"$phongfresnelranges"', '"$phongtint"', '"$phongalbedotint"',
        '"$phongexponenttexture"', '"$envmaptint"', '"$selfillum"',
        '"$selfillumtint"', '"$selfillummask"', '"$rimlight"',
        '"$rimlightexponent"', '"$rimlightboost"', '"$rimmask"')),
    'normal alpha preserved': 'prepareEditorNormalMap' in material_cpp and
                              'argb(r, g, b, alpha)' in material_cpp and
                              'prepareEditorNormalMap(bumpImage)' in material_cpp,
    'exponent texture loaded': 'result->hasPhongExponentTexture' in material_cpp and
                               'result->phongExponentImage.valid()' in material_cpp,
    'all shader variants support Source controls':
        hardware.count('uniform samplerCube uEnvironmentMap;') == 4 and
        hardware.count('uniform sampler2D uPhongExponentMap;') == 4 and
        hardware.count('uniform vec3 uPhongFresnelRanges;') == 4 and
        hardware.count('uniform vec3 uPhongTint;') == 4 and
        hardware.count('uniform vec3 uEnvMapTint;') == 4 and
        hardware.count('vec4 bumpSample = uHasBumpMap != 0') == 4 and
        hardware.count('uniform sampler2D uSelfIllumMask;') == 4 and
        hardware.count('uniform int uUseSelfIllum;') == 4 and
        hardware.count('uniform int uUseRimLight;') == 4,
    'VertexLitGeneric self illumination':
        hardware.count('if (uUseSelfIllum != 0)') == 4 and
        hardware.count('uHasSelfIllumMask != 0') >= 4 and
        hardware.count('linearAlbedo * uSelfIllumTint') == 4 and
        hardware.count(': vec3(baseAlphaMask);') == 4 and
        hardware.count('if (uUseSelfIllumFresnel != 0)') == 4 and
        hardware.count('uSelfIllumFresnelMinMaxExp') >= 8 and
        hardware.count('selfIllumMask = vec3(baseAlphaMask * selfIllumFresnel);') == 4,
    'VertexLitGeneric rim lighting':
        hardware.count('if (uUseRimLight != 0)') == 4 and
        hardware.count('clamp(uRimLightExponent, 1.0, 128.0)') == 4 and
        hardware.count('clamp(uRimLightBoost, 0.0, 16.0)') == 4 and
        hardware.count('clamp(exponentSample.a, 0.0, 1.0)') == 4 and
        hardware.count('const float rimFadeStart = 256.0;') == 4 and
        hardware.count('const float rimFadeEnd = 2048.0;') == 4 and
        hardware.count('float rimViewDistance = length(uCameraPosition - vWorldPosition);') == 4 and
        hardware.count('float rimMultiply = rimMask * rimFresnel * rimDistanceFade;') == 4 and
        hardware.count('rimDistanceFade *= rimDistanceFade;') == 4 and
        hardware.count('float rimLobe = pow(normalHalf, rimExponent);') == 4 and
        hardware.count('directRimLighting * rimMultiply') == 4 and
        hardware.count('vec3(0.08) * ambientRimAmount') == 4 and
        'max(rimFresnel, rimLobe)' not in hardware and
        'rimLightEnabled_ && usePhong' in hardware,
    'normal alpha Phong mask': hardware.count('clamp(bumpSample.a, 0.0, 1.0)') == 4,
    'piecewise Fresnel ranges': hardware.count('fresnelCoordinate < 0.5') == 4 and
                                hardware.count('uPhongFresnelRanges.x') == 4,
    'Source exponent and albedo-tint map': hardware.count('1.0 + 149.0 * exponentSample.r') == 4 and
                                           hardware.count('mix(vec3(1.0), linearAlbedo, exponentSample.g)') == 4 and
                                           'zeroPhongTint' in hardware and
                                           'material->phongTintDefined' in hardware,
    'current-sky envmap': hardware.count('texture(uEnvironmentMap, reflectedView)') == 2 and
                          hardware.count('textureCube(uEnvironmentMap, reflectedView)') == 2 and
                          'environmentColor = mix(vec3(0.08' not in hardware,
    'water single current-sky layer with refractive bend':
        hardware.count('environmentDirection = normalize(') == 4 and
        hardware.count('refractedDirection = refract(') == 4 and
        hardware.count('visibleSkyHemisphere = geometricNormal * interfaceSide') == 4 and
        hardware.count('refractiveBend = clamp(') == 4 and
        hardware.count('baseUv, -2.0') == 4 and
        'refractedColor' not in hardware and
        'stableFresnel' not in hardware and 'fakeReflection' not in hardware,
    'GPU texture lifecycle': all(token in hardware for token in (
        'GLuint phongExponentId', 'glDeleteTextures(1, &texture.phongExponentId)',
        'GL_TEXTURE4', 'GL_TEXTURE5', 'environmentCubeMap_')),
    'per-material feature gating': 'configureMaterialEffects' in hardware and
                                   'material->editorBumpMapSupported' in hardware and
                                   'material->editorPhongSupported' in hardware and
                                   'material->editorSpecularSupported' in hardware and
                                   '!material->ssBump' in hardware,
    'mode-only effects': 'advancedMaterialPreview && owner_->phongEnabled_' in hardware and
                         'advancedMaterialPreview && owner_->specularEnabled_' in hardware and
                         'advancedMaterialPreview && owner_->bumpMapsEnabled_' in hardware and
                         'advancedMaterialPreview && owner_->lightWarpEnabled_' in hardware and
                         'advancedMaterialPreview && owner_->selfIllumEnabled_' in hardware and
                         'advancedMaterialPreview && owner_->rimLightEnabled_' in hardware,
    'renderer intensity multipliers': hardware.count('clamp(uPhongIntensity, 0.0, 4.0) * phongMask') == 4 and
                                      hardware.count('uSpecularIntensity * 2.40') == 4 and
                                      hardware.count('clamp(uBumpMapIntensity, 0.0, 4.0)') == 4,
    'actual intensity values retained': hardware.count('setUniformValue("uPhongIntensity", phongIntensity_)') == 2 and
                                        hardware.count('setUniformValue("uSpecularIntensity", specularIntensity_)') == 2 and
                                        hardware.count('setUniformValue("uBumpMapIntensity", bumpMapIntensity_)') == 2,
    'bounded stable highlights': hardware.count('materialHighlight = materialHighlight /') == 4 and
                                 hardware.count('max(vec3(1.0) - linearTexture, vec3(0.12))') == 4,
    'View options': all(token in main_cpp for token in (
        'view.materialPhong', 'view.materialSpecular', 'view.materialBumpMaps',
        'view.materialLightWarp', 'view.materialSelfIllum', 'view.materialRimLight',
        'view.materialEffectsIntensity', 'Effect &Intensity...', 'Material &Effects')),
    'settings persist': all(token in main_cpp for token in (
        'render/materialPhong', 'render/materialSpecular', 'render/materialBumpMaps',
        'render/materialLightWarp', 'render/materialSelfIllum', 'render/materialRimLight',
        'render/materialPhongIntensity', 'render/materialSpecularIntensity',
        'render/materialBumpMapIntensity')),
    'state reaches viewports': 'setMaterialEffectsEnabled' in main_hpp and
                               'setMaterialEffectIntensities' in main_hpp and
                               'setMaterialEffectsEnabled' in view_hpp and
                               'setMaterialEffectIntensities' in view_hpp and
                               'view->setMaterialEffectsEnabled' in document_cpp,
    'model browser shares sky and options': 'viewport_->setMaterialEffectsEnabled' in document_cpp and
                                            'scene_->skyName = skyName_' in document_cpp,
    'portable regression': all(token in tests for token in (
        'test/phong_mask', 'phongFresnelRanges', 'phongTintDefined', 'hasPhongExponentTexture',
        'envMapTint', 'bumpImage.pixels[0]', 'selfIllumTint',
        'hasSelfIllumMask', 'rimLightExponent', 'rimLightBoost',
        'rimMaskFromExponentAlpha')),
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    for name in failed:
        print(f'FAIL: {name}')
    raise SystemExit(1)

print('Source-style Phong, self-illumination, rim-light, current-sky reflection, mode gating, persistence, and regression validation passed.')
