#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
app = root / "src" / "linux_qt" / "app"
errors: list[str] = []

theme_hpp = app / "HammerTheme.hpp"
theme_cpp = app / "HammerTheme.cpp"
main_cpp = app / "MainWindow.cpp"
entry_cpp = app / "main.cpp"
cmake = root / "src" / "linux_qt" / "CMakeLists.txt"

for path in [theme_hpp, theme_cpp, main_cpp, entry_cpp, cmake, root / "MODERN_QT_THEME.md"]:
    if not path.exists():
        errors.append(f"missing theme file: {path}")

if not errors:
    theme_text = theme_cpp.read_text(encoding="utf-8")
    main_text = main_cpp.read_text(encoding="utf-8")
    entry_text = entry_cpp.read_text(encoding="utf-8")
    cmake_text = cmake.read_text(encoding="utf-8")

    required_theme_tokens = [
        'ThemeSettingsKey = "appearance/theme"',
        'QStyleHints::colorSchemeChanged',
        'QStyleFactory::create(QStringLiteral("Fusion"))',
        'Mode::System', 'Mode::Light', 'Mode::Dark',
        'application.setPalette(makePalette(dark))',
        'application.setStyleSheet(makeStyleSheet(dark))',
    ]
    for token in required_theme_tokens:
        if token not in theme_text:
            errors.append(f"theme implementation missing token: {token}")

    required_ui_tokens = [
        'tr("&Appearance")',
        '"Follow &System"',
        '"&Light"',
        '"&Dark"',
        'HammerTheme::saveMode(choice.mode)',
        'mainWindow/geometry',
        'mainWindow/state',
    ]
    for token in required_ui_tokens:
        if token not in main_text:
            errors.append(f"main window missing token: {token}")

    if 'HammerTheme::initialize(application)' not in entry_text:
        errors.append("application does not initialize HammerTheme")
    for filename in ['app/HammerTheme.cpp', 'app/HammerTheme.hpp']:
        if filename not in cmake_text:
            errors.append(f"CMake target missing {filename}")

    forbidden = ['MS Sans Serif', '#d4d0c8', 'applyClassicAppearance']
    for token in forbidden:
        if token in main_text or token in theme_text:
            errors.append(f"legacy hard-coded theme token remains: {token}")

if errors:
    print("Modern theme validation failed:")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)

print("Modern Qt6 theme validation passed.")
print("System/light/dark modes, persistence, and compact Hammer styling are present.")
