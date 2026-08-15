#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
paths = {
    "editor_hpp": root / "src/linux_qt/vmf/VmfEditor.hpp",
    "editor_cpp": root / "src/linux_qt/vmf/VmfEditor.cpp",
    "fgd_hpp": root / "src/linux_qt/fgd/FgdDatabase.hpp",
    "fgd_cpp": root / "src/linux_qt/fgd/FgdDatabase.cpp",
    "view_hpp": root / "src/linux_qt/app/MapViewWidget.hpp",
    "view_cpp": root / "src/linux_qt/app/MapViewWidget.cpp",
    "document_cpp": root / "src/linux_qt/app/MapDocumentWidget.cpp",
    "main_cpp": root / "src/linux_qt/app/MainWindow.cpp",
    "tests": root / "src/linux_qt/tests/vmf_document_tests.cpp",
    "cmake": root / "src/linux_qt/CMakeLists.txt",
    "doc": root / "OBJECT_CREATION_FGD.md",
}
errors: list[str] = []
for label, path in paths.items():
    if not path.exists():
        errors.append(f"missing {label}: {path}")

required = {
    "editor_hpp": [
        "ClipboardData copySelection() const", "bool paste(", "bool duplicateSelection(",
        "createBlock", "createPointEntity", "scaleSelection", "rotateSelection",
    ],
    "editor_cpp": [
        "remap(remap, clone)", "setEditorMetadata", "scaleSelectionInTransaction",
        "rotateSelectionInTransaction", "createPointEntity", "createBlock",
    ],
    "fgd_hpp": [
        "enum class ClassKind", "enum class PropertyType", "effectiveProperties",
        "pointClasses", "loadFile",
    ],
    "fgd_cpp": [
        'directive == "include"', "@include", "appendEffectiveProperties",
        "PropertyType::Choices", "PropertyType::Flags",
    ],
    "view_hpp": [
        "enum class Tool { Selection, Block, Entity, Decal, Overlay }", "enum class TransformMode { Scale, Rotate }",
        "blockCreationRequested", "entityCreationRequested", "interactionCanceled",
    ],
    "view_cpp": [
        "Tool::Block", "Tool::Entity", "resizeDeltaRequested", "rotateDeltaRequested",
        "15.0 * Pi / 180.0", "emit interactionCanceled()",
    ],
    "document_cpp": [
        "copySelection", "cutSelection", "duplicateSelection", "createBlock",
        "createEntity", "entityDefaults", 'tr("SmartEdit")', "PropertyType::Color255",
    ],
    "main_cpp": [
        'QStringLiteral("edit.cut")', 'QStringLiteral("edit.copy")',
        'QStringLiteral("edit.paste")', 'QStringLiteral("edit.duplicate")',
        'tr("Load &Game Data (.fgd)...")', "loadFgdPath", "refreshEntityClasses",
    ],
    "tests": [
        "block tool creates and selects a world solid", "paste adds independent brush and entity clones",
        "rotation handles rotate selection around a pivot", "FGD files load with relative @include directives",
        "canceling a transform restores VMF data without an undo step",
    ],
    "cmake": ["add_library(hammer_fgd", "target_link_libraries(hammer-qt6 PRIVATE", "hammer_fgd"],
}
for label, tokens in required.items():
    path = paths[label]
    if not path.exists():
        continue
    text = path.read_text(encoding="utf-8")
    for token in tokens:
        if token not in text:
            errors.append(f"{path.name} missing token: {token}")

if errors:
    print("Object creation/FGD validation failed:")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)

print("Object creation and FGD structure validation passed.")
print("Creation, clipboard cloning, transforms, cancellation, FGD loading, and SmartEdit wiring are present.")
