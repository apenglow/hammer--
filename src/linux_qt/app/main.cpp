#include "CollabServer.hpp"
#include "MainWindow.hpp"
#include "HammerTheme.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QPixmap>
#include <QSplashScreen>
#include <QSurfaceFormat>
#include <QSettings>
#include <array>
#include <csignal>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {
QPixmap loadHammerLogo()
{
    const QString candidates[] = {
        QStringLiteral(":/hammer/app_logo.png"),
        QStringLiteral(HAMMER_APP_LOGO_PATH),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/hammerminusminus/app_logo.png"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/hammer/app_logo.png")
    };
    for (const QString& path : candidates) {
        QPixmap logo(path);
        if (!logo.isNull()) return logo;
    }
    return QPixmap(QStringLiteral(":/hammer/forge.ico"));
}

QIcon loadHammerWindowIcon()
{
    const QString candidates[] = {
        QStringLiteral(":/hammer/window_icon.png"),
#ifdef HAMMER_WINDOW_ICON_PATH
        QStringLiteral(HAMMER_WINDOW_ICON_PATH),
#endif
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/hammerminusminus/window_icon.png"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/hammer/window_icon.png"),
        QStringLiteral(":/hammer/app_logo.png")
    };

    QPixmap source;
    for (const QString& path : candidates) {
        if (source.load(path)) break;
    }
    if (source.isNull()) source.load(QStringLiteral(":/hammer/forge.ico"));

    QIcon icon;
    constexpr std::array<int, 7> sizes{16, 20, 24, 32, 48, 64, 128};
    for (const int size : sizes) {
        icon.addPixmap(source.scaled(QSize(size, size), Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
    }
    return icon;
}

void migrateLegacyHammerSettings()
{
    QSettings current;
    if (!current.allKeys().isEmpty()) return;

    // Preserve the existing configuration across the application renames.
    // Copy only into an empty new profile so subsequent launches use the
    // Hammer-- application identity without losing game paths, FGD selection,
    // window layout, or rendering preferences. The testing-build profile is
    // the most recent predecessor; the original Hammer profile is the oldest.
    QSettings testingBuild(QSettings::NativeFormat, QSettings::UserScope,
                           QStringLiteral(HAMMER_SETTINGS_ORGANIZATION),
                           QStringLiteral("hammerplusplus-testing"));
    QSettings legacy(QSettings::NativeFormat, QSettings::UserScope,
                     QStringLiteral("Valve"), QStringLiteral("Hammer"));
    QSettings& source = !testingBuild.allKeys().isEmpty() ? testingBuild : legacy;
    for (const QString& key : source.allKeys()) current.setValue(key, source.value(key));
    current.sync();
}

void migrateGeneralGroupSettings()
{
    // Settings used to live under a group named "general". QSettings' INI
    // backend collides that name with its magic top-level [General] section:
    // it writes the group escaped as [%General] and reads it back with a
    // capital G ("General/key"), so lowercase "general/key" lookups silently
    // returned nothing on the next launch. The keys now live under "editor/";
    // carry over values stored under either historical spelling.
    QSettings settings;
    const char* keys[] = {"bspsourceExecutable", "lastOpenDirectory"};
    for (const char* key : keys) {
        const QString newKey = QStringLiteral("editor/") + QLatin1StringView(key);
        if (!settings.value(newKey).toString().isEmpty()) continue;
        for (const QString& prefix :
             {QStringLiteral("General/"), QStringLiteral("general/")}) {
            const QString value = settings.value(prefix + QLatin1StringView(key)).toString();
            if (!value.isEmpty()) {
                settings.setValue(newKey, value);
                break;
            }
        }
    }
    settings.remove(QStringLiteral("General"));
    settings.remove(QStringLiteral("general"));
    settings.sync();
}
} // namespace

namespace {

// Headless session host: `hammerminusminus -server map.vmf [-port N] [-name X]`.
// Runs under QCoreApplication — no display, no GL, no widgets — and hosts the
// map for GUI editors to join. Peers' work autosaves back into the file.
int runServerMode(int argc, char** argv)
{
#ifdef Q_OS_WIN
    // The exe is GUI-subsystem, so server logs vanish unless the parent
    // terminal's console is attached.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral(HAMMER_APP_ID));
    QCoreApplication::setApplicationVersion(QStringLiteral(HAMMER_PORT_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral(HAMMER_SETTINGS_ORGANIZATION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Hammer-- headless collaboration server"));
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    parser.addHelpOption();
    QCommandLineOption serverOption(QStringLiteral("server"),
                                    QStringLiteral("Host a headless session"));
    QCommandLineOption portOption(QStringLiteral("port"),
                                  QStringLiteral("UDP port to listen on"),
                                  QStringLiteral("port"),
                                  QString::number(CollabSession::kDefaultPort));
    QCommandLineOption nameOption(QStringLiteral("name"),
                                  QStringLiteral("Display name of the server"),
                                  QStringLiteral("name"), QStringLiteral("Server"));
    QCommandLineOption customDirOption(
        QStringLiteral("customdir"),
        QStringLiteral("Folder of loose custom content (materials/, models/) to share"),
        QStringLiteral("dir"));
    parser.addOption(serverOption);
    parser.addOption(portOption);
    parser.addOption(nameOption);
    parser.addOption(customDirOption);
    parser.addPositionalArgument(QStringLiteral("map"),
                                 QStringLiteral("VMF map file to host"));
    parser.process(application);

    const QStringList maps = parser.positionalArguments();
    if (maps.size() != 1) {
        std::fprintf(stderr, "usage: %s -server <map.vmf> [-port N] [-name X]\n", argv[0]);
        return 2;
    }
    bool portOk = false;
    const int port = parser.value(portOption).toInt(&portOk);
    if (!portOk || port < 1 || port > 65535) {
        std::fprintf(stderr, "error: invalid port\n");
        return 2;
    }

    CollabServer server;
    QString error;
    if (!server.start(maps.first(), quint16(port), parser.value(nameOption),
                      parser.value(customDirOption), &error)) {
        std::fprintf(stderr, "error: %s\n", qPrintable(error));
        return 1;
    }

    // A last save on any orderly exit, including the signal path below.
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &server,
                     [&server] { server.saveIfChanged(); });
    std::signal(SIGINT, [](int) { QCoreApplication::quit(); });
    std::signal(SIGTERM, [](int) { QCoreApplication::quit(); });

    return application.exec();
}

} // namespace

int main(int argc, char** argv)
{
    // Server mode is decided before any GUI machinery: it must run on
    // machines with no display at all.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-server") == 0 || std::strcmp(argv[i], "--server") == 0)
            return runServerMode(argc, argv);
    }

#if defined(__GLIBC__)
    // Decoded textures are large buffers (a 2048 square VTF is 16 MB as ARGB32).
    // glibc normally mmaps allocations this big and returns them to the OS on
    // free, but freeing one raises its dynamic mmap threshold - up to 32 MB - and
    // from then on those buffers come from the heap arena and are never handed
    // back. The visible effect is that dropping hundreds of megabytes of texture
    // cache does not lower reported memory at all. Pinning the threshold keeps
    // large image buffers on the mmap path, so releasing them is immediately
    // visible outside the process.
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    // Also trim the arena eagerly once a sizeable run of it goes free.
    mallopt(M_TRIM_THRESHOLD, 4 * 1024 * 1024);
#endif

    // Keep the QWidget hierarchy on the ordinary raster backing store. The 3D
    // camera uses an independent off-screen OpenGL context and therefore does
    // not require QOpenGLWidget's top-level QRhi composition path.
    qputenv("QT_WIDGETS_RHI", QByteArrayLiteral("0"));
    qputenv("QT_WIDGETS_HIGHDPI_DOWNSCALE", QByteArrayLiteral("0"));

    // Request desktop OpenGL explicitly. With no format set, Qt resolved to an
    // OpenGL ES 3.2 context on this machine, which compiles EsFragmentShader
    // instead of DesktopFragmentShader. That ES shader runs at mediump, and
    // Source world coordinates reach +/-16384 - far beyond half float's useful
    // range - so normalize(uCameraPosition - vWorldPosition) went coarse and
    // cubemap reflections collapsed into a flat wash unless the camera was
    // close. These are the same values the water-preview harness requests;
    // NVIDIA returns a 4.6 compatibility context for them.
    // Desktop OpenGL 4.6 is now a hard requirement (the ES and pre-4.6 shader
    // paths were removed), so request it explicitly rather than relying on the
    // driver to hand back something newer than asked for. Compatibility profile
    // is retained because the renderer is not core-profile clean.
    QSurfaceFormat glFormat;
    glFormat.setRenderableType(QSurfaceFormat::OpenGL);
    glFormat.setVersion(4, 6);
    glFormat.setProfile(QSurfaceFormat::CompatibilityProfile);
    glFormat.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(glFormat);
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral(HAMMER_APP_ID));
    // No setApplicationDisplayName: Qt appends it (dash-separated) to every
    // window and dialog title. The main window composes its own
    // "<map> - Hammer--" title and popups keep just their own names.
    QApplication::setApplicationVersion(QStringLiteral(HAMMER_PORT_VERSION));
    QApplication::setOrganizationName(QStringLiteral(HAMMER_SETTINGS_ORGANIZATION));
    QApplication::setDesktopFileName(QStringLiteral(HAMMER_APP_ID));
    migrateLegacyHammerSettings();
    migrateGeneralGroupSettings();

    HammerTheme::initialize(application);
    const QPixmap appLogo = loadHammerLogo();
    application.setWindowIcon(loadHammerWindowIcon());

    const QPixmap splashPixmap = appLogo.isNull()
        ? QPixmap(256, 256)
        : appLogo.scaled(QSize(256, 256), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QSplashScreen splash(splashPixmap);
    splash.setWindowFlag(Qt::FramelessWindowHint);
    if (!splashPixmap.isNull()) {
        splash.show();
        application.processEvents();
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Hammer--, a Linux/Qt6 Source map editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("maps"),
                                 QStringLiteral("VMF map files to open"),
                                 QStringLiteral("[maps...]"));
    parser.process(application);

    MainWindow window(parser.positionalArguments());
    window.show();
    if (splash.isVisible()) splash.finish(&window);
    return application.exec();
}
