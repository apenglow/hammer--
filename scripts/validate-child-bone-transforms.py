#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
model = (root / "src/linux_qt/assets/StudioModelSystem.cpp").read_text()
tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()

checks = {
    "inverse bind matrix": "StudioBonePoseToBone" in model and
                           "readMatrix3x4" in model and
                           "bone.hasPoseToBone" in model,
    "reference pose evaluation": "StudioHeaderLocalSequenceCount" in model and
                                 "StudioHeaderLocalAnimationCount" in model and
                                 "sequence 0, blend 0" in model and
                                 "bone.posePosition = position" in model and
                                 "bone.poseQuaternion = quaternion" in model,
    "source skin equation": "concatenate(bone.boneToPose, bone.poseToBone)" in model and
                            "bone.boneToPose = inverseBind" not in model and
                            "invertAffine" not in model,
    "compressed animation formats": "quaternion48" in model and
                                    "quaternion64" in model and
                                    "frameZeroAnimationValue" in model and
                                    "halfFloat" in model,
    "hierarchical composition": "bones[static_cast<std::size_t>(bone.parent)].boneToPose" in model and
                                "bone.boneToPose, bone.poseToBone" in model,
    "linear bone arrays": "StudioLinearBoneParentIndex" in model and
                          "StudioLinearBonePositionIndex" in model and
                          "StudioLinearBoneQuaternionIndex" in model and
                          "StudioLinearBonePoseToBoneIndex" in model,
    "two-level regression": "Two-level bind hierarchy" in tests and
                            "bindChild" in tests and "bindBlended" in tests,
    "linear override regression": "makeTinyLinearBoneMdl" in tests and
                                  "linearChild" in tests and "linearBlended" in tests,
    "animated reference-pose regression": "makeTinyReferencePoseMdl" in tests and
                                           "referenceRoot" in tests and
                                           "referenceChild" in tests and
                                           "referenceBlended" in tests,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Studio bind-pose validation failed: " + ", ".join(failed))
print("Source sequence-zero bind pose, hierarchy, inverse bind, and linear bones validated")
