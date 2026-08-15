#pragma once

#include <QString>

class QApplication;

namespace HammerTheme {

enum class Mode {
    System,
    Light,
    Dark
};

void initialize(QApplication& application);
void apply(QApplication& application, Mode mode);
Mode loadMode();
void saveMode(Mode mode);
QString displayName(Mode mode);
QString settingsValue(Mode mode);
Mode modeFromSettingsValue(const QString& value);

} // namespace HammerTheme
