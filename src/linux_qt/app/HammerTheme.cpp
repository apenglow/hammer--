#include "HammerTheme.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QProxyStyle>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

#include <utility>

namespace {
constexpr auto ThemeSettingsKey = "appearance/theme";

// Fusion reports SH_Menu_Scrollable = 0, so a menu taller than the screen
// wraps into extra columns (or clips) instead of scrolling. Force the hint on
// so every QMenu — the menubar dropdowns included — gets scroll arrows when
// it runs out of vertical space.
class HammerProxyStyle final : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* returnData) const override
    {
        if (hint == QStyle::SH_Menu_Scrollable) return 1;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

bool systemUsesDarkColors(const QApplication& application)
{
    const Qt::ColorScheme scheme = application.styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) {
        return true;
    }
    if (scheme == Qt::ColorScheme::Light) {
        return false;
    }

    return application.palette().color(QPalette::Window).lightness() < 128;
}

bool modeUsesDarkColors(const QApplication& application, HammerTheme::Mode mode)
{
    switch (mode) {
    case HammerTheme::Mode::Dark:
        return true;
    case HammerTheme::Mode::Light:
        return false;
    case HammerTheme::Mode::System:
        return systemUsesDarkColors(application);
    }
    return false;
}

QPalette makePalette(bool dark)
{
    QPalette palette;
    if (dark) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#202124")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e8eaed")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#17181b")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#24262a")));
        palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#303238")));
        palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#f1f3f4")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#e8eaed")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#2b2d31")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e8eaed")));
        palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#4f8cff")));
        palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Link, QColor(QStringLiteral("#7aa7ff")));
        palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#9aa0a6")));
        palette.setColor(QPalette::Light, QColor(QStringLiteral("#3c4043")));
        palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#34363a")));
        palette.setColor(QPalette::Mid, QColor(QStringLiteral("#303238")));
        palette.setColor(QPalette::Dark, QColor(QStringLiteral("#111216")));
        palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#000000")));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#7f848b")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#7f848b")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#7f848b")));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#3a4e72")));
    } else {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#f3f4f6")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#202124")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f6f7f9")));
        palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#202124")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#202124")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#f8f9fb")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#202124")));
        palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2f6fed")));
        palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Link, QColor(QStringLiteral("#245fc7")));
        palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#6f7782")));
        palette.setColor(QPalette::Light, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#eef0f3")));
        palette.setColor(QPalette::Mid, QColor(QStringLiteral("#d7dbe0")));
        palette.setColor(QPalette::Dark, QColor(QStringLiteral("#aab0b8")));
        palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#6b7280")));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#9aa0a6")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#9aa0a6")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#9aa0a6")));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#b8c9ec")));
    }
    return palette;
}

QString makeStyleSheet(bool dark)
{
    const QString window = dark ? QStringLiteral("#202124") : QStringLiteral("#f3f4f6");
    const QString panel = dark ? QStringLiteral("#27292d") : QStringLiteral("#ffffff");
    const QString panelAlt = dark ? QStringLiteral("#24262a") : QStringLiteral("#f8f9fb");
    const QString input = dark ? QStringLiteral("#17181b") : QStringLiteral("#ffffff");
    const QString border = dark ? QStringLiteral("#3b3e44") : QStringLiteral("#d7dbe0");
    const QString borderStrong = dark ? QStringLiteral("#50545b") : QStringLiteral("#b8bec7");
    const QString text = dark ? QStringLiteral("#e8eaed") : QStringLiteral("#202124");
    const QString muted = dark ? QStringLiteral("#a9afb7") : QStringLiteral("#5f6670");
    const QString hover = dark ? QStringLiteral("#35383e") : QStringLiteral("#eef2f8");
    const QString pressed = dark ? QStringLiteral("#41454c") : QStringLiteral("#dfe7f5");
    const QString accent = dark ? QStringLiteral("#4f8cff") : QStringLiteral("#2f6fed");
    const QString accentSoft = dark ? QStringLiteral("#263a5f") : QStringLiteral("#dce8ff");
    const QString mdi = dark ? QStringLiteral("#141519") : QStringLiteral("#aeb3ba");
    const QString splitter = dark ? QStringLiteral("#111216") : QStringLiteral("#c5c9cf");

    QString sheet = QStringLiteral(R"HAMMER_QSS(
        QMainWindow {
            background: @window;
        }
        QMenuBar {
            background: @panel;
            color: @text;
            border-bottom: 1px solid @border;
            spacing: 3px;
            padding: 2px 4px;
        }
        QMenuBar::item {
            background: transparent;
            border-radius: 4px;
            padding: 4px 8px;
        }
        QMenuBar::item:selected,
        QMenuBar::item:pressed {
            background: @hover;
        }
        QMenu {
            background: @panel;
            color: @text;
            border: 1px solid @borderStrong;
            border-radius: 6px;
            padding: 5px;
        }
        QMenu::item {
            border-radius: 4px;
            padding: 5px 28px 5px 24px;
        }
        QMenu::item:selected {
            background: @accentSoft;
            color: @text;
        }
        QMenu::separator {
            height: 1px;
            background: @border;
            margin: 5px 8px;
        }
        QMenu::scroller {
            height: 14px;
            background: @panelAlt;
        }
        QToolBar {
            background: @panel;
            border: none;
            border-bottom: 1px solid @border;
            spacing: 2px;
            padding: 3px;
        }
        QToolBar[orientation="vertical"] {
            border-right: 1px solid @border;
            border-bottom: none;
        }
        QToolBar::separator {
            background: @border;
            width: 1px;
            height: 1px;
            margin: 4px;
        }
        QToolButton {
            background: transparent;
            color: @text;
            border: 1px solid transparent;
            border-radius: 4px;
            margin: 0;
            padding: 3px;
        }
        QToolButton:hover {
            background: @hover;
            border-color: @border;
        }
        QToolButton:pressed,
        QToolButton:checked {
            background: @accentSoft;
            border-color: @accent;
        }
        QDockWidget {
            color: @text;
        }
        QDockWidget::title {
            background: @panelAlt;
            border: 1px solid @border;
            padding: 6px 8px;
            text-align: left;
        }
        QDockWidget > QWidget {
            background: @window;
        }
        QGroupBox {
            color: @text;
            border: 1px solid @border;
            border-radius: 5px;
            margin-top: 9px;
            padding-top: 6px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 8px;
            padding: 0 4px;
            color: @muted;
        }
        QPushButton,
        QComboBox,
        QSpinBox,
        QDoubleSpinBox,
        QLineEdit {
            min-height: 22px;
        }
        QPushButton {
            background: @panelAlt;
            color: @text;
            border: 1px solid @borderStrong;
            border-radius: 4px;
            padding: 3px 9px;
        }
        QPushButton:hover {
            background: @hover;
            border-color: @accent;
        }
        QPushButton:pressed {
            background: @pressed;
        }
        QLineEdit,
        QPlainTextEdit,
        QTextEdit,
        QListView,
        QListWidget,
        QTreeView,
        QTreeWidget,
        QComboBox,
        QSpinBox,
        QDoubleSpinBox {
            background: @input;
            color: @text;
            border: 1px solid @borderStrong;
            border-radius: 4px;
            selection-background-color: @accent;
            selection-color: white;
        }
        QLineEdit,
        QComboBox,
        QSpinBox,
        QDoubleSpinBox {
            padding: 2px 6px;
        }
        QComboBox::drop-down,
        QSpinBox::up-button,
        QSpinBox::down-button,
        QDoubleSpinBox::up-button,
        QDoubleSpinBox::down-button {
            border: none;
            width: 20px;
        }
        QCheckBox,
        QRadioButton,
        QLabel {
            color: @text;
        }
        QCheckBox,
        QRadioButton {
            spacing: 6px;
        }
        QTabWidget::pane {
            border: 1px solid @border;
            border-radius: 5px;
            background: @panel;
            top: -1px;
        }
        QTabBar::tab {
            background: @panelAlt;
            color: @muted;
            border: 1px solid @border;
            border-bottom: none;
            padding: 6px 10px;
            margin-right: 2px;
            border-top-left-radius: 5px;
            border-top-right-radius: 5px;
        }
        QTabBar::tab:selected {
            background: @panel;
            color: @text;
            border-color: @borderStrong;
        }
        QSplitter::handle {
            background: @splitter;
        }
        QSplitter::handle:hover {
            background: @accent;
        }
        QMdiArea#HammerMdiClient {
            background: @mdi;
        }
        QMdiSubWindow {
            background: @panel;
            border: 1px solid @borderStrong;
        }
        QStatusBar {
            background: @panel;
            border-top: 1px solid @border;
        }
        QStatusBar::item {
            border: none;
        }
        QLabel[hammerStatusPane="true"] {
            background: @panelAlt;
            color: @muted;
            border: 1px solid @border;
            border-radius: 3px;
            padding: 2px 6px;
            margin: 2px 1px;
        }
        QToolTip {
            background: @panel;
            color: @text;
            border: 1px solid @borderStrong;
            padding: 4px 6px;
        }
        QScrollBar:vertical {
            background: @window;
            width: 12px;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: @borderStrong;
            min-height: 24px;
            border-radius: 5px;
            margin: 2px;
        }
        QScrollBar:horizontal {
            background: @window;
            height: 12px;
            margin: 0;
        }
        QScrollBar::handle:horizontal {
            background: @borderStrong;
            min-width: 24px;
            border-radius: 5px;
            margin: 2px;
        }
        QScrollBar::add-line,
        QScrollBar::sub-line,
        QScrollBar::add-page,
        QScrollBar::sub-page {
            background: none;
            border: none;
        }
    )HAMMER_QSS");

    // Replace longer tokens first. Replacing @panel before @panelAlt turns
    // the latter into invalid strings such as "#ffffffAlt".
    const std::pair<const char*, QString> replacements[] = {
        {"@borderStrong", borderStrong}, {"@accentSoft", accentSoft},
        {"@panelAlt", panelAlt}, {"@splitter", splitter},
        {"@window", window}, {"@pressed", pressed},
        {"@border", border}, {"@panel", panel}, {"@input", input},
        {"@accent", accent}, {"@muted", muted}, {"@hover", hover},
        {"@text", text}, {"@mdi", mdi}
    };
    for (const auto& [token, value] : replacements) {
        sheet.replace(QString::fromLatin1(token), value);
    }
    return sheet;
}
} // namespace

namespace HammerTheme {

void initialize(QApplication& application)
{
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        // The proxy takes ownership of the Fusion instance.
        application.setStyle(new HammerProxyStyle(fusion));
    }

    apply(application, loadMode());

    QObject::connect(application.styleHints(), &QStyleHints::colorSchemeChanged,
                     &application, [&application](Qt::ColorScheme) {
        if (loadMode() == Mode::System) {
            apply(application, Mode::System);
        }
    });
}

void apply(QApplication& application, Mode mode)
{
    const bool dark = modeUsesDarkColors(application, mode);
    application.setProperty("hammerThemeMode", settingsValue(mode));
    application.setProperty("hammerDarkTheme", dark);
    application.setPalette(makePalette(dark));
    application.setStyleSheet(makeStyleSheet(dark));
}

Mode loadMode()
{
    QSettings settings;
    return modeFromSettingsValue(settings.value(QString::fromLatin1(ThemeSettingsKey),
                                                QStringLiteral("system")).toString());
}

void saveMode(Mode mode)
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(ThemeSettingsKey), settingsValue(mode));
}

QString displayName(Mode mode)
{
    switch (mode) {
    case Mode::System: return QStringLiteral("System");
    case Mode::Light: return QStringLiteral("Light");
    case Mode::Dark: return QStringLiteral("Dark");
    }
    return QStringLiteral("System");
}

QString settingsValue(Mode mode)
{
    switch (mode) {
    case Mode::System: return QStringLiteral("system");
    case Mode::Light: return QStringLiteral("light");
    case Mode::Dark: return QStringLiteral("dark");
    }
    return QStringLiteral("system");
}

Mode modeFromSettingsValue(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("light")) {
        return Mode::Light;
    }
    if (normalized == QStringLiteral("dark")) {
        return Mode::Dark;
    }
    return Mode::System;
}

} // namespace HammerTheme
