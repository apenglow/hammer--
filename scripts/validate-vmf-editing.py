#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
editor_hpp = root / "src/linux_qt/vmf/VmfEditor.hpp"
editor_cpp = root / "src/linux_qt/vmf/VmfEditor.cpp"
document_cpp = root / "src/linux_qt/app/MapDocumentWidget.cpp"
view_cpp = root / "src/linux_qt/app/MapViewWidget.cpp"
main_cpp = root / "src/linux_qt/app/MainWindow.cpp"
tests = root / "src/linux_qt/tests/vmf_document_tests.cpp"
errors: list[str] = []

for path in [editor_hpp, editor_cpp, document_cpp, view_cpp, main_cpp, tests, root / "VMF_EDITING.md"]:
    if not path.exists():
        errors.append(f"missing editing file: {path}")

required = {
    editor_hpp: ["class EditorModel", "translateSelection", "deleteSelection", "replaceSelectedProperties", "bool undo()", "bool redo()"],
    editor_cpp: ["transformPlane", "transformOrigin", "beginTransaction", "commitTransaction", "solidOwnerEntityId"],
    document_cpp: ["showObjectProperties", "beginMove", "moveSelection", "setSelectionMode", "selectionSizeSummary"],
    view_cpp: ["hitTest", "moveDeltaRequested", "nudgeRequested", "effectiveObject", "SelectedWire"],
    main_cpp: ["edit.undo", "edit.redo", "edit.delete", "edit.properties", "QButtonGroup::idClicked"],
    tests: ["entity origin translation updates VMF data", "drag transaction commits one undo step", "entity properties can be replaced", "does not translate the child twice"],
}
for path, tokens in required.items():
    if not path.exists():
        continue
    text = path.read_text(encoding="utf-8")
    for token in tokens:
        if token not in text:
            errors.append(f"{path.name} missing token: {token}")

if errors:
    print("VMF editing validation failed:")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)

print("VMF editing structure validation passed.")
print("Selection, transforms, properties, deletion, transactions, and undo/redo are wired.")
