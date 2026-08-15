#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
studio_h = (root / "src/linux_qt/assets/StudioModelSystem.hpp").read_text()
studio_cpp = (root / "src/linux_qt/assets/StudioModelSystem.cpp").read_text()
scene_h = (root / "src/linux_qt/vmf/VmfScene.hpp").read_text()
scene_cpp = (root / "src/linux_qt/vmf/VmfScene.cpp").read_text()
renderer = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
browser = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()
asset_tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()
vmf_tests = (root / "src/linux_qt/tests/vmf_document_tests.cpp").read_text()
cmake = (root / "src/linux_qt/CMakeLists.txt").read_text()

checks = {
    "cached sequence metadata": all(token in studio_h for token in (
        "struct StudioSequence", "std::vector<StudioSequence> sequences",
        "std::shared_ptr<const StudioAnimationData> animationData", "sequenceIndex")),
    "source vertex skinning retained": all(token in studio_h for token in (
        "sourcePosition", "sourceNormal", "sourceTangent", "boneWeights",
        "boneIndices", "influenceCount")),
    "inline and external animation blocks": all(token in studio_cpp for token in (
        "StudioHeaderAnimationBlockNameIndex", "StudioHeaderAnimationBlockCount",
        "StudioHeaderAnimationBlockIndex", "animationChunk", "source.ani",
        "StudioAnimationSectionFrames", "StudioAnimationSectionIndex")),
    "included-model virtual sequence view": all(token in studio_cpp for token in (
        "StudioHeaderIncludeModelCount", "StudioHeaderIncludeModelIndex",
        "includedModelPaths", "SequenceBinding", "AnimationBinding", "boneToRoot",
        "masterAnimations", "resolveAnimationBinding", "appendSource",
        "visitedAnimationModels", "sourceIndex", "localSequence")),
    "TF2 external animation block compatibility": all(token in studio_cpp for token in (
        "basename", "sourceDirectory + relativeName", "External animation-block offsets",
        "index < 0", "animationByName", "payloadScore")),
    "neutral sequence blend selection": all(token in studio_cpp for token in (
        "StudioSequenceParameterStart", "StudioSequenceParameterEnd",
        "neutralBlend", "nearest each sequence parameter's neutral value")),
    "compressed channels and interpolation": all(token in studio_cpp for token in (
        "extractAnimationValue", "extractAnimationAxis", "quaternionSlerp",
        "AnimRawPosition", "AnimRawRotation", "AnimRawRotation2",
        "AnimPosition", "AnimRotation", "fraction",
        "valid == 1 && total == 1")),
    "general sequence sampler": all(token in studio_cpp for token in (
        "evaluateSequence", "StudioModelSystem::sampleAnimation", "skinStudioVertex",
        "Re-apply sequence zero through the same general evaluator")),
    "entity animation state": all(token in scene_h for token in (
        "animationSequence", "animationSequenceIndex", "animationPlaybackRate",
        "animationCycle", "animateModel")),
    "VMF animation keys": all(token in scene_cpp for token in (
        'propertyCi("DefaultAnim")', 'propertyCi("sequence")',
        'propertyCi("playbackrate")', 'propertyCi("animationrate")',
        'propertyCi("cycle")')),
    "no accidental prop_dynamic autoplay": all(token in vmf_tests for token in (
        "prop_dynamic without DefaultAnim", "an empty DefaultAnim keeps a dynamic prop")) and
        'classLower == "prop_dynamic"' not in scene_cpp[scene_cpp.find("marker.animateModel"):scene_cpp.find("scene.entities.push_back", scene_cpp.find("marker.animateModel"))],
    "GPU matrix-palette skinning": all(token in renderer for token in (
        "MaxStudioPaletteBones", "paletteBones", "uGpuSkinning",
        "uBoneRow0[32]", "sampleAnimationMatrices", "uploadStudioPalette",
        "GL_STATIC_DRAW")) and "uploadStudioPose" not in renderer,
    "world geometry remains static": "vertices.data(), GL_STATIC_DRAW);" in renderer[renderer.find("worldVbo_"):renderer.find("worldScene_ = &scene")],
    "failed matrix sample reference fallback": all(token in renderer for token in (
        "if (!studioModels_ ||", "sampleAnimationMatrices",
        "sequence = -1", "pose = model.referencePoseMatrices")),
    "animation render timer": all(token in renderer for token in (
        "hasAnimatedModels_", "hasAnimatedContent", "animationPlaybackRate",
        "sequence.duration", "animationScene_ != scene", "animationStart_ = now")),
    "model-browser controls": all(token in browser for token in (
        'tr("Sequence:")', 'tr("Play")', "animationRate", "setSequence",
        "setPlaying", "setPlaybackRate")),
    "model-browser selection persists": all(token in browser for token in (
        "ModelBrowserSelection", "selected->sequence", 'QStringLiteral("DefaultAnim")',
        "selected->playbackRate", 'QStringLiteral("playbackrate")')),
    "SmartEdit sequence chooser": "makeSequencePropertyEditor" in browser and
                                    'key.compare(QStringLiteral("DefaultAnim")' in browser,
    "portable animated MDL fixture": all(token in asset_tests for token in (
        "makeTinyAnimatedMdl", 'label="root_move"', "sampleAnimation(*animatedModel",
        "animationMiddle", "animatedDistance>4.9f", "sampleAnimationMatrices",
        "paletteX-animationMiddle", "makeTinyRotatingAnimatedMdl",
        "rotatingMiddle[0][0].x-6.171573f", "makeTinyExternalAnimatedMdl",
        "makeTinyExternalAni", "externalDistance>4.9f")),
    "included player animation fixture": all(token in asset_tests for token in (
        "makeTinyIncludedAnimationRootMdl", "makeTinyIncludedAnimationMdl",
        "included_player_move", "includedModel->sequenceCount()==1",
        "different local bone order", 'setBoneName(bytes,RootBone,"root")',
        "includedDistance>4.9f")),
    "TF2 virtual animation fixture": all(token in asset_tests for token in (
        "makeTinyTf2VirtualRootMdl", "makeTinyTf2SequenceGroupMdl",
        "makeTinyTf2ChannelGroupMdl", "makeTinyTf2ChannelAni",
        "TF2-style basename", "valid external offset zero",
        "tf2VirtualDistance>4.9f")),
    "TF2 human shared-skeleton mapping": all(token in studio_cpp for token in (
        "hierarchyCompatiblePrefix", "sharedPrefixIndexFallback",
        "virtualgroup_t::masterBone", "Robot player models")) and
        all(token in asset_tests for token in (
            "makeTinyTf2HumanRootMdl", "human_extra",
            "same unnamed animation skeleton", "tf2HumanDistance>4.9f")),
    "TF2 human forward sequence replacement": all(token in studio_cpp for token in (
        "masterSequences", "SequenceOverride", "sequenceByLabel",
        "virtualmodel_t::AppendSequences", "forward declaration wins lookup")) and
        all(token in asset_tests for token in (
            "$declaresequence", "STUDIO_OVERRIDE", "ForwardSequence",
            "tf2HumanModel->valid && tf2HumanModel->sequenceCount()==1")),
    "virtual sequence-group base pose": all(token in studio_cpp for token in (
        "sequenceBoneForRoot", "sequence-owning group's base", "sequenceSource.bones",
        "rootWeights(data.bones.size(), 0.0f)")),
    "neutral blend fixture": all(token in asset_tests for token in (
        "makeTinyNeutralBlendMdl", "neutral_blend",
        "neutralBlendModel->sequences[0].frameCount==2",
        "neutralDistance>4.9f")),
    "dedicated passing animation test": "HAMMER_ANIMATION_ONLY" in asset_tests and
                                         "hammer-studio-animation-tests" in cmake,
    "VMF animation regression": all(token in vmf_tests for token in (
        'animatedProp.setValue("DefaultAnim"', 'animatedProp.setValue("playbackrate"',
        'animatedProp.setValue("cycle"', "cycler previews sequence zero")),
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    for name in failed:
        print(f"FAIL: {name}")
    raise SystemExit(1)

print("Cached Source prop playback, TF2 virtual animation groups, human sequence overrides, shared-skeleton bone mapping, relative ANI blocks, neutral blends, browser controls, VMF state, GPU matrix-palette skinning, and regressions validated.")
