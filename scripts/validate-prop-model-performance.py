#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
model = (root / "src/linux_qt/assets/StudioModelSystem.cpp").read_text()
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
wayland = (root / "src/linux_qt/app/WaylandPointerLock.cpp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()

assert "VvdVertexDataStart = 56" in model
assert "setLe<std::uint32_t>(bytes,56,64)" in tests
assert "layoutRefreshTimer_->setSingleShot(true)" in main
assert "subWindow->showMaximized();" not in main.split("void MainWindow::normalizeResizableLayout()", 1)[1].split("void MainWindow::restoreWindowLayout()", 1)[0]
assert "if (subWindow->geometry() != target)" in main
assert "if (next == regionRect_) return;" in wayland
assert "window->requestUpdate()" not in wayland
assert "else if (mouseCaptured_)" in view
print("prop model and resize responsiveness validation passed")
