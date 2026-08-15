#include "MainWindow.hpp"

#include "RecentFiles.hpp"
#include "PreviewRenderGate.hpp"

#include "FaceEditSheet.hpp"
#include "ScrollMenu.hpp"
#include "HammerIconFactory.hpp"
#include "ToolbarIcons.hpp"
#include "HammerTheme.hpp"
#include "FreemanWindow.hpp"
#include "CollabSession.hpp"
#include "MapDocumentWidget.hpp"
#include "MapViewWidget.hpp"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QInputDialog>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QEventLoop>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QHeaderView>
#include <QRegularExpression>
#include <QTableWidget>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QLayout>
#include <QMenu>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QPixmap>
#include <QDebug>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>

#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <utility>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <tuple>
#include <vector>

namespace {
const QString MapEditStrip = QStringLiteral(":/hammer/mapedit256.bmp");

QWidget* wrapWithLayout(QLayout* layout)
{
    auto* widget = new QWidget;
    widget->setLayout(layout);
    return widget;
}

class TextureBrowserViewportWatcher final : public QObject
{
public:
    explicit TextureBrowserViewportWatcher(std::function<void()> callback, QObject* parent = nullptr)
        : QObject(parent), callback_(std::move(callback))
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::LayoutRequest:
            if (callback_) callback_();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> callback_;
};

class HammerStatusPaneLabel final : public QLabel
{
public:
    HammerStatusPaneLabel(const QString& text, int preferredWidth)
        : QLabel(text), preferredWidth_(preferredWidth)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        QSize hint = QLabel::sizeHint();
        hint.setWidth(std::max(hint.width(), preferredWidth_));
        return hint;
    }

    QSize minimumSizeHint() const override
    {
        QSize hint = QLabel::minimumSizeHint();
        hint.setWidth(0);
        return hint;
    }

private:
    int preferredWidth_{0};
};

QLabel* makeStatusPane(const QString& text, int preferredWidth)
{
    auto* label = new HammerStatusPaneLabel(text, preferredWidth);
    label->setMinimumWidth(0);
    label->setFrameStyle(QFrame::NoFrame);
    label->setProperty("hammerStatusPane", true);
    label->setMargin(0);
    return label;
}

QPixmap makeTexturePreview()
{
    QPixmap pixmap(96, 96);
    QPainter painter(&pixmap);
    painter.fillRect(pixmap.rect(), QColor(18, 18, 18));
    constexpr int tile = 12;
    for (int y = 0; y < pixmap.height(); y += tile) {
        for (int x = 0; x < pixmap.width(); x += tile) {
            const bool light = ((x / tile) + (y / tile)) % 2 == 0;
            painter.fillRect(x, y, tile, tile, light ? QColor(104, 104, 104) : QColor(54, 54, 54));
        }
    }
    painter.setPen(QColor(235, 235, 235));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("tools/\ntoolsnodraw"));
    return pixmap;
}

// `blendImage` is a WorldVertexTransition $basetexture2. When present the
// preview shows the transition itself rather than just the base layer:
// $basetexture in the upper-left corner lerping along the diagonal to
// $basetexture2 in the lower-right, which is the blend a displacement paints.
QPixmap materialPreviewPixmap(const hammer::assets::Image& image, const QSize& size,
                                  int animationOffset = 0,
                                  const hammer::assets::Image* flowMap = nullptr,
                                  float flowCycleRate = 1.0f,
                                  float flowDistance = 0.10f,
                                  float flowMapScale = 1.0f,
                                  const hammer::assets::Image* blendImage = nullptr)
{
    if (!image.valid()) return makeTexturePreview().scaled(size, Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation);
    const QImage wrapped(reinterpret_cast<const uchar*>(image.pixels.data()),
                         image.width, image.height,
                         image.width * static_cast<int>(sizeof(std::uint32_t)),
                         QImage::Format_ARGB32);
    QImage source = wrapped.copy();
    if (animationOffset != 0 && source.width() > 1 && source.height() > 1 &&
        flowMap && flowMap->valid()) {
        // The browser uses the same two-phase flow convention as the GPU.
        // animationOffset advances by three every 120 ms, hence 0.04 seconds
        // per offset unit.
        const double cycle = animationOffset * 0.04 * flowCycleRate;
        const double phaseA = cycle - std::floor(cycle);
        const double phaseB = (cycle + 0.5) - std::floor(cycle + 0.5);
        const double blend = std::abs(phaseA * 2.0 - 1.0);
        QImage flowed(source.size(), source.format());
        auto blendPixel = [blend](std::uint32_t a, std::uint32_t b) {
            auto channel = [blend](std::uint32_t lhs, std::uint32_t rhs, int shift) {
                const double x = static_cast<double>((lhs >> shift) & 255u);
                const double y = static_cast<double>((rhs >> shift) & 255u);
                return static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(std::lround(x + (y - x) * blend)), 0, 255));
            };
            return (channel(a, b, 24) << 24) |
                   (channel(a, b, 16) << 16) |
                   (channel(a, b, 8) << 8) |
                   channel(a, b, 0);
        };
        for (int y = 0; y < source.height(); ++y) {
            auto* target = reinterpret_cast<std::uint32_t*>(flowed.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                const double flowU = static_cast<double>(x) / source.width() *
                                     flowMap->width * flowMapScale;
                const double flowV = static_cast<double>(y) / source.height() *
                                     flowMap->height * flowMapScale;
                const std::uint32_t encoded = flowMap->sampleWrapped(flowU, flowV);
                const double flowX = 1.0 - 2.0 * ((encoded >> 16) & 255u) / 255.0;
                const double flowY = 2.0 * ((encoded >> 8) & 255u) / 255.0 - 1.0;
                const double distanceX = flowDistance * source.width();
                const double distanceY = flowDistance * source.height();
                const std::uint32_t sampleA = image.sampleWrapped(
                    x - flowX * phaseA * distanceX,
                    y + flowY * phaseA * distanceY);
                const std::uint32_t sampleB = image.sampleWrapped(
                    x - flowX * phaseB * distanceX,
                    y + flowY * phaseB * distanceY);
                target[x] = blendPixel(sampleA, sampleB);
            }
        }
        source = std::move(flowed);
    } else if (animationOffset != 0 && source.width() > 1 && source.height() > 1) {
        const int offsetX = animationOffset % source.width();
        const int offsetY = (animationOffset / 2) % source.height();
        QImage shifted(source.size(), source.format());
        shifted.fill(Qt::transparent);
        QPainter painter(&shifted);
        for (int y = -source.height(); y <= source.height(); y += source.height()) {
            for (int x = -source.width(); x <= source.width(); x += source.width()) {
                painter.drawImage(x + offsetX, y + offsetY, source);
            }
        }
        source = std::move(shifted);
    }
    if (blendImage && blendImage->valid() && source.width() > 0 && source.height() > 0) {
        // Blend weight runs along the diagonal: 0 at the top-left corner, 1 at
        // the bottom-right, averaged from the two axes so the transition band
        // is the anti-diagonal.
        QImage blended(source.size(), QImage::Format_ARGB32);
        const double lastColumn = std::max(1, source.width() - 1);
        const double lastRow = std::max(1, source.height() - 1);
        for (int y = 0; y < source.height(); ++y) {
            const auto* primaryRow =
                reinterpret_cast<const std::uint32_t*>(source.constScanLine(y));
            auto* target = reinterpret_cast<std::uint32_t*>(blended.scanLine(y));
            // The two textures need not share dimensions, so the secondary is
            // sampled by normalized position rather than by pixel index.
            const double secondaryV = static_cast<double>(y) / source.height() *
                                      blendImage->height;
            for (int x = 0; x < source.width(); ++x) {
                const double weight = std::clamp(
                    (x / lastColumn + y / lastRow) * 0.5, 0.0, 1.0);
                const double secondaryU = static_cast<double>(x) / source.width() *
                                          blendImage->width;
                const std::uint32_t primary = primaryRow[x];
                const std::uint32_t secondary = blendImage->sampleWrapped(secondaryU, secondaryV);
                auto channel = [&](int shift) {
                    const double from = static_cast<double>((primary >> shift) & 255u);
                    const double to = static_cast<double>((secondary >> shift) & 255u);
                    return static_cast<std::uint32_t>(std::clamp(
                        static_cast<int>(std::lround(from + (to - from) * weight)), 0, 255));
                };
                target[x] = (channel(24) << 24) | (channel(16) << 16) |
                            (channel(8) << 8) | channel(0);
            }
        }
        source = std::move(blended);
    }
    return QPixmap::fromImage(source).scaled(size, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation);
}

int textureBrowserThumbnailSize()
{
    const int configured = QSettings().value(QStringLiteral("textures/browserThumbnailSize"), 64).toInt();
    return configured == 32 || configured == 64 || configured == 128 || configured == 256
        ? configured : 64;
}

bool textureBrowserAnimationsEnabled()
{
    return QSettings().value(QStringLiteral("textures/animatePreviews"), true).toBool();
}

// Builds the Material Browser's 3D preview scene: a cube resting on a
// sculpted (power-3, alpha-painted) displacement, every face using the
// given material. Going through VMF text + buildScene reuses the exact
// displacement tessellation and texture-axis pipeline the real views use.
std::shared_ptr<const hammer::vmf::Scene> buildMaterialPreviewScene(const QString& materialName,
                                                                    const std::string& skyName,
                                                                    bool water)
{
    const std::string material = materialName.toStdString();
    std::string vmf = "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n";
    int nextId = 2;

    const auto side = [&](const std::array<double, 9>& plane, const char* uAxis,
                          const char* vAxis, const std::string& extra = {}) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
                      "side\n{\n\"id\" \"%d\"\n"
                      "\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n"
                      "\"material\" \"%s\"\n"
                      "\"uaxis\" \"[%s 0] 0.25\"\n\"vaxis\" \"[%s 0] 0.25\"\n"
                      "\"lightmapscale\" \"16\"\n",
                      nextId++, plane[0], plane[1], plane[2], plane[3], plane[4], plane[5],
                      plane[6], plane[7], plane[8], material.c_str(), uAxis, vAxis);
        vmf += buffer;
        vmf += extra;
        vmf += "}\n";
    };
    const auto box = [&](double ax, double ay, double az, double bx, double by, double bz,
                         const std::string& topExtra) {
        vmf += "solid\n{\n";
        char idLine[48];
        std::snprintf(idLine, sizeof(idLine), "\"id\" \"%d\"\n", nextId++);
        vmf += idLine;
        side({ax, ay, bz, ax, by, bz, bx, by, bz}, "1 0 0", "0 -1 0", topExtra); // +Z
        side({ax, by, az, ax, ay, az, bx, ay, az}, "1 0 0", "0 -1 0");           // -Z
        side({ax, ay, bz, ax, ay, az, ax, by, az}, "0 1 0", "0 0 -1");           // -X
        side({bx, by, bz, bx, by, az, bx, ay, az}, "0 1 0", "0 0 -1");           // +X
        side({bx, ay, bz, bx, ay, az, ax, ay, az}, "1 0 0", "0 0 -1");           // -Y
        side({ax, by, bz, ax, by, az, bx, by, az}, "1 0 0", "0 0 -1");           // +Y
        vmf += "}\n";
    };

    // Sculpted ground: gentle rolling bumps, flat under the cube, and an
    // alpha gradient so blend materials show their painted second texture.
    std::string disp = "dispinfo\n{\n\"power\" \"3\"\n"
                       "\"startposition\" \"[-256 -256 0]\"\n\"elevation\" \"0\"\n"
                       "\"subdiv\" \"0\"\nnormals\n{\n";
    std::string distances = "distances\n{\n";
    std::string alphas = "alphas\n{\n";
    for (int row = 0; row < 9; ++row) {
        char rowKey[16];
        std::snprintf(rowKey, sizeof(rowKey), "\"row%d\" \"", row);
        std::string normalsRow(rowKey), distancesRow(rowKey), alphasRow(rowKey);
        for (int column = 0; column < 9; ++column) {
            const double dx = column - 4.0;
            const double dy = row - 4.0;
            const double centerFalloff = std::min(1.0, (dx * dx + dy * dy) / 10.0);
            const double height = centerFalloff *
                (14.0 + 10.0 * std::sin(column * 1.1) * std::cos(row * 0.9));
            const int alpha = static_cast<int>(std::clamp(
                255.0 * (column + row) / 16.0, 0.0, 255.0));
            char value[64];
            normalsRow += column ? " 0 0 1" : "0 0 1";
            std::snprintf(value, sizeof(value), column ? " %.1f" : "%.1f", height);
            distancesRow += value;
            std::snprintf(value, sizeof(value), column ? " %d" : "%d", alpha);
            alphasRow += value;
        }
        disp += normalsRow + "\"\n";
        distances += distancesRow + "\"\n";
        alphas += alphasRow + "\"\n";
    }
    disp += "}\n" + distances + "}\n" + alphas + "}\n}\n";

    if (water) {
        // Water reads as a surface, not a solid: one flat brush covering the
        // same footprint as the displacement, nothing else.
        box(-256, -256, -32, 256, 256, 0, {});
    } else {
        box(-256, -256, -32, 256, 256, 0, disp); // ground
        box(-32, -32, 0, 32, 32, 64, {});        // cube
    }
    vmf += "}\n";

    const auto document = hammer::vmf::Document::parse(std::move(vmf));
    if (!document) return nullptr;
    auto scene = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
    scene->skyName = skyName;
    scene->invalidateLineage();
    return scene;
}

QString materialToolTip(const std::shared_ptr<const hammer::assets::Material>& material)
{
    if (!material) return QObject::tr("Material could not be loaded");
    QStringList lines;
    lines << QString::fromStdString(material->name)
          << QObject::tr("Shader: %1").arg(QString::fromStdString(material->shader));
    if (material->image.valid()) {
        lines << QObject::tr("Size: %1 x %2").arg(material->image.width).arg(material->image.height);
    }
    if (!material->baseTexture.empty()) {
        lines << QObject::tr("Texture: %1").arg(QString::fromStdString(material->baseTexture));
    }
    if (!material->note.empty()) lines << QString::fromStdString(material->note);
    if (!material->error.empty()) lines << QString::fromStdString(material->error);
    lines << QObject::tr("VMT: %1").arg(material->vmtSource.empty()
        ? QObject::tr("not found") : QString::fromStdString(material->vmtSource));
    if (!material->vtfSource.empty()) {
        lines << QObject::tr("VTF: %1").arg(QString::fromStdString(material->vtfSource));
    }
    if (material->waterHasFlowMap) {
        lines << QObject::tr("Flow map: %1").arg(QString::fromStdString(material->waterFlowMap));
        if (!material->waterFlowSource.empty()) {
            lines << QObject::tr("Flow VTF: %1").arg(QString::fromStdString(material->waterFlowSource));
        }
    }
    return lines.join(QLatin1Char('\n'));
}


QPixmap loadHammerLogoPixmap()
{
    QStringList candidates;
#ifdef HAMMER_APP_LOGO_PATH
    candidates << QStringLiteral(HAMMER_APP_LOGO_PATH);
#endif
    candidates << QStringLiteral(":/hammer/app_logo.png");
    candidates << QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("../share/hammerminusminus/app_logo.png"));
    for (const QString& candidate : candidates) {
        QPixmap logo(candidate);
        if (!logo.isNull()) return logo;
    }
    return QPixmap(QStringLiteral(":/hammer/forge.ico"));
}

QIcon loadSidebarToolIcon(const QString& fileName)
{
    QStringList candidates;
#ifdef HAMMER_TOOL_ICON_DIR
    // Prefer the exact files supplied by the user. This path is compiled from
    // the current source tree and works for normal in-tree Fedora builds even
    // if a stale AUTORCC object is present in an existing build directory.
    candidates << QDir(QString::fromUtf8(HAMMER_TOOL_ICON_DIR)).filePath(fileName);
#endif
    candidates << QStringLiteral(":/hammer/tool_icons/") + fileName;
    candidates << QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("../share/hammerminusminus/tool_icons/") + fileName);

    QPixmap source;
    QString loadedFrom;
    for (const QString& candidate : candidates) {
        if (source.load(candidate)) {
            loadedFrom = candidate;
            break;
        }
    }
    if (source.isNull()) {
        qWarning().noquote() << "Hammer--: unable to load sidebar tool icon" << fileName
                             << "from" << candidates.join(QStringLiteral(", "));
        return {};
    }

    const QPixmap scaled = source.scaled(QSize(36, 36), Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
    QIcon icon;
    icon.addPixmap(scaled, QIcon::Normal, QIcon::Off);
    icon.addPixmap(scaled, QIcon::Normal, QIcon::On);
    icon.addPixmap(scaled, QIcon::Active, QIcon::Off);
    icon.addPixmap(scaled, QIcon::Active, QIcon::On);
    icon.addPixmap(scaled, QIcon::Selected, QIcon::Off);
    icon.addPixmap(scaled, QIcon::Selected, QIcon::On);
    qDebug().noquote() << "Hammer--: loaded sidebar icon" << fileName << "from" << loadedFrom;
    return icon;
}
}

MainWindow::MainWindow(const QStringList& paths, QWidget* parent)
    : QMainWindow(parent), fgd_(std::make_shared<hammer::fgd::Database>())
{
    setObjectName(QStringLiteral("HammerMainFrame"));
    setWindowTitle(QStringLiteral(HAMMER_APP_DISPLAY_NAME));
    setWindowIcon(QApplication::windowIcon());
    resize(1440, 900);
    setMinimumSize(640, 480);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks);

    mdiArea_ = new QMdiArea(this);
    mdiArea_->setObjectName(QStringLiteral("HammerMdiClient"));
    mdiArea_->setViewMode(QMdiArea::SubWindowView);
    mdiArea_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mdiArea_->setOption(QMdiArea::DontMaximizeSubWindowOnActivation, false);
    setCentralWidget(mdiArea_);

    // Coalesce continuous resize/dock geometry notifications. Re-applying a
    // maximized MDI state from every QResizeEvent caused a resize feedback loop
    // and made the whole editor repaint continuously on several Linux desktops.
    layoutRefreshTimer_ = new QTimer(this);
    layoutRefreshTimer_->setSingleShot(true);
    layoutRefreshTimer_->setTimerType(Qt::CoarseTimer);
    layoutRefreshTimer_->setInterval(120);
    connect(layoutRefreshTimer_, &QTimer::timeout,
            this, &MainWindow::normalizeResizableLayout);

    createMenus();
    createMdiSystemButton();
    createToolbars();
    createRightControlBars();
    createMessageWindow();
    createStatusBar();
    QSettings startupSettings;
    materialRenderingEnabled_ = startupSettings.value(QStringLiteral("render/materials3d"), true).toBool();
    wireframeOverlayEnabled_ = startupSettings.value(QStringLiteral("render/wireframeOverlay3d"), false).toBool();
    displacementSolidMaskEnabled_ =
        startupSettings.value(QStringLiteral("render/displacementSolidMask"), true).toBool();
    const QString savedTexturedMode = startupSettings.value(
        QStringLiteral("render/texturedMode"), QStringLiteral("unlit")).toString();
    if (savedTexturedMode == QStringLiteral("ray-traced-preview")) {
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
        texturedRenderMode_ = MapViewWidget::TexturedRenderMode::RayTracedPreview;
#else
        texturedRenderMode_ = MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons;
#endif
    } else if (savedTexturedMode == QStringLiteral("shaded-material-polygons")) {
        texturedRenderMode_ = MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons;
    } else if (savedTexturedMode == QStringLiteral("shaded")) {
        texturedRenderMode_ = MapViewWidget::TexturedRenderMode::Shaded;
    } else {
        texturedRenderMode_ = MapViewWidget::TexturedRenderMode::Unlit;
    }
    phongEnabled_ = startupSettings.value(QStringLiteral("render/materialPhong"), true).toBool();
    specularEnabled_ = startupSettings.value(QStringLiteral("render/materialSpecular"), true).toBool();
    bumpMapsEnabled_ = startupSettings.value(QStringLiteral("render/materialBumpMaps"), true).toBool();
    hdrEnabled_ = startupSettings.value(QStringLiteral("render/hdr"), true).toBool();
    rayTracedGamma_ = std::clamp(
        startupSettings.value(QStringLiteral("render/rayTracedGamma"), 2.2).toFloat(), 0.5f, 5.0f);
    lightWarpEnabled_ = startupSettings.value(QStringLiteral("render/materialLightWarp"), true).toBool();
    selfIllumEnabled_ = startupSettings.value(QStringLiteral("render/materialSelfIllum"), true).toBool();
    rimLightEnabled_ = startupSettings.value(QStringLiteral("render/materialRimLight"), true).toBool();
    if (texturedViewAction_) texturedViewAction_->setChecked(texturedRenderMode_ == MapViewWidget::TexturedRenderMode::Unlit);
    if (shadedTexturedViewAction_) shadedTexturedViewAction_->setChecked(texturedRenderMode_ == MapViewWidget::TexturedRenderMode::Shaded);
    if (shadedMaterialPolygonsViewAction_) shadedMaterialPolygonsViewAction_->setChecked(
        texturedRenderMode_ == MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons);
    if (rayTracedPreviewAction_) rayTracedPreviewAction_->setChecked(
        texturedRenderMode_ == MapViewWidget::TexturedRenderMode::RayTracedPreview);
    if (wireframeOverlayAction_) wireframeOverlayAction_->setChecked(wireframeOverlayEnabled_);
    if (displacementSolidMaskAction_) {
        displacementSolidMaskAction_->setChecked(displacementSolidMaskEnabled_);
    }
    if (hdrAction_) hdrAction_->setChecked(hdrEnabled_);

    const QString savedFgd = startupSettings.value(QStringLiteral("gameData/lastFgd")).toString();
    if (!savedFgd.isEmpty()) loadFgdPath(savedFgd, false);
    QString savedGamePath = startupSettings.value(QStringLiteral("game/gameDirectory")).toString();
    if (savedGamePath.isEmpty()) {
        savedGamePath = startupSettings.value(QStringLiteral("game/lastGameInfo")).toString();
    }
    if (!savedGamePath.isEmpty()) loadGameInfoPath(savedGamePath, false);
    connect(mdiArea_, &QMdiArea::subWindowActivated, this, [this](QMdiSubWindow*) {
        updateMdiSystemButtonState();
        if (MapDocumentWidget* document = activeDocument()) {
            selectionPane_->setText(document->selectionSummary());
            sizePane_->setText(document->selectionSizeSummary());
            setWindowTitle(tr("%1 - Hammer--").arg(document->displayName()));
            applyObjectBarSettings(document);
            document->setTransformMode(transformMode_);
            if (faceEditSheet_) {
                faceEditSheet_->setFaceValues(document->faceEditValues());
                std::optional<int> dispPower;
                std::optional<double> dispElevation;
                document->displacementAttributeValues(dispPower, dispElevation);
                faceEditSheet_->setDisplacementAttributes(dispPower, dispElevation);
                document->setLightmapGridVisible(lightmapGridVisible_);
            }
            if (currentToolId_ == QStringLiteral("tool.block")) document->setTool(MapViewWidget::Tool::Block);
            else if (currentToolId_ == QStringLiteral("tool.entity")) document->setTool(MapViewWidget::Tool::Entity);
            else if (currentToolId_ == QStringLiteral("tool.decals")) document->setTool(MapViewWidget::Tool::Decal);
            else if (currentToolId_ == QStringLiteral("tool.overlay")) document->setTool(MapViewWidget::Tool::Overlay);
            else if (currentToolId_ == QStringLiteral("tool.magnify")) document->setTool(MapViewWidget::Tool::Magnify);
            else if (currentToolId_ == QStringLiteral("tool.camera")) document->setTool(MapViewWidget::Tool::Camera);
            else if (currentToolId_ == QStringLiteral("tool.clipper")) document->setTool(MapViewWidget::Tool::Clipper);
            else if (currentToolId_ == QStringLiteral("tool.morph")) document->setTool(MapViewWidget::Tool::Morph);
        else if (currentToolId_ == QStringLiteral("tool.textureApplication")) document->setTool(MapViewWidget::Tool::TextureApplication);
            else document->setTool(MapViewWidget::Tool::Selection);
        } else {
            selectionPane_->setText(tr("no selection"));
            sizePane_->setText(QStringLiteral("0 x 0 x 0"));
            setWindowTitle(QStringLiteral(HAMMER_APP_DISPLAY_NAME));
        }
        updateEditActions();
        updateProjectionActions();
    });
    restoreWindowLayout();
    if (paths.isEmpty()) {
        createDocument();
    } else {
        for (const QString& path : paths) createDocument(path);
        if (mdiArea_->subWindowList().isEmpty()) createDocument();
    }

    setPrompt(tr("Ready"));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    mdiArea_->closeAllSubWindows();
    if (mdiArea_->currentSubWindow()) {
        event->ignore();
        return;
    }
    saveWindowLayout();
    event->accept();
}

bool MainWindow::event(QEvent* event)
{
    // A maximized QMdiSubWindow appends " - [<subwindow title>]" to the main
    // window title, duplicating the map name the title already carries, and
    // Qt never removes the suffix when the subwindow title changes to empty.
    // Strip it the moment it lands instead, keeping "<map> - Hammer--" as the
    // one and only title text.
    if (event->type() == QEvent::WindowTitleChange && !strippingMdiTitleSuffix_) {
        const QString title = windowTitle();
        const int suffix = title.indexOf(QStringLiteral(" - ["));
        if (suffix >= 0 && title.endsWith(QLatin1Char(']'))) {
            strippingMdiTitleSuffix_ = true;
            setWindowTitle(title.left(suffix));
            strippingMdiTitleSuffix_ = false;
        }
    }
    return QMainWindow::event(event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // Let Qt perform the live resize itself, then make one delayed stale-MDI
    // correction after resizing settles. The old 35 ms cadence repeatedly
    // forced another full viewport render during interactive resizing.
    scheduleResizableLayoutRefresh();
    updateMdiSystemButtonGeometry();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        normalizeResizableLayout();
        updateMdiSystemButtonGeometry();
        updateMdiSystemButtonState();
    });
}

void MainWindow::scheduleResizableLayoutRefresh()
{
    if (layoutRefreshTimer_) layoutRefreshTimer_->start();
}

void MainWindow::normalizeResizableLayout()
{
    if (!mdiArea_) return;
    QWidget* viewport = mdiArea_->viewport();
    if (!viewport) return;

    // Qt normally tracks this itself. Some Wayland styles leave a maximized MDI
    // child one layout step behind after docks move, so correct the geometry
    // directly only when it is actually stale. Calling showMaximized() here
    // generated another layout/resize cycle and was the source of the lag.
    if (QMdiSubWindow* subWindow = mdiArea_->activeSubWindow();
        subWindow && subWindow->isMaximized()) {
        const QRect target = viewport->rect();
        if (subWindow->geometry() != target) subWindow->setGeometry(target);
    }
}

void MainWindow::restoreWindowLayout()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("mainWindow/geometry")).toByteArray();
    const QByteArray state = settings.value(QStringLiteral("mainWindow/state")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    if (!state.isEmpty()) {
        restoreState(state, 3);
    }
}

void MainWindow::saveWindowLayout() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("mainWindow/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("mainWindow/state"), saveState(3));
}

QAction* MainWindow::command(const QString& id, const QString& text, const QKeySequence& shortcut)
{
    if (commands_.contains(id)) {
        QAction* existing = commands_.value(id);
        if (!text.isEmpty()) {
            existing->setText(text);
        }
        if (!shortcut.isEmpty()) {
            existing->setShortcut(shortcut);
        }
        return existing;
    }

    auto* action = new QAction(text, this);
    action->setObjectName(id);
    action->setShortcut(shortcut);
    connect(action, &QAction::triggered, this, [this, action] {
        if (!action->property("implemented").toBool()) {
            setPrompt(tr("%1 — command wiring pending").arg(action->text().remove('&')));
        }
    });
    commands_.insert(id, action);
    return action;
}

QAction* MainWindow::addMenuCommand(QMenu* menu, const QString& id, const QString& text,
                                    const QKeySequence& shortcut, bool checkable, bool checked)
{
    QAction* action = command(id, text, shortcut);
    action->setCheckable(checkable);
    action->setChecked(checked);
    menu->addAction(action);
    return action;
}

QAction* MainWindow::addDrawnCommand(QToolBar* bar, const QString& id, const QString& text,
                                     const QString& iconName, bool checkable)
{
    QAction* action = commands_.contains(id) ? command(id) : command(id, text);
    action->setIcon(ToolbarIcons::icon(iconName));
    action->setToolTip(text);
    action->setStatusTip(text);
    action->setCheckable(checkable);
    bar->addAction(action);
    return action;
}

QMenu* MainWindow::addScrollMenu(const QString& title)
{
    // Menubar dropdowns scroll below the menubar when they run out of screen
    // height instead of QMenu's default slide-up-over-the-menubar placement.
    auto* menu = new ScrollMenu(title, menuBar());
    menuBar()->addMenu(menu);
    return menu;
}

void MainWindow::createMenus()
{
    QMenu* file = addScrollMenu(tr("&File"));
    QAction* newMap = addMenuCommand(file, QStringLiteral("file.new"), tr("&New"), QKeySequence::New);
    QAction* openMap = addMenuCommand(file, QStringLiteral("file.open"), tr("&Open..."), QKeySequence::Open);
    QAction* closeMap = addMenuCommand(file, QStringLiteral("file.close"), tr("&Close"));
    QAction* saveMap = addMenuCommand(file, QStringLiteral("file.save"), tr("&Save"), QKeySequence::Save);
    QAction* saveMapAs = addMenuCommand(file, QStringLiteral("file.saveAs"), tr("Save &As..."));
    file->addSeparator();
    QAction* runMap = addMenuCommand(file, QStringLiteral("file.runMap"), tr("R&un Map..."), QKeySequence(Qt::Key_F9));
    runMap->setProperty("implemented", true);
    connect(runMap, &QAction::triggered, this, &MainWindow::runMapCompile);
    file->addSeparator();
    // Built from the desktop's own recently-used store rather than a private
    // list, so this menu and Files' "Recent" tab always agree. Repopulated
    // when the menu opens: another Hammer window, or any other application,
    // may have written the store since it was last shown.
    recentFilesMenu_ = file->addMenu(tr("&Recent Files"));
    // Map file names repeat across games, so each entry shows its full path.
    recentFilesMenu_->setToolTipsVisible(true);
    connect(recentFilesMenu_, &QMenu::aboutToShow, this, &MainWindow::refreshRecentFilesMenu);
    refreshRecentFilesMenu();
    file->addSeparator();
    QAction* exit = addMenuCommand(file, QStringLiteral("app.exit"), tr("E&xit"), QKeySequence::Quit);

    newMap->setProperty("implemented", true);
    openMap->setProperty("implemented", true);
    closeMap->setProperty("implemented", true);
    saveMap->setProperty("implemented", true);
    saveMapAs->setProperty("implemented", true);
    exit->setProperty("implemented", true);
    connect(newMap, &QAction::triggered, this, [this] { createDocument(); });
    connect(openMap, &QAction::triggered, this, &MainWindow::openDocument);
    connect(closeMap, &QAction::triggered, this, [this] {
        if (QMdiSubWindow* sub = mdiArea_->activeSubWindow()) {
            sub->close();
        }
    });
    connect(saveMap, &QAction::triggered, this, &MainWindow::saveDocument);
    connect(saveMapAs, &QAction::triggered, this, &MainWindow::saveDocumentAs);
    connect(exit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = addScrollMenu(tr("&Edit"));
    QAction* undo = addMenuCommand(edit, QStringLiteral("edit.undo"), tr("&Undo"), QKeySequence::Undo);
    QAction* redo = addMenuCommand(edit, QStringLiteral("edit.redo"), tr("&Redo"), QKeySequence::Redo);
    undoRedoActiveAction_ = addMenuCommand(edit, QStringLiteral("edit.undoRedoActive"),
                                           tr("Undo/Redo Active"), {}, true, undoRedoActive_);
    undoRedoActiveAction_->setProperty("implemented", true);
    undoRedoActiveAction_->setStatusTip(tr("Keep undo/redo history in memory; unchecking "
                                           "discards the history of every open map"));
    connect(undoRedoActiveAction_, &QAction::toggled, this, &MainWindow::setUndoRedoActive);
    edit->addSeparator();
    QAction* findEntities = addMenuCommand(edit, QStringLiteral("edit.findEntities"),
                                          tr("&Find entities..."),
                                          QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(findEntities, &QAction::triggered, this, &MainWindow::showFindEntitiesDialog);
    edit->addSeparator();
    QAction* cut = addMenuCommand(edit, QStringLiteral("edit.cut"), tr("Cu&t"), QKeySequence::Cut);
    QAction* copy = addMenuCommand(edit, QStringLiteral("edit.copy"), tr("&Copy"), QKeySequence::Copy);
    QAction* paste = addMenuCommand(edit, QStringLiteral("edit.paste"), tr("&Paste"), QKeySequence::Paste);
    QAction* duplicate = addMenuCommand(edit, QStringLiteral("edit.duplicate"), tr("D&uplicate"), QKeySequence(QStringLiteral("Ctrl+D")));
    QAction* pasteSpecial = addMenuCommand(edit, QStringLiteral("edit.pasteSpecial"), tr("Pa&ste Special..."));
    QAction* deleteObjects = addMenuCommand(edit, QStringLiteral("edit.delete"), tr("&Delete"), QKeySequence::Delete);
    edit->addSeparator();
    QAction* clearSelection = addMenuCommand(edit, QStringLiteral("edit.clearSelection"), tr("C&lear Selection"), QKeySequence(QStringLiteral("Shift+Q")));
    QAction* selectAll = addMenuCommand(edit, QStringLiteral("edit.selectAll"), tr("Select &All"), QKeySequence::SelectAll);
    edit->addSeparator();
    QAction* properties = addMenuCommand(edit, QStringLiteral("edit.properties"), tr("Pr&operties"), QKeySequence(QStringLiteral("Alt+Enter")));

    for (QAction* action : {undo, redo, findEntities, cut, copy, paste, pasteSpecial, duplicate, deleteObjects, clearSelection, selectAll, properties}) {
        action->setProperty("implemented", true);
    }
    connect(undo, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->undo(); });
    connect(redo, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->redo(); });
    connect(copy, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) {
            clipboard_ = document->copySelection();
            setPrompt(tr("Copied %1 object(s)").arg(static_cast<qulonglong>(clipboard_.objects.size())));
            updateEditActions();
        }
    });
    connect(cut, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) {
            if (document->cutSelection(clipboard_)) updateEditActions();
        }
    });
    connect(paste, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->paste(clipboard_); });
    connect(pasteSpecial, &QAction::triggered, this, &MainWindow::showPasteSpecialDialog);
    connect(duplicate, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->duplicateSelection(); });
    connect(deleteObjects, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->deleteSelection(); });
    connect(clearSelection, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->clearSelection(); });
    connect(selectAll, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->selectAll(); });
    connect(properties, &QAction::triggered, this, [this] { if (MapDocumentWidget* document = activeDocument()) document->showObjectProperties(this); });

    QMenu* map = addScrollMenu(tr("&Map"));
    QAction* snapGrid = addMenuCommand(map, QStringLiteral("map.snapGrid"), tr("&Snap to Grid"), QKeySequence(QStringLiteral("Shift+W")), true, true);
    QAction* showGrid = addMenuCommand(map, QStringLiteral("view.grid"), tr("Sho&w Grid"), QKeySequence(QStringLiteral("Shift+R")), true, true);
    QMenu* grid = map->addMenu(tr("&Grid Settings"));
    QAction* gridLower = addMenuCommand(grid, QStringLiteral("map.gridLower"), tr("&Smaller grid"), QKeySequence(QStringLiteral("[")));
    QAction* gridHigher = addMenuCommand(grid, QStringLiteral("map.gridHigher"), tr("&Bigger grid"), QKeySequence(QStringLiteral("]")));
    snapGrid->setProperty("implemented", true);
    gridLower->setProperty("implemented", true);
    gridHigher->setProperty("implemented", true);
    connect(snapGrid, &QAction::toggled, this, [this](bool enabled) {
        gridSnapEnabled_ = enabled;
        applyGridSettings();
        setPrompt(enabled ? tr("Snap to grid enabled") : tr("Snap to grid disabled"));
    });
    connect(gridLower, &QAction::triggered, this, [this] {
        gridSpacing_ = std::max(1, gridSpacing_ / 2);
        applyGridSettings();
        setPrompt(tr("Grid: %1 units").arg(gridSpacing_));
    });
    connect(gridHigher, &QAction::triggered, this, [this] {
        gridSpacing_ = std::min(512, gridSpacing_ * 2);
        applyGridSettings();
        setPrompt(tr("Grid: %1 units").arg(gridSpacing_));
    });
    map->addSeparator();
    addMenuCommand(map, QStringLiteral("map.showBrushNumber"), tr("Show Selected Brush Number"));
    QAction* entityReport = addMenuCommand(map, QStringLiteral("map.entityReport"),
                                          tr("&Entity Report..."));
    entityReport->setProperty("implemented", true);
    connect(entityReport, &QAction::triggered, this, &MainWindow::showEntityReportDialog);
    QAction* entityGallery = addMenuCommand(map, QStringLiteral("map.entityGallery"),
                                           tr("Entity G&allery"));
    entityGallery->setProperty("implemented", true);
    entityGallery->setStatusTip(tr("Paste one of every entity in the game data into the map "
                                   "(debugging aid)"));
    connect(entityGallery, &QAction::triggered, this, [this] {
        MapDocumentWidget* document = activeDocument();
        if (!document) return;
        if (loadedFgdPath_.isEmpty()) {
            QMessageBox::information(this, tr("Entity Gallery"),
                                     tr("Load a game data (.fgd) file first: the gallery is built "
                                        "from the entity classes it defines."));
            return;
        }
        const std::size_t created = document->createEntityGallery();
        if (created == 0) {
            QMessageBox::information(this, tr("Entity Gallery"),
                                     tr("The loaded game data defines no point entities."));
            return;
        }
        setPrompt(tr("Entity gallery: created %1 entities").arg(static_cast<qulonglong>(created)));
        updateEditActions();
    });
    QAction* checkProblems = addMenuCommand(map, QStringLiteral("map.check"),
                                            tr("&Check for Problems"),
                                            QKeySequence(QStringLiteral("Alt+P")));
    checkProblems->setProperty("implemented", true);
    connect(checkProblems, &QAction::triggered, this, &MainWindow::showCheckForProblemsDialog);
    map->addSeparator();
    QAction* loadPointfile = addMenuCommand(map, QStringLiteral("map.loadPointfile"),
                                           tr("&Load Pointfile"));
    QAction* unloadPointfile = addMenuCommand(map, QStringLiteral("map.unloadPointfile"),
                                             tr("&Unload Pointfile"));
    loadPointfile->setProperty("implemented", true);
    unloadPointfile->setProperty("implemented", true);
    loadPointfile->setStatusTip(tr("Trace the leak vbsp reported, from an entity to the hole"));
    connect(loadPointfile, &QAction::triggered, this, &MainWindow::loadPointFile);
    connect(unloadPointfile, &QAction::triggered, this, [this] {
        MapDocumentWidget* document = activeDocument();
        if (!document) return;
        if (!document->hasPointFile()) {
            setPrompt(tr("No pointfile is loaded"));
            return;
        }
        document->unloadPointFile();
        setPrompt(tr("Unloaded pointfile"));
        updateEditActions();
    });
    QAction* loadPortal = addMenuCommand(map, QStringLiteral("map.loadPortal"),
                                         tr("&Load Portal File"));
    QAction* unloadPortal = addMenuCommand(map, QStringLiteral("map.unloadPortal"),
                                           tr("&Unload Portal File"));
    loadPortal->setProperty("implemented", true);
    unloadPortal->setProperty("implemented", true);
    loadPortal->setStatusTip(tr("View the visleaf portals vbsp wrote for vvis"));
    connect(loadPortal, &QAction::triggered, this, &MainWindow::loadPortalFile);
    connect(unloadPortal, &QAction::triggered, this, [this] {
        MapDocumentWidget* document = activeDocument();
        if (!document) return;
        if (!document->hasPortalFile()) {
            setPrompt(tr("No portal file is loaded"));
            return;
        }
        document->unloadPortalFile();
        setPrompt(tr("Unloaded portal file"));
        updateEditActions();
    });
    map->addSeparator();
    QAction* mapInformation = addMenuCommand(map, QStringLiteral("map.information"), tr("Show &Information"));
    QAction* mapProperties = addMenuCommand(map, QStringLiteral("map.properties"), tr("&Map Properties..."));
    showGrid->setProperty("implemented", true);
    mapInformation->setProperty("implemented", true);
    mapProperties->setProperty("implemented", true);
    connect(showGrid, &QAction::toggled, this, [this](bool enabled) {
        if (MapDocumentWidget* document = activeDocument()) {
            document->setGridVisible(enabled);
        }
    });
    connect(mapInformation, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) {
            QMessageBox::information(this, tr("Map Information"),
                                     tr("File: %1\n%2")
                                         .arg(document->filePath().isEmpty() ? tr("Untitled") : document->filePath(),
                                              document->mapSummary()));
        }
    });
    connect(mapProperties, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) document->showMapProperties(this);
    });

    QMenu* view = addScrollMenu(tr("&View"));
    QMenu* screenElements = view->addMenu(tr("Screen &Elements"));
    screenElements->setObjectName(QStringLiteral("screenElementsMenu"));

    QMenu* appearance = view->addMenu(tr("&Appearance"));
    auto* appearanceGroup = new QActionGroup(this);
    appearanceGroup->setExclusive(true);
    const HammerTheme::Mode currentTheme = HammerTheme::loadMode();
    const struct AppearanceChoice {
        const char* id;
        const char* text;
        HammerTheme::Mode mode;
    } appearanceChoices[] = {
        {"view.appearanceSystem", "Follow &System", HammerTheme::Mode::System},
        {"view.appearanceLight", "&Light", HammerTheme::Mode::Light},
        {"view.appearanceDark", "&Dark", HammerTheme::Mode::Dark}
    };
    for (const AppearanceChoice& choice : appearanceChoices) {
        QAction* action = addMenuCommand(appearance, QString::fromLatin1(choice.id),
                                         tr(choice.text), {}, true, choice.mode == currentTheme);
        action->setProperty("implemented", true);
        action->setData(HammerTheme::settingsValue(choice.mode));
        appearanceGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, choice] {
            HammerTheme::saveMode(choice.mode);
            HammerTheme::apply(*qApp, choice.mode);
            setPrompt(tr("Appearance: %1").arg(HammerTheme::displayName(choice.mode)));
        });
    }
    QAction* autosize = addMenuCommand(view, QStringLiteral("view.autosize"), tr("&Autosize 4 Views"), QKeySequence(QStringLiteral("Ctrl+A")));
    QAction* maximizeView = addMenuCommand(view, QStringLiteral("view.maximize"), tr("&Maximize/Restore Active View"), QKeySequence(QStringLiteral("Shift+Z")));

    QMenu* projection = view->addMenu(tr("3D &Projection"));
    auto* projectionGroup = new QActionGroup(this);
    projectionGroup->setExclusive(true);
    perspectiveProjectionAction_ = addMenuCommand(
        projection, QStringLiteral("view.projectionPerspective"),
        tr("&Perspective Projection"), {}, true, true);
    orthographicProjectionAction_ = addMenuCommand(
        projection, QStringLiteral("view.projectionOrthographic"),
        tr("&Orthographic Projection"), {}, true, false);
    projectionGroup->addAction(perspectiveProjectionAction_);
    projectionGroup->addAction(orthographicProjectionAction_);
    perspectiveProjectionAction_->setProperty("implemented", true);
    orthographicProjectionAction_->setProperty("implemented", true);
    connect(perspectiveProjectionAction_, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) {
            document->setCameraProjection(MapViewWidget::ProjectionMode::Perspective);
            setPrompt(tr("3D view: Perspective Projection"));
        }
        updateProjectionActions();
    });
    connect(orthographicProjectionAction_, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) {
            document->setCameraProjection(MapViewWidget::ProjectionMode::Orthographic);
            setPrompt(tr("3D view: Orthographic Projection"));
        }
        updateProjectionActions();
    });

    // ID_VIEW_3DLIGHTMAP_GRID ("3D &Lightmap Grid" on hammer.rc's View menu).
    lightmapGridAction_ = addMenuCommand(view, QStringLiteral("view.lightmapGrid"),
                                         tr("3D &Lightmap Grid"), {}, true, false);
    lightmapGridAction_->setProperty("implemented", true);
    connect(lightmapGridAction_, &QAction::triggered, this,
            [this](bool checked) { setLightmapGridVisible(checked); });

    toolTexturesMenu_ = view->addMenu(tr("Tool &Textures"));
    toolTexturesMenu_->setObjectName(QStringLiteral("toolTexturesMenu"));
    connect(toolTexturesMenu_, &QMenu::aboutToShow,
            this, &MainWindow::rebuildToolTexturesMenu);

    view->addSeparator();
    // CMapDoc's F2/F3/F4: the highlighted pane becomes that 2D view. Any pane
    // can hold any type, so all four can be 2D or several can be cameras.
    const auto addViewKindCommand = [&](const QString& id, const QString& text,
                                        const QKeySequence& shortcut, MapViewWidget::Kind kind) {
        QAction* action = addMenuCommand(view, id, text, shortcut);
        action->setProperty("implemented", true);
        action->setStatusTip(tr("Change the highlighted view to this type"));
        connect(action, &QAction::triggered, this, [this, kind] { setActiveViewKind(kind); });
        return action;
    };
    addViewKindCommand(QStringLiteral("view.xy"), tr("2D &X/Y"), QKeySequence(Qt::Key_F2),
                       MapViewWidget::Kind::Top);
    addViewKindCommand(QStringLiteral("view.yz"), tr("2D &Y/Z"), QKeySequence(Qt::Key_F4),
                       MapViewWidget::Kind::Front);
    addViewKindCommand(QStringLiteral("view.xz"), tr("2D X/&Z"), QKeySequence(Qt::Key_F3),
                       MapViewWidget::Kind::Side);
    view->addSeparator();
    addMenuCommand(view, QStringLiteral("view.wireframe"), tr("3D &Wireframe"));
    addMenuCommand(view, QStringLiteral("view.filled"), tr("3D &Filled Polygons"));
    auto* texturedModeGroup = new QActionGroup(this);
    texturedModeGroup->setExclusive(true);
    texturedViewAction_ = addMenuCommand(view, QStringLiteral("view.textured"),
                                         tr("3D &Textured Polygons"),
                                         QKeySequence(Qt::Key_F5), true, true);
    shadedTexturedViewAction_ = addMenuCommand(view, QStringLiteral("view.shaded"),
                                               tr("3D Textured &Shaded Polygons"),
                                               QKeySequence(QStringLiteral("Shift+F5")), true, false);
    shadedMaterialPolygonsViewAction_ = addMenuCommand(
        view, QStringLiteral("view.shadedMaterialPolygons"),
        tr("3D Textured Shaded + &Materials Polygons"), {}, true, false);
    rayTracedPreviewAction_ = addMenuCommand(
        view, QStringLiteral("view.rayTracedPreview"),
        tr("3D &Ray-Traced Preview"), QKeySequence(QStringLiteral("Ctrl+Shift+F5")),
        true, false);
    texturedModeGroup->addAction(texturedViewAction_);
    texturedModeGroup->addAction(shadedTexturedViewAction_);
    texturedModeGroup->addAction(shadedMaterialPolygonsViewAction_);
    texturedModeGroup->addAction(rayTracedPreviewAction_);
    texturedViewAction_->setProperty("implemented", true);
    shadedTexturedViewAction_->setProperty("implemented", true);
    shadedMaterialPolygonsViewAction_->setProperty("implemented", true);
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    rayTracedPreviewAction_->setProperty("implemented", true);
#else
    rayTracedPreviewAction_->setEnabled(false);
    rayTracedPreviewAction_->setToolTip(
        tr("Requires the Vulkan SDK, glslangValidator, and a GPU/driver with "
           "VK_KHR_acceleration_structure plus VK_KHR_ray_query"));
#endif
    connect(texturedViewAction_, &QAction::triggered, this, [this] {
        setActiveViewKind(MapViewWidget::Kind::Perspective);
        setMaterialRenderingEnabled(true);
        setTexturedRenderMode(MapViewWidget::TexturedRenderMode::Unlit);
    });
    connect(shadedTexturedViewAction_, &QAction::triggered, this, [this] {
        setActiveViewKind(MapViewWidget::Kind::Perspective);
        setMaterialRenderingEnabled(true);
        setTexturedRenderMode(MapViewWidget::TexturedRenderMode::Shaded);
    });
    connect(shadedMaterialPolygonsViewAction_, &QAction::triggered, this, [this] {
        setActiveViewKind(MapViewWidget::Kind::Perspective);
        setMaterialRenderingEnabled(true);
        setTexturedRenderMode(MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons);
    });
    connect(rayTracedPreviewAction_, &QAction::triggered, this, [this] {
        setActiveViewKind(MapViewWidget::Kind::Perspective);
        setMaterialRenderingEnabled(true);
        setTexturedRenderMode(MapViewWidget::TexturedRenderMode::RayTracedPreview);
    });

    wireframeOverlayAction_ = addMenuCommand(view, QStringLiteral("view.wireframeOverlay"),
                                             tr("Overlay 3D &Wireframe"), {}, true, false);
    wireframeOverlayAction_->setProperty("implemented", true);
    connect(wireframeOverlayAction_, &QAction::toggled,
            this, &MainWindow::setWireframeOverlayEnabled);
    // CMapDoc::OnToggleDispSolidMask / OnUpdateToggleSolidMask. Checked by
    // default, matching CMapDoc's m_bDispSolidDrawMask = true.
    displacementSolidMaskAction_ = addMenuCommand(
        view, QStringLiteral("view.displacementSolidMask"),
        tr("&Mask Displacement Solid Faces"), {}, true, true);
    displacementSolidMaskAction_->setProperty("implemented", true);
    connect(displacementSolidMaskAction_, &QAction::toggled,
            this, &MainWindow::setDisplacementSolidMaskEnabled);
    view->addSeparator();
    addMenuCommand(view, QStringLiteral("view.centerSelection"), tr("&Center Views on Selection"), QKeySequence(QStringLiteral("Ctrl+E")));
    addMenuCommand(view, QStringLiteral("view.center3d"), tr("Center &3D Views on Selection"));
    addMenuCommand(view, QStringLiteral("view.gotoCoordinates"), tr("Go to Coordinates..."));
    addMenuCommand(view, QStringLiteral("view.gotoBrush"), tr("G&o to Brush Number..."), QKeySequence(QStringLiteral("Ctrl+Shift+G")));
    view->addSeparator();
    addMenuCommand(view, QStringLiteral("view.connections"), tr("Show C&onnections"), {}, true, true);
    addMenuCommand(view, QStringLiteral("view.helpers"), tr("Show Helpers"), {}, true, true);
    addMenuCommand(view, QStringLiteral("view.models2d"), tr("Show Models in 2D"), {}, true, true);
    addMenuCommand(view, QStringLiteral("view.hideItems"), tr("&Hide Items"), {}, true, false);
    addMenuCommand(view, QStringLiteral("view.hideNames"), tr("Hide Entity &Names"), {}, true, false);
    view->addSeparator();
    QAction* moveVisgroup =
        addMenuCommand(view, QStringLiteral("view.moveVisgroup"), tr("Move Selection To V&isgroup"));
    moveVisgroup->setProperty("implemented", true);
    connect(moveVisgroup, &QAction::triggered, this, &MainWindow::showNewVisGroupDialog);
    // VisGroups > QuickHide (VDC "Hammer VisGroups"). Session-only visibility:
    // the hidden objects leave every view until Unhide, and nothing about the
    // hide is written to the VMF.
    QAction* quickHide = addMenuCommand(view, QStringLiteral("view.quickHide"),
                                        tr("&QuickHide Objects"),
                                        QKeySequence(QStringLiteral("H")));
    QAction* quickHideUnselected =
        addMenuCommand(view, QStringLiteral("view.quickHideUnselected"),
                       tr("QuickHide &Unselected Objects"),
                       QKeySequence(QStringLiteral("Ctrl+H")));
    QAction* quickHideUnhide = addMenuCommand(view, QStringLiteral("view.quickHideUnhide"),
                                              tr("Un&hide QuickHide Objects"),
                                              QKeySequence(QStringLiteral("U")));
    QAction* quickHideToVisGroup =
        addMenuCommand(view, QStringLiteral("view.quickHideToVisGroup"),
                       tr("Convert QuickHide objects to VisGroup..."));
    for (QAction* action : {quickHide, quickHideUnselected, quickHideUnhide, quickHideToVisGroup})
        action->setProperty("implemented", true);
    connect(quickHide, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) document->quickHideSelected();
    });
    connect(quickHideUnselected, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) document->quickHideUnselected();
    });
    connect(quickHideUnhide, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) document->quickHideUnhideAll();
    });
    connect(quickHideToVisGroup, &QAction::triggered, this,
            &MainWindow::showQuickHideToVisGroupDialog);
    view->addSeparator();
    addMenuCommand(view, QStringLiteral("view.opaque"), tr("&Draw Materials Opaque"), {}, true, false);
    hdrAction_ = addMenuCommand(view, QStringLiteral("view.hdr"), tr("HDR"), {}, true,
                                hdrEnabled_);
    hdrAction_->setProperty("implemented", true);
    hdrAction_->setStatusTip(tr("Auto-exposure, bloom and HDR light values in the "
                                "ray-traced 3D view"));
    connect(hdrAction_, &QAction::toggled, this, &MainWindow::setHdrEnabled);
    rayTracedGammaAction_ = addMenuCommand(view, QStringLiteral("view.rayTracedGamma"),
                                           tr("Ray-traced &Gamma..."));
    rayTracedGammaAction_->setProperty("implemented", true);
    rayTracedGammaAction_->setStatusTip(tr("Display gamma applied to the ray-traced 3D view"));
    connect(rayTracedGammaAction_, &QAction::triggered,
            this, &MainWindow::showRayTracedGammaDialog);
    autosize->setProperty("implemented", true);
    maximizeView->setProperty("implemented", true);
    connect(autosize, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) {
            document->autosizeViews();
        }
    });
    connect(maximizeView, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) {
            document->maximizeActiveView();
        }
    });

    QMenu* tools = addScrollMenu(tr("&Tools"));
    QAction* carve = addMenuCommand(tools, QStringLiteral("tools.carve"), tr("&Carve"), QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(carve, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) document->carveSelection();
    });
    carve->setProperty("implemented", true);
    // Ctrl+H belongs to QuickHide Unselected Objects (VDC); Make Hollow is an
    // unimplemented placeholder here, so it gives the shortcut up rather than
    // shadow a working command.
    addMenuCommand(tools, QStringLiteral("tools.hollow"), tr("Make Hollow"));
    tools->addSeparator();
    // CMapDoc::OnToolsGroup / OnToolsUngroup.
    QAction* group = addMenuCommand(tools, QStringLiteral("tools.group"), tr("&Group"),
                                    QKeySequence(QStringLiteral("Ctrl+G")));
    QAction* ungroup = addMenuCommand(tools, QStringLiteral("tools.ungroup"), tr("&Ungroup"),
                                      QKeySequence(QStringLiteral("Ctrl+U")));
    group->setProperty("implemented", true);
    ungroup->setProperty("implemented", true);
    connect(group, &QAction::triggered, this, [this] {
        MapDocumentWidget* document = activeDocument();
        if (!document) return;
        if (document->selectionCount() < 2) {
            QMessageBox::information(this, tr("Group"),
                                     tr("Select at least two objects to group."));
            return;
        }
        if (!document->groupsActive()) {
            QMessageBox::information(
                this, tr("Group"),
                tr("Grouping is off while Ignore Groups is on or the selection mode is not "
                   "Groups."));
            return;
        }
        // OnToolsGroup's warning: regrouping pulls objects out of whatever
        // group they are in now.
        if (document->selectionCrossesExistingGroups() &&
            QMessageBox::warning(
                this, tr("Group"),
                tr("Some selected objects already belong to a group. Grouping them now will "
                   "remove them from it. Continue?"),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        document->groupSelection();
    });
    connect(ungroup, &QAction::triggered, this, [this] {
        MapDocumentWidget* document = activeDocument();
        if (!document) return;
        if (!document->ungroupSelection()) {
            statusBar()->showMessage(tr("No whole group is selected to ungroup"), 4000);
        }
    });
    tools->addSeparator();
    QAction* toEntity = addMenuCommand(tools, QStringLiteral("tools.toEntity"), tr("&Tie to Entity"), QKeySequence(QStringLiteral("Ctrl+T")));
    connect(toEntity, &QAction::triggered, this, [this] {
        if (MapDocumentWidget* document = activeDocument()) document->tieSelectionToEntity();
    });
    addMenuCommand(tools, QStringLiteral("tools.toWorld"), tr("&Move to World"), QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    tools->addSeparator();
    addMenuCommand(tools, QStringLiteral("tool.textureApplication"), tr("Te&xture Application"), QKeySequence(QStringLiteral("Shift+A")));
    QAction* replaceTextures = addMenuCommand(tools, QStringLiteral("tools.replaceTextures"), tr("R&eplace Textures..."));
    connect(replaceTextures, &QAction::triggered, this, &MainWindow::showReplaceTexturesDialog);
    addMenuCommand(tools, QStringLiteral("tools.textureLock"), tr("Texture &Lock"), QKeySequence(QStringLiteral("Shift+L")), true, true);
    tools->addSeparator();
    addMenuCommand(tools, QStringLiteral("tools.soundBrowser"), tr("&Sound Browser..."), QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    tools->addSeparator();
    QAction* transform = addMenuCommand(tools, QStringLiteral("tools.transform"), tr("Transform Handles: Resize"), QKeySequence(QStringLiteral("Ctrl+M")));
    transform->setProperty("implemented", true);
    transformHandlesAction_ = transform;
    connect(transform, &QAction::triggered, this, [this] {
        switch (transformMode_) {
        case MapViewWidget::TransformMode::Scale: transformMode_ = MapViewWidget::TransformMode::Translate; break;
        case MapViewWidget::TransformMode::Translate: transformMode_ = MapViewWidget::TransformMode::Rotate; break;
        case MapViewWidget::TransformMode::Rotate: transformMode_ = MapViewWidget::TransformMode::Scale; break;
        }
        updateTransformHandlesActionText();
        if (MapDocumentWidget* document = activeDocument()) document->setTransformMode(transformMode_);
    });
    addMenuCommand(tools, QStringLiteral("tools.snapSelected"), tr("Snap Selected to Grid"), QKeySequence(QStringLiteral("Ctrl+B")));
    addMenuCommand(tools, QStringLiteral("tools.snapIndividual"), tr("Snap Selected to Grid Individually"), QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    addMenuCommand(tools, QStringLiteral("tools.centerOrigins"), tr("Center &Origins"));
    QMenu* align = tools->addMenu(tr("&Align objects"));
    addMenuCommand(align, QStringLiteral("tools.alignLeft"), tr("To &Left"));
    addMenuCommand(align, QStringLiteral("tools.alignRight"), tr("To &Right"));
    addMenuCommand(align, QStringLiteral("tools.alignTop"), tr("To &Top"));
    addMenuCommand(align, QStringLiteral("tools.alignBottom"), tr("To &Bottom"));
    QMenu* flip = tools->addMenu(tr("&Flip Objects"));
    addMenuCommand(flip, QStringLiteral("tools.flipHorizontal"), tr("&Horizontally"), QKeySequence(QStringLiteral("Ctrl+L")));
    addMenuCommand(flip, QStringLiteral("tools.flipVertical"), tr("&Vertically"), QKeySequence(QStringLiteral("Ctrl+I")));
    addMenuCommand(tools, QStringLiteral("tools.createPrefab"), tr("C&reate Prefab"), QKeySequence(QStringLiteral("Ctrl+R")));
    tools->addSeparator();
    QAction* buildCubemaps = addMenuCommand(tools, QStringLiteral("tools.buildCubemaps"),
                                            tr("&Build Cubemaps"));
    buildCubemaps->setProperty("implemented", true);
    connect(buildCubemaps, &QAction::triggered, this, &MainWindow::buildCubemaps);
    QAction* clearShaderCache = addMenuCommand(tools, QStringLiteral("tools.clearShaderCache"),
                                               tr("Clear Shader &Cache"));
    clearShaderCache->setProperty("implemented", true);
    connect(clearShaderCache, &QAction::triggered, this, &MainWindow::clearShaderCache);
    tools->addSeparator();
    QAction* configureGame = addMenuCommand(tools, QStringLiteral("tools.configureGame"), tr("Configure &Game Directory..."));
    configureGame->setProperty("implemented", true);
    connect(configureGame, &QAction::triggered, this, &MainWindow::configureGameDirectory);
    QAction* loadGameData = addMenuCommand(tools, QStringLiteral("tools.loadFgd"), tr("Load &Game Data (.fgd)..."));
    loadGameData->setProperty("implemented", true);
    connect(loadGameData, &QAction::triggered, this, &MainWindow::loadFgd);
    QAction* options = addMenuCommand(tools, QStringLiteral("tools.options"), tr("&Options..."));
    options->setProperty("implemented", true);
    connect(options, &QAction::triggered, this, &MainWindow::showOptions);

    QMenu* window = addScrollMenu(tr("&Window"));
    addMenuCommand(window, QStringLiteral("window.new"), tr("&New Window"));
    QAction* cascade = addMenuCommand(window, QStringLiteral("window.cascade"), tr("&Cascade"));
    QAction* tile = addMenuCommand(window, QStringLiteral("window.tile"), tr("&Tile"));
    addMenuCommand(window, QStringLiteral("window.arrange"), tr("&Arrange Icons"));
    window->addSeparator();
    QAction* messages = addMenuCommand(window, QStringLiteral("view.messages"), tr("&Messages"), QKeySequence(QStringLiteral("Alt+F3")), true, true);
    cascade->setProperty("implemented", true);
    tile->setProperty("implemented", true);
    messages->setProperty("implemented", true);
    connect(cascade, &QAction::triggered, mdiArea_, &QMdiArea::cascadeSubWindows);
    connect(tile, &QAction::triggered, mdiArea_, &QMdiArea::tileSubWindows);

    QMenu* collaborate = addScrollMenu(tr("Colla&borate"));
    collabHostAction_ = addMenuCommand(collaborate, QStringLiteral("collab.host"),
                                       tr("&Host Session..."));
    collabHostAction_->setProperty("implemented", true);
    connect(collabHostAction_, &QAction::triggered, this, &MainWindow::hostCollabSession);
    collabJoinAction_ = addMenuCommand(collaborate, QStringLiteral("collab.join"),
                                       tr("&Join Session..."));
    collabJoinAction_->setProperty("implemented", true);
    connect(collabJoinAction_, &QAction::triggered, this, &MainWindow::joinCollabSession);
    collabKickAction_ = addMenuCommand(collaborate, QStringLiteral("collab.kick"),
                                       tr("&Kick Collaborator..."));
    collabKickAction_->setProperty("implemented", true);
    connect(collabKickAction_, &QAction::triggered, this, &MainWindow::kickCollaborator);
    collabChatAction_ = addMenuCommand(collaborate, QStringLiteral("collab.chat"),
                                       tr("Session &Chat"), QKeySequence(), true, false);
    collabChatAction_->setProperty("implemented", true);
    connect(collabChatAction_, &QAction::toggled, this, [this](bool visible) {
        if (visible) ensureCollabChatDock();
        if (collabChatDock_) collabChatDock_->setVisible(visible);
    });
    collaborate->addSeparator();
    collabLeaveAction_ = addMenuCommand(collaborate, QStringLiteral("collab.leave"),
                                        tr("&Leave Session"));
    collabLeaveAction_->setProperty("implemented", true);
    connect(collabLeaveAction_, &QAction::triggered, this, [this] { leaveCollabSession(); });
    updateCollabActions();

    QMenu* help = addScrollMenu(tr("&Help"));
    addMenuCommand(help, QStringLiteral("help.topics"), tr("&Help Topics"));
    help->addSeparator();
    QAction* freeman = addMenuCommand(help, QStringLiteral("app.deployFreeman"),
                                      tr("Deploy the Freeman"));
    freeman->setProperty("implemented", true);
    connect(freeman, &QAction::triggered, this, [this] {
        MapDocumentWidget* document = activeDocument();
        if (!document) {
            setPrompt(tr("Open a map before deploying the Freeman"));
            return;
        }
        const auto spawn = document->selectedPointEntityOrigin();
        if (!spawn) {
            QMessageBox::information(this, tr("Deploy the Freeman"),
                                     tr("Select a point entity first — the Freeman spawns at its location."));
            return;
        }
        auto* window = new FreemanWindow(document->scene(), materials_, *spawn,
                                         FreemanWindow::Mode::Freeman,
                                         &document->vmfDocument(), this);
        window->setAttribute(Qt::WA_DeleteOnClose);
        // The editor's 3D view stops rendering while the Freeman owns the GPU.
        document->setPerspectiveRenderingPaused(true);
        QPointer<MapDocumentWidget> paused(document);
        connect(window, &QObject::destroyed, this, [paused] {
            if (paused) paused->setPerspectiveRenderingPaused(false);
        });
        window->show();
    });
    QAction* soldier = addMenuCommand(help, QStringLiteral("app.deploySoldier"),
                                      tr("Deploy the Soldier (TF2)"));
    soldier->setProperty("implemented", true);
    connect(soldier, &QAction::triggered, this, [this] {
        MapDocumentWidget* document = activeDocument();
        if (!document) {
            setPrompt(tr("Open a map before deploying the Soldier"));
            return;
        }
        // The mode needs the TF2 content actually mounted: launcher + rocket
        // models, launcher sounds, and the soldier's pain lines.
        const auto missing = FreemanWindow::soldierAssetsMissing(materials_.get());
        if (!missing.empty()) {
            QStringList lines;
            for (const auto& path : missing) lines << QString::fromStdString(path);
            QMessageBox::information(
                this, tr("Deploy the Soldier"),
                tr("TF2 assets are not mounted — missing:\n%1").arg(lines.join(QLatin1Char('\n'))));
            return;
        }
        const auto spawn = document->selectedPointEntityOrigin();
        if (!spawn) {
            QMessageBox::information(this, tr("Deploy the Soldier"),
                                     tr("Select a point entity first — the Soldier spawns at its location."));
            return;
        }
        auto* window = new FreemanWindow(document->scene(), materials_, *spawn,
                                         FreemanWindow::Mode::Soldier,
                                         &document->vmfDocument(), this);
        window->setAttribute(Qt::WA_DeleteOnClose);
        document->setPerspectiveRenderingPaused(true);
        QPointer<MapDocumentWidget> paused(document);
        connect(window, &QObject::destroyed, this, [paused] {
            if (paused) paused->setPerspectiveRenderingPaused(false);
        });
        window->show();
    });
    help->addSeparator();
    QAction* about = addMenuCommand(help, QStringLiteral("app.about"), tr("&About..."));
    about->setProperty("implemented", true);
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::createMdiSystemButton()
{
    // Qt's automatic maximized-MDI system-menu icon is style-dependent and
    // ends up smaller and vertically offset on several Linux themes. Use a
    // real zero-margin tool button whose square follows the menu-bar height.
    mdiSystemButton_ = new QToolButton(menuBar());
    mdiSystemButton_->setObjectName(QStringLiteral("HammerMdiSystemButton"));
    mdiSystemButton_->setAutoRaise(true);
    mdiSystemButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    mdiSystemButton_->setFocusPolicy(Qt::NoFocus);
    mdiSystemButton_->setContentsMargins(0, 0, 0, 0);
    mdiSystemButton_->setIcon(QApplication::windowIcon());
    mdiSystemButton_->setToolTip(tr("Document window menu"));
    mdiSystemButton_->setStyleSheet(QStringLiteral(
        "QToolButton#HammerMdiSystemButton { margin: 0; padding: 0; border: 0; border-radius: 0; }"
        "QToolButton#HammerMdiSystemButton:hover { background: palette(midlight); }"
        "QToolButton#HammerMdiSystemButton:pressed { background: palette(mid); }"));

    mdiSystemMenu_ = new QMenu(mdiSystemButton_);
    connect(mdiSystemMenu_, &QMenu::aboutToShow, this, [this] {
        mdiSystemMenu_->clear();
        QMdiSubWindow* subWindow = mdiArea_ ? mdiArea_->activeSubWindow() : nullptr;

        QAction* restore = mdiSystemMenu_->addAction(tr("&Restore"));
        QAction* minimize = mdiSystemMenu_->addAction(tr("Mi&nimize"));
        QAction* maximize = mdiSystemMenu_->addAction(tr("Ma&ximize"));
        mdiSystemMenu_->addSeparator();
        QAction* close = mdiSystemMenu_->addAction(tr("&Close"));

        const bool available = subWindow != nullptr;
        restore->setEnabled(available && (subWindow->isMaximized() || subWindow->isMinimized()));
        minimize->setEnabled(available && !subWindow->isMinimized());
        maximize->setEnabled(available && !subWindow->isMaximized());
        close->setEnabled(available);
        if (!available) return;

        connect(restore, &QAction::triggered, subWindow, &QMdiSubWindow::showNormal);
        connect(minimize, &QAction::triggered, subWindow, &QMdiSubWindow::showMinimized);
        connect(maximize, &QAction::triggered, subWindow, &QMdiSubWindow::showMaximized);
        connect(close, &QAction::triggered, subWindow, &QMdiSubWindow::close);
    });
    mdiSystemButton_->setMenu(mdiSystemMenu_);
    mdiSystemButton_->setPopupMode(QToolButton::InstantPopup);
    menuBar()->setCornerWidget(mdiSystemButton_, Qt::TopLeftCorner);
    updateMdiSystemButtonGeometry();
    updateMdiSystemButtonState();
}

void MainWindow::updateMdiSystemButtonGeometry()
{
    if (!mdiSystemButton_ || !menuBar()) return;

    // Use the actual laid-out menu-bar height so the icon button is exactly
    // the same square as the MDI controls at the opposite side. Filling the
    // complete menu-bar height also prevents the top-edge bias produced by
    // QMenuBar's default corner-widget placement.
    const int menuHeight = std::max(menuBar()->height(), menuBar()->sizeHint().height());
    const int buttonExtent = std::clamp(menuHeight, 22, 30);
    const int iconExtent = std::max(18, buttonExtent - 4);
    mdiSystemButton_->setFixedSize(buttonExtent, buttonExtent);
    mdiSystemButton_->setIconSize(QSize(iconExtent, iconExtent));
    mdiSystemButton_->updateGeometry();
}

void MainWindow::updateMdiSystemButtonState()
{
    if (!mdiSystemButton_) return;
    QMdiSubWindow* subWindow = mdiArea_ ? mdiArea_->activeSubWindow() : nullptr;
    mdiSystemButton_->setVisible(subWindow != nullptr);
    mdiSystemButton_->setEnabled(subWindow != nullptr);
}

void MainWindow::createToolbars()
{
    const QSize topIconSize(20, 20);

    auto* mapView = addToolBar(tr("Mapview Bar"));
    mapView->setObjectName(QStringLiteral("MapviewBar"));
    mapView->setIconSize(topIconSize);
    toolBars_.insert(QStringLiteral("mapview"), mapView);
    addDrawnCommand(mapView, QStringLiteral("view.grid"), tr("Toggle 2D grid"), QStringLiteral("grid2d"), true);
    addDrawnCommand(mapView, QStringLiteral("view.3dGrid"), tr("Toggle 3D grid"), QStringLiteral("grid3d"), true);
    addDrawnCommand(mapView, QStringLiteral("map.gridLower"), tr("Smaller grid"), QStringLiteral("gridSmaller"));
    addDrawnCommand(mapView, QStringLiteral("map.gridHigher"), tr("Larger grid"), QStringLiteral("gridLarger"));
    mapView->addSeparator();
    addDrawnCommand(mapView, QStringLiteral("view.loadWindowState"), tr("Load window state"), QStringLiteral("loadWindowState"));
    addDrawnCommand(mapView, QStringLiteral("view.saveWindowState"), tr("Save window state"), QStringLiteral("saveWindowState"));

    auto* undoRedo = addToolBar(tr("Undo/Redo Bar"));
    undoRedo->setObjectName(QStringLiteral("UndoRedoBar"));
    undoRedo->setIconSize(topIconSize);
    toolBars_.insert(QStringLiteral("undo"), undoRedo);
    addDrawnCommand(undoRedo, QStringLiteral("edit.undo"), tr("Undo"), QStringLiteral("undo"));
    addDrawnCommand(undoRedo, QStringLiteral("edit.redo"), tr("Redo"), QStringLiteral("redo"));

    auto* operations = addToolBar(tr("Mapoperations Bar"));
    operations->setObjectName(QStringLiteral("MapOperationsBar"));
    operations->setIconSize(topIconSize);
    toolBars_.insert(QStringLiteral("operations"), operations);
    // Buttons and grouping follow the VDC "Hammer Map Operations Toolbar" page.
    const struct Operation { const char* id; const char* text; const char* icon; bool checkable; bool separatorBefore; } operationSpecs[] = {
        {"tools.carve", "Carve", "carve", false, false},
        {"tools.group", "Group", "group", false, true},
        {"tools.ungroup", "Ungroup", "ungroup", false, false},
        {"tools.ignoreGroups", "Toggle group ignore", "ignoreGroups", true, false},
        {"view.hideSelected", "Hide selected", "hideSelected", false, true},
        {"view.hideUnselected", "Hide unselected", "hideUnselected", false, false},
        {"view.quickHide", "QuickHide objects", "quickHide", false, false},
        {"view.quickHideUnselected", "QuickHide unselected objects", "quickHideUnselected", false, false},
        {"view.quickHideUnhide", "Unhide QuickHide objects", "unhideQuickHide", false, false},
        {"view.showAllVisGroups", "Show all VisGroups", "showAllVisGroups", false, false},
        {"edit.cut", "Cut", "cut", false, true},
        {"edit.copy", "Copy", "copy", false, false},
        {"edit.paste", "Paste", "paste", false, false},
        {"view.cordon", "Toggle cordon state", "cordonToggle", true, true},
        {"tools.editCordon", "Edit cordon bounds", "cordonEdit", false, false},
        {"view.radiusCulling", "Radius culling", "radiusCulling", true, false},
        {"tools.selectByHandles", "Toggle select-by-handles", "selectByHandles", true, true},
        {"tools.infiniteSelection", "Toggle auto-selection", "autoSelect", true, false},
        {"tools.textureAlign", "Align to world/face", "textureAlign", true, true},
        {"disp.solid", "Displacement mask", "dispMask", true, false},
        {"disp.3d", "Enable displacements", "disp3d", true, false},
        {"disp.walkable", "Display walkable", "dispWalkable", true, false},
        {"disp.removed", "Displacement edge collapse", "dispEdgeCollapse", true, false},
        {"tools.textureLock", "Texture lock", "textureLock", true, true},
        {"tools.textureScaleLock", "Texture scale lock", "textureScaleLock", true, false},
        {"file.runMap", "Run map", "runMap", false, true},
        {"view.helpers", "Show helpers", "showHelpers", true, true},
        {"view.models2d", "Toggle models in 2D", "models2d", true, false},
        {"view.modelFade", "Toggle model fade preview", "modelFade", true, false},
        {"view.collision", "Show collision models wireframe", "collisionWire", true, false},
        {"view.detailObjects", "Show detail objects", "detailObjects", true, false},
        {"view.nodraw", "Show nodraw faces", "nodraw", true, false}
    };
    for (const Operation& spec : operationSpecs) {
        if (spec.separatorBefore) {
            operations->addSeparator();
        }
        QAction* action = addDrawnCommand(operations, QString::fromLatin1(spec.id), tr(spec.text),
                                          QString::fromLatin1(spec.icon), spec.checkable);
        // The detail-object button drives the real detail-prop rendering; the
        // rest of this toolbar's draw modes are still placeholders.
        if (action && std::string_view(spec.id) == "view.detailObjects") {
            detailObjectsAction_ = action;
            action->setProperty("implemented", true);
            action->setChecked(detailPropsVisible_);
            connect(action, &QAction::triggered, this,
                    [this](bool checked) { setDetailPropsVisible(checked); });
        }
        // CMapDoc::OnShowNoDrawBrushes (ID_SHOW_NODRAW_BRUSHES, "Shows nodraw
        // faces"). The original hides the face in CMapFace::Render when the
        // option is off, which is per-face hiding of one material - exactly
        // what View > Tool Textures does here, so the button is a shortcut for
        // that menu's tools/toolsnodraw entry rather than a second mechanism.
        if (action && std::string_view(spec.id) == "view.nodraw") {
            nodrawAction_ = action;
            action->setProperty("implemented", true);
            action->setToolTip(tr("Show nodraw faces (View > Tool Textures: %1)")
                                   .arg(QStringLiteral("tools/toolsnodraw")));
            // Options.general.bShowNoDrawBrushes defaults TRUE.
            action->setChecked(true);
            connect(action, &QAction::toggled, this, [this](bool visible) {
                MapDocumentWidget* document = activeDocument();
                if (!document) return;
                document->setToolTextureVisible(QStringLiteral("tools/toolsnodraw"), visible);
                setPrompt(visible ? tr("Showing nodraw faces") : tr("Hiding nodraw faces"));
            });
        }
        // Options.general.bIgnoreGroups: picks stop expanding to whole groups,
        // so a single member of a group can be worked on.
        if (action && std::string_view(spec.id) == "tools.ignoreGroups") {
            action->setProperty("implemented", true);
            connect(action, &QAction::toggled, this, [this](bool ignore) {
                if (MapDocumentWidget* document = activeDocument())
                    document->setIgnoreGroups(ignore);
            });
        }
        // CVisGroup::ShowAllVisGroups: an override that shows everything
        // without clearing what each visgroup has hidden.
        if (action && std::string_view(spec.id) == "view.showAllVisGroups") {
            action->setProperty("implemented", true);
            action->setCheckable(true);
            connect(action, &QAction::toggled, this, [this](bool showAll) {
                if (MapDocumentWidget* document = activeDocument())
                    document->setShowAllVisGroups(showAll);
                scheduleVisGroupTreeRebuild();
            });
        }
        // The VDC's "Hide selected objects" is the Move Selection to VisGroup
        // flow, which is where its New VisGroup dialog comes from.
        if (action && (std::string_view(spec.id) == "view.hideSelected" ||
                       std::string_view(spec.id) == "view.hideUnselected")) {
            const bool unselected = std::string_view(spec.id) == "view.hideUnselected";
            action->setProperty("implemented", true);
            connect(action, &QAction::triggered, this, [this, unselected] {
                MapDocumentWidget* document = activeDocument();
                if (!document) return;
                // "Hide unselected" has no VisGroup dialog in the original; it
                // inverts the selection first so the same flow applies.
                if (unselected) document->invertSelection();
                showNewVisGroupDialog();
            });
        }
    }
    operations->addSeparator();
    addDrawnCommand(operations, QStringLiteral("map.snapGrid"), tr("Snap to grid"), QStringLiteral("snapGrid"), true);
    addDrawnCommand(operations, QStringLiteral("map.gridLower"), tr("Smaller grid"), QStringLiteral("gridSmaller"));
    addDrawnCommand(operations, QStringLiteral("map.gridHigher"), tr("Larger grid"), QStringLiteral("gridLarger"));

    // mainfrm.cpp docks these left-to-right as Map Operations, Undo/Redo, Map View.
    insertToolBar(mapView, operations);
    insertToolBar(mapView, undoRedo);

    auto* editTools = addToolBar(tr("Maptools Bar"));
    editTools->setObjectName(QStringLiteral("MapEditToolsBar"));
    editTools->setIconSize(QSize(36, 36));
    editTools->setToolButtonStyle(Qt::ToolButtonIconOnly);
    addToolBar(Qt::LeftToolBarArea, editTools);
    toolBars_.insert(QStringLiteral("edittools"), editTools);

    auto* toolGroup = new QActionGroup(this);
    toolGroup->setExclusive(true);
    // Exact user-supplied filename mapping. Keep these names literal so the
    // sidebar cannot silently fall back to the wrong strip cell.
    const struct Tool { const char* id; const char* text; const char* iconFile; } toolSpecs[] = {
        {"tool.pointer", "Selection tool", "select_tool.png"},
        {"tool.magnify", "Magnify", "magnify_tool.png"},
        {"tool.camera", "Camera", "camera_tool.png"},
        {"tool.entity", "Entity tool", "entity_tool.png"},
        {"tool.block", "Block tool", "brush_tool.png"},
        {"tool.textureApplication", "Texture application", "face_tool.png"},
        {"tool.applyTexture", "Apply current texture", "texture_tool.png"},
        {"tool.decals", "Apply decals", "decal_tool.png"},
        {"tool.overlay", "Overlay tool", "overlay_tool.png"},
        {"tool.clipper", "Clipping tool", "clip_tool.png"},
        {"tool.morph", "Vertex manipulation", "vertex_tool.png"}
    };
    const QKeySequence shortcuts[] = {
        QKeySequence(QStringLiteral("Shift+S")), QKeySequence(QStringLiteral("Shift+G")),
        QKeySequence(QStringLiteral("Shift+C")), QKeySequence(QStringLiteral("Shift+E")),
        QKeySequence(QStringLiteral("Shift+B")), QKeySequence(QStringLiteral("Shift+A")),
        QKeySequence(QStringLiteral("Shift+T")), QKeySequence(QStringLiteral("Shift+D")),
        QKeySequence(QStringLiteral("Shift+O")), QKeySequence(QStringLiteral("Shift+X")),
        QKeySequence(QStringLiteral("Shift+V"))
    };
    for (int i = 0; i < 11; ++i) {
        if (i == 3 || i == 5 || i == 9) {
            editTools->addSeparator();
        }

        QAction* action = command(QString::fromLatin1(toolSpecs[i].id), tr(toolSpecs[i].text), shortcuts[i]);
        // "Apply current texture" is a command, not a tool mode: the original
        // applies the texture and leaves the active tool alone (mainfrm.cpp
        // routes ID_TOOLS_APPLYTEXTURE to CMapDoc, not to CToolManager).
        const bool momentary = QLatin1StringView(toolSpecs[i].id) ==
                               QLatin1StringView("tool.applyTexture");
        action->setCheckable(!momentary);
        action->setToolTip(tr(toolSpecs[i].text));
        action->setStatusTip(tr(toolSpecs[i].text));
        action->setProperty("implemented", true);

        QIcon icon = HammerIconFactory::fromStrip(MapEditStrip, i, 40, 32);
        if (toolSpecs[i].iconFile) {
            const QIcon supplied = loadSidebarToolIcon(QString::fromLatin1(toolSpecs[i].iconFile));
            if (!supplied.isNull()) icon = supplied;
        }
        action->setIcon(icon);
        action->setProperty("sidebarIconFile",
                            toolSpecs[i].iconFile ? QString::fromLatin1(toolSpecs[i].iconFile) : QString());

        // Use a real tool button instead of relying on QAction icon updates
        // after QToolBar has already created its internal button. This makes
        // the supplied PNGs visible consistently across Qt styles.
        auto* button = new QToolButton(editTools);
        button->setObjectName(QStringLiteral("HammerSidebarToolButton"));
        button->setDefaultAction(action);
        button->setIcon(icon);
        button->setIconSize(QSize(36, 36));
        button->setFixedSize(46, 44);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        editTools->addWidget(button);

        if (momentary) {
            connect(action, &QAction::triggered, this, &MainWindow::applyCurrentTextureCommand);
            continue;
        }
        toolGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, action] { selectTool(action); });
    }
    commands_.value(QStringLiteral("tool.pointer"))->setChecked(true);
}

void MainWindow::createRightControlBars()
{
    // One scrolling sidebar replaces the four stacked dock widgets the port
    // used to mimic Hammer's control bars. The sections are QGroupBoxes so
    // they pick up the same themed styling as the Face Edit sheet and the
    // model/texture browsers, and the whole column lives in a QScrollArea:
    // when the window is too short the sidebar scrolls instead of Qt crushing
    // the sections into each other.
    auto* sidebar = new QWidget;
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(8, 8, 8, 8);
    sidebarLayout->setSpacing(8);

    auto* selectionBox = new QGroupBox(tr("Selection Mode"), sidebar);
    auto* selectionLayout = new QHBoxLayout(selectionBox);
    selectionLayout->setContentsMargins(8, 6, 8, 8);
    selectionLayout->setSpacing(4);
    auto* selectionModes = new QButtonGroup(this);
    selectionModes->setExclusive(true);
    const struct SelectionChoice { const char* text; int id; } selectionChoices[] = {
        {"Groups", 0}, {"Objects", 1}, {"Solids", 2}
    };
    for (const SelectionChoice& choice : selectionChoices) {
        auto* button = new QPushButton(tr(choice.text));
        button->setCheckable(true);
        button->setChecked(choice.id == 0);
        selectionModes->addButton(button, choice.id);
        selectionLayout->addWidget(button);
    }
    connect(selectionModes, &QButtonGroup::idClicked, this, [this](int id) {
        if (MapDocumentWidget* document = activeDocument()) {
            document->setSelectionMode(id == 2 ? MapDocumentWidget::SelectionMode::Solids :
                                       id == 1 ? MapDocumentWidget::SelectionMode::Objects :
                                                 MapDocumentWidget::SelectionMode::Groups);
        }
    });
    sidebarLayout->addWidget(selectionBox);

    auto* textureBox = new QGroupBox(tr("Textures"), sidebar);
    auto* textureLayout = new QGridLayout(textureBox);
    textureLayout->setContentsMargins(8, 6, 8, 8);
    textureLayout->setHorizontalSpacing(6);
    textureLayout->setVerticalSpacing(4);
    textureLayout->addWidget(new QLabel(tr("Texture group:")), 0, 0, 1, 2);
    auto* textureGroup = new QComboBox;
    textureGroup->addItems({tr("All Textures"), tr("Brick"), tr("Concrete"), tr("Metal"), tr("Tools")});
    textureLayout->addWidget(textureGroup, 1, 0, 1, 2);
    textureLayout->addWidget(new QLabel(tr("Current texture:")), 2, 0, 1, 2);
    textureCombo_ = new QComboBox;
    textureCombo_->setEditable(true);
    textureCombo_->addItems({QStringLiteral("tools/toolsnodraw"), QStringLiteral("brick/brickwall001a"), QStringLiteral("concrete/concretefloor001a")});
    textureLayout->addWidget(textureCombo_, 3, 0, 1, 2);
    connect(textureCombo_, &QComboBox::currentTextChanged, this, [this](const QString& material) {
        if (MapDocumentWidget* document = activeDocument()) document->setCurrentMaterial(material);
        updateTexturePreview(material);
        if (faceEditSheet_) faceEditSheet_->setCurrentMaterial(material);
    });
    texturePreview_ = new QLabel;
    texturePreview_->setPixmap(makeTexturePreview());
    texturePreview_->setFixedSize(100, 100);
    texturePreview_->setAlignment(Qt::AlignCenter);
    texturePreview_->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    textureLayout->addWidget(texturePreview_, 4, 0, 3, 1, Qt::AlignTop);
    textureSizeLabel_ = new QLabel(QStringLiteral("64 x 64"));
    textureLayout->addWidget(textureSizeLabel_, 4, 1, Qt::AlignTop);
    auto* browseTextures = new QPushButton(tr("&Browse..."));
    connect(browseTextures, &QPushButton::clicked, this, &MainWindow::showMaterialBrowser);
    textureLayout->addWidget(browseTextures, 5, 1);
    auto* replaceTexturesButton = new QPushButton(tr("&Replace..."));
    connect(replaceTexturesButton, &QPushButton::clicked, this, &MainWindow::showReplaceTexturesDialog);
    textureLayout->addWidget(replaceTexturesButton, 6, 1);
    sidebarLayout->addWidget(textureBox);

    auto* filterBox = new QGroupBox(tr("Filter Control"), sidebar);
    auto* filterLayout = new QVBoxLayout(filterBox);
    filterLayout->setContentsMargins(8, 6, 8, 8);
    filterLayout->setSpacing(4);
    // The VisGroups tree (hammer/filtercontrol.cpp, grouplist.cpp). Each item's
    // check box is the visgroup's shown/hidden/partial state, derived from its
    // members' visgroupshown flags.
    auto* tabs = new QTabWidget;
    visGroupTree_ = new QTreeWidget;
    visGroupTree_->setHeaderHidden(true);
    // Renaming in place is the original's "select the VisGroup, then click on
    // it again" gesture.
    visGroupTree_->setEditTriggers(QAbstractItemView::SelectedClicked |
                                   QAbstractItemView::EditKeyPressed);
    autoVisGroupTree_ = new QTreeWidget;
    autoVisGroupTree_->setHeaderHidden(true);
    // Auto visgroups are generated, so they cannot be renamed or deleted
    // (hammer/editgroups.cpp refuses); only their check box does anything.
    tabs->addTab(visGroupTree_, tr("User"));
    tabs->addTab(autoVisGroupTree_, tr("Auto"));
    // The tree is the one section that benefits from spare height; everything
    // else keeps its natural size. It never shrinks below a usable height —
    // past that the sidebar scrolls.
    tabs->setMinimumHeight(150);
    filterLayout->addWidget(tabs, 1);
    auto* filterButtons = new QHBoxLayout;
    showAllVisGroupsButton_ = new QPushButton(tr("&Show All"));
    showAllVisGroupsButton_->setCheckable(true);
    showAllVisGroupsButton_->setToolTip(
        tr("Show every VisGroup without changing what each one has hidden"));
    filterButtons->addWidget(showAllVisGroupsButton_);
    editVisGroupsButton_ = new QPushButton(tr("&Edit"));
    editVisGroupsButton_->setToolTip(tr("Rename, recolor, create and delete VisGroups"));
    markVisGroupButton_ = new QPushButton(tr("&Mark"));
    markVisGroupButton_->setToolTip(tr("Select the visible objects in this VisGroup"));
    moveVisGroupUpButton_ = new QPushButton(QStringLiteral("▲"));
    moveVisGroupDownButton_ = new QPushButton(QStringLiteral("▼"));
    moveVisGroupUpButton_->setToolTip(tr("Move the selected VisGroup up among its siblings"));
    moveVisGroupDownButton_->setToolTip(tr("Move the selected VisGroup down among its siblings"));
    for (QPushButton* button : {showAllVisGroupsButton_, editVisGroupsButton_, markVisGroupButton_,
                                moveVisGroupUpButton_, moveVisGroupDownButton_}) {
        // Sidebar buttons must never steal Return from the views.
        button->setAutoDefault(false);
    }
    filterButtons->addWidget(editVisGroupsButton_);
    filterButtons->addWidget(markVisGroupButton_);
    filterButtons->addWidget(moveVisGroupUpButton_);
    filterButtons->addWidget(moveVisGroupDownButton_);
    filterLayout->addLayout(filterButtons);
    // Mark and the reorder arrows need a selected VisGroup, the same way the
    // Entity Report's buttons wait for a selected entity.
    connect(visGroupTree_, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::updateVisGroupButtonStates);

    connect(showAllVisGroupsButton_, &QPushButton::toggled, this, [this](bool showAll) {
        if (MapDocumentWidget* document = activeDocument()) document->setShowAllVisGroups(showAll);
        scheduleVisGroupTreeRebuild();
    });
    connect(visGroupTree_, &QTreeWidget::itemChanged, this,
            &MainWindow::handleVisGroupItemChanged);
    connect(autoVisGroupTree_, &QTreeWidget::itemChanged, this,
            &MainWindow::handleAutoVisGroupItemChanged);
    connect(autoVisGroupTree_, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::updateVisGroupButtonStates);
    connect(editVisGroupsButton_, &QPushButton::clicked, this,
            &MainWindow::showEditVisGroupsDialog);
    connect(markVisGroupButton_, &QPushButton::clicked, this, [this] {
        MapDocumentWidget* document = activeDocument();
        const int visGroupId = selectedVisGroupId();
        if (!document || visGroupId == 0) return;
        const std::size_t marked = document->markVisGroup(visGroupId);
        statusBar()->showMessage(marked > 0
                                     ? tr("Marked %1 object(s)").arg(static_cast<qulonglong>(marked))
                                     : tr("That VisGroup has no visible objects to mark"),
                                 4000);
    });
    const auto moveSelected = [this](bool up) {
        MapDocumentWidget* document = activeDocument();
        const int visGroupId = selectedVisGroupId();
        if (!document || visGroupId == 0) return;
        if (document->moveVisGroup(visGroupId, up)) scheduleVisGroupTreeRebuild();
    };
    connect(moveVisGroupUpButton_, &QPushButton::clicked, this,
            [moveSelected] { moveSelected(true); });
    connect(moveVisGroupDownButton_, &QPushButton::clicked, this,
            [moveSelected] { moveSelected(false); });
    sidebarLayout->addWidget(filterBox, 1);

    auto* objectBox = new QGroupBox(tr("New Objects"), sidebar);
    auto* objectLayout = new QGridLayout(objectBox);
    objectLayout->setContentsMargins(8, 6, 8, 8);
    objectLayout->setHorizontalSpacing(6);
    objectLayout->setVerticalSpacing(4);
    objectLayout->addWidget(new QLabel(tr("Move selected:")), 0, 0);
    objectLayout->addWidget(new QPushButton(tr("to&World")), 0, 1);
    objectLayout->addWidget(new QPushButton(tr("to&Entity")), 0, 2);
    objectLayout->addWidget(new QLabel(tr("&Categories:")), 1, 0, 1, 3);
    auto* categories = new QComboBox;
    categories->addItems({tr("Entities"), tr("Primitives"), tr("Prefabs")});
    objectLayout->addWidget(categories, 2, 0, 1, 3);
    objectLayout->addWidget(new QLabel(tr("&Objects:")), 3, 0, 1, 3);
    objectCombo_ = new QComboBox;
    objectCombo_->setEditable(true);
    objectCombo_->addItems({QStringLiteral("info_player_start"), QStringLiteral("light"), QStringLiteral("prop_static")});
    objectLayout->addWidget(objectCombo_, 4, 0, 1, 3);
    connect(objectCombo_, &QComboBox::currentTextChanged, this, [this, categories](const QString& name) {
        if (categories->currentIndex() == 1) {
            primitiveName_ = name;
            if (MapDocumentWidget* document = activeDocument()) document->setPrimitiveKindByName(name);
        } else if (MapDocumentWidget* document = activeDocument()) {
            document->setEntityClass(name);
        }
    });
    // Switching category swaps the Objects list; the entity list is stashed
    // so typed classnames survive a round trip through Primitives.
    auto entityItems = std::make_shared<QStringList>();
    connect(categories, &QComboBox::currentIndexChanged, this, [this, categories, entityItems](int index) {
        const QSignalBlocker blocker(objectCombo_);
        if (index == 1) {
            entityItems->clear();
            for (int i = 0; i < objectCombo_->count(); ++i) *entityItems << objectCombo_->itemText(i);
            objectCombo_->clear();
            objectCombo_->addItems({QStringLiteral("Block"), QStringLiteral("Wedge"),
                                    QStringLiteral("Cylinder"), QStringLiteral("Spike")});
            objectCombo_->setCurrentText(primitiveName_);
            if (MapDocumentWidget* document = activeDocument())
                document->setPrimitiveKindByName(primitiveName_);
        } else {
            objectCombo_->clear();
            if (!entityItems->isEmpty()) objectCombo_->addItems(*entityItems);
            else objectCombo_->addItems({QStringLiteral("info_player_start"), QStringLiteral("light"),
                                         QStringLiteral("prop_static")});
            if (MapDocumentWidget* document = activeDocument())
                document->setEntityClass(objectCombo_->currentText());
        }
    });
    auto* randomYaw = new QCheckBox(tr("Random Yaw"));
    objectLayout->addWidget(randomYaw, 5, 0, 1, 2);
    auto* faces = new QSpinBox;
    faces->setRange(3, 64);
    faces->setValue(8);
    connect(faces, &QSpinBox::valueChanged, this, [this](int count) {
        primitiveFaces_ = count;
        if (MapDocumentWidget* document = activeDocument()) document->setPrimitiveFaces(count);
    });
    objectLayout->addWidget(faces, 5, 2);
    objectLayout->addWidget(new QPushButton(tr("&Create Prefab")), 6, 0, 1, 3);
    sidebarLayout->addWidget(objectBox);

    // Scroll instead of shrink: the sections keep their natural heights and a
    // vertical scrollbar appears when the window is too short for all of them.
    // No Ignored size policies and no SetNoConstraint — those were exactly the
    // machinery that let the old docks crush their contents.
    auto* scroll = new QScrollArea;
    scroll->setWidget(sidebar);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // With the horizontal bar off, content narrower than its minimum would be
    // clipped unreachably; forbid dragging the dock that thin instead.
    scroll->setMinimumWidth(sidebar->minimumSizeHint().width() +
                            scroll->verticalScrollBar()->sizeHint().width() + 2);

    auto* dock = new QDockWidget(tr("Editor Sidebar"), this);
    dock->setObjectName(QStringLiteral("EditorSidebar"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setWidget(scroll);
    dock->setMaximumWidth(340);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    controlBars_.insert(QStringLiteral("EditorSidebar"), dock);
    connect(dock, &QDockWidget::dockLocationChanged, this,
            [this](Qt::DockWidgetArea) { scheduleResizableLayoutRefresh(); });
    connect(dock, &QDockWidget::topLevelChanged, this,
            [this](bool) { scheduleResizableLayoutRefresh(); });
    connect(dock, &QDockWidget::visibilityChanged, this,
            [this](bool) { scheduleResizableLayoutRefresh(); });

    // The old per-dock toggles live on as per-section toggles, keeping the
    // Screen Elements menu entries (and their mnemonics) intact.
    const auto sectionToggle = [this](QGroupBox* box, const QString& text) {
        auto* action = new QAction(text, this);
        action->setCheckable(true);
        action->setChecked(true);
        connect(action, &QAction::toggled, box, &QWidget::setVisible);
        return action;
    };
    QAction* selectionToggle = sectionToggle(selectionBox, tr("Selection &Mode Bar"));
    QAction* objectToggle = sectionToggle(objectBox, tr("&Object Bar"));
    QAction* filterToggle = sectionToggle(filterBox, tr("&Filter Control"));
    QAction* textureToggle = sectionToggle(textureBox, tr("Te&xture Bar"));

    dock->toggleViewAction()->setText(tr("Editor &Sidebar"));
    toolBars_.value(QStringLiteral("edittools"))->toggleViewAction()->setText(tr("Map&tools Bar"));
    toolBars_.value(QStringLiteral("operations"))->toggleViewAction()->setText(tr("Mapo&perations Bar"));
    toolBars_.value(QStringLiteral("mapview"))->toggleViewAction()->setText(tr("Map&view Bar"));

    if (QMenu* screenElements = menuBar()->findChild<QMenu*>(QStringLiteral("screenElementsMenu"))) {
        screenElements->addAction(dock->toggleViewAction());
        screenElements->addAction(selectionToggle);
        screenElements->addAction(objectToggle);
        screenElements->addAction(toolBars_.value(QStringLiteral("edittools"))->toggleViewAction());
        screenElements->addAction(toolBars_.value(QStringLiteral("operations"))->toggleViewAction());
        screenElements->addAction(toolBars_.value(QStringLiteral("mapview"))->toggleViewAction());
        screenElements->addAction(filterToggle);
        screenElements->addAction(textureToggle);
        screenElements->addSeparator();
        QAction* statusToggle = screenElements->addAction(tr("&Status Bar"));
        statusToggle->setCheckable(true);
        statusToggle->setChecked(true);
        connect(statusToggle, &QAction::toggled, statusBar(), &QStatusBar::setVisible);
    }
}

void MainWindow::createMessageWindow()
{
    messageDock_ = new QDockWidget(tr("Messages"), this);
    messageDock_->setObjectName(QStringLiteral("MessageWindow"));
    messageDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    messageOutput_ = new QPlainTextEdit(messageDock_);
    messageOutput_->setReadOnly(true);
    messageOutput_->setMaximumBlockCount(5000);
    messageOutput_->setPlainText(tr("Hammer-- Linux/Qt6 build\n"
                                    "Classic Hammer frame initialized from the original menu, toolbar, and dialog resources.\n"
                                    "VMF parsing, editing, object creation, clipboard transforms, and FGD SmartEdit are active.\n"));
    messageDock_->setWidget(messageOutput_);
    addDockWidget(Qt::BottomDockWidgetArea, messageDock_);
    resizeDocks({messageDock_}, {100}, Qt::Vertical);

    if (QAction* messages = commands_.value(QStringLiteral("view.messages"))) {
        messages->setChecked(true);
        connect(messages, &QAction::toggled, messageDock_, &QDockWidget::setVisible);
        connect(messageDock_, &QDockWidget::visibilityChanged, messages, &QAction::setChecked);
    }
}

void MainWindow::createStatusBar()
{
    promptPane_ = makeStatusPane({}, 220);
    selectionPane_ = makeStatusPane(tr("no selection"), 118);
    coordinatesPane_ = makeStatusPane(QStringLiteral("0, 0, 0"), 120);
    sizePane_ = makeStatusPane(QStringLiteral("0 x 0 x 0"), 105);
    gridPane_ = makeStatusPane(tr("Grid: 16  Zoom: 1.00"), 145);
    snapPane_ = makeStatusPane(tr("Snap: %1").arg(gridSpacing_), 72);

    statusBar()->addWidget(promptPane_, 1);
    statusBar()->addPermanentWidget(selectionPane_);
    statusBar()->addPermanentWidget(coordinatesPane_);
    statusBar()->addPermanentWidget(sizePane_);
    statusBar()->addPermanentWidget(gridPane_);
    statusBar()->addPermanentWidget(snapPane_);
}

void MainWindow::updateTransformHandlesActionText()
{
    if (!transformHandlesAction_) return;
    switch (transformMode_) {
    case MapViewWidget::TransformMode::Scale:
        transformHandlesAction_->setText(tr("Transform Handles: Resize")); break;
    case MapViewWidget::TransformMode::Translate:
        transformHandlesAction_->setText(tr("Transform Handles: Move")); break;
    case MapViewWidget::TransformMode::Rotate:
        transformHandlesAction_->setText(tr("Transform Handles: Rotate")); break;
    }
}

MapDocumentWidget* MainWindow::createDocument(const QString& path, const QString& recentPath)
{
    // Undo/Redo Active is a session-scoped escape hatch for a map that is
    // eating memory, never a saved preference: opening or creating a map arms
    // it again, as does a fresh launch (nothing about it is persisted).
    if (undoRedoActiveAction_ && !undoRedoActiveAction_->isChecked()) {
        undoRedoActiveAction_->setChecked(true);  // runs setUndoRedoActive
    } else {
        undoRedoActive_ = true;
    }

    auto* document = new MapDocumentWidget(fgd_);
    document->setMaterialSystem(materials_);
    document->setMaterialRenderingEnabled(materialRenderingEnabled_);
    document->setWireframeOverlayEnabled(wireframeOverlayEnabled_);
    document->setHdrEnabled(hdrEnabled_);
    document->setUndoRedoActive(undoRedoActive_);
    document->setRayTracedGamma(rayTracedGamma_);
    document->setDisplacementSolidMaskEnabled(displacementSolidMaskEnabled_);
    document->setTexturedRenderMode(texturedRenderMode_);
    document->setMaterialEffectsEnabled(phongEnabled_, specularEnabled_, bumpMapsEnabled_,
                                        lightWarpEnabled_, selfIllumEnabled_, rimLightEnabled_);
    document->setMaterialEffectIntensities(phongIntensity_, specularIntensity_, bumpMapIntensity_);
    applyObjectBarSettings(document);
    document->setGridSnapEnabled(gridSnapEnabled_);
    document->setGridSpacing(gridSpacing_);
    document->setTransformMode(transformMode_);
    if (currentToolId_ == QStringLiteral("tool.block")) document->setTool(MapViewWidget::Tool::Block);
    else if (currentToolId_ == QStringLiteral("tool.entity")) document->setTool(MapViewWidget::Tool::Entity);
    else if (currentToolId_ == QStringLiteral("tool.decals")) document->setTool(MapViewWidget::Tool::Decal);
    else if (currentToolId_ == QStringLiteral("tool.overlay")) document->setTool(MapViewWidget::Tool::Overlay);
    else if (currentToolId_ == QStringLiteral("tool.magnify")) document->setTool(MapViewWidget::Tool::Magnify);
    else if (currentToolId_ == QStringLiteral("tool.camera")) document->setTool(MapViewWidget::Tool::Camera);
    else if (currentToolId_ == QStringLiteral("tool.clipper")) document->setTool(MapViewWidget::Tool::Clipper);
    else if (currentToolId_ == QStringLiteral("tool.morph")) document->setTool(MapViewWidget::Tool::Morph);
        else if (currentToolId_ == QStringLiteral("tool.textureApplication")) document->setTool(MapViewWidget::Tool::TextureApplication);
    else document->setTool(MapViewWidget::Tool::Selection);
    if (!path.isEmpty()) {
        // Loading prompt: group-box sectioning like the rest of the dialogs;
        // HammerTheme styles the bar itself.
        QDialog progressDialog(this, Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
        progressDialog.setWindowTitle(tr("Loading Map"));
        progressDialog.setModal(true);
        auto* progressLayout = new QVBoxLayout(&progressDialog);
        auto* progressBox = new QGroupBox(tr("Loading %1").arg(QFileInfo(path).fileName()),
                                          &progressDialog);
        auto* progressBoxLayout = new QVBoxLayout(progressBox);
        auto* progressStage = new QLabel(tr("Reading…"), progressBox);
        progressStage->setWordWrap(true);
        auto* progressBar = new QProgressBar(progressBox);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBoxLayout->addWidget(progressStage);
        progressBoxLayout->addWidget(progressBar);
        progressLayout->addWidget(progressBox);
        progressDialog.setFixedWidth(420);
        progressDialog.show();
        QCoreApplication::processEvents();

        QString error;
        const auto progress = [progressStage, progressBar](int percent, const QString& stage) {
            progressBar->setValue(percent);
            progressStage->setText(stage);
            QCoreApplication::processEvents();
        };
        if (!document->loadFromFile(path, &error, progress)) {
            progressDialog.close();
            QMessageBox::critical(this, tr("Open VMF"),
                                  tr("Could not open %1.\n\n%2").arg(path, error));
            document->deleteLater();
            return nullptr;
        }
        // Put the map in the desktop's Recent list, the way any application
        // that opens a document does. Nothing does this for us: the file
        // manager reads a store the application has to append to itself.
        const QString recorded = recentPath.isEmpty() ? path : recentPath;
        QString recentError;
        if (hammer::app::registerRecentlyUsedFile(recorded, &recentError)) {
            appendMessage(tr("Added %1 to Recent Files").arg(recorded));
        } else {
            appendMessage(tr("Could not add %1 to Recent Files — %2")
                              .arg(recorded, recentError.isEmpty() ? tr("unknown error")
                                                                   : recentError));
        }
    }

    // Keep the MDI minimize/maximize/close controls, but suppress Qt's
    // automatic system-menu icon. The menu bar uses HammerMdiSystemButton,
    // which has deterministic size and alignment across Linux styles.
    const Qt::WindowFlags documentFlags = Qt::SubWindow | Qt::WindowTitleHint |
        Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint;
    auto* subWindow = mdiArea_->addSubWindow(document, documentFlags);
    subWindow->setAttribute(Qt::WA_DeleteOnClose);
    subWindow->setWindowTitle(document->displayName() + QStringLiteral("[*]"));
    subWindow->setWindowModified(document->isModified());
    subWindow->resize(900, 650);
    subWindow->showMaximized();
    updateMdiSystemButtonState();

    connect(document, &MapDocumentWidget::coordinatesChanged, coordinatesPane_, &QLabel::setText);
    connect(document, &MapDocumentWidget::transformModeChanged, this,
            [this](MapViewWidget::TransformMode mode) {
        transformMode_ = mode;
        updateTransformHandlesActionText();
    });
    connect(document, &MapDocumentWidget::activeViewChanged, this, [this](MapViewWidget* view) {
        if (view) {
            setPrompt(tr("%1 view active").arg(view->title()));
        }
    });
    connect(document, &MapDocumentWidget::titleChanged, subWindow, [subWindow](const QString& title) {
        subWindow->setWindowTitle(title + QStringLiteral("[*]"));
    });
    connect(document, &MapDocumentWidget::modifiedChanged, subWindow, &QWidget::setWindowModified);
    connect(document, &MapDocumentWidget::documentMessage, this, [this](const QString& message) {
        appendMessage(message);
        setPrompt(message);
    });
    connect(document, &MapDocumentWidget::editStateChanged, this, &MainWindow::updateEditActions);
    connect(document, &MapDocumentWidget::toolTextureVisibilityChanged, this, [this, document] {
        if (activeDocument() == document) updateNodrawActionState();
    });
    // Undo/redo, deletes and paste all change visgroup membership counts and
    // can restore a whole visgroup, so the Filter Control tree follows the
    // same signal every other edit-state readout does.
    connect(document, &MapDocumentWidget::editStateChanged, this, [this, document] {
        if (activeDocument() == document) scheduleVisGroupTreeRebuild();
    });
    connect(document, &MapDocumentWidget::faceSelectionChanged, this,
            [this, document](const FaceEditValues& values) {
                if (faceEditSheet_ && activeDocument() == document) {
                    faceEditSheet_->setFaceValues(values);
                    // CFaceEditDispPage::UpdateDialogData refreshes the
                    // Attributes fields from the face list too.
                    std::optional<int> dispPower;
                    std::optional<double> dispElevation;
                    document->displacementAttributeValues(dispPower, dispElevation);
                    faceEditSheet_->setDisplacementAttributes(dispPower, dispElevation);
                }
            });
    connect(document, &MapDocumentWidget::currentMaterialLifted, this,
            [this, document](const QString& material) {
                if (activeDocument() != document) return;
                // Mirrors, not a second writer: the combo's currentTextChanged
                // will call setCurrentMaterial with the same value again, which
                // is a no-op, and the sheet's setter is guarded, so this settles
                // in one hop rather than looping.
                if (textureCombo_) textureCombo_->setCurrentText(material);
                if (faceEditSheet_) faceEditSheet_->setCurrentMaterial(material);
            });
    connect(document, &MapDocumentWidget::selectionToolRequested, this, [this] {
        // Clipper3D::OnEscape with no clip line: back to TOOL_POINTER.
        if (QAction* pointer = commands_.value(QStringLiteral("tool.pointer"))) pointer->trigger();
    });
    connect(document, &MapDocumentWidget::selectionChanged, this,
            [this](const QString& summary, const QString& sizeSummary) {
                if (sender() != activeDocument()) return;
                selectionPane_->setText(summary);
                sizePane_->setText(sizeSummary);
            });

    selectionPane_->setText(document->selectionSummary());
    sizePane_->setText(document->selectionSizeSummary());
    updateEditActions();
    updateProjectionActions();
    appendMessage(path.isEmpty()
                      ? tr("Created new map — %1").arg(document->mapSummary())
                      : tr("Opened %1 — %2").arg(path, document->mapSummary()));
    setPrompt(path.isEmpty() ? tr("New map") : tr("Opened %1").arg(path));
    return document;
}

void MainWindow::openDocument()
{
    const QString bspSource = QSettings().value(QStringLiteral("editor/bspsourceExecutable")).toString().trimmed();
    const QString filter = bspSource.isEmpty()
        ? tr("Valve Map Files (*.vmf);;All Files (*.*)")
        : tr("Valve Map Files (*.vmf);;Compiled Maps (*.bsp);;All Files (*.*)");
    // Start where the user last opened a map from, so the maps folder does
    // not have to be re-navigated on every launch.
    const QString lastDirectory =
        QSettings().value(QStringLiteral("editor/lastOpenDirectory")).toString();
    // The native chooser is a separate window that Qt cannot see as modal, so
    // the ray-traced preview has to be told to stand down for its lifetime -
    // otherwise it renders straight through the dialog and starves it of the
    // GUI thread. See PreviewRenderGate.hpp.
    QString path;
    {
        const hammer::app::PreviewRenderSuspension suspendPreview;
        path = QFileDialog::getOpenFileName(this, tr("Open"), lastDirectory, filter);
    }
    if (path.isEmpty()) {
        return;
    }
    QSettings().setValue(QStringLiteral("editor/lastOpenDirectory"),
                         QFileInfo(path).absolutePath());
    openMapPath(path);
}

void MainWindow::openMapPath(const QString& chosen)
{
    // "chosen" is what the desktop's Recent list should remember even when a
    // BSP is decompiled into a temporary VMF first; "path" is what actually
    // gets loaded.
    QString path = chosen;
    const QString bspSource =
        QSettings().value(QStringLiteral("editor/bspsourceExecutable")).toString().trimmed();
    if (path.endsWith(QStringLiteral(".bsp"), Qt::CaseInsensitive)) {
        if (bspSource.isEmpty()) {
            QMessageBox::warning(this, tr("Open BSP"),
                tr("Configure a BSPSource executable in Tools > Options > General to decompile BSP files."));
            return;
        }
        QString error;
        QString contentDir;
        const QString converted = convertBspToVmf(path, &error, &contentDir);
        if (converted.isEmpty()) {
            QMessageBox::critical(this, tr("Decompile BSP"), error);
            return;
        }
        appendMessage(tr("Decompiled %1 — %2").arg(path, converted));
        offerExtractedBspContent(path, contentDir);
        path = converted;
    }
    createDocument(path, chosen);
}

void MainWindow::refreshRecentFilesMenu()
{
    if (!recentFilesMenu_) return;
    recentFilesMenu_->clear();

    QString error;
    // Ten is what fits without turning the File menu into a scrolling list.
    const std::vector<hammer::app::RecentlyUsedFile> recent =
        hammer::app::recentlyUsedFiles(10, &error);
    if (recent.empty()) {
        QAction* empty = recentFilesMenu_->addAction(
            error.isEmpty() ? tr("No Recent Files") : tr("Recent Files unavailable"));
        empty->setEnabled(false);
        if (!error.isEmpty()) empty->setToolTip(error);
        return;
    }

    int index = 1;
    for (const hammer::app::RecentlyUsedFile& entry : recent) {
        // "&1 name" the way every other application numbers this menu, with the
        // full path as the tooltip because map file names repeat across games.
        QAction* action = recentFilesMenu_->addAction(
            QStringLiteral("&%1 %2").arg(index).arg(QFileInfo(entry.path).fileName()));
        action->setToolTip(entry.visited.isValid()
                               ? tr("%1\nLast opened %2")
                                     .arg(entry.path,
                                          entry.visited.toLocalTime().toString(
                                              QStringLiteral("yyyy-MM-dd HH:mm")))
                               : entry.path);
        action->setProperty("implemented", true);
        const QString path = entry.path;
        connect(action, &QAction::triggered, this, [this, path] { openMapPath(path); });
        ++index;
    }
}

QString MainWindow::convertBspToVmf(const QString& bspPath, QString* error,
                                    QString* extractedContentDir)
{
    const QString bspSource = QSettings().value(QStringLiteral("editor/bspsourceExecutable")).toString().trimmed();
    const QFileInfo bspInfo(bspPath);
    QString outputDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (outputDir.isEmpty()) {
        outputDir = bspInfo.absolutePath();
    }
    QString safeBase = bspInfo.completeBaseName();
    safeBase.replace(QLatin1Char(' '), QLatin1Char('_'));
    const QString outputVmf = QDir(outputDir).filePath(safeBase + QStringLiteral("_d.vmf"));

    // BSPSource's launcher script expands its arguments unquoted ($*), so any
    // whitespace in a path splits it into several bogus inputs. Feed it a
    // whitespace-free copy of the BSP when needed.
    QString inputBsp = bspInfo.absoluteFilePath();
    QString tempInput;
    if (inputBsp.contains(QLatin1Char(' '))) {
        tempInput = QDir(outputDir).filePath(safeBase + QStringLiteral(".bsp"));
        QFile::remove(tempInput);
        if (!QFile::copy(inputBsp, tempInput)) {
            if (error) {
                *error = tr("Could not copy %1 to a temporary location for decompiling.").arg(inputBsp);
            }
            return {};
        }
        inputBsp = tempInput;
    }

    QString program = bspSource;
    QStringList arguments;
    if (bspSource.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)) {
        program = QStringLiteral("java");
        arguments << QStringLiteral("-jar") << bspSource;
    }
    // BSPSource unpacks the BSP's embedded pakfile into a directory named
    // after the output VMF (without the extension), next to it. Smart unpack
    // stays on so vbsp-generated files (cubemap VTFs etc.) are skipped.
    arguments << QStringLiteral("--unpack_embedded");
    arguments << QStringLiteral("-o") << outputVmf << inputBsp;
    const auto contentDirFor = [](const QString& vmfPath) {
        QString dir = vmfPath;
        dir.chop(4); // strip ".vmf"
        return dir;
    };
    const auto cleanupTempInput = [&tempInput] {
        if (!tempInput.isEmpty()) {
            QFile::remove(tempInput);
        }
    };

    if (QFileInfo(outputVmf).isDir()) {
        QDir(outputVmf).removeRecursively();
    } else {
        QFile::remove(outputVmf);
    }
    QDir(contentDirFor(outputVmf)).removeRecursively();

    QProgressDialog progress(tr("Decompiling %1…").arg(bspInfo.fileName()), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();

    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) {
        cleanupTempInput();
        if (error) {
            *error = tr("Failed to launch BSPSource (%1): %2").arg(program, process.errorString());
        }
        return {};
    }
    while (!process.waitForFinished(100)) {
        QCoreApplication::processEvents();
        if (progress.wasCanceled()) {
            process.kill();
            process.waitForFinished(3000);
            cleanupTempInput();
            if (error) {
                *error = tr("Decompile canceled.");
            }
            return {};
        }
    }
    progress.close();
    cleanupTempInput();

    const QString processOutput = (QString::fromLocal8Bit(process.readAllStandardError()) +
                                   QString::fromLocal8Bit(process.readAllStandardOutput())).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = tr("BSPSource exited with code %1.%2")
                         .arg(process.exitCode())
                         .arg(processOutput.isEmpty() ? QString() : QStringLiteral("\n\n") + processOutput);
        }
        return {};
    }
    if (QFileInfo(outputVmf).isFile()) {
        if (extractedContentDir) *extractedContentDir = contentDirFor(outputVmf);
        return outputVmf;
    }
    // Some BSPSource builds ignore -o and write <name>_d.vmf next to the input.
    const QString sibling = bspInfo.absoluteDir().filePath(bspInfo.completeBaseName() + QStringLiteral("_d.vmf"));
    if (QFileInfo(sibling).isFile()) {
        if (extractedContentDir) *extractedContentDir = contentDirFor(sibling);
        return sibling;
    }
    if (error) {
        *error = tr("BSPSource finished but no VMF was produced at %1.%2")
                     .arg(outputVmf)
                     .arg(processOutput.isEmpty() ? QString() : QStringLiteral("\n\n") + processOutput);
    }
    return {};
}

void MainWindow::offerExtractedBspContent(const QString& bspPath, const QString& contentDir)
{
    if (contentDir.isEmpty()) return;
    QDir extracted(contentDir);
    if (!extracted.exists()) return;
    const auto removeExtracted = [&extracted] { extracted.removeRecursively(); };

    // Count what BSPSource actually unpacked; an empty directory means the
    // map had no (relevant) embedded files.
    int fileCount = 0;
    {
        QDirIterator it(contentDir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            ++fileCount;
        }
    }
    if (fileCount == 0) {
        removeExtracted();
        return;
    }

    if (loadedGameInfoPath_.isEmpty()) {
        appendMessage(tr("%1 contains %2 packed files, but no game directory is configured — "
                         "skipping asset extraction.").arg(bspPath).arg(fileCount));
        removeExtracted();
        return;
    }

    const QString gameDir = QFileInfo(loadedGameInfoPath_).absolutePath();
    QString mapName = QFileInfo(bspPath).completeBaseName();
    mapName.replace(QLatin1Char(' '), QLatin1Char('_'));
    const QString targetDir =
        QDir(gameDir).filePath(QStringLiteral("custom/") + mapName);

    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, tr("Extract Packed Assets"),
        tr("%1 contains %2 packed files (materials, models, sounds…).\n\n"
           "Extract them to:\n%3\n\n"
           "The folder will be mounted as custom content and will override "
           "matching game content.")
            .arg(QFileInfo(bspPath).fileName())
            .arg(fileCount)
            .arg(targetDir),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes) {
        removeExtracted();
        return;
    }

    std::error_code ec;
    const std::filesystem::path source(contentDir.toStdString());
    const std::filesystem::path target(targetDir.toStdString());
    std::filesystem::create_directories(target, ec);
    std::filesystem::copy(source, target,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        QMessageBox::critical(this, tr("Extract Packed Assets"),
            tr("Could not copy extracted assets to %1:\n%2")
                .arg(targetDir, QString::fromStdString(ec.message())));
        removeExtracted();
        return;
    }
    removeExtracted();
    appendMessage(tr("Extracted %1 packed files to %2").arg(fileCount).arg(targetDir));

    // Remount the game search paths. gameinfo.txt normally lists custom/*
    // ahead of all other content, so the freshly extracted folder takes
    // priority over any conflicting stock assets.
    loadGameInfoPath(loadedGameInfoPath_, false);

    // If this gameinfo has no custom/* search path, force-mount the folder
    // ahead of everything so the extracted content still wins conflicts.
    if (gameFileSystem_) {
        // Match GameFileSystem's canonicalization (Steam paths often go
        // through symlinks such as ~/.steam/steam).
        std::error_code canonicalError;
        auto mountedPath = std::filesystem::weakly_canonical(
            std::filesystem::path(targetDir.toStdString()), canonicalError);
        if (canonicalError) {
            mountedPath = std::filesystem::path(targetDir.toStdString()).lexically_normal();
        }
        const bool alreadyMounted = std::any_of(
            gameFileSystem_->locations().begin(), gameFileSystem_->locations().end(),
            [&](const hammer::assets::SearchLocation& location) {
                return location.kind == hammer::assets::SearchLocation::Kind::Directory &&
                       location.path == mountedPath;
            });
        if (!alreadyMounted && gameFileSystem_->mountOverrideDirectory(mountedPath)) {
            appendMessage(tr("Mounted override content: %1").arg(targetDir));
            materials_ = std::make_shared<hammer::assets::MaterialSystem>(gameFileSystem_);
            refreshMaterialList();
            for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
                if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
                    document->setMaterialSystem(materials_);
                }
            }
        }
    }
}

void MainWindow::runMapCompile()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) {
        setPrompt(tr("Run Map — no open map"));
        return;
    }

    // Program variables come from Tools > Options > Build Programs; the Run
    // Map dialog is an ordered list of shell steps referencing them as $NAME.
    QSettings settings;
    QHash<QString, QString> programs;
    settings.beginGroup(QStringLiteral("buildPrograms"));
    for (const QString& name : settings.childKeys()) {
        const QString command = settings.value(name).toString().trimmed();
        if (!command.isEmpty()) programs.insert(name, command);
    }
    settings.endGroup();
    if (programs.isEmpty()) {
        // The Options table has not been saved yet; honor the previous
        // version's fixed executable slots so an existing setup keeps
        // compiling. Options seeds its table from the same keys.
        const auto quoteLegacy = [](const QString& value) -> QString {
            if (value.isEmpty() || value.contains(QLatin1Char('"'))) return value;
            if (QFileInfo::exists(value)) return QLatin1Char('"') + value + QLatin1Char('"');
            const qsizetype space = value.indexOf(QLatin1Char(' '));
            if (space > 0 && QFileInfo::exists(value.mid(space + 1).trimmed())) {
                return value.left(space) + QStringLiteral(" \"") +
                       value.mid(space + 1).trimmed() + QLatin1Char('"');
            }
            return value;
        };
        const std::pair<QString, QString> legacySlots[] = {
            {QStringLiteral("VBSP"), QStringLiteral("editor/vbspExecutable")},
            {QStringLiteral("VVIS"), QStringLiteral("editor/vvisExecutable")},
            {QStringLiteral("VRAD"), QStringLiteral("editor/vradExecutable")},
            {QStringLiteral("GAME"), QStringLiteral("editor/gameExecutable")},
        };
        for (const auto& [name, legacyKey] : legacySlots) {
            const QString value = settings.value(legacyKey).toString().trimmed();
            if (!value.isEmpty()) programs.insert(name, quoteLegacy(value));
        }
    }

    struct Step { bool enabled; QString command; };
    QList<Step> steps;
    const int storedSteps = settings.beginReadArray(QStringLiteral("editor/runMapSteps"));
    for (int index = 0; index < storedSteps; ++index) {
        settings.setArrayIndex(index);
        steps.append({settings.value(QStringLiteral("enabled"), true).toBool(),
                      settings.value(QStringLiteral("command")).toString()});
    }
    settings.endArray();
    // No hardcoded fallback list: with nothing stored, the dialog loads the
    // "Normal" preset below instead of a legacy "(custom)" step set.
    const bool noStoredSteps = steps.isEmpty();

    QDialog configDialog(this);
    configDialog.setWindowTitle(tr("Run Map"));
    configDialog.resize(680, 460);
    auto* configLayout = new QVBoxLayout(&configDialog);

    // Presets: named copies of the step list, stored under
    // runMapPresets/<name>/steps. The working list stays independent, so
    // tweaking steps never silently rewrites a preset.
    auto* presetBox = new QGroupBox(tr("Preset"), &configDialog);
    auto* presetBar = new QHBoxLayout(presetBox);
    auto* presetCombo = new QComboBox;
    presetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* savePreset = new QPushButton(tr("Save Preset…"));
    auto* deletePreset = new QPushButton(tr("Delete Preset"));
    presetBar->addWidget(presetCombo, 1);
    presetBar->addWidget(savePreset);
    presetBar->addWidget(deletePreset);
    configLayout->addWidget(presetBox);

    auto* stepsBox = new QGroupBox(tr("Compile Steps"), &configDialog);
    auto* stepsLayout = new QVBoxLayout(stepsBox);
    auto* stepTable = new QTableWidget(0, 1);
    stepTable->setHorizontalHeaderLabels({tr("Step")});
    stepTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    stepTable->verticalHeader()->setVisible(false);
    stepTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    const auto addStepRow = [stepTable](bool enabled, const QString& command, int at = -1) {
        const int row = at < 0 ? stepTable->rowCount() : at;
        stepTable->insertRow(row);
        auto* item = new QTableWidgetItem(command);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
        stepTable->setItem(row, 0, item);
    };
    for (const Step& step : steps) addStepRow(step.enabled, step.command);
    stepsLayout->addWidget(stepTable, 1);

    auto* stepButtons = new QHBoxLayout;
    auto* addStep = new QPushButton(tr("Add"));
    auto* removeStep = new QPushButton(tr("Remove"));
    auto* moveUp = new QPushButton(tr("Up"));
    auto* moveDown = new QPushButton(tr("Down"));
    stepButtons->addWidget(addStep);
    stepButtons->addWidget(removeStep);
    stepButtons->addWidget(moveUp);
    stepButtons->addWidget(moveDown);
    stepButtons->addStretch();
    stepsLayout->addLayout(stepButtons);
    connect(addStep, &QPushButton::clicked, &configDialog, [stepTable, addStepRow] {
        addStepRow(true, QString());
        stepTable->setCurrentCell(stepTable->rowCount() - 1, 0);
        stepTable->editItem(stepTable->item(stepTable->rowCount() - 1, 0));
    });
    connect(removeStep, &QPushButton::clicked, &configDialog, [stepTable] {
        if (stepTable->currentRow() >= 0) stepTable->removeRow(stepTable->currentRow());
    });
    const auto moveRow = [stepTable, addStepRow](int delta) {
        const int row = stepTable->currentRow();
        const int target = row + delta;
        if (row < 0 || target < 0 || target >= stepTable->rowCount()) return;
        QTableWidgetItem* item = stepTable->item(row, 0);
        const QString command = item ? item->text() : QString();
        const bool enabled = item && item->checkState() == Qt::Checked;
        stepTable->removeRow(row);
        addStepRow(enabled, command, target);
        stepTable->setCurrentCell(target, 0);
    };
    connect(moveUp, &QPushButton::clicked, &configDialog, [moveRow] { moveRow(-1); });
    connect(moveDown, &QPushButton::clicked, &configDialog, [moveRow] { moveRow(1); });

    const QString customEntry = tr("(custom)");
    const auto writePreset = [](const QString& name, const QList<Step>& presetSteps) {
        QSettings s;
        s.remove(QStringLiteral("runMapPresets/") + name);
        s.beginGroup(QStringLiteral("runMapPresets/") + name);
        s.beginWriteArray(QStringLiteral("steps"));
        for (int i = 0; i < presetSteps.size(); ++i) {
            s.setArrayIndex(i);
            s.setValue(QStringLiteral("enabled"), presetSteps[i].enabled);
            s.setValue(QStringLiteral("command"), presetSteps[i].command);
        }
        s.endArray();
        s.endGroup();
        QStringList order = s.value(QStringLiteral("editor/runMapPresetOrder")).toStringList();
        if (!order.contains(name)) {
            order.append(name);
            s.setValue(QStringLiteral("editor/runMapPresetOrder"), order);
        }
    };
    // The dropdown lists presets in the order they were created, not
    // alphabetically; runMapPresetOrder records that order.
    const auto presetNames = [] {
        QSettings s;
        s.beginGroup(QStringLiteral("runMapPresets"));
        const QStringList existing = s.childGroups();
        s.endGroup();
        QStringList order = s.value(QStringLiteral("editor/runMapPresetOrder")).toStringList();
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [&](const QString& name) { return !existing.contains(name); }),
                    order.end());
        for (const QString& name : existing) {
            if (!order.contains(name)) order.append(name);
        }
        return order;
    };
    // First run: seed the stock Hammer compile configurations.
    bool presetsJustSeeded = false;
    if (presetNames().isEmpty()) {
        presetsJustSeeded = true;
        writePreset(QStringLiteral("Normal"), {
            {true, QStringLiteral("$VBSP -game $gamedir $file")},
            {true, QStringLiteral("$VVIS -game $gamedir $bsp")},
            {true, QStringLiteral("$VRAD -staticproppolys -staticproplighting -textureshadows -game $gamedir $bsp")},
            {true, QStringLiteral("cp $bsp $gamedir/maps/")},
            {true, QStringLiteral("$GAME -dev -console -allowdebug -hijack -game $gamedir +map $mapname")},
        });
        writePreset(QStringLiteral("Fast"), {
            {true, QStringLiteral("$VBSP -game $gamedir $file")},
            {true, QStringLiteral("$VVIS -fast -game $gamedir $bsp")},
            {true, QStringLiteral("$VRAD -staticproppolys -textureshadows -fast -game $gamedir $bsp")},
            {true, QStringLiteral("cp $bsp $gamedir/maps/")},
            {true, QStringLiteral("$GAME -dev -console -allowdebug -hijack -game $gamedir +map $mapname")},
        });
        writePreset(QStringLiteral("Final"), {
            {true, QStringLiteral("$VBSP -game $gamedir $file")},
            {true, QStringLiteral("$VVIS -game $gamedir $bsp")},
            {true, QStringLiteral("$VRAD -final -staticproppolys -staticproplighting -textureshadows -game $gamedir $bsp")},
            {true, QStringLiteral("cp $bsp $gamedir/maps/")},
            {true, QStringLiteral("$GAME -multirun -novid -buildcubemaps -game $gamedir +map $mapname")},
            {true, QStringLiteral("$GAME -dev -console -allowdebug -hijack -game $gamedir +map $mapname")},
        });
        writePreset(QStringLiteral("Only Entities"), {
            {true, QStringLiteral("$VBSP -onlyents -game $gamedir $file")},
            {true, QStringLiteral("cp $bsp $gamedir/maps/")},
            {true, QStringLiteral("$GAME -dev -console -allowdebug -hijack -game $gamedir +map $mapname")},
        });
    }
    const auto reloadPresetCombo = [presetCombo, presetNames, customEntry](const QString& select) {
        QSignalBlocker blocker(presetCombo);
        presetCombo->clear();
        presetCombo->addItem(customEntry);
        presetCombo->addItems(presetNames());
        const int index = presetCombo->findText(select, Qt::MatchFixedString);
        presetCombo->setCurrentIndex(index >= 0 ? index : 0);
    };
    const auto tableSteps = [stepTable] {
        QList<Step> result;
        for (int row = 0; row < stepTable->rowCount(); ++row) {
            const QTableWidgetItem* item = stepTable->item(row, 0);
            if (!item || item->text().trimmed().isEmpty()) continue;
            result.append({item->checkState() == Qt::Checked, item->text().trimmed()});
        }
        return result;
    };
    const auto loadPresetSteps = [stepTable, addStepRow](const QString& name) {
        QSettings s;
        s.beginGroup(QStringLiteral("runMapPresets/") + name);
        const int count = s.beginReadArray(QStringLiteral("steps"));
        QList<Step> loaded;
        for (int i = 0; i < count; ++i) {
            s.setArrayIndex(i);
            loaded.append({s.value(QStringLiteral("enabled"), true).toBool(),
                           s.value(QStringLiteral("command")).toString()});
        }
        s.endArray();
        s.endGroup();
        stepTable->setRowCount(0);
        for (const Step& step : loaded) addStepRow(step.enabled, step.command);
    };
    const QString lastPreset =
        settings.value(QStringLiteral("editor/runMapLastPreset")).toString();
    if (noStoredSteps || presetsJustSeeded) {
        // Fresh profile (or first run after the preset feature): open on the
        // Normal preset instead of an empty or legacy "(custom)" list.
        reloadPresetCombo(QStringLiteral("Normal"));
        if (presetCombo->currentIndex() > 0) loadPresetSteps(presetCombo->currentText());
    } else {
        reloadPresetCombo(lastPreset);
    }
    connect(presetCombo, &QComboBox::activated, &configDialog,
            [presetCombo, loadPresetSteps](int index) {
        if (index == 0) return; // (custom) keeps the current list
        loadPresetSteps(presetCombo->currentText());
    });
    connect(savePreset, &QPushButton::clicked, &configDialog,
            [&configDialog, presetCombo, tableSteps, reloadPresetCombo, customEntry,
             writePreset] {
        const QString suggestion = presetCombo->currentIndex() > 0
            ? presetCombo->currentText() : QString();
        const QString name = QInputDialog::getText(
            &configDialog, tr("Save Preset"), tr("Preset name:"),
            QLineEdit::Normal, suggestion).trimmed();
        if (name.isEmpty() || name == customEntry) return;
        if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
            QMessageBox::warning(&configDialog, tr("Save Preset"),
                                 tr("Preset names cannot contain slashes."));
            return;
        }
        writePreset(name, tableSteps());
        reloadPresetCombo(name);
    });
    connect(deletePreset, &QPushButton::clicked, &configDialog,
            [&configDialog, presetCombo, reloadPresetCombo] {
        if (presetCombo->currentIndex() <= 0) return;
        const QString name = presetCombo->currentText();
        if (QMessageBox::question(&configDialog, tr("Delete Preset"),
                                  tr("Delete preset \"%1\"?").arg(name)) != QMessageBox::Yes) {
            return;
        }
        QSettings s;
        s.remove(QStringLiteral("runMapPresets/") + name);
        QStringList order = s.value(QStringLiteral("editor/runMapPresetOrder")).toStringList();
        order.removeAll(name);
        s.setValue(QStringLiteral("editor/runMapPresetOrder"), order);
        reloadPresetCombo(QString());
    });

    auto* placeholderHint = new QLabel(
        tr("Checked steps run in order, chained with && (a failed step stops the rest).\n"
           "$NAME expands to a Build Programs variable (Tools > Options). Paths: "
           "$file (saved VMF), $bsp ($file with .bsp), $mapname, $gamedir."));
    placeholderHint->setEnabled(false);
    placeholderHint->setWordWrap(true);
    stepsLayout->addWidget(placeholderHint);
    configLayout->addWidget(stepsBox, 1);
    auto* configButtons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    configButtons->button(QDialogButtonBox::Ok)->setText(tr("Go!"));
    configLayout->addWidget(configButtons);
    connect(configButtons, &QDialogButtonBox::accepted, &configDialog, &QDialog::accept);
    connect(configButtons, &QDialogButtonBox::rejected, &configDialog, &QDialog::reject);
    if (configDialog.exec() != QDialog::Accepted) return;

    settings.setValue(QStringLiteral("editor/runMapLastPreset"),
                      presetCombo->currentIndex() > 0 ? presetCombo->currentText()
                                                      : QString());
    steps.clear();
    for (int row = 0; row < stepTable->rowCount(); ++row) {
        const QTableWidgetItem* item = stepTable->item(row, 0);
        if (!item || item->text().trimmed().isEmpty()) continue;
        steps.append({item->checkState() == Qt::Checked, item->text().trimmed()});
    }
    settings.remove(QStringLiteral("editor/runMapSteps"));
    settings.beginWriteArray(QStringLiteral("editor/runMapSteps"));
    for (int index = 0; index < steps.size(); ++index) {
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("enabled"), steps[index].enabled);
        settings.setValue(QStringLiteral("command"), steps[index].command);
    }
    settings.endArray();

    // Every checked step must resolve its $NAME references before anything
    // runs; a typo'd or unconfigured variable aborts up front.
    QStringList activeSteps;
    for (const Step& step : steps) {
        if (step.enabled) activeSteps << step.command;
    }
    if (activeSteps.isEmpty()) return;
    QStringList unresolved;
    static const QRegularExpression variablePattern(
        QStringLiteral("\\$([A-Za-z_][A-Za-z0-9_]*)"));
    static const QStringList pathPlaceholders{
        QStringLiteral("file"), QStringLiteral("bsp"),
        QStringLiteral("mapname"), QStringLiteral("gamedir")};
    for (const QString& stepCommand : activeSteps) {
        auto matches = variablePattern.globalMatch(stepCommand);
        while (matches.hasNext()) {
            const QString name = matches.next().captured(1);
            if (pathPlaceholders.contains(name)) continue;
            if (!programs.contains(name) && !unresolved.contains(name)) unresolved << name;
        }
    }
    if (!unresolved.isEmpty()) {
        QMessageBox::warning(this, tr("Run Map"),
            tr("These variables are not defined in Tools > Options > Build Programs: %1")
                .arg(unresolved.join(QStringLiteral(", "))));
        return;
    }

    // The compilers read the VMF from disk, so the document must be saved.
    if (document->filePath().isEmpty()) {
        saveDocumentAs();
        if (document->filePath().isEmpty()) return;
    } else if (document->isModified()) {
        QString error;
        if (!document->save(&error)) {
            QMessageBox::critical(this, tr("Run Map"), error);
            return;
        }
    }

    const QString vmfPath = QFileInfo(document->filePath()).absoluteFilePath();
    QString bspPath = vmfPath;
    if (bspPath.endsWith(QStringLiteral(".vmf"), Qt::CaseInsensitive)) bspPath.chop(4);
    bspPath += QStringLiteral(".bsp");
    const QString gameDir = loadedGameInfoPath_.isEmpty()
        ? QString() : QFileInfo(loadedGameInfoPath_).absolutePath();

    // Values are single-quoted for bash so paths with spaces survive; the
    // template itself stays verbatim to keep &&, redirects, and flags working.
    const auto shellQuote = [](QString value) {
        value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QStringLiteral("'") + value + QStringLiteral("'");
    };
    const auto substitute = [&](QString text) {
        text.replace(QStringLiteral("$mapname"),
                     shellQuote(QFileInfo(vmfPath).completeBaseName()));
        text.replace(QStringLiteral("$gamedir"), shellQuote(gameDir));
        text.replace(QStringLiteral("$file"), shellQuote(vmfPath));
        text.replace(QStringLiteral("$bsp"), shellQuote(bspPath));
        return text;
    };
    // $NAME program variables expand verbatim (their commands may carry env
    // vars, wine prefixes, and their own quoting); path placeholders expand
    // quoted. Longer names first so $VBSPFAST is never eaten by $VBSP.
    QStringList programNames = programs.keys();
    std::sort(programNames.begin(), programNames.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });
    QStringList stepCommands;
    for (const QString& stepCommand : activeSteps) {
        QString expanded = stepCommand;
        for (const QString& name : programNames) {
            expanded.replace(QStringLiteral("$") + name, programs.value(name));
        }
        stepCommands << substitute(expanded);
    }
    // && chains the steps so a failed vbsp stops vvis/vrad and the game
    // never launches on a broken compile.
    const QString command = stepCommands.join(QStringLiteral(" && "));

    // Modeless log window: vvis/vrad can run for minutes, so the editor must
    // stay responsive and the process streams instead of blocking.
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Compile — %1").arg(QFileInfo(vmfPath).fileName()));
    dialog->resize(720, 480);
    auto* layout = new QVBoxLayout(dialog);
    auto* outputBox = new QGroupBox(tr("Compile Output"), dialog);
    auto* outputLayout = new QVBoxLayout(outputBox);
    auto* output = new QPlainTextEdit;
    output->setReadOnly(true);
    output->setMaximumBlockCount(50000);
    QFont mono = output->font();
    mono.setFamilies({QStringLiteral("monospace")});
    mono.setStyleHint(QFont::TypeWriter);
    output->setFont(mono);
    outputLayout->addWidget(output);
    layout->addWidget(outputBox, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    auto* process = new QProcess(dialog);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QFileInfo(vmfPath).absolutePath());
    connect(process, &QProcess::readyReadStandardOutput, output, [process, output] {
        output->appendPlainText(QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed());
    });
    connect(process, &QProcess::errorOccurred, output, [process, output](QProcess::ProcessError) {
        output->appendPlainText(QStringLiteral("*** %1").arg(process->errorString()));
    });
    connect(process, &QProcess::finished, output,
            [output](int exitCode, QProcess::ExitStatus status) {
        output->appendPlainText(status == QProcess::NormalExit
            ? tr("*** Compile finished with exit code %1").arg(exitCode)
            : tr("*** Compile crashed"));
    });
    connect(dialog, &QObject::destroyed, process, [process] {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(3000);
        }
    });

    output->appendPlainText(QStringLiteral("$ %1\n").arg(command));
    process->start(QStringLiteral("bash"), {QStringLiteral("-c"), command});
    dialog->show();
    appendMessage(tr("Compiling %1").arg(vmfPath));
}

void MainWindow::saveDocument()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) {
        return;
    }
    if (document->filePath().isEmpty()) {
        saveDocumentAs();
        return;
    }

    QString error;
    if (!document->save(&error)) {
        QMessageBox::critical(this, tr("Save VMF"), error);
        return;
    }
    setPrompt(tr("Saved %1").arg(document->displayName()));
}

void MainWindow::saveDocumentAs()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) {
        return;
    }

    QString suggested = document->filePath();
    if (suggested.isEmpty()) {
        suggested = QStringLiteral("untitled.vmf");
    }
    QString path;
    {
        // The preview must not render behind the native chooser; see openDocument().
        const hammer::app::PreviewRenderSuspension suspendPreview;
        path = QFileDialog::getSaveFileName(
            this, tr("Save As"), suggested, tr("Valve Map Files (*.vmf);;All Files (*.*)"));
    }
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".vmf");
    }

    QString error;
    if (!document->saveAs(path, &error)) {
        QMessageBox::critical(this, tr("Save VMF"), error);
        return;
    }
    setPrompt(tr("Saved %1").arg(document->displayName()));
}

void MainWindow::appendMessage(const QString& message)
{
    if (messageOutput_) {
        messageOutput_->appendPlainText(message);
    }
}

CollabSession* MainWindow::ensureCollabSession()
{
    if (collabSession_) return collabSession_;
    collabSession_ = new CollabSession(this);
    connect(collabSession_, &CollabSession::statusMessage, this, [this](const QString& message) {
        statusBar()->showMessage(message, 6000);
    });
    connect(collabSession_, &CollabSession::remoteDelta, this,
            [this](const hammer::vmf::SyncDelta& delta) {
        if (!collabDocument_) return;
        collabApplyingRemote_ = true;
        collabDocument_->applyRemoteDelta(delta);
        collabApplyingRemote_ = false;
    });
    connect(collabSession_, &CollabSession::bootstrapDocument, this,
            [this](const hammer::vmf::Document& document, int idBase, int idSpan) {
        MapDocumentWidget* target = createDocument();
        if (!target) {
            leaveCollabSession(tr("Could not create a window for the shared map"));
            return;
        }
        target->adoptCollabDocument(document);
        target->setCollabIdRange(idBase, idSpan);
        bindCollabDocument(target);
    });
    connect(collabSession_, &CollabSession::resyncDocument, this,
            [this](const hammer::vmf::Document& document) {
        if (!collabDocument_) return;
        collabApplyingRemote_ = true;
        collabDocument_->adoptCollabDocument(document);
        collabApplyingRemote_ = false;
    });
    // Queued: the offer arrives inside the session's network pump; a modal
    // dialog must not run there.
    connect(collabSession_, &CollabSession::assetsOffered, this,
            [this](int fileCount, qint64 totalBytes) {
        const QString size = QLocale().formattedDataSize(totalBytes);
        const auto choice = QMessageBox::question(
            this, tr("Download Custom Assets"),
            tr("The session host shares %n custom asset file(s) (%1) this map uses.\n\n"
               "Download them into your custom content folder?\n"
               "(They land in their own subfolder and never overwrite your files.)",
               "", fileCount)
                .arg(size),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice == QMessageBox::Yes) collabSession_->acceptAssetOffer();
        else collabSession_->declineAssetOffer();
    }, Qt::QueuedConnection);
    connect(collabSession_, &CollabSession::assetsDownloaded, this,
            [this](int fileCount, const QString& directory) {
        appendMessage(tr("Downloaded %1 custom assets from the session host").arg(fileCount));
        mountCollabDownloads(directory);
    }, Qt::QueuedConnection);
    connect(collabSession_, &CollabSession::peerPosesChanged, this,
            [this](const QList<CollabPeerPose>& poses) {
        if (collabDocument_) collabDocument_->setCollabPeerPoses(poses);
    });
    connect(collabSession_, &CollabSession::peerListChanged, this,
            [this] { updateCollabActions(); });
    connect(collabSession_, &CollabSession::chatMessageReceived, this,
            [this](const QString& from, const QString& text) {
        ensureCollabChatDock();
        // Never force the dock open mid-edit: the log accumulates either
        // way, and a hidden chat announces itself in the status bar instead.
        if (!collabChatDock_->isVisible())
            statusBar()->showMessage(tr("Chat — %1: %2").arg(from, text), 5000);
        collabChatLog_->appendPlainText(QStringLiteral("[%1] %2: %3")
                                            .arg(QTime::currentTime().toString(
                                                     QStringLiteral("hh:mm")),
                                                 from, text));
    });
    connect(collabSession_, &CollabSession::sessionEnded, this, [this](const QString& reason) {
        leaveCollabSession(reason);
    });
    // Publishes this editor's camera at the presence rate; the session
    // rate-limits and skips unchanged poses itself.
    collabPoseTimer_ = new QTimer(this);
    collabPoseTimer_->setInterval(250);
    connect(collabPoseTimer_, &QTimer::timeout, this, [this] {
        if (!collabSession_ || !collabSession_->active() || !collabDocument_) return;
        MapViewWidget* perspective = collabDocument_->perspectiveView();
        if (!perspective) return;
        const hammer::camera::State& camera = perspective->cameraState();
        constexpr double kRadToDeg = 57.29577951308232;
        collabSession_->updateLocalPose(camera.position.x, camera.position.y, camera.position.z,
                                        camera.pitchRadians * kRadToDeg,
                                        camera.yawRadians * kRadToDeg);
    });
    return collabSession_;
}

QString MainWindow::promptCollabName()
{
    QSettings settings;
    const QString stored = settings.value(QStringLiteral("collab/name")).toString();
    const QString fallback = stored.isEmpty()
        ? qEnvironmentVariable("USER", tr("Mapper"))
        : stored;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Display Name"),
                                               tr("Name shown to other editors:"),
                                               QLineEdit::Normal, fallback, &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty()) return {};
    settings.setValue(QStringLiteral("collab/name"), name);
    return name;
}

void MainWindow::ensureCollabChatDock()
{
    if (collabChatDock_) return;
    collabChatDock_ = new QDockWidget(tr("Session Chat"), this);
    collabChatDock_->setObjectName(QStringLiteral("CollabChat"));
    collabChatDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    auto* body = new QWidget(collabChatDock_);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    collabChatLog_ = new QPlainTextEdit(body);
    collabChatLog_->setReadOnly(true);
    collabChatLog_->setMaximumBlockCount(2000);
    layout->addWidget(collabChatLog_, 1);
    collabChatInput_ = new QLineEdit(body);
    collabChatInput_->setPlaceholderText(tr("Message the session — Enter to send"));
    collabChatInput_->setMaxLength(500);
    layout->addWidget(collabChatInput_);
    collabChatDock_->setWidget(body);
    addDockWidget(Qt::BottomDockWidgetArea, collabChatDock_);
    connect(collabChatInput_, &QLineEdit::returnPressed, this, [this] {
        if (!collabSession_ || !collabSession_->active()) return;
        collabSession_->sendChat(collabChatInput_->text());
        collabChatInput_->clear();
    });
    connect(collabChatDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (collabChatAction_) collabChatAction_->setChecked(visible);
    });
}

void MainWindow::kickCollaborator()
{
    if (!collabSession_ || collabSession_->role() != CollabSession::Role::Host) return;
    const auto peers = collabSession_->connectedPeers();
    if (peers.empty()) {
        statusBar()->showMessage(tr("No collaborators are connected"), 4000);
        return;
    }
    QStringList labels;
    for (const auto& [name, id] : peers) labels << name;
    bool accepted = false;
    const QString picked = QInputDialog::getItem(this, tr("Kick Collaborator"),
                                                 tr("Remove from the session:"), labels, 0,
                                                 false, &accepted);
    if (!accepted) return;
    const int index = labels.indexOf(picked);
    if (index >= 0) collabSession_->kickPeer(peers[std::size_t(index)].second);
}

void MainWindow::hostCollabSession()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) {
        statusBar()->showMessage(tr("Open the map you want to share first"), 4000);
        return;
    }
    const QString name = promptCollabName();
    if (name.isEmpty()) return;
    bool accepted = false;
    const int port = QInputDialog::getInt(this, tr("Host Session"),
                                          tr("Listen on UDP port (collaborators connect here):"),
                                          CollabSession::kDefaultPort, 1024, 65535, 1, &accepted);
    if (!accepted) return;
    QString error;
    CollabSession* session = ensureCollabSession();
    session->setLocalName(name);
    if (!session->hostSession(quint16(port), document->editorModel().document(), &error)) {
        QMessageBox::warning(this, tr("Host Session"), tr("Could not host session: %1").arg(error));
        return;
    }
    session->setSharedFileSystem(gameFileSystem_);
    // The host takes the first id window; joiners get the following ones.
    document->setCollabIdRange(CollabSession::kIdSpan, CollabSession::kIdSpan);
    bindCollabDocument(document);
}

void MainWindow::joinCollabSession()
{
    const QString name = promptCollabName();
    if (name.isEmpty()) return;
    bool accepted = false;
    const QString target = QInputDialog::getText(
        this, tr("Join Session"), tr("Host address (name/ip, optionally :port):"), QLineEdit::Normal,
        QStringLiteral("localhost"), &accepted);
    if (!accepted || target.trimmed().isEmpty()) return;
    QString address = target.trimmed();
    quint16 port = CollabSession::kDefaultPort;
    const qsizetype colon = address.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool portOk = false;
        const int parsed = address.mid(colon + 1).toInt(&portOk);
        if (portOk && parsed > 0 && parsed <= 65535) {
            port = quint16(parsed);
            address.truncate(colon);
        }
    }
    QString error;
    CollabSession* session = ensureCollabSession();
    session->setLocalName(name);
    if (!session->joinSession(address, port, &error)) {
        QMessageBox::warning(this, tr("Join Session"), tr("Could not join session: %1").arg(error));
        return;
    }
    session->setSharedFileSystem(gameFileSystem_);
    if (gameFileSystem_ && !gameFileSystem_->gameDirectory().empty()) {
        session->setAssetDownloadDirectory(QString::fromStdString(
            (gameFileSystem_->gameDirectory() / "custom" / "collab_downloads").string()));
    }
    updateCollabActions();
    // The document arrives asynchronously via bootstrapDocument.
}

void MainWindow::leaveCollabSession(const QString& reason)
{
    if (collabSession_) collabSession_->leave();
    disconnect(collabEditConnection_);
    disconnect(collabDestroyedConnection_);
    if (collabPoseTimer_) collabPoseTimer_->stop();
    if (collabChatDock_) {
        if (collabChatLog_) collabChatLog_->appendPlainText(tr("— session ended —"));
        collabChatDock_->setVisible(false);
    }
    if (collabDocument_) {
        collabDocument_->setCollabIdRange(0, 0);
        collabDocument_->setCollabPeerPoses({});
    }
    collabDocument_ = nullptr;
    updateCollabActions();
    statusBar()->showMessage(reason.isEmpty() ? tr("Left the collaborative session") : reason, 6000);
}

void MainWindow::bindCollabDocument(MapDocumentWidget* document)
{
    collabDocument_ = document;
    // modifiedChanged only fires from notifyDocumentState — the choke point
    // every committed edit (undo/redo included) passes through — while
    // editStateChanged also fires on plain selection changes, which would run
    // a whole-document diff per click.
    collabEditConnection_ = connect(document, &MapDocumentWidget::modifiedChanged, this, [this] {
        if (collabApplyingRemote_ || !collabSession_ || !collabSession_->active()) return;
        if (!collabDocument_) return;
        collabSession_->localDocumentChanged(collabDocument_->editorModel().document());
    });
    collabDestroyedConnection_ = connect(document, &QObject::destroyed, this, [this] {
        leaveCollabSession(tr("The shared map was closed; session ended"));
    });
    if (collabPoseTimer_) collabPoseTimer_->start();
    ensureCollabChatDock();
    collabChatDock_->setVisible(true);
    collabChatLog_->appendPlainText(tr("— session started —"));
    updateCollabActions();
}

void MainWindow::mountCollabDownloads(const QString& directory)
{
    // Same recipe as the extracted-BSP content mount: force the folder ahead
    // of every search path, then rebuild the material world on top of it.
    if (!gameFileSystem_) return;
    std::error_code canonicalError;
    auto mountedPath = std::filesystem::weakly_canonical(
        std::filesystem::path(directory.toStdString()), canonicalError);
    if (canonicalError) {
        mountedPath = std::filesystem::path(directory.toStdString()).lexically_normal();
    }
    const bool alreadyMounted = std::any_of(
        gameFileSystem_->locations().begin(), gameFileSystem_->locations().end(),
        [&](const hammer::assets::SearchLocation& location) {
            return location.kind == hammer::assets::SearchLocation::Kind::Directory &&
                   location.path == mountedPath;
        });
    // An already-mounted folder still falls through: the files changed
    // underneath it, so the material caches need flushing regardless.
    if (!alreadyMounted && !gameFileSystem_->mountOverrideDirectory(mountedPath)) return;
    appendMessage(tr("Mounted downloaded session assets: %1").arg(directory));
    materials_ = std::make_shared<hammer::assets::MaterialSystem>(gameFileSystem_);
    refreshMaterialList();
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setMaterialSystem(materials_);
        }
    }
}

void MainWindow::updateCollabActions()
{
    const bool active = collabSession_ && collabSession_->active();
    if (collabHostAction_) collabHostAction_->setEnabled(!active);
    if (collabJoinAction_) collabJoinAction_->setEnabled(!active);
    if (collabLeaveAction_) collabLeaveAction_->setEnabled(active);
    if (collabChatAction_) collabChatAction_->setEnabled(active || collabChatDock_);
    if (collabKickAction_) {
        collabKickAction_->setEnabled(active &&
                                      collabSession_->role() == CollabSession::Role::Host &&
                                      !collabSession_->connectedPeers().empty());
    }
}

MapDocumentWidget* MainWindow::activeDocument() const
{
    QMdiSubWindow* sub = mdiArea_->activeSubWindow();
    return sub ? qobject_cast<MapDocumentWidget*>(sub->widget()) : nullptr;
}

void MainWindow::setPrompt(const QString& text)
{
    if (promptPane_) {
        promptPane_->setText(text);
    }
}

// ID_VIEW_SHOWDETAILOBJECTS. Source's detail props - the grass VBSP scatters
// over "%detailtype" surfaces - are drawn by both 3D backends; a grassy map
// places tens of thousands of sprites, so this is a performance switch too.
void MainWindow::setDetailPropsVisible(bool visible)
{
    detailPropsVisible_ = visible;
    if (detailObjectsAction_ && detailObjectsAction_->isChecked() != visible) {
        detailObjectsAction_->setChecked(visible);
    }
    for (QMdiSubWindow* sub : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(sub->widget())) {
            document->setDetailPropsVisible(visible);
        }
    }
    setPrompt(visible ? tr("Detail objects on") : tr("Detail objects off"));
}

// ID_VIEW_3DLIGHTMAP_GRID. The original's draw type is per 3D view; the port
// keeps it on the frame so the menu item and the sheet's checkbox agree, and
// pushes it onto every open document.
void MainWindow::setLightmapGridVisible(bool visible)
{
    lightmapGridVisible_ = visible;
    if (lightmapGridAction_ && lightmapGridAction_->isChecked() != visible) {
        lightmapGridAction_->setChecked(visible);
    }
    if (faceEditSheet_) faceEditSheet_->setLightmapGridVisible(visible);
    for (QMdiSubWindow* sub : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(sub->widget())) {
            document->setLightmapGridVisible(visible);
        }
    }
    setPrompt(visible ? tr("3D Lightmap Grid on") : tr("3D Lightmap Grid off"));
}

// Hammer creates one CFaceEditSheet per main frame (CMainFrame::m_pFaceEditSheet)
// and shows/hides it with the Texture Application tool.
void MainWindow::ensureFaceEditSheet()
{
    if (faceEditSheet_) return;
    faceEditSheet_ = new FaceEditSheet(this);
    faceEditSheet_->setMaterialSystem(materials_);
    faceEditSheet_->setMaterialNames(mountedMaterialNames_);
    if (textureCombo_) faceEditSheet_->setCurrentMaterial(textureCombo_->currentText());

    const auto withDocument = [this](auto&& action) {
        if (MapDocumentWidget* document = activeDocument()) action(document);
    };

    connect(faceEditSheet_, &FaceEditSheet::applyRequested, this, [this, withDocument] {
        withDocument([this](MapDocumentWidget* document) {
            document->setTreatFacesAsOne(faceEditSheet_->treatAsOne());
            document->applyFaceEdit(faceEditSheet_->currentEdit(true));
        });
    });
    connect(faceEditSheet_, &FaceEditSheet::mappingEdited, this, [this, withDocument] {
        withDocument([this](MapDocumentWidget* document) {
            // The mapping fields alone; the material is only applied by Apply,
            // by a right-click, or by an Apply click mode.
            document->applyFaceEdit(faceEditSheet_->currentEdit(false));
        });
    });
    connect(faceEditSheet_, &FaceEditSheet::justifyRequested, this,
            [this, withDocument](int justification) {
                withDocument([this, justification](MapDocumentWidget* document) {
                    document->setTreatFacesAsOne(faceEditSheet_->treatAsOne());
                    document->justifyFaceSelection(justification);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::alignRequested, this,
            [withDocument](int alignment) {
                withDocument([alignment](MapDocumentWidget* document) {
                    document->alignFaceSelection(alignment);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::hideMaskChanged, this,
            [withDocument](bool hidden) {
                withDocument([hidden](MapDocumentWidget* document) {
                    document->setFaceSelectionMaskHidden(hidden);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::materialChanged, this,
            [this, withDocument](const QString& material) {
                if (textureCombo_ && !material.trimmed().isEmpty()) {
                    textureCombo_->setCurrentText(material);
                }
                withDocument([&material](MapDocumentWidget* document) {
                    document->setCurrentMaterial(material);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::clickModeChanged, this, [withDocument](int mode) {
        withDocument([mode](MapDocumentWidget* document) {
            document->setFaceClickMode(static_cast<FaceEditSheet::ClickMode>(mode));
        });
    });
    // Smoothing Groups page.
    connect(faceEditSheet_, &FaceEditSheet::smoothingGroupToggled, this,
            [withDocument](int group, bool add) {
                withDocument([group, add](MapDocumentWidget* document) {
                    document->toggleFaceSmoothingGroup(group, add);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::smoothingVisualGroupChanged, this,
            [withDocument](int group) {
                withDocument([group](MapDocumentWidget* document) {
                    document->setShownSmoothingGroup(group);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::smoothingGroupSelectRequested, this,
            [withDocument](int group) {
                withDocument([group](MapDocumentWidget* document) {
                    document->selectFacesInSmoothingGroup(group);
                });
            });
    // Displacement page (hammer/faceedit_disppage.cpp).
    connect(faceEditSheet_, &FaceEditSheet::displacementCreateRequested, this,
            [withDocument](int power) {
                withDocument([power](MapDocumentWidget* document) {
                    document->createFaceDisplacements(power);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::displacementDestroyRequested, this, [withDocument] {
        withDocument([](MapDocumentWidget* document) { document->destroyFaceDisplacements(); });
    });
    connect(faceEditSheet_, &FaceEditSheet::displacementToolChanged, this,
            [this, withDocument](int tool) {
                const auto settings = faceEditSheet_->displacementPaintSettings();
                withDocument([tool, settings](MapDocumentWidget* document) {
                    document->setDisplacementPaintSettings(settings);
                    document->setDisplacementTool(static_cast<DisplacementTool>(tool));
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::displacementPaintSettingsChanged, this,
            [this, withDocument] {
                const auto settings = faceEditSheet_->displacementPaintSettings();
                withDocument([settings](MapDocumentWidget* document) {
                    document->setDisplacementPaintSettings(settings);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::displacementAttributesApplied, this,
            [withDocument](std::optional<int> power, std::optional<double> elevation,
                           std::optional<double> scale, double previousScale) {
                hammer::vmf::EditorModel::DisplacementAttributeEdit edit;
                edit.power = power;
                edit.elevation = elevation;
                edit.scale = scale;
                edit.previousScale = previousScale;
                withDocument([edit](MapDocumentWidget* document) {
                    document->applyDisplacementAttributes(edit);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::displacementNoiseRequested, this,
            [withDocument](double minimum, double maximum) {
                withDocument([minimum, maximum](MapDocumentWidget* document) {
                    document->applyDisplacementNoise(minimum, maximum);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::displacementSewRequested, this, [withDocument] {
        withDocument([](MapDocumentWidget* document) { document->sewFaceDisplacements(); });
    });
    // Lightmap page.
    connect(faceEditSheet_, &FaceEditSheet::lightmapScaleApplied, this,
            [withDocument](int scale) {
                withDocument([scale](MapDocumentWidget* document) {
                    document->applyLightmapScale(scale);
                });
            });
    connect(faceEditSheet_, &FaceEditSheet::lightmapGridToggled, this, [this](bool visible) {
        setLightmapGridVisible(visible);
    });
    connect(faceEditSheet_, &FaceEditSheet::browseRequested, this, &MainWindow::showMaterialBrowser);
    connect(faceEditSheet_, &FaceEditSheet::replaceRequested, this,
            &MainWindow::showReplaceTexturesDialog);
    connect(faceEditSheet_, &FaceEditSheet::sheetClosed, this, [this, withDocument] {
        // The group tint belongs to the sheet; it goes away with it.
        withDocument([](MapDocumentWidget* document) {
            document->setShownSmoothingGroup(0);
            // CFaceEditDispPage::CloseAllDialogs: the paint tools go with the sheet.
            document->setDisplacementTool(DisplacementTool::Select);
        });
        // CFaceEditSheet::OnClose deactivates the tool back to the pointer.
        QAction* pointer = commands_.value(QStringLiteral("tool.pointer"));
        if (!pointer || currentToolId_ == QStringLiteral("tool.pointer")) return;
        pointer->setChecked(true);
        selectTool(pointer);
    });
}

void MainWindow::setFaceEditSheetVisible(bool visible)
{
    if (!visible) {
        if (faceEditSheet_) faceEditSheet_->hide();
        if (MapDocumentWidget* document = activeDocument()) document->setShownSmoothingGroup(0);
        return;
    }
    ensureFaceEditSheet();
    faceEditSheet_->setMaterialNames(mountedMaterialNames_);
    if (MapDocumentWidget* document = activeDocument()) {
        document->setFaceClickMode(faceEditSheet_->clickMode());
        document->setTreatFacesAsOne(faceEditSheet_->treatAsOne());
        document->setFaceSelectionMaskHidden(faceEditSheet_->hideMask());
        faceEditSheet_->setFaceValues(document->faceEditValues());
        std::optional<int> dispPower;
        std::optional<double> dispElevation;
        document->displacementAttributeValues(dispPower, dispElevation);
        faceEditSheet_->setDisplacementAttributes(dispPower, dispElevation);
    }
    faceEditSheet_->setLightmapGridVisible(lightmapGridVisible_);
    faceEditSheet_->show();
    faceEditSheet_->raise();
}

// ID_TOOLS_APPLYTEXTURE (Shift+T). A momentary command: the active tool stays
// selected and the texture is applied in one undo step.
void MainWindow::applyCurrentTextureCommand()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;
    if (textureCombo_) document->setCurrentMaterial(textureCombo_->currentText());
    document->applyCurrentTexture();
    updateEditActions();
}

void MainWindow::selectTool(QAction* action)
{
    if (!action) return;
    // Clipper3D::OnActivate only iterates the clip mode when the clipper is
    // ALREADY the active tool; re-selecting it must not reset the clip line.
    if (action->objectName() == QStringLiteral("tool.clipper") &&
        currentToolId_ == QStringLiteral("tool.clipper")) {
        if (MapDocumentWidget* document = activeDocument()) {
            setPrompt(tr("Clipping tool: %1").arg(document->cycleClipMode()));
            return;
        }
    }
    // Morph3D::OnActivate's IsActiveTool branch calls ToggleMode instead of
    // rebuilding the morph: Shift+V cycles the handle display mode.
    if (action->objectName() == QStringLiteral("tool.morph") &&
        currentToolId_ == QStringLiteral("tool.morph")) {
        if (MapDocumentWidget* document = activeDocument(); document && document->morphActive()) {
            setPrompt(tr("Vertex manipulation: %1").arg(document->cycleMorphHandleMode()));
            return;
        }
    }
    currentToolId_ = action->objectName();
    if (MapDocumentWidget* document = activeDocument()) {
        if (currentToolId_ == QStringLiteral("tool.block")) document->setTool(MapViewWidget::Tool::Block);
        else if (currentToolId_ == QStringLiteral("tool.entity")) document->setTool(MapViewWidget::Tool::Entity);
        else if (currentToolId_ == QStringLiteral("tool.decals")) document->setTool(MapViewWidget::Tool::Decal);
        else if (currentToolId_ == QStringLiteral("tool.overlay")) document->setTool(MapViewWidget::Tool::Overlay);
        else if (currentToolId_ == QStringLiteral("tool.magnify")) document->setTool(MapViewWidget::Tool::Magnify);
        else if (currentToolId_ == QStringLiteral("tool.camera")) document->setTool(MapViewWidget::Tool::Camera);
    else if (currentToolId_ == QStringLiteral("tool.clipper")) document->setTool(MapViewWidget::Tool::Clipper);
    else if (currentToolId_ == QStringLiteral("tool.morph")) document->setTool(MapViewWidget::Tool::Morph);
        else if (currentToolId_ == QStringLiteral("tool.textureApplication")) document->setTool(MapViewWidget::Tool::TextureApplication);
        else document->setTool(MapViewWidget::Tool::Selection);
        applyObjectBarSettings(document);
    }
    // CToolMaterial's sheet is shown while the tool is active and hidden the
    // moment another tool takes over (CFaceEditSheet::SetVisibility).
    setFaceEditSheetVisible(currentToolId_ == QStringLiteral("tool.textureApplication"));
    setPrompt(action->text());
}

void MainWindow::updateEditActions()
{
    MapDocumentWidget* document = activeDocument();
    const bool hasDocument = document != nullptr;
    // Switching maps, and any edit that could rebuild the scene, can change
    // which tool textures are hidden - the nodraw button mirrors that state.
    updateNodrawActionState();
    QAction* undo = commands_.value(QStringLiteral("edit.undo"));
    QAction* redo = commands_.value(QStringLiteral("edit.redo"));
    QAction* cut = commands_.value(QStringLiteral("edit.cut"));
    QAction* copy = commands_.value(QStringLiteral("edit.copy"));
    QAction* paste = commands_.value(QStringLiteral("edit.paste"));
    QAction* pasteSpecial = commands_.value(QStringLiteral("edit.pasteSpecial"));
    QAction* findEntities = commands_.value(QStringLiteral("edit.findEntities"));
    QAction* duplicate = commands_.value(QStringLiteral("edit.duplicate"));
    QAction* deleteObjects = commands_.value(QStringLiteral("edit.delete"));
    QAction* clearSelection = commands_.value(QStringLiteral("edit.clearSelection"));
    QAction* selectAll = commands_.value(QStringLiteral("edit.selectAll"));
    QAction* properties = commands_.value(QStringLiteral("edit.properties"));
    if (undo) {
        undo->setEnabled(hasDocument && document->canUndo());
        undo->setText(hasDocument ? document->undoText() : tr("&Undo"));
    }
    if (redo) {
        redo->setEnabled(hasDocument && document->canRedo());
        redo->setText(hasDocument ? document->redoText() : tr("&Redo"));
    }
    const bool hasSelection = hasDocument && document->selectionCount() > 0;
    if (cut) cut->setEnabled(hasSelection);
    if (copy) copy->setEnabled(hasSelection);
    if (duplicate) duplicate->setEnabled(hasSelection);
    if (paste) paste->setEnabled(hasDocument && !clipboard_.empty());
    if (pasteSpecial) pasteSpecial->setEnabled(hasDocument && !clipboard_.empty());
    if (findEntities) findEntities->setEnabled(hasDocument);
    if (deleteObjects) deleteObjects->setEnabled(hasSelection);
    if (clearSelection) clearSelection->setEnabled(hasSelection);
    if (properties) properties->setEnabled(hasDocument && document->selectionCount() == 1);
    if (selectAll) selectAll->setEnabled(hasDocument);
    // Pointfile commands: Load needs a document, Unload needs a loaded trace,
    // so the menu shows at a glance whether one is up.
    if (QAction* loadPointfile = commands_.value(QStringLiteral("map.loadPointfile")))
        loadPointfile->setEnabled(hasDocument);
    if (QAction* unloadPointfile = commands_.value(QStringLiteral("map.unloadPointfile")))
        unloadPointfile->setEnabled(hasDocument && document->hasPointFile());
    if (QAction* loadPortal = commands_.value(QStringLiteral("map.loadPortal")))
        loadPortal->setEnabled(hasDocument);
    if (QAction* unloadPortal = commands_.value(QStringLiteral("map.unloadPortal")))
        unloadPortal->setEnabled(hasDocument && document->hasPortalFile());
}

void MainWindow::updateProjectionActions()
{
    const MapDocumentWidget* document = activeDocument();
    const bool hasDocument = document != nullptr;
    const bool perspective = !document ||
        document->cameraProjection() == MapViewWidget::ProjectionMode::Perspective;

    if (perspectiveProjectionAction_) {
        QSignalBlocker blocker(perspectiveProjectionAction_);
        perspectiveProjectionAction_->setEnabled(hasDocument);
        perspectiveProjectionAction_->setChecked(perspective);
    }
    if (orthographicProjectionAction_) {
        QSignalBlocker blocker(orthographicProjectionAction_);
        orthographicProjectionAction_->setEnabled(hasDocument);
        orthographicProjectionAction_->setChecked(!perspective);
    }
}

void MainWindow::updateNodrawActionState()
{
    if (!nodrawAction_) return;
    MapDocumentWidget* document = activeDocument();
    QSignalBlocker blocker(nodrawAction_);
    nodrawAction_->setEnabled(document != nullptr);
    // The document owns the state; the button only mirrors it, so changing
    // nodraw from the Tool Textures menu moves the button and vice versa.
    nodrawAction_->setChecked(!document ||
                              document->toolTextureVisible(QStringLiteral("tools/toolsnodraw")));
}

void MainWindow::rebuildToolTexturesMenu()
{
    if (!toolTexturesMenu_) return;
    toolTexturesMenu_->clear();

    MapDocumentWidget* document = activeDocument();
    if (!document) {
        QAction* unavailable = toolTexturesMenu_->addAction(tr("No map is open"));
        unavailable->setEnabled(false);
        return;
    }

    const QStringList materials = document->toolTextureMaterials();
    if (materials.isEmpty()) {
        QAction* unavailable = toolTexturesMenu_->addAction(
            tr("No tools/ textures are used in this level"));
        unavailable->setEnabled(false);
        return;
    }

    const bool allVisible = std::all_of(materials.cbegin(), materials.cend(),
        [document](const QString& material) {
            return document->toolTextureVisible(material);
        });

    QAction* showAll = toolTexturesMenu_->addAction(tr("Show All Tool Textures"));
    showAll->setObjectName(QStringLiteral("showAllToolTexturesAction"));
    showAll->setCheckable(true);
    showAll->setChecked(allVisible);
    showAll->setProperty("implemented", true);
    const QPointer<MapDocumentWidget> guardedDocument(document);
    connect(showAll, &QAction::toggled, this,
            [this, guardedDocument](bool visible) {
        if (!guardedDocument) return;
        guardedDocument->setAllToolTexturesVisible(visible);
        setPrompt(visible ? tr("All tool textures shown")
                          : tr("All tool textures hidden"));
    });

    toolTexturesMenu_->addSeparator();
    for (const QString& material : materials) {
        QAction* action = toolTexturesMenu_->addAction(material);
        action->setCheckable(true);
        action->setChecked(document->toolTextureVisible(material));
        action->setData(material);
        action->setProperty("implemented", true);
        connect(action, &QAction::toggled, this,
                [this, guardedDocument, material](bool visible) {
            if (!guardedDocument) return;
            guardedDocument->setToolTextureVisible(material, visible);
            setPrompt(visible ? tr("Showing %1").arg(material)
                              : tr("Hiding %1").arg(material));
        });
    }
}

void MainWindow::loadFgd()
{
    const QString startPath = loadedFgdPath_.isEmpty() ? QString{} : QFileInfo(loadedFgdPath_).absolutePath();
    // Hold the preview off the GUI thread while the native chooser is up.
    const hammer::app::PreviewRenderSuspension suspendPreview;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load Game Data"), startPath, tr("Forge Game Data (*.fgd);;All Files (*.*)"));
    if (!path.isEmpty()) loadFgdPath(path, true);
}

bool MainWindow::loadFgdPath(const QString& path, bool reportErrors)
{
    auto replacement = std::make_shared<hammer::fgd::Database>();
    hammer::fgd::ParseError parseError;
    std::string ioError;
    if (!replacement->loadFile(std::filesystem::path(path.toUtf8().toStdString()), &parseError, &ioError)) {
        const QString message = !ioError.empty()
            ? QString::fromStdString(ioError)
            : tr("FGD parse error at line %1, column %2: %3")
                  .arg(static_cast<qulonglong>(parseError.line))
                  .arg(static_cast<qulonglong>(parseError.column))
                  .arg(QString::fromStdString(parseError.message));
        if (reportErrors) QMessageBox::critical(this, tr("Load Game Data"), message);
        else appendMessage(tr("Could not reload game data %1: %2").arg(path, message));
        return false;
    }

    fgd_ = std::move(replacement);
    loadedFgdPath_ = QFileInfo(path).absoluteFilePath();
    QSettings().setValue(QStringLiteral("gameData/lastFgd"), loadedFgdPath_);
    refreshEntityClasses();
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setFgdDatabase(fgd_);
            applyObjectBarSettings(document);
        }
    }
    const QString message = tr("Loaded %1 entity classes from %2")
                                .arg(static_cast<qulonglong>(fgd_->classes().size()))
                                .arg(loadedFgdPath_);
    appendMessage(message);
    setPrompt(message);
    return true;
}

void MainWindow::clearShaderCache()
{
    // Qt stores linked OpenGL program binaries under the generic cache location
    // in a directory named qtshadercache-<abi>-<version>. Match the family with
    // a glob rather than one exact name, so a Qt upgrade cannot leave a stale
    // cache behind that this command silently misses. Only directories with that
    // prefix are touched - never the cache root itself.
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    QDir base(root);
    const QStringList caches = base.entryList({QStringLiteral("qtshadercache*")},
                                              QDir::Dirs | QDir::NoDotAndDotDot);
    if (root.isEmpty() || caches.isEmpty()) {
        QMessageBox::information(this, tr("Clear Shader Cache"),
                                 tr("No cached shader binaries were found."));
        return;
    }

    int fileCount = 0;
    qint64 byteCount = 0;
    QStringList failed;
    for (const QString& cache : caches) {
        const QString path = base.absoluteFilePath(cache);
        QDirIterator iterator(path, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            iterator.next();
            ++fileCount;
            byteCount += iterator.fileInfo().size();
        }
        if (!QDir(path).removeRecursively()) failed.push_back(cache);
    }

    QString message = tr("Removed %n cached shader binaries (%1).", nullptr, fileCount)
                          .arg(QLocale().formattedDataSize(byteCount));
    // Programs already linked in this session keep running from memory; the
    // cache is only consulted when a viewport builds its program.
    message += QLatin1Char('\n');
    message += tr("Shaders are recompiled the next time a 3D view is created.");
    if (!failed.isEmpty()) {
        message += QLatin1Char('\n');
        message += tr("Could not remove: %1").arg(failed.join(QStringLiteral(", ")));
    }
    QMessageBox::information(this, tr("Clear Shader Cache"), message);
    setPrompt(tr("Cleared %1 of cached shader binaries")
                  .arg(QLocale().formattedDataSize(byteCount)));
}

void MainWindow::buildCubemaps()
{
    MapDocumentWidget* document = activeDocument();
    MapViewWidget* view = document ? document->perspectiveView() : nullptr;
    if (!view || !view->canBuildCubemaps()) {
        QMessageBox::warning(this, tr("Build Cubemaps"),
                             view ? tr("Building cubemaps needs a loaded map and the "
                                       "ray-traced renderer.")
                                  : tr("No open map."));
        return;
    }

    // A bake ray-traces six faces per probe, so it can take a noticeable moment
    // on a large map. It is synchronous, so say so with the cursor.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool built = view->buildCubemaps(error);
    QApplication::restoreOverrideCursor();

    if (!built) {
        QMessageBox::warning(this, tr("Build Cubemaps"), error);
        return;
    }
    setPrompt(tr("Built cubemaps — reflections now use the ray-traced bake"));
}

void MainWindow::configureGameDirectory()
{
    const QString startPath = loadedGameInfoPath_.isEmpty()
        ? QDir::homePath() : QFileInfo(loadedGameInfoPath_).absolutePath();
    // Hold the preview off the GUI thread while the native chooser is up.
    const hammer::app::PreviewRenderSuspension suspendPreview;
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Configure Game Directory"), startPath,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!directory.isEmpty()) loadGameInfoPath(directory, true);
}

bool MainWindow::loadGameInfoPath(const QString& path, bool reportErrors)
{
    QFileInfo inputInfo(path);
    const QString gameInfoPath = inputInfo.isDir()
        ? QDir(inputInfo.absoluteFilePath()).filePath(QStringLiteral("gameinfo.txt"))
        : inputInfo.absoluteFilePath();
    const QFileInfo gameInfoInfo(gameInfoPath);
    if (!gameInfoInfo.exists() || !gameInfoInfo.isFile()) {
        const QString message = tr("The selected game directory does not contain gameinfo.txt:\n%1")
                                    .arg(gameInfoPath);
        if (reportErrors) QMessageBox::critical(this, tr("Game Configuration"), message);
        else appendMessage(message);
        return false;
    }

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError assetError;
    const std::filesystem::path nativePath(gameInfoPath.toUtf8().toStdString());
    if (!fileSystem->configure(nativePath, &assetError)) {
        QString message = QString::fromUtf8(assetError.message.c_str()).trimmed();
        if (message.isEmpty()) {
            message = tr("Hammer-- could not mount the search paths from:\n%1\n\n"
                         "Check that the referenced Steam apps are installed and that the VPK files are readable.")
                          .arg(gameInfoInfo.absoluteFilePath());
        }
        appendMessage(tr("Game configuration failed for %1: %2")
                          .arg(gameInfoInfo.absoluteFilePath(), message));
        if (reportErrors) QMessageBox::critical(this, tr("Game Configuration"), message);
        return false;
    }

    gameFileSystem_ = std::move(fileSystem);
    materials_ = std::make_shared<hammer::assets::MaterialSystem>(gameFileSystem_);
    refreshMaterialList();
    loadedGameInfoPath_ = gameInfoInfo.absoluteFilePath();
    QSettings settings;
    settings.setValue(QStringLiteral("game/gameDirectory"), gameInfoInfo.absolutePath());
    settings.setValue(QStringLiteral("game/lastGameInfo"), loadedGameInfoPath_);
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setMaterialSystem(materials_);
            document->setMaterialRenderingEnabled(materialRenderingEnabled_);
            document->setWireframeOverlayEnabled(wireframeOverlayEnabled_);
            document->setHdrEnabled(hdrEnabled_);
            document->setRayTracedGamma(rayTracedGamma_);
            document->setDisplacementSolidMaskEnabled(displacementSolidMaskEnabled_);
            document->setTexturedRenderMode(texturedRenderMode_);
            document->setMaterialEffectsEnabled(phongEnabled_, specularEnabled_, bumpMapsEnabled_,
                                                lightWarpEnabled_, selfIllumEnabled_, rimLightEnabled_);
            document->setMaterialEffectIntensities(
                phongIntensity_, specularIntensity_, bumpMapIntensity_);
        }
    }
    const QString message = tr("Game directory configured: %1 (%2)")
        .arg(gameInfoInfo.absolutePath(), QString::fromStdString(gameFileSystem_->summary()));
    appendMessage(message);
    for (const hammer::assets::SearchLocation& location : gameFileSystem_->locations()) {
        const QString kind = location.kind == hammer::assets::SearchLocation::Kind::Vpk
            ? tr("VPK") : tr("directory");
        appendMessage(tr("Game mount: %1 [%2, AppID %3] %4")
            .arg(kind,
                 QString::fromStdString(location.pathId),
                 QString::number(location.appId),
                 QString::fromStdString(location.path.string())));
    }
    for (const std::string& warning : gameFileSystem_->warnings()) {
        appendMessage(tr("Game mount warning: %1").arg(QString::fromUtf8(warning.c_str())));
    }
    const QString probeVmt = QStringLiteral("materials/brick/brickwall001a.vmt");
    const QString probeVtf = QStringLiteral("materials/brick/brickwall001a.vtf");
    const auto vmtSource = gameFileSystem_->sourceForFile(probeVmt.toStdString());
    const auto vtfSource = gameFileSystem_->sourceForFile(probeVtf.toStdString());
    appendMessage(tr("HL2 material probe: brick/brickwall001a VMT=%1 VTF=%2")
        .arg(vmtSource ? QString::fromStdString(vmtSource->path.string()) : tr("missing"),
             vtfSource ? QString::fromStdString(vtfSource->path.string()) : tr("missing")));
    const auto probeMaterial = materials_->material("brick/brickwall001a");
    if (probeMaterial && !probeMaterial->missing && probeMaterial->image.valid()) {
        appendMessage(tr("HL2 material load: OK — %1 x %2, shader %3, base texture %4")
            .arg(probeMaterial->image.width)
            .arg(probeMaterial->image.height)
            .arg(QString::fromStdString(probeMaterial->shader),
                 QString::fromStdString(probeMaterial->baseTexture)));
    } else {
        const QString reason = probeMaterial
            ? QString::fromStdString(probeMaterial->error)
            : tr("MaterialSystem returned no material");
        appendMessage(tr("HL2 material load: FAILED — %1").arg(reason));
    }
    setPrompt(message);
    return true;
}

void MainWindow::refreshMaterialList()
{
    mountedMaterialNames_.clear();
    if (materials_) {
        const std::vector<std::string> names = materials_->materialNames();
        mountedMaterialNames_.reserve(static_cast<qsizetype>(names.size()));
        for (const std::string& name : names) {
            mountedMaterialNames_.push_back(QString::fromStdString(name));
        }
    }
    if (!textureCombo_) return;

    const QString current = textureCombo_->currentText().trimmed();
    QSignalBlocker blocker(textureCombo_);
    textureCombo_->clear();
    textureCombo_->addItems(mountedMaterialNames_);
    if (!current.isEmpty()) {
        const int index = textureCombo_->findText(current, Qt::MatchFixedString);
        if (index >= 0) textureCombo_->setCurrentIndex(index);
        else textureCombo_->setEditText(current);
    } else if (!mountedMaterialNames_.isEmpty()) {
        textureCombo_->setCurrentIndex(0);
    }
    updateTexturePreview(textureCombo_->currentText());
    if (faceEditSheet_) faceEditSheet_->setMaterialNames(mountedMaterialNames_);
}

void MainWindow::updateTexturePreview(const QString& materialName)
{
    if (!texturePreview_ || !textureSizeLabel_) return;
    if (!materials_ || materialName.trimmed().isEmpty()) {
        texturePreview_->setPixmap(makeTexturePreview());
        textureSizeLabel_->setText(tr("No material"));
        return;
    }

    const auto material = materials_->material(materialName.trimmed().toStdString());
    if (!material || !material->image.valid()) {
        texturePreview_->setPixmap(makeTexturePreview());
        textureSizeLabel_->setText(tr("Missing"));
        return;
    }
    texturePreview_->setPixmap(materialPreviewPixmap(
        material->image, texturePreview_->size(), 0, nullptr, 1.0f, 0.10f, 1.0f,
        material->blended ? &material->image2 : nullptr));
    textureSizeLabel_->setText(tr("%1 x %2").arg(material->image.width).arg(material->image.height));
    texturePreview_->setToolTip(materialToolTip(material));
}

void MainWindow::showMaterialBrowser()
{
    const QString current = textureCombo_ ? textureCombo_->currentText() : QString{};
    const QString picked = pickMaterial(current);
    if (!picked.isEmpty() && textureCombo_) textureCombo_->setCurrentText(picked);
}

namespace {
// A full material list carries one thumbnail pixmap per item - for 8712 materials
// at 64 px that is roughly 147 MB of small buffers. Destroying the dialog frees
// them, but they sit below glibc's mmap threshold, so the pages stay in the arena
// and process RSS never falls: closing and reopening the browser looks like an
// unbounded leak. Measured 8712 icons: 172 MB resident after close, 27 MB once
// this runs. Call it when a thumbnail-heavy dialog goes away.
void releaseDialogPixmapPages()
{
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}
} // namespace

QString MainWindow::pickMaterial(const QString& initialMaterial)
{
    if (mountedMaterialNames_.isEmpty()) {
        QMessageBox::information(this, tr("Material Browser"),
            tr("No materials are indexed. Configure a game directory containing gameinfo.txt first."));
        return {};
    }

    const auto thumbnailPixels = std::make_shared<int>(textureBrowserThumbnailSize());
    // Thumbnails are rasterised at logical size x device pixel ratio, so that is
    // the only resolution worth decoding. Anything larger is discarded by the
    // scale in materialPreviewPixmap after costing full-surface memory.
    const auto previewCap = [](const QWidget* widget, int logicalPixels) {
        const qreal ratio = widget ? widget->devicePixelRatioF() : 1.0;
        return std::max(16, static_cast<int>(std::ceil(logicalPixels * std::max(1.0, ratio))));
    };

    const bool animatePreviews = textureBrowserAnimationsEnabled();
    const auto materialSystem = materials_;
    QString modFolder = materialSystem && materialSystem->fileSystem()
        ? QFileInfo(QString::fromStdString(materialSystem->fileSystem()->gameDirectory().string())).fileName()
        : QString{};
    if (modFolder.isEmpty()) modFolder = QStringLiteral("game");
    const auto displayMaterialPath = [modFolder](const QString& materialName) {
        return QStringLiteral("%1/materials/%2").arg(modFolder, materialName);
    };

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Material Browser"));
    dialog.resize(980, 720);
    auto* layout = new QVBoxLayout(&dialog);

    // Group-box sections in a splitter, matching the Model Browser and the
    // right-hand sidebar's page style.
    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* materialsBox = new QGroupBox(tr("Materials"), splitter);
    auto* materialsLayout = new QVBoxLayout(materialsBox);

    auto* browserTools = new QHBoxLayout;
    auto* filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter mounted materials…"));
    browserTools->addWidget(filter, 1);
    auto* previewSizeButton = new QToolButton(&dialog);
    previewSizeButton->setPopupMode(QToolButton::InstantPopup);
    auto* previewSizeMenu = new QMenu(previewSizeButton);
    previewSizeButton->setMenu(previewSizeMenu);
    previewSizeButton->setText(tr("Preview: %1 x %1").arg(*thumbnailPixels));
    std::vector<QAction*> previewSizeActions;
    for (const int size : {32, 64, 128, 256}) {
        QAction* action = previewSizeMenu->addAction(tr("%1 x %1").arg(size));
        action->setCheckable(true);
        action->setData(size);
        action->setChecked(size == *thumbnailPixels);
        previewSizeActions.push_back(action);
    }
    browserTools->addWidget(previewSizeButton);
    materialsLayout->addLayout(browserTools);

    auto* list = new QListWidget;
    list->setViewMode(QListView::IconMode);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setWrapping(true);
    list->setWordWrap(true);
    list->setUniformItemSizes(true);
    list->setIconSize(QSize(*thumbnailPixels, *thumbnailPixels));
    list->setGridSize(QSize(*thumbnailPixels + 34, *thumbnailPixels + 54));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    materialsLayout->addWidget(list, 1);

    // Right column: Preview, 3D Preview, then Texture Info and Files side
    // by side.
    auto* details = new QWidget(splitter);
    details->setMinimumWidth(330);
    details->setMaximumWidth(480);
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);

    auto* previewBox = new QGroupBox(tr("Preview"), details);
    auto* previewLayout = new QVBoxLayout(previewBox);
    // Square label that shrinks with the column instead of overflowing it,
    // so the section margins stay consistent with the 3D preview below.
    class SquarePreviewLabel final : public QLabel
    {
    public:
        using QLabel::QLabel;
        bool hasHeightForWidth() const override { return true; }
        int heightForWidth(int width) const override { return std::min(width, 256); }
        QSize sizeHint() const override { return {256, 256}; }
        void setSourcePixmap(const QPixmap& pixmap)
        {
            source_ = pixmap;
            applyScaled();
        }

    protected:
        void resizeEvent(QResizeEvent* event) override
        {
            QLabel::resizeEvent(event);
            applyScaled();
        }

    private:
        void applyScaled()
        {
            if (source_.isNull()) {
                clear();
                return;
            }
            setPixmap(source_.scaled(size(), Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
        }
        QPixmap source_;
    };
    auto* detailPreview = new SquarePreviewLabel;
    detailPreview->setMinimumSize(64, 64);
    detailPreview->setMaximumSize(256, 256);
    detailPreview->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    detailPreview->setAlignment(Qt::AlignCenter);
    detailPreview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    previewLayout->addWidget(detailPreview, 0, Qt::AlignHCenter);

    auto* preview3dBox = new QGroupBox(tr("3D Preview"), details);
    auto* preview3dLayout = new QVBoxLayout(preview3dBox);
    auto* preview3d = new MapViewWidget(MapViewWidget::Kind::Perspective, preview3dBox);
    preview3d->setGridVisible(false);
    preview3d->setMaterialSystem(materialSystem);
    preview3d->setMaterialRenderingEnabled(true);
    preview3d->setTexturedRenderMode(texturedRenderMode_);
    preview3d->setMaterialEffectsEnabled(phongEnabled_, specularEnabled_, bumpMapsEnabled_,
                                         lightWarpEnabled_, selfIllumEnabled_, rimLightEnabled_);
    preview3d->setMaterialEffectIntensities(phongIntensity_, specularIntensity_,
                                            bumpMapIntensity_);
    // Scales down with the dialog rather than forcing a tall fixed panel.
    preview3d->setMinimumHeight(150);
    preview3dLayout->addWidget(preview3d);
    const auto previewSceneMaterial = std::make_shared<QString>();
    // Current map's skybox (worldspawn skyname), so the preview matches the
    // lighting environment the material will actually be seen in.
    const std::string previewSkyName = [this]() -> std::string {
        if (MapDocumentWidget* document = activeDocument()) {
            if (const auto scene = document->scene()) return scene->skyName;
        }
        return {};
    }();

    const auto makeSelectableLabel = [](QWidget* parent) {
        auto* label = new QLabel(parent);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        return label;
    };
    auto* infoBox = new QGroupBox(tr("Texture Info"), details);
    auto* infoLayout = new QFormLayout(infoBox);
    auto* detailName = makeSelectableLabel(infoBox);
    auto* detailShader = makeSelectableLabel(infoBox);
    auto* detailSize = makeSelectableLabel(infoBox);
    infoLayout->addRow(tr("Path:"), detailName);
    infoLayout->addRow(tr("Shader:"), detailShader);
    infoLayout->addRow(tr("Size:"), detailSize);

    auto* filesBox = new QGroupBox(tr("Files"), details);
    auto* filesLayout = new QFormLayout(filesBox);
    auto* detailVmt = makeSelectableLabel(filesBox);
    auto* detailVtf = makeSelectableLabel(filesBox);
    filesLayout->addRow(tr("VMT:"), detailVmt);
    filesLayout->addRow(tr("VTF:"), detailVtf);

    detailsLayout->addWidget(previewBox);
    detailsLayout->addWidget(preview3dBox, 1);
    detailsLayout->addWidget(infoBox);
    detailsLayout->addWidget(filesBox);
    splitter->addWidget(materialsBox);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto* progress = new QLabel(tr("Preparing previews…"));
    layout->addWidget(progress);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    constexpr int MaterialNameRole = Qt::UserRole;
    constexpr int AnimatedPreviewRole = Qt::UserRole + 1;
    constexpr int ThumbnailLoadedRole = Qt::UserRole + 2;

    const auto placeholder = std::make_shared<QPixmap>(makeTexturePreview().scaled(
        QSize(*thumbnailPixels, *thumbnailPixels), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    for (const QString& materialName : mountedMaterialNames_) {
        auto* item = new QListWidgetItem(QIcon(*placeholder), displayMaterialPath(materialName));
        item->setData(MaterialNameRole, materialName);
        item->setData(ThumbnailLoadedRole, false);
        item->setSizeHint(list->gridSize());
        list->addItem(item);
    }

    const auto loadedThumbnailCount = std::make_shared<int>(0);
    const auto priorityOrder = std::make_shared<std::vector<int>>();
    const auto priorityCursor = std::make_shared<std::size_t>(0);
    const auto priorityDirty = std::make_shared<bool>(true);

    auto markThumbnailLoaded = [=](QListWidgetItem* item) {
        if (!item || item->data(ThumbnailLoadedRole).toBool()) return;
        item->setData(ThumbnailLoadedRole, true);
        ++(*loadedThumbnailCount);
        *priorityDirty = true;
    };

    auto updateDetails = [=](QListWidgetItem* item, int animationOffset = 0) {
        if (!item || !materialSystem) {
            detailPreview->setSourcePixmap(*placeholder);
            detailName->clear();
            detailShader->clear();
            detailSize->clear();
            detailVmt->clear();
            detailVtf->clear();
            return;
        }
        const QString name = item->data(MaterialNameRole).toString();
        const auto material = materialSystem->previewMaterial(name.toStdString(),
                                                             previewCap(detailPreview, 256));
        if (!material) return;
        const QPixmap preview = materialPreviewPixmap(
            material->image, QSize(256, 256),
            material->previewAnimated ? animationOffset : 0,
            material->waterHasFlowMap ? &material->waterFlowImage : nullptr,
            material->waterFlowCycleRate, material->waterFlowDistance,
            material->waterFlowMapScale,
            material->blended ? &material->image2 : nullptr);
        detailPreview->setSourcePixmap(preview);
        if (*previewSceneMaterial != name) {
            // Rebuild only on selection change; the animation timer reuses
            // the same scene.
            *previewSceneMaterial = name;
            if (auto scene = buildMaterialPreviewScene(name, previewSkyName,
                                                       material->water)) {
                preview3d->setScene(scene, true);
            }
        }
        detailName->setText(name);
        detailShader->setText(QString::fromStdString(material->shader));
        detailSize->setText(material->image.valid()
            ? QStringLiteral("%1 x %2").arg(material->image.width).arg(material->image.height)
            : tr("—"));
        // Show only the mod-relative location: "tf/tf2_textures_dir.vpk" (or a
        // loose dir like "tf2z") plus the resource path inside it.
        const auto shortSource = [](const std::string& source, const QString& resource) {
            if (source.empty()) return QString();
            const QString native = QDir::fromNativeSeparators(QString::fromStdString(source));
            const QStringList parts = native.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            QString location;
            if (parts.size() >= 2 && native.endsWith(QStringLiteral(".vpk"), Qt::CaseInsensitive)) {
                location = parts[parts.size() - 2] + QLatin1Char('/') + parts.last();
            } else if (!parts.isEmpty()) {
                location = parts.last();
            }
            return location + QLatin1Char('/') + resource;
        };
        const QString vmtShort = shortSource(
            material->vmtSource, QStringLiteral("materials/%1.vmt").arg(name));
        const QString vtfShort = shortSource(
            material->vtfSource,
            QStringLiteral("materials/%1.vtf")
                .arg(QString::fromStdString(material->baseTexture)));
        detailVmt->setText(vmtShort.isEmpty() ? tr("not found") : vmtShort);
        detailVtf->setText(vtfShort.isEmpty() ? tr("—") : vtfShort);
        detailVmt->setToolTip(QString::fromStdString(material->vmtSource));
        detailVtf->setToolTip(QString::fromStdString(material->vtfSource));
        item->setToolTip(materialToolTip(material));
        item->setData(AnimatedPreviewRole, material->previewAnimated);
        if (!material->missing && material->image.valid()) {
            item->setIcon(QIcon(materialPreviewPixmap(
                material->image, QSize(*thumbnailPixels, *thumbnailPixels),
                material->previewAnimated ? animationOffset : 0,
                material->waterHasFlowMap ? &material->waterFlowImage : nullptr,
                material->waterFlowCycleRate, material->waterFlowDistance,
                material->waterFlowMapScale,
                material->blended ? &material->image2 : nullptr)));
        }
        markThumbnailLoaded(item);
    };

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem*) {
        dialog.accept();
    });
    connect(list, &QListWidget::currentItemChanged, &dialog,
            [updateDetails](QListWidgetItem* current, QListWidgetItem*) {
        updateDetails(current);
    });

    auto rebuildThumbnailPriority = [=] {
        struct Candidate {
            int itemIndex{0};
            int group{1};
            qint64 distance{0};
        };

        std::vector<Candidate> candidates;
        candidates.reserve(static_cast<std::size_t>(list->count()));
        const QRect viewportRect = list->viewport()->rect();
        const QPoint viewportCenter = viewportRect.center();
        const int currentRow = std::max(0, list->currentRow());

        auto axisGap = [](int itemMinimum, int itemMaximum,
                          int viewportMinimum, int viewportMaximum) -> qint64 {
            if (itemMaximum < viewportMinimum) return viewportMinimum - itemMaximum;
            if (itemMinimum > viewportMaximum) return itemMinimum - viewportMaximum;
            return 0;
        };

        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* item = list->item(i);
            if (!item || item->isHidden() || item->data(ThumbnailLoadedRole).toBool()) continue;

            const QRect itemRect = list->visualItemRect(item);
            const bool onScreen = itemRect.isValid() && itemRect.intersects(viewportRect);
            qint64 distance = 0;
            if (itemRect.isValid()) {
                const qint64 dx = axisGap(itemRect.left(), itemRect.right(),
                                          viewportRect.left(), viewportRect.right());
                const qint64 dy = axisGap(itemRect.top(), itemRect.bottom(),
                                          viewportRect.top(), viewportRect.bottom());
                const QPoint itemCenter = itemRect.center();
                const qint64 centerDx = itemCenter.x() - viewportCenter.x();
                const qint64 centerDy = itemCenter.y() - viewportCenter.y();
                const qint64 centerDistance = centerDx * centerDx + centerDy * centerDy;
                distance = onScreen ? centerDistance
                                    : (dx * dx + dy * dy) * 4096 + centerDistance;
            } else {
                const qint64 rowDistance = std::abs(i - currentRow);
                distance = std::numeric_limits<qint64>::max() / 4 + rowDistance;
            }
            candidates.push_back({i, onScreen ? 0 : 1, distance});
        }

        std::stable_sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                return std::tie(left.group, left.distance, left.itemIndex) <
                       std::tie(right.group, right.distance, right.itemIndex);
            });

        priorityOrder->clear();
        priorityOrder->reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            priorityOrder->push_back(candidate.itemIndex);
        }
        *priorityCursor = 0;
        *priorityDirty = false;
    };

    auto* thumbnailTimer = new QTimer(&dialog);
    thumbnailTimer->setInterval(0);
    connect(thumbnailTimer, &QTimer::timeout, &dialog, [=] {
        if (*priorityDirty || *priorityCursor >= priorityOrder->size()) {
            rebuildThumbnailPriority();
        }
        if (priorityOrder->empty()) {
            thumbnailTimer->stop();
            progress->setText(tr("Loaded %1 of %2 previews — %3 px thumbnails%4")
                .arg(*loadedThumbnailCount).arg(list->count()).arg(*thumbnailPixels)
                .arg(animatePreviews ? tr(", animation enabled") : QString()));
            return;
        }

        constexpr int batchSize = 6;
        int loaded = 0;
        while (*priorityCursor < priorityOrder->size() && loaded < batchSize) {
            const int itemIndex = (*priorityOrder)[(*priorityCursor)++];
            QListWidgetItem* item = list->item(itemIndex);
            if (!item || item->isHidden() || item->data(ThumbnailLoadedRole).toBool()) continue;

            if (materialSystem) {
                const auto material = materialSystem->previewMaterial(
                    item->data(MaterialNameRole).toString().toStdString(),
                    previewCap(list, *thumbnailPixels));
                if (material) {
                    item->setIcon(QIcon(materialPreviewPixmap(
                        material->image, QSize(*thumbnailPixels, *thumbnailPixels), 0,
                        material->waterHasFlowMap ? &material->waterFlowImage : nullptr,
                        material->waterFlowCycleRate, material->waterFlowDistance,
                        material->waterFlowMapScale,
                        material->blended ? &material->image2 : nullptr)));
                    item->setToolTip(materialToolTip(material));
                    item->setData(AnimatedPreviewRole, material->previewAnimated);
                }
            }
            markThumbnailLoaded(item);
            ++loaded;
        }

        progress->setText(tr("Loaded %1 of %2 previews — visible textures first — %3 px thumbnails%4")
            .arg(*loadedThumbnailCount).arg(list->count()).arg(*thumbnailPixels)
            .arg(animatePreviews ? tr(", animation enabled") : QString()));
        if (*priorityCursor >= priorityOrder->size()) *priorityDirty = true;
        if (*loadedThumbnailCount >= list->count()) thumbnailTimer->stop();
    });

    auto requestThumbnailReprioritization = [=] {
        *priorityDirty = true;
        *priorityCursor = 0;
        if (!thumbnailTimer->isActive() && *loadedThumbnailCount < list->count()) {
            thumbnailTimer->start();
        }
    };

    for (QAction* action : previewSizeActions) {
        connect(action, &QAction::triggered, &dialog, [=](bool) {
            const int requested = action->data().toInt();
            if (requested == *thumbnailPixels) return;
            *thumbnailPixels = requested;
            QSettings().setValue(QStringLiteral("textures/browserThumbnailSize"), requested);
            previewSizeButton->setText(tr("Preview: %1 x %1").arg(requested));
            for (QAction* candidate : previewSizeActions)
                candidate->setChecked(candidate == action);

            list->setIconSize(QSize(requested, requested));
            list->setGridSize(QSize(requested + 34, requested + 54));
            *placeholder = makeTexturePreview().scaled(
                QSize(requested, requested), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            *loadedThumbnailCount = 0;
            for (int index = 0; index < list->count(); ++index) {
                QListWidgetItem* item = list->item(index);
                item->setData(ThumbnailLoadedRole, false);
                item->setIcon(QIcon(*placeholder));
                item->setSizeHint(list->gridSize());
            }
            list->doItemsLayout();
            *priorityDirty = true;
            *priorityCursor = 0;

            // Retune the preview cache for the new size. Stepping down reuses the
            // images already in memory (downscale, then drop the originals);
            // stepping up discards the too-small entries so the new size comes
            // from the correct mip. Either way this cannot disturb the 3D views:
            // anything their batches are drawing is still referenced, so it is
            // skipped and keeps its full quality. Clearing the icons above
            // dropped this dialog's own references.
            if (materialSystem) {
                const std::size_t freed = materialSystem->adjustPreviewCache(
                    previewCap(list, requested));
                if (freed > 0) {
                    setPrompt(tr("Preview size %1 px — released %2 of unused texture cache")
                                  .arg(requested)
                                  .arg(QLocale().formattedDataSize(
                                      static_cast<qint64>(freed))));
                }
            }

            if (list->currentItem()) updateDetails(list->currentItem());
            requestThumbnailReprioritization();
        });
    }

    connect(list->verticalScrollBar(), &QScrollBar::valueChanged, &dialog,
            [requestThumbnailReprioritization](int) { requestThumbnailReprioritization(); });
    connect(list->horizontalScrollBar(), &QScrollBar::valueChanged, &dialog,
            [requestThumbnailReprioritization](int) { requestThumbnailReprioritization(); });
    connect(list->verticalScrollBar(), &QScrollBar::rangeChanged, &dialog,
            [requestThumbnailReprioritization](int, int) { requestThumbnailReprioritization(); });

    auto* viewportWatcher = new TextureBrowserViewportWatcher(
        requestThumbnailReprioritization, list->viewport());
    list->viewport()->installEventFilter(viewportWatcher);

    connect(filter, &QLineEdit::textChanged, &dialog, [=](const QString& text) {
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* item = list->item(i);
            item->setHidden(!item->text().contains(text, Qt::CaseInsensitive));
        }
        list->doItemsLayout();
        requestThumbnailReprioritization();
    });

    thumbnailTimer->start();

    int animationOffset = 0;
    auto* animationTimer = new QTimer(&dialog);
    animationTimer->setInterval(120);
    if (animatePreviews) {
        connect(animationTimer, &QTimer::timeout, &dialog, [=, &animationOffset]() mutable {
            animationOffset += 3;
            const QRect viewportRect = list->viewport()->rect();
            for (int i = 0; i < list->count(); ++i) {
                QListWidgetItem* item = list->item(i);
                if (item->isHidden() || !item->data(AnimatedPreviewRole).toBool()) continue;
                if (!list->visualItemRect(item).intersects(viewportRect)) continue;
                const auto material = materialSystem->previewMaterial(
                    item->data(MaterialNameRole).toString().toStdString(),
                    previewCap(list, *thumbnailPixels));
                if (material && material->image.valid()) {
                    item->setIcon(QIcon(materialPreviewPixmap(
                        material->image, QSize(*thumbnailPixels, *thumbnailPixels), animationOffset,
                        material->waterHasFlowMap ? &material->waterFlowImage : nullptr,
                        material->waterFlowCycleRate, material->waterFlowDistance,
                        material->waterFlowMapScale,
                        material->blended ? &material->image2 : nullptr)));
                }
            }
            if (list->currentItem() && list->currentItem()->data(AnimatedPreviewRole).toBool()) {
                updateDetails(list->currentItem(), animationOffset);
            }
        });
        animationTimer->start();
    }

    QListWidgetItem* currentMaterialItem = nullptr;
    const QString currentMaterial = initialMaterial;
    for (int index = 0; index < list->count(); ++index) {
        QListWidgetItem* item = list->item(index);
        if (item->data(MaterialNameRole).toString().compare(currentMaterial, Qt::CaseInsensitive) == 0) {
            currentMaterialItem = item;
            break;
        }
    }
    if (currentMaterialItem) {
        list->setCurrentItem(currentMaterialItem);
        list->scrollToItem(currentMaterialItem, QAbstractItemView::PositionAtCenter);
    } else if (list->count() > 0) {
        list->setCurrentRow(0);
    }
    requestThumbnailReprioritization();

    const int result = dialog.exec();
    QString picked;
    if (result == QDialog::Accepted && list->currentItem())
        picked = list->currentItem()->data(MaterialNameRole).toString();

    // The dialog and every thumbnail it built are gone by the time this returns,
    // so hand their pages back rather than leaving them stranded in the arena.
    list->clear();
    releaseDialogPixmapPages();
    return picked;
}

// hammer/replacetexdlg.cpp CReplaceTexDlg + CMapDoc::ReplaceTextures. The
// dialog resource (IDD_REPLACETEX) and CMapDoc::ReplaceTextures itself are not
// in this source tree (only replacetexdlg.cpp/.h and the resource.h ids
// survive), so the layout is reconstructed from the DDX fields the original
// binds: IDC_FIND/IDC_REPLACE edits with IDC_FINDPIC/IDC_REPLACEPIC preview
// swatches and IDC_BROWSEFIND/IDC_BROWSEREPLACE buttons, an IDC_INMARKED
// "Marked objects"/"Everything" radio pair, an IDC_ACTION exact/partial/
// substitute radio group, IDC_MARKONLY, and IDC_RESCALETEXTURECOORDINATES.
// IDC_HIDDEN ("include hidden objects") gates the replace on the per-object
// visibility VisGroups and QuickHide now provide.
void MainWindow::showEntityReportDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Entity Report"));
    dialog.resize(720, 560);
    auto* layout = new QVBoxLayout(&dialog);

    // Group-box sections like the Material and Model browsers: the list and its
    // three action buttons on top, the filters underneath.
    auto* entitiesBox = new QGroupBox(tr("Entities"), &dialog);
    auto* entitiesLayout = new QHBoxLayout(entitiesBox);
    auto* list = new QListWidget;
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    entitiesLayout->addWidget(list, 1);

    auto* actions = new QVBoxLayout;
    auto* goTo = new QPushButton(tr("&Go to"));
    auto* remove = new QPushButton(tr("&Delete"));
    auto* properties = new QPushButton(tr("&Properties"));
    for (QPushButton* button : {goTo, remove, properties}) {
        button->setAutoDefault(false);
        button->setEnabled(false);
        actions->addWidget(button);
    }
    actions->addStretch(1);
    entitiesLayout->addLayout(actions);
    layout->addWidget(entitiesBox, 1);

    auto* filterBox = new QGroupBox(tr("Filter"), &dialog);
    auto* filterLayout = new QVBoxLayout(filterBox);
    auto* kinds = new QHBoxLayout;
    auto* kindAll = new QRadioButton(tr("&All entities"));
    auto* kindBrush = new QRadioButton(tr("&Brush entities"));
    auto* kindPoint = new QRadioButton(tr("Poi&nt entities"));
    kindAll->setChecked(true);
    kinds->addWidget(kindAll);
    kinds->addWidget(kindBrush);
    kinds->addWidget(kindPoint);
    kinds->addStretch(1);
    auto* includeHidden = new QCheckBox(tr("&Include hidden objects"));
    includeHidden->setToolTip(tr("Entities hidden by a VisGroup or QuickHide, and brush "
                                 "entities whose solids are all hidden by the tool-texture "
                                 "filter"));
    kinds->addWidget(includeHidden);
    filterLayout->addLayout(kinds);

    auto* searchGrid = new QGridLayout;
    auto* keyEdit = new QLineEdit;
    auto* valueEdit = new QLineEdit;
    auto* exact = new QCheckBox(tr("E&xact"));
    auto* classEdit = new QLineEdit;
    auto* keyLabel = new QLabel(tr("By &key/value:"));
    keyLabel->setBuddy(keyEdit);
    auto* classLabel = new QLabel(tr("By &class:"));
    classLabel->setBuddy(classEdit);
    keyEdit->setPlaceholderText(tr("key"));
    valueEdit->setPlaceholderText(tr("value"));
    classEdit->setPlaceholderText(tr("classname"));
    searchGrid->addWidget(keyLabel, 0, 0);
    searchGrid->addWidget(keyEdit, 0, 1);
    searchGrid->addWidget(valueEdit, 0, 2);
    searchGrid->addWidget(exact, 0, 3);
    searchGrid->addWidget(classLabel, 1, 0);
    searchGrid->addWidget(classEdit, 1, 1, 1, 2);
    searchGrid->setColumnStretch(1, 1);
    searchGrid->setColumnStretch(2, 1);
    filterLayout->addLayout(searchGrid);
    layout->addWidget(filterBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const QPointer<MapDocumentWidget> guarded(document);
    auto* countLabel = new QLabel;
    countLabel->setEnabled(false);
    filterLayout->addWidget(countLabel);

    const auto refresh = [=] {
        list->clear();
        if (!guarded) return;
        const QString wantedKey = keyEdit->text().trimmed();
        const QString wantedValue = valueEdit->text().trimmed();
        const QString wantedClass = classEdit->text().trimmed();
        int shown = 0;
        for (const auto& entry : guarded->entityReport()) {
            if (entry.hidden && !includeHidden->isChecked()) continue;
            if (kindBrush->isChecked() && !entry.brushEntity) continue;
            if (kindPoint->isChecked() && entry.brushEntity) continue;
            if (!wantedClass.isEmpty() &&
                !entry.classname.contains(wantedClass, Qt::CaseInsensitive)) {
                continue;
            }
            if (!wantedKey.isEmpty() || !wantedValue.isEmpty()) {
                // Partial matches count unless Exact is ticked, and an empty
                // key means "any key with this value" (and the reverse).
                const auto matches = [&](const QString& text, const QString& wanted) {
                    if (wanted.isEmpty()) return true;
                    return exact->isChecked() ? text.compare(wanted, Qt::CaseInsensitive) == 0
                                              : text.contains(wanted, Qt::CaseInsensitive);
                };
                const bool anyPair = std::any_of(
                    entry.properties.cbegin(), entry.properties.cend(),
                    [&](const std::pair<QString, QString>& property) {
                        return matches(property.first, wantedKey) &&
                               matches(property.second, wantedValue);
                    });
                if (!anyPair) continue;
            }

            QString text = entry.classname.isEmpty() ? tr("(no classname)") : entry.classname;
            if (!entry.targetName.isEmpty()) text = tr("%1 — %2").arg(entry.targetName, text);
            if (entry.hidden) text = tr("%1 (hidden)").arg(text);
            auto* item = new QListWidgetItem(text, list);
            item->setData(Qt::UserRole, entry.id);
            item->setToolTip(tr("%1 #%2").arg(entry.brushEntity ? tr("Brush entity")
                                                                : tr("Point entity"))
                                 .arg(entry.id));
            ++shown;
        }
        countLabel->setText(tr("%n entity(s) listed", nullptr, shown));
    };

    const auto selectedIds = [list] {
        std::vector<int> ids;
        for (const QListWidgetItem* item : list->selectedItems()) {
            ids.push_back(item->data(Qt::UserRole).toInt());
        }
        return ids;
    };

    // Selecting in the report selects in the 2D and 3D views.
    connect(list, &QListWidget::itemSelectionChanged, &dialog, [=] {
        const std::vector<int> ids = selectedIds();
        const bool any = !ids.empty();
        goTo->setEnabled(any);
        remove->setEnabled(any);
        properties->setEnabled(ids.size() == 1);
        if (!guarded) return;
        guarded->selectEntitiesById(ids);
        updateEditActions();
    });
    connect(goTo, &QPushButton::clicked, &dialog, [=] {
        if (guarded) guarded->centerViewsOnSelection();
    });
    connect(remove, &QPushButton::clicked, &dialog, [=] {
        if (!guarded) return;
        guarded->deleteSelection();
        updateEditActions();
        refresh();
    });
    connect(properties, &QPushButton::clicked, &dialog, [=, &dialog] {
        if (!guarded) return;
        guarded->showObjectProperties(&dialog);
        refresh();
    });
    for (QLineEdit* edit : {keyEdit, valueEdit, classEdit}) {
        connect(edit, &QLineEdit::textChanged, &dialog, [=](const QString&) { refresh(); });
    }
    const std::array<QAbstractButton*, 5> toggles{kindAll, kindBrush, kindPoint, includeHidden, exact};
    for (QAbstractButton* toggle : toggles) {
        connect(toggle, &QAbstractButton::toggled, &dialog, [=](bool) { refresh(); });
    }

    refresh();
    dialog.exec();
}

// Map > Check for Problems (hammer/mapcheckdlg.cpp, IDD_MAPCHECK). Binds: the
// IDC_ERRORS list with IDC_GO / IDC_FIX / IDC_FIXALL beside it, the read-only
// IDC_DESCRIPTION box under it, and IDC_CHECK_VISIBLE_ONLY, whose state is the
// persistent Options.general.bCheckVisibleMapErrors.
// Map > Load Pointfile (hammer/mapdoc.cpp CMapDoc::OnMapLoadpointfile). When a
// pointfile sits beside the map it is offered first; anything else is picked
// through an open dialog.
void MainWindow::loadPointFile()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;

    QString path = document->defaultPointFilePath();
    const bool haveDefault = !path.isEmpty() && QFileInfo::exists(path);
    if (haveDefault) {
        const auto answer = QMessageBox::question(
            this, tr("Load Pointfile"),
            tr("Load default pointfile?\n(%1)").arg(QDir::toNativeSeparators(path)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::No) path.clear();
    } else {
        path.clear();
    }

    if (path.isEmpty()) {
        const QString startIn = document->filePath().isEmpty()
            ? QString()
            : QFileInfo(document->filePath()).absolutePath();
        path = QFileDialog::getOpenFileName(this, tr("Load Pointfile"), startIn,
                                            tr("Pointfiles (*.lin *.pts);;All files (*)"));
        if (path.isEmpty()) return;
    }

    QString error;
    if (!document->loadPointFile(path, &error)) {
        QMessageBox::warning(this, tr("Load Pointfile"),
                             tr("Couldn't load pointfile.\n%1\n\n%2")
                                 .arg(QDir::toNativeSeparators(path), error));
        return;
    }
    setPrompt(tr("Loaded pointfile: %1 points from %2")
                  .arg(QString::number(static_cast<qulonglong>(document->pointFilePointCount())),
                       QFileInfo(path).fileName()));
    updateEditActions();
}

// Map > Load Portal File. The .prt vbsp leaves beside the map for vvis holds
// the visleaf portals; loading it here replaces a trip through Glview.
void MainWindow::loadPortalFile()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;

    QString path = document->defaultPortalFilePath();
    const bool haveDefault = !path.isEmpty() && QFileInfo::exists(path);
    if (haveDefault) {
        const auto answer = QMessageBox::question(
            this, tr("Load Portal File"),
            tr("Load default portal file?\n(%1)").arg(QDir::toNativeSeparators(path)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::No) path.clear();
    } else {
        path.clear();
    }

    if (path.isEmpty()) {
        const QString startIn = document->filePath().isEmpty()
            ? QString()
            : QFileInfo(document->filePath()).absolutePath();
        path = QFileDialog::getOpenFileName(this, tr("Load Portal File"), startIn,
                                            tr("Portal files (*.prt);;All files (*)"));
        if (path.isEmpty()) return;
    }

    QString error;
    if (!document->loadPortalFile(path, &error)) {
        QMessageBox::warning(this, tr("Load Portal File"),
                             tr("Couldn't load portal file.\n%1\n\n%2")
                                 .arg(QDir::toNativeSeparators(path), error));
        return;
    }
    setPrompt(tr("Loaded portal file: %1 portals from %2")
                  .arg(QString::number(static_cast<qulonglong>(document->portalCount())),
                       QFileInfo(path).fileName()));
    updateEditActions();
}

void MainWindow::showCheckForProblemsDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;

    QSettings settings;
    const QString visibleOnlyKey = QStringLiteral("mapCheck/visibleOnly");

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Check for Problems"));
    dialog.resize(640, 480);
    auto* layout = new QVBoxLayout(&dialog);

    auto* problemsBox = new QGroupBox(tr("Problems"), &dialog);
    auto* problemsLayout = new QHBoxLayout(problemsBox);
    auto* list = new QListWidget;
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    problemsLayout->addWidget(list, 1);

    auto* actions = new QVBoxLayout;
    auto* goTo = new QPushButton(tr("&Go to error"));
    auto* fix = new QPushButton(tr("&Fix"));
    auto* fixAll = new QPushButton(tr("Fix &all (of type)"));
    for (QPushButton* button : {goTo, fix, fixAll}) {
        button->setAutoDefault(false);
        button->setEnabled(false);
        actions->addWidget(button);
    }
    actions->addStretch(1);
    problemsLayout->addLayout(actions);
    layout->addWidget(problemsBox, 1);

    auto* descriptionBox = new QGroupBox(tr("Description"), &dialog);
    auto* descriptionLayout = new QVBoxLayout(descriptionBox);
    auto* description = new QPlainTextEdit;
    description->setReadOnly(true);
    description->setMaximumHeight(96);
    descriptionLayout->addWidget(description);
    layout->addWidget(descriptionBox);

    auto* visibleOnly = new QCheckBox(tr("Check &visible parts of the map only"), &dialog);
    visibleOnly->setChecked(settings.value(visibleOnlyKey, false).toBool());
    visibleOnly->setToolTip(tr("Skip solids hidden by the tool-texture filter, which is the only "
                               "object hiding this port models"));
    layout->addWidget(visibleOnly);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const QPointer<MapDocumentWidget> guarded(document);
    std::vector<MapDocumentWidget::MapProblem> problems;

    const auto selectedProblems = [&] {
        std::vector<MapDocumentWidget::MapProblem> selected;
        for (const QListWidgetItem* item : list->selectedItems()) {
            const int index = item->data(Qt::UserRole).toInt();
            if (index >= 0 && index < static_cast<int>(problems.size())) {
                selected.push_back(problems[static_cast<std::size_t>(index)]);
            }
        }
        return selected;
    };

    // Fixes invalidate the rows they did not touch (a deleted entity takes its
    // other problems with it), so the list is always rebuilt from a fresh
    // check rather than carrying the original's per-row "Fixed" flags.
    const auto refresh = [&] {
        list->clear();
        description->clear();
        if (!guarded) return;
        problems = guarded->checkForProblems(visibleOnly->isChecked());
        for (std::size_t i = 0; i < problems.size(); ++i) {
            auto* item = new QListWidgetItem(problems[i].text, list);
            item->setData(Qt::UserRole, static_cast<int>(i));
        }
        goTo->setEnabled(false);
        fix->setEnabled(false);
        fixAll->setEnabled(false);
    };

    const auto applyFixes = [&](bool allOfType) {
        if (!guarded) return;
        const std::vector<MapDocumentWidget::MapProblem> selected = selectedProblems();
        std::vector<MapDocumentWidget::MapProblem> toFix;
        for (const MapDocumentWidget::MapProblem& problem : selected) {
            if (!problem.canFix) continue;
            if (!allOfType) {
                toFix.push_back(problem);
                continue;
            }
            for (const MapDocumentWidget::MapProblem& candidate : problems) {
                if (candidate.canFix && candidate.type == problem.type) toFix.push_back(candidate);
            }
        }
        if (toFix.empty()) return;
        guarded->fixProblems(toFix);
        updateEditActions();
        refresh();
    };

    connect(list, &QListWidget::itemSelectionChanged, &dialog, [&] {
        const std::vector<MapDocumentWidget::MapProblem> selected = selectedProblems();
        goTo->setEnabled(std::any_of(selected.cbegin(), selected.cend(),
                                     [](const MapDocumentWidget::MapProblem& problem) {
                                         return !problem.objects.empty();
                                     }));
        const bool fixable = std::any_of(selected.cbegin(), selected.cend(),
                                         [](const MapDocumentWidget::MapProblem& problem) {
                                             return problem.canFix;
                                         });
        fix->setEnabled(fixable);
        fixAll->setEnabled(fixable);
        description->setPlainText(selected.empty() ? QString() : selected.front().description);
    });

    // "Go to error": select every selected error's objects and center on them,
    // which is CMapCheckDlg::GotoSelectedErrors.
    const auto gotoSelection = [&] {
        if (!guarded) return;
        std::vector<hammer::vmf::ObjectRef> objects;
        for (const MapDocumentWidget::MapProblem& problem : selectedProblems()) {
            objects.insert(objects.end(), problem.objects.cbegin(), problem.objects.cend());
        }
        if (objects.empty()) return;
        guarded->selectObjects(std::move(objects));
        guarded->centerViewsOnSelection();
        updateEditActions();
    };
    connect(goTo, &QPushButton::clicked, &dialog, gotoSelection);
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
            [&](QListWidgetItem*) { gotoSelection(); });
    connect(fix, &QPushButton::clicked, &dialog, [&] { applyFixes(false); });
    connect(fixAll, &QPushButton::clicked, &dialog, [&] { applyFixes(true); });
    connect(visibleOnly, &QCheckBox::toggled, &dialog, [&](bool checked) {
        QSettings().setValue(visibleOnlyKey, checked);
        refresh();
    });

    refresh();
    // CMapCheckDlg::CheckForProblems only shows the dialog when the first check
    // found something.
    if (problems.empty()) {
        QMessageBox::information(this, tr("Check for Problems"), tr("No errors were found."));
        return;
    }
    dialog.exec();
}

void MainWindow::showFindEntitiesDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Find Entities"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* findBox = new QGroupBox(tr("Find"), &dialog);
    auto* findLayout = new QVBoxLayout(findBox);
    auto* nameRow = new QHBoxLayout;
    auto* nameEdit = new QLineEdit(lastEntitySearch_);
    nameEdit->selectAll();
    auto* nameLabel = new QLabel(tr("Entity &name:"));
    nameLabel->setBuddy(nameEdit);
    nameRow->addWidget(nameLabel);
    nameRow->addWidget(nameEdit, 1);
    findLayout->addLayout(nameRow);
    auto* hint = new QLabel(tr("Entities whose name matches exactly are selected and "
                               "centered in the views."));
    hint->setWordWrap(true);
    hint->setEnabled(false);
    findLayout->addWidget(hint);
    layout->addWidget(findBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    // Enter in the name field is the natural way to run the search.
    connect(nameEdit, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
    dialog.setMinimumWidth(380);
    if (dialog.exec() != QDialog::Accepted) return;

    const QString wanted = nameEdit->text().trimmed();
    if (wanted.isEmpty()) return;
    lastEntitySearch_ = wanted;

    const std::size_t found = document->selectEntitiesByName(wanted);
    if (found == 0) {
        QMessageBox::information(this, tr("Find Entities"),
                                 tr("No entity named \"%1\" is in this map.").arg(wanted));
        return;
    }
    setPrompt(tr("Selected %1 entit%2 named \"%3\"")
                  .arg(static_cast<qulonglong>(found))
                  .arg(found == 1 ? tr("y") : tr("ies"), wanted));
    updateEditActions();
}

void MainWindow::showPasteSpecialDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document || clipboard_.empty()) return;

    // The clipboard's own size, which the '<' buttons read in: one click on Z
    // stacks the copies exactly one stair high.
    hammer::vmf::Vec3 extents{};
    if (clipboard_.bounds.valid) {
        extents = {clipboard_.bounds.maximum.x - clipboard_.bounds.minimum.x,
                   clipboard_.bounds.maximum.y - clipboard_.bounds.minimum.y,
                   clipboard_.bounds.maximum.z - clipboard_.bounds.minimum.z};
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Paste Special"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* copiesBox = new QGroupBox(tr("Paste"), &dialog);
    auto* copiesLayout = new QHBoxLayout(copiesBox);
    auto* copies = new QSpinBox;
    copies->setRange(1, 4096);
    copies->setValue(std::max(1, pasteSpecialOptions_.copies));
    auto* copiesLabel = new QLabel(tr("&Number of copies to paste:"));
    // A QLabel mnemonic only works once it has a buddy; without one the '&' is
    // silently dropped and Alt+N goes nowhere.
    copiesLabel->setBuddy(copies);
    copiesLayout->addWidget(copiesLabel);
    copiesLayout->addWidget(copies, 1);
    layout->addWidget(copiesBox);

    auto* placementBox = new QGroupBox(tr("Placement"), &dialog);
    auto* placementLayout = new QVBoxLayout(placementBox);
    auto* startAtOriginal = new QCheckBox(tr("Start at center of &original"));
    startAtOriginal->setChecked(pasteSpecialOptions_.startAtOriginal);
    startAtOriginal->setToolTip(tr("Off: the copies are placed relative to the center of the 2D views"));
    auto* groupCopies = new QCheckBox(tr("&Group copies"));
    groupCopies->setChecked(pasteSpecialOptions_.groupCopies);
    placementLayout->addWidget(startAtOriginal);
    placementLayout->addWidget(groupCopies);
    layout->addWidget(placementBox);

    // Offset and rotation share a layout shape: a labelled spin box per axis,
    // with the offset row carrying the '<' button that fills in the selection's
    // own size along that axis.
    const auto makeAxisBox = [&dialog](const QString& title, QDoubleSpinBox* (&fields)[3],
                                       double minimum, double maximum) {
        auto* box = new QGroupBox(title, &dialog);
        auto* grid = new QGridLayout(box);
        static const char* const axisNames[3] = {QT_TR_NOOP("X:"), QT_TR_NOOP("Y:"), QT_TR_NOOP("Z:")};
        for (int axis = 0; axis < 3; ++axis) {
            fields[axis] = new QDoubleSpinBox;
            fields[axis]->setRange(minimum, maximum);
            fields[axis]->setDecimals(2);
            grid->addWidget(new QLabel(tr(axisNames[axis])), axis, 0);
            grid->addWidget(fields[axis], axis, 1);
        }
        grid->setColumnStretch(1, 1);
        return box;
    };

    QDoubleSpinBox* offsetFields[3] = {};
    auto* offsetBox = makeAxisBox(tr("Offset (accumulative)"), offsetFields, -65536.0, 65536.0);
    auto* offsetGrid = qobject_cast<QGridLayout*>(offsetBox->layout());
    const double extentValues[3] = {extents.x, extents.y, extents.z};
    for (int axis = 0; axis < 3; ++axis) {
        auto* readIn = new QPushButton(QStringLiteral("<"));
        readIn->setToolTip(tr("Use the size of the clipboard contents along this axis"));
        readIn->setFixedWidth(28);
        readIn->setAutoDefault(false);
        const double extent = extentValues[axis];
        QDoubleSpinBox* field = offsetFields[axis];
        connect(readIn, &QPushButton::clicked, &dialog, [field, extent] { field->setValue(extent); });
        offsetGrid->addWidget(readIn, axis, 2);
    }
    offsetFields[0]->setValue(pasteSpecialOptions_.offset.x);
    offsetFields[1]->setValue(pasteSpecialOptions_.offset.y);
    offsetFields[2]->setValue(pasteSpecialOptions_.offset.z);
    layout->addWidget(offsetBox);

    QDoubleSpinBox* rotationFields[3] = {};
    auto* rotationBox = makeAxisBox(tr("Rotation (accumulative)"), rotationFields, -360.0, 360.0);
    for (QDoubleSpinBox* field : rotationFields) field->setSuffix(tr("°"));
    rotationFields[0]->setValue(pasteSpecialOptions_.rotation.x);
    rotationFields[1]->setValue(pasteSpecialOptions_.rotation.y);
    rotationFields[2]->setValue(pasteSpecialOptions_.rotation.z);
    layout->addWidget(rotationBox);


    auto* namesBox = new QGroupBox(tr("Entity Names"), &dialog);
    auto* namesLayout = new QVBoxLayout(namesBox);
    auto* uniqueNames = new QCheckBox(tr("Make pasted entity names &unique"));
    uniqueNames->setChecked(pasteSpecialOptions_.uniqueEntityNames);
    auto* prefixRow = new QHBoxLayout;
    auto* prefixEdit = new QLineEdit(QString::fromStdString(pasteSpecialOptions_.namePrefix));
    prefixEdit->setPlaceholderText(tr("No prefix"));
    auto* prefixLabel = new QLabel(tr("Add this &prefix to all named entities:"));
    prefixLabel->setBuddy(prefixEdit);
    prefixRow->addWidget(prefixLabel);
    prefixRow->addWidget(prefixEdit, 1);
    namesLayout->addWidget(uniqueNames);
    namesLayout->addLayout(prefixRow);
    layout->addWidget(namesBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    hammer::vmf::PasteSpecialOptions options;
    options.copies = copies->value();
    options.startAtOriginal = startAtOriginal->isChecked();
    options.groupCopies = groupCopies->isChecked();
    options.offset = {offsetFields[0]->value(), offsetFields[1]->value(), offsetFields[2]->value()};
    options.rotation = {rotationFields[0]->value(), rotationFields[1]->value(),
                        rotationFields[2]->value()};
    options.uniqueEntityNames = uniqueNames->isChecked();
    options.namePrefix = prefixEdit->text().trimmed().toStdString();
    pasteSpecialOptions_ = options;

    if (document->pasteSpecial(clipboard_, options) == 0) {
        setPrompt(tr("Paste Special created nothing"));
    }
    updateEditActions();
}

void MainWindow::showReplaceTexturesDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;

    const bool hasSelection = document->selectionCount() > 0;
    const QString seedMaterial = textureCombo_ ? textureCombo_->currentText() : QString{};

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Replace Textures"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* fields = new QGridLayout;
    auto* findEdit = new QLineEdit(seedMaterial);
    auto* findPreview = new QLabel;
    findPreview->setFixedSize(48, 48);
    findPreview->setAlignment(Qt::AlignCenter);
    findPreview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    auto* findBrowse = new QPushButton(tr("Bro&wse..."));
    auto* replaceEdit = new QLineEdit;
    auto* replacePreview = new QLabel;
    replacePreview->setFixedSize(48, 48);
    replacePreview->setAlignment(Qt::AlignCenter);
    replacePreview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    auto* replaceBrowse = new QPushButton(tr("B&rowse..."));

    fields->addWidget(new QLabel(tr("Find:")), 0, 0);
    fields->addWidget(findEdit, 0, 1);
    fields->addWidget(findBrowse, 0, 2);
    fields->addWidget(findPreview, 0, 3);
    fields->addWidget(new QLabel(tr("Replace:")), 1, 0);
    fields->addWidget(replaceEdit, 1, 1);
    fields->addWidget(replaceBrowse, 1, 2);
    fields->addWidget(replacePreview, 1, 3);
    layout->addLayout(fields);

    const auto refreshPreview = [this](QLineEdit* edit, QLabel* preview) {
        const QString name = edit->text().trimmed();
        if (!materials_ || name.isEmpty()) {
            preview->setPixmap(QPixmap());
            preview->clear();
            return;
        }
        const auto material = materials_->material(name.toStdString());
        if (!material || !material->image.valid()) {
            preview->setPixmap(QPixmap());
            preview->clear();
            return;
        }
        preview->setPixmap(materialPreviewPixmap(
            material->image, preview->size(), 0, nullptr, 1.0f, 0.10f, 1.0f,
            material->blended ? &material->image2 : nullptr));
    };
    connect(findEdit, &QLineEdit::textChanged, &dialog, [=](const QString&) { refreshPreview(findEdit, findPreview); });
    connect(replaceEdit, &QLineEdit::textChanged, &dialog, [=](const QString&) { refreshPreview(replaceEdit, replacePreview); });
    connect(findBrowse, &QPushButton::clicked, &dialog, [=] {
        const QString picked = pickMaterial(findEdit->text());
        if (!picked.isEmpty()) findEdit->setText(picked);
    });
    connect(replaceBrowse, &QPushButton::clicked, &dialog, [=] {
        const QString picked = pickMaterial(replaceEdit->text());
        if (!picked.isEmpty()) replaceEdit->setText(picked);
    });

    auto* scopeBox = new QGroupBox(tr("Replace in:"));
    auto* scopeLayout = new QVBoxLayout(scopeBox);
    auto* scopeMarked = new QRadioButton(tr("&Marked objects"));
    auto* scopeAll = new QRadioButton(tr("&Everything"));
    scopeMarked->setEnabled(hasSelection);
    scopeMarked->setChecked(hasSelection);
    scopeAll->setChecked(!hasSelection);
    scopeLayout->addWidget(scopeMarked);
    scopeLayout->addWidget(scopeAll);
    layout->addWidget(scopeBox);

    auto* actionBox = new QGroupBox(tr("Action"));
    auto* actionLayout = new QVBoxLayout(actionBox);
    auto* actionExact = new QRadioButton(tr("&Exact match"));
    auto* actionPartial = new QRadioButton(tr("&Partial match"));
    auto* actionSubstitute = new QRadioButton(tr("&Substitute partial matches"));
    actionExact->setChecked(true);
    actionLayout->addWidget(actionExact);
    actionLayout->addWidget(actionPartial);
    actionLayout->addWidget(actionSubstitute);
    layout->addWidget(actionBox);

    auto* markOnly = new QCheckBox(tr("&Mark only (select matches instead of replacing)"));
    auto* rescale = new QCheckBox(tr("&Rescale texture coordinates"));
    // IDC_HIDDEN. Off by default, so a replace only touches what is on screen.
    auto* includeHidden = new QCheckBox(tr("Include &hidden objects"));
    includeHidden->setToolTip(tr("Also replace on solids hidden by a VisGroup or QuickHide"));
    layout->addWidget(markOnly);
    layout->addWidget(rescale);
    layout->addWidget(includeHidden);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    refreshPreview(findEdit, findPreview);
    refreshPreview(replaceEdit, replacePreview);

    if (dialog.exec() != QDialog::Accepted) return;

    hammer::vmf::ReplaceTexturesRequest request;
    request.find = findEdit->text().trimmed().toStdString();
    request.replace = replaceEdit->text().trimmed().toStdString();
    request.mode = actionSubstitute->isChecked() ? hammer::vmf::TextureMatchMode::SubstitutePartial
                  : actionPartial->isChecked()   ? hammer::vmf::TextureMatchMode::Partial
                                                  : hammer::vmf::TextureMatchMode::Exact;
    request.selectionOnly = scopeMarked->isChecked() && hasSelection;
    request.markOnly = markOnly->isChecked();
    request.rescaleTextureCoordinates = rescale->isChecked();
    request.includeHidden = includeHidden->isChecked();
    request.hiddenSolidIds = document->hiddenSolidIds();

    if (request.find.empty()) {
        QMessageBox::information(this, tr("Replace Textures"), tr("Enter a texture to find."));
        return;
    }

    const auto materialSystem = materials_;
    const auto materialSize = [materialSystem](const std::string& name, int& width, int& height) -> bool {
        if (!materialSystem || name.empty()) return false;
        const auto material = materialSystem->material(name);
        if (!material || !material->image.valid()) return false;
        width = material->image.width;
        height = material->image.height;
        return true;
    };

    document->replaceTextures(request, materialSize);
    updateEditActions();
}

void MainWindow::applyGridSettings()
{
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setGridSnapEnabled(gridSnapEnabled_);
            document->setGridSpacing(gridSpacing_);
        }
    }
    if (snapPane_) {
        snapPane_->setText(gridSnapEnabled_ ? tr("Snap: %1").arg(gridSpacing_)
                                            : tr("Snap: Off"));
    }
}

void MainWindow::setMaterialRenderingEnabled(bool enabled)
{
    materialRenderingEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("render/materials3d"), enabled);
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setMaterialRenderingEnabled(enabled);
        }
    }
    setPrompt(enabled ? tr("3D material rendering enabled") : tr("3D material rendering disabled"));
}

void MainWindow::setDisplacementSolidMaskEnabled(bool enabled)
{
    displacementSolidMaskEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("render/displacementSolidMask"), enabled);
    if (displacementSolidMaskAction_ && displacementSolidMaskAction_->isChecked() != enabled) {
        QSignalBlocker blocker(displacementSolidMaskAction_);
        displacementSolidMaskAction_->setChecked(enabled);
    }
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setDisplacementSolidMaskEnabled(enabled);
        }
    }
    setPrompt(enabled ? tr("Displacement solid faces masked in 3D views")
                      : tr("Displacement solid faces shown in 3D views"));
}

void MainWindow::setHdrEnabled(bool enabled)
{
    hdrEnabled_ = enabled;
    QSettings settings;
    settings.setValue(QStringLiteral("render/hdr"), enabled);
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget()))
            document->setHdrEnabled(enabled);
    }
    setPrompt(tr("HDR %1").arg(enabled ? tr("On") : tr("Off")));
}

void MainWindow::setUndoRedoActive(bool active)
{
    undoRedoActive_ = active;
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget()))
            document->setUndoRedoActive(active);
    }
    updateEditActions();
    setPrompt(active ? tr("Undo/redo history is being kept")
                     : tr("Undo/redo history discarded and no longer kept"));
}

void MainWindow::setRayTracedGamma(float gamma)
{
    rayTracedGamma_ = std::clamp(gamma, 0.5f, 5.0f);
    QSettings settings;
    settings.setValue(QStringLiteral("render/rayTracedGamma"), rayTracedGamma_);
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget()))
            document->setRayTracedGamma(rayTracedGamma_);
    }
    setPrompt(tr("Ray-traced gamma %1").arg(rayTracedGamma_, 0, 'f', 2));
}

void MainWindow::updateVisGroupButtonStates()
{
    MapDocumentWidget* document = activeDocument();
    const bool hasDocument = document != nullptr;
    const int selected = selectedVisGroupId();
    const bool hasVisGroup = hasDocument && selected != 0;
    // Auto visgroups are generated: they can be marked and toggled, but never
    // renamed, recolored, deleted or reordered.
    const bool userVisGroup = hasVisGroup && !hammer::vmf::isAutoVisGroupId(selected);
    if (showAllVisGroupsButton_) showAllVisGroupsButton_->setEnabled(hasDocument);
    if (editVisGroupsButton_) editVisGroupsButton_->setEnabled(hasDocument);
    if (markVisGroupButton_) markVisGroupButton_->setEnabled(hasVisGroup);
    if (moveVisGroupUpButton_) moveVisGroupUpButton_->setEnabled(userVisGroup);
    if (moveVisGroupDownButton_) moveVisGroupDownButton_->setEnabled(userVisGroup);
}

int MainWindow::selectedVisGroupId() const
{
    // Mark works on either tab; a negative id is an auto visgroup. Reorder and
    // Edit are user-only, which updateVisGroupButtonStates enforces.
    for (QTreeWidget* tree : {visGroupTree_, autoVisGroupTree_}) {
        if (!tree || !tree->isVisible()) continue;
        if (const QTreeWidgetItem* item = tree->currentItem())
            return item->data(0, Qt::UserRole).toInt();
    }
    if (!visGroupTree_) return 0;
    const QTreeWidgetItem* item = visGroupTree_->currentItem();
    return item ? item->data(0, Qt::UserRole).toInt() : 0;
}

void MainWindow::scheduleVisGroupTreeRebuild()
{
    if (visGroupTreeRebuildPending_) return;
    visGroupTreeRebuildPending_ = true;
    // Queued, so the rebuild runs after QTreeWidget has finished with whatever
    // item triggered it. Tying the call to "this" lets Qt drop it if the window
    // goes away first.
    QMetaObject::invokeMethod(
        this,
        [this] {
            visGroupTreeRebuildPending_ = false;
            rebuildVisGroupTree();
            rebuildAutoVisGroupTree();
        },
        Qt::QueuedConnection);
}

void MainWindow::rebuildVisGroupTree()
{
    if (!visGroupTree_) return;
    // Repopulating fires itemChanged for every check state written; without
    // this guard each rebuild would toggle every visgroup it just drew.
    updatingVisGroupTree_ = true;
    const int previous = selectedVisGroupId();
    visGroupTree_->clear();
    MapDocumentWidget* document = activeDocument();
    if (showAllVisGroupsButton_) {
        QSignalBlocker blocker(showAllVisGroupsButton_);
        showAllVisGroupsButton_->setChecked(document && document->showAllVisGroups());
        showAllVisGroupsButton_->setEnabled(document != nullptr);
    }
    if (!document) {
        updatingVisGroupTree_ = false;
        updateVisGroupButtonStates();
        return;
    }

    const std::vector<hammer::vmf::VisGroupDef> groups = document->visGroups();
    QHash<int, QTreeWidgetItem*> items;
    QTreeWidgetItem* restore = nullptr;
    // Parents are always written before their children in the VMF, so one pass
    // in file order can attach every child to an item that already exists.
    for (const hammer::vmf::VisGroupDef& group : groups) {
        if (group.automatic) continue; // the Auto tab's business, not this one
        QTreeWidgetItem* parent = items.value(group.parentId, nullptr);
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(visGroupTree_);
        const std::size_t members = document->visGroupMemberCount(group.id);
        item->setText(0, QStringLiteral("%1 (%2)")
                             .arg(QString::fromStdString(group.name))
                             .arg(static_cast<qulonglong>(members)));
        item->setData(0, Qt::UserRole, group.id);
        // The swatch is the visgroup's color, which the Edit dialog changes.
        QPixmap swatch(12, 12);
        swatch.fill(QColor(group.red, group.green, group.blue));
        item->setIcon(0, QIcon(swatch));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        switch (document->visGroupState(group.id)) {
        case hammer::vmf::VisGroupState::Shown:
            item->setCheckState(0, Qt::Checked);
            break;
        case hammer::vmf::VisGroupState::Hidden:
            item->setCheckState(0, Qt::Unchecked);
            break;
        case hammer::vmf::VisGroupState::Partial:
            item->setCheckState(0, Qt::PartiallyChecked);
            break;
        }
        item->setExpanded(true);
        items.insert(group.id, item);
        if (group.id == previous) restore = item;
    }
    if (restore) visGroupTree_->setCurrentItem(restore);
    updatingVisGroupTree_ = false;
    updateVisGroupButtonStates();
}

void MainWindow::rebuildAutoVisGroupTree()
{
    if (!autoVisGroupTree_) return;
    // Same guard as the User tree: writing check states re-emits itemChanged.
    updatingVisGroupTree_ = true;
    const int previous = autoVisGroupTree_->currentItem()
                             ? autoVisGroupTree_->currentItem()->data(0, Qt::UserRole).toInt()
                             : 0;
    autoVisGroupTree_->clear();
    MapDocumentWidget* document = activeDocument();
    if (!document) {
        updatingVisGroupTree_ = false;
        return;
    }

    QHash<int, QTreeWidgetItem*> items;
    QTreeWidgetItem* restore = nullptr;
    // autoVisGroups() returns parents before children, so one pass suffices.
    for (const MapDocumentWidget::AutoVisGroupNode& node : document->autoVisGroups()) {
        QTreeWidgetItem* parent = items.value(static_cast<int>(node.parent), nullptr);
        auto* item = parent ? new QTreeWidgetItem(parent)
                            : new QTreeWidgetItem(autoVisGroupTree_);
        item->setText(0, QStringLiteral("%1 (%2)")
                             .arg(node.name)
                             .arg(static_cast<qulonglong>(node.memberCount)));
        item->setData(0, Qt::UserRole, static_cast<int>(node.id));
        // Checkable but not editable: an auto visgroup has no name to change.
        item->setFlags((item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        switch (document->autoVisGroupState(node.id)) {
        case hammer::vmf::VisGroupState::Shown:
            item->setCheckState(0, Qt::Checked);
            break;
        case hammer::vmf::VisGroupState::Hidden:
            item->setCheckState(0, Qt::Unchecked);
            break;
        case hammer::vmf::VisGroupState::Partial:
            item->setCheckState(0, Qt::PartiallyChecked);
            break;
        }
        item->setExpanded(true);
        items.insert(static_cast<int>(node.id), item);
        if (static_cast<int>(node.id) == previous) restore = item;
    }
    if (restore) autoVisGroupTree_->setCurrentItem(restore);
    updatingVisGroupTree_ = false;
    updateVisGroupButtonStates();
}

void MainWindow::handleAutoVisGroupItemChanged(QTreeWidgetItem* item, int column)
{
    if (updatingVisGroupTree_ || !item || column != 0) return;
    MapDocumentWidget* document = activeDocument();
    if (!document) return;
    const int id = item->data(0, Qt::UserRole).toInt();
    if (id == 0) return;
    // Read the state before calling out: the document republishes the scene,
    // which schedules the rebuild that deletes this item.
    const bool show = item->checkState(0) != Qt::Unchecked;
    document->setAutoVisGroupVisible(static_cast<hammer::vmf::AutoVisGroup>(id), show);
    scheduleVisGroupTreeRebuild();
}

void MainWindow::handleVisGroupItemChanged(QTreeWidgetItem* item, int column)
{
    if (updatingVisGroupTree_ || !item || column != 0) return;
    MapDocumentWidget* document = activeDocument();
    if (!document) return;
    const int visGroupId = item->data(0, Qt::UserRole).toInt();
    if (visGroupId == 0) return;

    // The item carries both the check box and the editable name, and Qt sends
    // one itemChanged for either. Comparing the typed text against the stored
    // name tells a rename apart from a visibility toggle. The label is
    // "<name> (<member count>)", so the count is stripped back off first.
    const QString label = item->text(0);
    const int open = label.lastIndexOf(QLatin1Char('('));
    const QString typedName = open > 0 ? label.left(open).trimmed() : label.trimmed();
    bool renamed = false;
    for (const hammer::vmf::VisGroupDef& group : document->visGroups()) {
        if (group.id != visGroupId) continue;
        if (typedName != QString::fromStdString(group.name) && !typedName.isEmpty())
            renamed = document->renameVisGroup(visGroupId, typedName);
        break;
    }

    // A partially checked visgroup goes fully shown on the next click, matching
    // the original's tri-state toggle.
    //
    // Read the check state BEFORE calling into the document: setVisGroupVisible
    // republishes the scene, which fires editStateChanged, which schedules a
    // rebuild that will delete this very item.
    const bool show = item->checkState(0) != Qt::Unchecked;
    if (!renamed) document->setVisGroupVisible(visGroupId, show);
    scheduleVisGroupTreeRebuild();
}

void MainWindow::showNewVisGroupDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;
    const std::size_t count = document->selectionCount();
    if (count == 0) {
        QMessageBox::information(this, tr("Move Selection to VisGroup"),
                                 tr("No objects are selected."));
        return;
    }

    // hammer/NewVisGroupDlg.cpp: name the new group, or drop the selection into
    // one that already exists, with a "hide them now" option.
    //
    // Group-box sections and a bottom button box, matching the Material and
    // Model browsers and the other command dialogs.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New VisGroup"));
    dialog.setModal(true);
    dialog.resize(420, 300);
    auto* layout = new QVBoxLayout(&dialog);

    auto* destinationBox = new QGroupBox(tr("Destination"), &dialog);
    auto* destinationLayout = new QGridLayout(destinationBox);
    auto* createNew = new QRadioButton(tr("Create a &new VisGroup named:"), destinationBox);
    createNew->setChecked(true);
    auto* nameEdit = new QLineEdit(destinationBox);
    // The original defaults the name to the object count, which is what the
    // VDC's "A VisGroup is created named \"8 objects\"" describes.
    nameEdit->setText(tr("%1 objects").arg(static_cast<qulonglong>(count)));
    nameEdit->setPlaceholderText(tr("VisGroup name"));
    nameEdit->selectAll();
    auto* useExisting = new QRadioButton(tr("Add to &existing VisGroup:"), destinationBox);
    auto* existingCombo = new QComboBox(destinationBox);
    for (const hammer::vmf::VisGroupDef& group : document->visGroups()) {
        if (group.automatic) continue;
        existingCombo->addItem(QString::fromStdString(group.name), group.id);
    }
    existingCombo->setEnabled(false);
    useExisting->setEnabled(existingCombo->count() > 0);
    destinationLayout->addWidget(createNew, 0, 0);
    destinationLayout->addWidget(nameEdit, 0, 1);
    destinationLayout->addWidget(useExisting, 1, 0);
    destinationLayout->addWidget(existingCombo, 1, 1);
    destinationLayout->setColumnStretch(1, 1);
    layout->addWidget(destinationBox);

    auto* optionsBox = new QGroupBox(tr("Options"), &dialog);
    auto* optionsLayout = new QVBoxLayout(optionsBox);
    auto* hide = new QCheckBox(tr("&Hide the objects now"), optionsBox);
    hide->setChecked(true);
    hide->setToolTip(tr("Leaves the VisGroup unchecked in the Filter Control list, "
                        "so its objects stay hidden until it is ticked"));
    auto* exclusive = new QCheckBox(tr("&Remove them from all other VisGroups"), optionsBox);
    optionsLayout->addWidget(hide);
    optionsLayout->addWidget(exclusive);
    layout->addWidget(optionsBox);

    auto* countLabel = new QLabel(tr("%1 object(s) selected").arg(static_cast<qulonglong>(count)),
                                  &dialog);
    countLabel->setEnabled(false);
    layout->addWidget(countLabel);
    layout->addStretch(1);

    connect(createNew, &QRadioButton::toggled, existingCombo, [existingCombo](bool on) {
        existingCombo->setEnabled(!on);
    });
    connect(createNew, &QRadioButton::toggled, nameEdit, &QLineEdit::setEnabled);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    bool ok = false;
    QString name;
    if (createNew->isChecked()) {
        name = nameEdit->text().trimmed();
        if (name.isEmpty()) name = tr("%1 objects").arg(static_cast<qulonglong>(count));
        ok = document->createVisGroupFromSelection(name, hide->isChecked(),
                                                   exclusive->isChecked()) != 0;
    } else {
        const int visGroupId = existingCombo->currentData().toInt();
        name = existingCombo->currentText();
        ok = document->addSelectionToVisGroup(visGroupId, hide->isChecked(),
                                              exclusive->isChecked());
    }
    scheduleVisGroupTreeRebuild();
    statusBar()->showMessage(ok ? tr("Moved %1 object(s) to VisGroup \"%2\"")
                                      .arg(static_cast<qulonglong>(count))
                                      .arg(name)
                                : tr("The selection could not be moved to a VisGroup"),
                             4000);
}

void MainWindow::showEditVisGroupsDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;

    // hammer/editgroups.cpp CEditGroups: rename, recolor, delete, and create
    // empty visgroups. Deleting one never deletes its objects.
    //
    // Same shape as the Entity Report: a titled group box holding the list with
    // its action buttons down the right, and the dialog's button box at the
    // bottom.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Object Groups"));
    dialog.setModal(true);
    dialog.resize(520, 420);
    auto* layout = new QVBoxLayout(&dialog);

    auto* groupsBox = new QGroupBox(tr("VisGroups"), &dialog);
    auto* groupsLayout = new QHBoxLayout(groupsBox);
    auto* list = new QListWidget;
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    groupsLayout->addWidget(list, 1);

    auto* actions = new QVBoxLayout;
    auto* newGroup = new QPushButton(tr("&New Group"));
    auto* rename = new QPushButton(tr("&Rename..."));
    auto* color = new QPushButton(tr("&Color..."));
    auto* remove = new QPushButton(tr("&Delete"));
    for (QPushButton* button : {newGroup, rename, color, remove}) {
        button->setAutoDefault(false);
        actions->addWidget(button);
    }
    // Everything but New needs a selected group, exactly as the Entity Report's
    // buttons wait for a selected entity.
    for (QPushButton* button : {rename, color, remove}) button->setEnabled(false);
    actions->addStretch(1);
    groupsLayout->addLayout(actions);
    layout->addWidget(groupsBox, 1);

    auto* countLabel = new QLabel(&dialog);
    countLabel->setEnabled(false);
    layout->addWidget(countLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    connect(list, &QListWidget::itemSelectionChanged, &dialog,
            [list, rename, color, remove] {
                const bool hasSelection = list->currentItem() != nullptr;
                for (QPushButton* button : {rename, color, remove})
                    button->setEnabled(hasSelection);
            });

    const auto reload = [document, list, countLabel, rename, color, remove] {
        const int previous = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toInt() : 0;
        list->clear();
        for (const hammer::vmf::VisGroupDef& group : document->visGroups()) {
            if (group.automatic) continue;
            auto* item = new QListWidgetItem(
                QStringLiteral("%1 (%2)")
                    .arg(QString::fromStdString(group.name))
                    .arg(static_cast<qulonglong>(document->visGroupMemberCount(group.id))),
                list);
            item->setData(Qt::UserRole, group.id);
            QPixmap swatch(12, 12);
            swatch.fill(QColor(group.red, group.green, group.blue));
            item->setIcon(QIcon(swatch));
            if (group.id == previous) list->setCurrentItem(item);
        }
        countLabel->setText(list->count() == 0
                                ? QObject::tr("This map has no VisGroups yet")
                                : QObject::tr("%1 VisGroup(s)").arg(list->count()));
        const bool hasSelection = list->currentItem() != nullptr;
        for (QPushButton* button : {rename, color, remove}) button->setEnabled(hasSelection);
    };
    reload();

    const auto currentId = [list] {
        const QListWidgetItem* item = list->currentItem();
        return item ? item->data(Qt::UserRole).toInt() : 0;
    };
    const auto currentName = [document, currentId]() -> QString {
        for (const hammer::vmf::VisGroupDef& group : document->visGroups()) {
            if (group.id == currentId()) return QString::fromStdString(group.name);
        }
        return {};
    };

    connect(newGroup, &QPushButton::clicked, &dialog, [this, document, reload, &dialog] {
        bool accepted = false;
        const QString name = QInputDialog::getText(&dialog, tr("New Group"), tr("Group name:"),
                                                   QLineEdit::Normal, tr("New Group"), &accepted);
        if (!accepted || name.trimmed().isEmpty()) return;
        // An empty visgroup survives until something else purges it; the VDC
        // warns that a visgroup with no objects gets removed from the list.
        document->createEmptyVisGroup(name);
        reload();
        scheduleVisGroupTreeRebuild();
    });
    connect(rename, &QPushButton::clicked, &dialog,
            [this, document, reload, currentId, currentName, &dialog] {
                const int id = currentId();
                if (id == 0) return;
                bool accepted = false;
                const QString name = QInputDialog::getText(&dialog, tr("Rename Group"),
                                                           tr("Group name:"), QLineEdit::Normal,
                                                           currentName(), &accepted);
                if (!accepted || name.trimmed().isEmpty()) return;
                document->renameVisGroup(id, name);
                reload();
                scheduleVisGroupTreeRebuild();
            });
    connect(color, &QPushButton::clicked, &dialog,
            [this, document, reload, currentId, &dialog] {
                const int id = currentId();
                if (id == 0) return;
                QColor initial(192, 192, 192);
                for (const hammer::vmf::VisGroupDef& group : document->visGroups()) {
                    if (group.id == id) initial = QColor(group.red, group.green, group.blue);
                }
                const QColor picked = QColorDialog::getColor(initial, &dialog, tr("Group Color"));
                if (!picked.isValid()) return;
                document->setVisGroupColor(id, picked);
                reload();
                scheduleVisGroupTreeRebuild();
            });
    connect(remove, &QPushButton::clicked, &dialog,
            [this, document, reload, currentId, currentName, &dialog] {
                const int id = currentId();
                if (id == 0) return;
                if (QMessageBox::question(
                        &dialog, tr("Delete Group"),
                        tr("Delete the VisGroup \"%1\"?\n\nIts objects are not deleted - they "
                           "become individual unlinked objects and are made visible again.")
                            .arg(currentName())) != QMessageBox::Yes) {
                    return;
                }
                document->deleteVisGroup(id);
                reload();
                scheduleVisGroupTreeRebuild();
            });

    dialog.exec();
    scheduleVisGroupTreeRebuild();
}

void MainWindow::showQuickHideToVisGroupDialog()
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;
    const std::size_t hidden = document->quickHiddenCount();
    if (hidden == 0) {
        QMessageBox::information(this, tr("Convert QuickHide objects to VisGroup"),
                                 tr("There are no QuickHide objects to convert."));
        return;
    }
    // VDC: the new VisGroup is named "_FromQuickHide(<count>)". The prompt lets
    // that be replaced, which the original command does not offer.
    const QString suggested =
        QStringLiteral("_FromQuickHide(%1)").arg(static_cast<qulonglong>(hidden));
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, tr("Convert QuickHide objects to VisGroup"),
                              tr("VisGroup name for the %1 hidden object(s):")
                                  .arg(static_cast<qulonglong>(hidden)),
                              QLineEdit::Normal, suggested, &accepted);
    if (!accepted) return;
    const QString trimmed = name.trimmed().isEmpty() ? suggested : name.trimmed();
    const std::size_t tagged = document->quickHideConvertToVisGroup(trimmed);
    if (tagged == 0) {
        QMessageBox::warning(this, tr("Convert QuickHide objects to VisGroup"),
                             tr("None of the hidden objects could be tagged."));
        return;
    }
    // The new VisGroup now owns the hiding: it is left unchecked in the Filter
    // Control tree, and its checkbox - not Unhide QuickHide Objects - is what
    // brings the objects back.
    scheduleVisGroupTreeRebuild();
    statusBar()->showMessage(tr("Moved %1 object(s) to VisGroup \"%2\"")
                                 .arg(static_cast<qulonglong>(tagged))
                                 .arg(trimmed),
                             4000);
}

void MainWindow::showRayTracedGammaDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Ray-traced Gamma"));
    dialog.setModal(true);

    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(
        tr("Display gamma for the ray-traced 3D view, as mat_monitorgamma does "
           "in-engine. 2.2 matches the game; higher values lift the shadows."),
        &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);

    // The slider works in hundredths so it can land on 2.2 exactly.
    auto* row = new QHBoxLayout;
    layout->addLayout(row);
    auto* slider = new QSlider(Qt::Horizontal, &dialog);
    slider->setRange(50, 500);
    slider->setSingleStep(5);
    slider->setPageStep(20);
    slider->setValue(qRound(rayTracedGamma_ * 100.0f));
    auto* value = new QLabel(&dialog);
    value->setMinimumWidth(value->fontMetrics().horizontalAdvance(QStringLiteral("0.00")) * 2);
    row->addWidget(slider, 1);
    row->addWidget(value);

    const float originalGamma = rayTracedGamma_;
    const auto applyPreview = [this, value](int hundredths) {
        const float gamma = static_cast<float>(hundredths) / 100.0f;
        value->setText(QString::number(gamma, 'f', 2));
        setRayTracedGamma(gamma);
    };
    connect(slider, &QSlider::valueChanged, &dialog, applyPreview);
    applyPreview(slider->value());

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset,
        &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked,
            &dialog, [slider] { slider->setValue(220); });

    if (dialog.exec() != QDialog::Accepted) setRayTracedGamma(originalGamma);
}

void MainWindow::setWireframeOverlayEnabled(bool enabled)
{
    wireframeOverlayEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("render/wireframeOverlay3d"), enabled);
    if (wireframeOverlayAction_ && wireframeOverlayAction_->isChecked() != enabled) {
        QSignalBlocker blocker(wireframeOverlayAction_);
        wireframeOverlayAction_->setChecked(enabled);
    }
    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setWireframeOverlayEnabled(enabled);
        }
    }
    setPrompt(enabled ? tr("3D wireframe overlay enabled")
                      : tr("3D wireframe overlay disabled"));
}

// View > 2D X/Y, 2D Y/Z, 2D X/Z and the 3D render-mode commands (F2-F5 in
// CMapDoc) retype the highlighted pane rather than a fixed viewport, so any of
// the four can show any 2D axis pair or the camera.
void MainWindow::setActiveViewKind(MapViewWidget::Kind kind)
{
    MapDocumentWidget* document = activeDocument();
    if (!document) return;
    document->setActiveViewKind(kind);
    updateProjectionActions();
    updateEditActions();
}

void MainWindow::setTexturedRenderMode(MapViewWidget::TexturedRenderMode mode)
{
    texturedRenderMode_ = mode;
    QString storedMode = QStringLiteral("unlit");
    if (mode == MapViewWidget::TexturedRenderMode::Shaded) {
        storedMode = QStringLiteral("shaded");
    } else if (mode == MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons) {
        storedMode = QStringLiteral("shaded-material-polygons");
    } else if (mode == MapViewWidget::TexturedRenderMode::RayTracedPreview) {
        storedMode = QStringLiteral("ray-traced-preview");
    }
    QSettings().setValue(QStringLiteral("render/texturedMode"), storedMode);

    const auto setChecked = [](QAction* action, bool checked) {
        if (!action || action->isChecked() == checked) return;
        QSignalBlocker blocker(action);
        action->setChecked(checked);
    };
    setChecked(texturedViewAction_, mode == MapViewWidget::TexturedRenderMode::Unlit);
    setChecked(shadedTexturedViewAction_, mode == MapViewWidget::TexturedRenderMode::Shaded);
    setChecked(shadedMaterialPolygonsViewAction_,
               mode == MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons);
    setChecked(rayTracedPreviewAction_,
               mode == MapViewWidget::TexturedRenderMode::RayTracedPreview);

    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setTexturedRenderMode(mode);
        }
    }
    if (mode == MapViewWidget::TexturedRenderMode::Unlit) {
        setPrompt(tr("3D textured polygons"));
    } else if (mode == MapViewWidget::TexturedRenderMode::Shaded) {
        setPrompt(tr("3D textured shaded polygons"));
    } else if (mode == MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons) {
        setPrompt(tr("3D textured shaded + materials polygons"));
    } else {
        setPrompt(tr("3D Vulkan RTX ray-traced preview"));
    }
}

void MainWindow::setMaterialEffectsEnabled(bool phong, bool specular, bool bumpMaps,
                                           bool lightWarp, bool selfIllum, bool rimLight)
{
    phongEnabled_ = phong;
    specularEnabled_ = specular;
    bumpMapsEnabled_ = bumpMaps;
    lightWarpEnabled_ = lightWarp;
    selfIllumEnabled_ = selfIllum;
    rimLightEnabled_ = rimLight;
    QSettings settings;
    settings.setValue(QStringLiteral("render/materialPhong"), phong);
    settings.setValue(QStringLiteral("render/materialSpecular"), specular);
    settings.setValue(QStringLiteral("render/materialBumpMaps"), bumpMaps);
    settings.setValue(QStringLiteral("render/materialLightWarp"), lightWarp);
    settings.setValue(QStringLiteral("render/materialSelfIllum"), selfIllum);
    settings.setValue(QStringLiteral("render/materialRimLight"), rimLight);

    for (QMdiSubWindow* subWindow : mdiArea_->subWindowList()) {
        if (auto* document = qobject_cast<MapDocumentWidget*>(subWindow->widget())) {
            document->setMaterialEffectsEnabled(phong, specular, bumpMaps,
                                                lightWarp, selfIllum, rimLight);
        }
    }
    setPrompt(tr("Material effects: Phong %1, Specular %2, Bump Maps %3, "
                 "Lightwarp %4, Self Illumination %5, Rim Light %6")
                  .arg(phong ? tr("On") : tr("Off"))
                  .arg(specular ? tr("On") : tr("Off"))
                  .arg(bumpMaps ? tr("On") : tr("Off"))
                  .arg(lightWarp ? tr("On") : tr("Off"))
                  .arg(selfIllum ? tr("On") : tr("Off"))
                  .arg(rimLight ? tr("On") : tr("Off")));
}


void MainWindow::refreshEntityClasses()
{
    if (!objectCombo_) return;
    const QString previous = objectCombo_->currentText();
    QSignalBlocker blocker(objectCombo_);
    objectCombo_->clear();
    if (fgd_ && !fgd_->empty()) {
        std::vector<QString> names;
        for (const hammer::fgd::EntityClass* entityClass : fgd_->pointClasses()) {
            names.push_back(QString::fromStdString(entityClass->name));
        }
        std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
        for (const QString& name : names) objectCombo_->addItem(name);
    }
    if (objectCombo_->count() == 0) {
        objectCombo_->addItems({QStringLiteral("info_player_start"), QStringLiteral("light"), QStringLiteral("prop_static")});
    }
    int index = objectCombo_->findText(previous, Qt::MatchFixedString);
    if (index < 0) index = objectCombo_->findText(QStringLiteral("info_player_start"), Qt::MatchFixedString);
    objectCombo_->setCurrentIndex(index >= 0 ? index : 0);
}

void MainWindow::applyObjectBarSettings(MapDocumentWidget* document)
{
    if (!document) return;
    document->setFgdDatabase(fgd_);
    if (textureCombo_) document->setCurrentMaterial(textureCombo_->currentText());
    if (objectCombo_) document->setEntityClass(objectCombo_->currentText());
    document->setPrimitiveKindByName(primitiveName_);
    document->setPrimitiveFaces(primitiveFaces_);
}

void MainWindow::showAbout()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About"));
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);
    auto* splash = new QLabel;
    const QPixmap logo = loadHammerLogoPixmap();
    if (!logo.isNull()) {
        splash->setPixmap(logo.scaled(QSize(256, 256), Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
    }
    splash->setAlignment(Qt::AlignCenter);
    layout->addWidget(splash);
    auto* aboutText = new QLabel(
        tr("Hammer--\nVersion: 4.1.01\nCopyright (C) 1996-2005, Valve Corporation. All rights reserved."));
    aboutText->setAlignment(Qt::AlignCenter);
    layout->addWidget(aboutText);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showOptions()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Options"));
    dialog.resize(640, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* tabs = new QTabWidget;

    auto* generalForm = new QFormLayout;
    auto* undoLevels = new QSpinBox;
    undoLevels->setRange(0, 999);
    undoLevels->setValue(50);
    generalForm->addRow(tr("Undo levels:"), undoLevels);
    generalForm->addRow(new QCheckBox(tr("Load default window positions with maps")));
    generalForm->addRow(new QCheckBox(tr("Use independent window configurations")));
    auto* bspSourceEdit = new QLineEdit;
    bspSourceEdit->setObjectName(QStringLiteral("BspSourcePath"));
    bspSourceEdit->setPlaceholderText(tr("Select the BSPSource executable or .jar"));
    bspSourceEdit->setText(QSettings().value(QStringLiteral("editor/bspsourceExecutable")).toString());
    bspSourceEdit->setToolTip(tr("When configured, the Open dialog accepts .bsp files and decompiles them to VMF with BSPSource."));
    auto* browseBspSource = new QPushButton(tr("Browse…"));
    auto* bspSourceRow = new QHBoxLayout;
    bspSourceRow->addWidget(bspSourceEdit, 1);
    bspSourceRow->addWidget(browseBspSource);
    generalForm->addRow(tr("BSPSource executable:"), bspSourceRow);
    connect(browseBspSource, &QPushButton::clicked, &dialog,
            [bspSourceEdit, &dialog] {
        const QString startPath = bspSourceEdit->text().trimmed().isEmpty()
            ? QDir::homePath() : QFileInfo(bspSourceEdit->text().trimmed()).absolutePath();
        // Hold the preview off the GUI thread while the native chooser is up.
        const hammer::app::PreviewRenderSuspension suspendPreview;
        const QString selected = QFileDialog::getOpenFileName(
            &dialog, tr("Select BSPSource Executable"), startPath,
            tr("BSPSource (bspsrc* *.jar);;All Files (*.*)"));
        if (!selected.isEmpty()) bspSourceEdit->setText(QFileInfo(selected).absoluteFilePath());
    });
    tabs->addTab(wrapWithLayout(generalForm), tr("General"));

    // Build Programs tab: named variables mapping to full command strings.
    // Run Map steps reference them as $NAME, so wine prefixes and env vars
    // belong right in the command.
    auto* buildLayout = new QVBoxLayout;
    auto* buildTable = new QTableWidget(0, 2);
    buildTable->setHorizontalHeaderLabels({tr("Variable"), tr("Command")});
    buildTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    buildTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    buildTable->verticalHeader()->setVisible(false);
    const auto addProgramRow = [buildTable](const QString& name, const QString& command) {
        const int row = buildTable->rowCount();
        buildTable->insertRow(row);
        buildTable->setItem(row, 0, new QTableWidgetItem(name));
        buildTable->setItem(row, 1, new QTableWidgetItem(command));
    };
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("buildPrograms"));
        QStringList names = settings.childKeys();
        if (names.isEmpty()) {
            // Seed from the previous fixed-slot settings, or with the
            // conventional names so the default Run Map steps line up.
            const std::pair<QString, QString> seeds[] = {
                {QStringLiteral("VBSP"), QStringLiteral("editor/vbspExecutable")},
                {QStringLiteral("VVIS"), QStringLiteral("editor/vvisExecutable")},
                {QStringLiteral("VRAD"), QStringLiteral("editor/vradExecutable")},
                {QStringLiteral("GAME"), QStringLiteral("editor/gameExecutable")},
            };
            settings.endGroup();
            // Variables now expand verbatim into the shell command, so quote
            // what the old fixed slots quoted automatically: a bare existing
            // path, or the path part of "wine /path/tool.exe".
            const auto quoteLegacy = [](const QString& value) -> QString {
                if (value.isEmpty() || value.contains(QLatin1Char('"'))) return value;
                if (QFileInfo::exists(value)) return QLatin1Char('"') + value + QLatin1Char('"');
                const qsizetype space = value.indexOf(QLatin1Char(' '));
                if (space > 0 && QFileInfo::exists(value.mid(space + 1).trimmed())) {
                    return value.left(space) + QStringLiteral(" \"") +
                           value.mid(space + 1).trimmed() + QLatin1Char('"');
                }
                return value;
            };
            for (const auto& [name, legacyKey] : seeds) {
                addProgramRow(name, quoteLegacy(settings.value(legacyKey).toString().trimmed()));
            }
        } else {
            names.sort(Qt::CaseInsensitive);
            for (const QString& name : names) {
                addProgramRow(name, settings.value(name).toString());
            }
            settings.endGroup();
        }
    }
    buildLayout->addWidget(buildTable, 1);
    auto* buildButtons = new QHBoxLayout;
    auto* addProgram = new QPushButton(tr("Add"));
    auto* removeProgram = new QPushButton(tr("Remove"));
    buildButtons->addWidget(addProgram);
    buildButtons->addWidget(removeProgram);
    buildButtons->addStretch();
    buildLayout->addLayout(buildButtons);
    connect(addProgram, &QPushButton::clicked, &dialog, [addProgramRow] {
        addProgramRow(QString(), QString());
    });
    connect(removeProgram, &QPushButton::clicked, &dialog, [buildTable] {
        const int row = buildTable->currentRow();
        if (row >= 0) buildTable->removeRow(row);
    });
    auto* buildHint = new QLabel(
        tr("Run Map steps (F9) reference these as $NAME. Commands run through bash, so "
           "launcher prefixes and environment variables work, e.g.\n"
           "VBSP  →  WINEPREFIX=~/.wine wine \"/path/to/vbsp.exe\""));
    buildHint->setEnabled(false);
    buildHint->setWordWrap(true);
    buildLayout->addWidget(buildHint);
    tabs->addTab(wrapWithLayout(buildLayout), tr("Build Programs"));

    auto* twoD = new QVBoxLayout;
    twoD->addWidget(new QCheckBox(tr("Display grid")));
    twoD->addWidget(new QCheckBox(tr("Highlight active viewport")));
    twoD->addWidget(new QCheckBox(tr("Show entity names")));
    twoD->addStretch();
    tabs->addTab(wrapWithLayout(twoD), tr("2D Views"));

    auto* threeD = new QVBoxLayout;
    threeD->addWidget(new QCheckBox(tr("Reverse mouse Y axis")));
    threeD->addWidget(new QCheckBox(tr("Use mouse look navigation")));
    auto* renderMaterials = new QCheckBox(tr("Render materials in 3D views"));
    renderMaterials->setChecked(materialRenderingEnabled_);
    renderMaterials->setToolTip(tr("Loads VMT/VTF materials through the configured gameinfo.txt search paths and VPK archives."));
    threeD->addWidget(renderMaterials);
    // Material effects used by the shaded + materials and ray-traced views.
    auto* materialEffects = new QGroupBox(tr("Material Effects"));
    auto* materialEffectsLayout = new QVBoxLayout(materialEffects);
    const auto addEffect = [materialEffectsLayout](const QString& text, bool checked) {
        auto* box = new QCheckBox(text);
        box->setChecked(checked);
        materialEffectsLayout->addWidget(box);
        return box;
    };
    auto* phongBox = addEffect(tr("Phong"), phongEnabled_);
    auto* specularBox = addEffect(tr("Specular"), specularEnabled_);
    auto* bumpMapsBox = addEffect(tr("Bump Maps"), bumpMapsEnabled_);
    auto* lightWarpBox = addEffect(tr("Lightwarp"), lightWarpEnabled_);
    auto* selfIllumBox = addEffect(tr("Self Illumination"), selfIllumEnabled_);
    auto* rimLightBox = addEffect(tr("Rim Light"), rimLightEnabled_);
    threeD->addWidget(materialEffects);
    threeD->addWidget(new QLabel(tr("Back clipping plane:")));
    auto* clip = new QSpinBox;
    clip->setRange(256, 65536);
    clip->setValue(4096);
    threeD->addWidget(clip);
    threeD->addStretch();
    tabs->addTab(wrapWithLayout(threeD), tr("3D Views"));

    auto* textures = new QVBoxLayout;
    textures->addWidget(new QLabel(tr("Texture browser thumbnail size:")));
    auto* thumbnailSize = new QComboBox;
    thumbnailSize->addItem(tr("32 x 32"), 32);
    thumbnailSize->addItem(tr("64 x 64"), 64);
    thumbnailSize->addItem(tr("128 x 128"), 128);
    thumbnailSize->addItem(tr("256 x 256"), 256);
    const int savedThumbnailSize = textureBrowserThumbnailSize();
    thumbnailSize->setCurrentIndex(std::max(0, thumbnailSize->findData(savedThumbnailSize)));
    textures->addWidget(thumbnailSize);
    auto* animateTextures = new QCheckBox(tr("Animate textures"));
    animateTextures->setChecked(textureBrowserAnimationsEnabled());
    animateTextures->setToolTip(tr("Animates water and other preview-capable materials in the visual browser."));
    textures->addWidget(animateTextures);
    textures->addStretch();
    tabs->addTab(wrapWithLayout(textures), tr("Textures"));

    auto* gamePage = new QWidget;
    auto* gameLayout = new QVBoxLayout(gamePage);
    auto* sourceGame = new QGroupBox(tr("Source Game"));
    auto* sourceGameLayout = new QGridLayout(sourceGame);
    auto* gameDirectoryEdit = new QLineEdit;
    gameDirectoryEdit->setObjectName(QStringLiteral("GameDirectoryPath"));
    gameDirectoryEdit->setPlaceholderText(tr("Select the directory containing gameinfo.txt"));
    if (!loadedGameInfoPath_.isEmpty()) {
        gameDirectoryEdit->setText(QFileInfo(loadedGameInfoPath_).absolutePath());
    } else {
        gameDirectoryEdit->setText(QSettings().value(QStringLiteral("game/gameDirectory")).toString());
    }
    auto* browseGameDirectory = new QPushButton(tr("Browse…"));
    auto* gameInfoPath = new QLabel;
    gameInfoPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gameInfoPath->setWordWrap(true);
    auto* gameStatus = new QLabel;
    gameStatus->setWordWrap(true);

    sourceGameLayout->addWidget(new QLabel(tr("Game directory:")), 0, 0);
    sourceGameLayout->addWidget(gameDirectoryEdit, 0, 1);
    sourceGameLayout->addWidget(browseGameDirectory, 0, 2);
    sourceGameLayout->addWidget(new QLabel(tr("Configuration file:")), 1, 0);
    sourceGameLayout->addWidget(gameInfoPath, 1, 1, 1, 2);
    sourceGameLayout->addWidget(gameStatus, 2, 0, 1, 3);
    sourceGameLayout->setColumnStretch(1, 1);

    auto updateGameStatus = [=, this] {
        const QString directory = QDir::cleanPath(gameDirectoryEdit->text().trimmed());
        if (directory.isEmpty()) {
            gameInfoPath->setText(tr("Not configured"));
            gameStatus->setText(tr("Choose a Source game directory. Hammer-- will read SearchPaths from gameinfo.txt, resolve Steam AppIDs, and mount loose files and VPK archives for the material renderer."));
            return;
        }
        const QString file = QDir(directory).filePath(QStringLiteral("gameinfo.txt"));
        gameInfoPath->setText(file);
        if (QFileInfo::exists(file)) {
            const bool active = !loadedGameInfoPath_.isEmpty() &&
                                QFileInfo(loadedGameInfoPath_).absoluteFilePath() == QFileInfo(file).absoluteFilePath();
            gameStatus->setText(active && gameFileSystem_
                ? tr("Active. %1").arg(QString::fromStdString(gameFileSystem_->summary()))
                : tr("gameinfo.txt found. Click Apply or OK to mount its search paths and enable material lookup."));
        } else {
            gameStatus->setText(tr("No gameinfo.txt was found in this directory."));
        }
    };
    connect(gameDirectoryEdit, &QLineEdit::textChanged, &dialog, [updateGameStatus] { updateGameStatus(); });
    connect(browseGameDirectory, &QPushButton::clicked, &dialog,
            [this, gameDirectoryEdit, &dialog] {
        const QString startPath = gameDirectoryEdit->text().trimmed().isEmpty()
            ? QDir::homePath() : gameDirectoryEdit->text().trimmed();
        // Hold the preview off the GUI thread while the native chooser is up.
        const hammer::app::PreviewRenderSuspension suspendPreview;
        const QString selected = QFileDialog::getExistingDirectory(
            &dialog, tr("Configure Game Directory"), startPath,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!selected.isEmpty()) gameDirectoryEdit->setText(QDir::cleanPath(selected));
    });
    updateGameStatus();

    auto* gameData = new QGroupBox(tr("Game Data"));
    auto* gameDataLayout = new QGridLayout(gameData);
    auto* fgdPathEdit = new QLineEdit;
    fgdPathEdit->setObjectName(QStringLiteral("FgdPath"));
    fgdPathEdit->setPlaceholderText(tr("Select the .fgd file that defines the game's entities"));
    fgdPathEdit->setText(!loadedFgdPath_.isEmpty()
        ? loadedFgdPath_ : QSettings().value(QStringLiteral("gameData/lastFgd")).toString());
    auto* browseFgd = new QPushButton(tr("Browse…"));
    auto* fgdStatus = new QLabel;
    fgdStatus->setWordWrap(true);
    fgdStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gameDataLayout->addWidget(new QLabel(tr("FGD file:")), 0, 0);
    gameDataLayout->addWidget(fgdPathEdit, 0, 1);
    gameDataLayout->addWidget(browseFgd, 0, 2);
    gameDataLayout->addWidget(fgdStatus, 1, 0, 1, 3);
    gameDataLayout->setColumnStretch(1, 1);

    auto updateFgdStatus = [=, this] {
        const QString path = fgdPathEdit->text().trimmed();
        if (path.isEmpty()) {
            fgdStatus->setText(tr("No FGD selected. Point entities will use generic markers and SmartEdit will only expose raw VMF keys."));
            return;
        }
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            fgdStatus->setText(tr("The selected FGD file does not exist."));
            return;
        }
        const bool active = !loadedFgdPath_.isEmpty() &&
            QFileInfo(loadedFgdPath_).absoluteFilePath() == info.absoluteFilePath();
        fgdStatus->setText(active && fgd_ && !fgd_->empty()
            ? tr("Active. %1 entity classes are loaded. FGD colors, sizes, descriptions, models, sprites, and SmartEdit definitions are available.")
                  .arg(static_cast<qulonglong>(fgd_->classes().size()))
            : tr("FGD found. Click Apply or OK to load its entity definitions."));
    };
    connect(fgdPathEdit, &QLineEdit::textChanged, &dialog, [updateFgdStatus] { updateFgdStatus(); });
    connect(browseFgd, &QPushButton::clicked, &dialog,
            [this, fgdPathEdit, &dialog] {
        const QString startPath = fgdPathEdit->text().trimmed().isEmpty()
            ? (!loadedGameInfoPath_.isEmpty() ? QFileInfo(loadedGameInfoPath_).absolutePath() : QDir::homePath())
            : QFileInfo(fgdPathEdit->text().trimmed()).absolutePath();
        // Hold the preview off the GUI thread while the native chooser is up.
        const hammer::app::PreviewRenderSuspension suspendPreview;
        const QString selected = QFileDialog::getOpenFileName(
            &dialog, tr("Select Game Data"), startPath,
            tr("Forge Game Data (*.fgd);;All Files (*.*)"));
        if (!selected.isEmpty()) fgdPathEdit->setText(QFileInfo(selected).absoluteFilePath());
    });
    updateFgdStatus();

    gameLayout->addWidget(sourceGame);
    gameLayout->addWidget(gameData);
    auto* explanation = new QLabel(tr(
        "The game directory supplies VMT/VTF assets through gameinfo.txt. The selected FGD supplies point-entity classes, "
        "SmartEdit properties, editor colors, bounding sizes, and model or sprite hints."));
    explanation->setWordWrap(true);
    gameLayout->addWidget(explanation);
    gameLayout->addStretch();
    tabs->addTab(gamePage, tr("Game Configurations"));

    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    layout->addWidget(buttons);

    auto applySettings = [=, this]() -> bool {
        // Persist the plain-value settings before the game/FGD loads below:
        // their early returns must not throw away the BSPSource path or the
        // texture preferences the user just entered.
        {
            QSettings settings;
            settings.setValue(QStringLiteral("textures/browserThumbnailSize"), thumbnailSize->currentData().toInt());
            settings.setValue(QStringLiteral("textures/animatePreviews"), animateTextures->isChecked());
            settings.setValue(QStringLiteral("editor/bspsourceExecutable"), bspSourceEdit->text().trimmed());
            settings.remove(QStringLiteral("buildPrograms"));
            settings.beginGroup(QStringLiteral("buildPrograms"));
            for (int row = 0; row < buildTable->rowCount(); ++row) {
                const QTableWidgetItem* nameItem = buildTable->item(row, 0);
                const QTableWidgetItem* commandItem = buildTable->item(row, 1);
                const QString name = nameItem ? nameItem->text().trimmed() : QString();
                const QString command = commandItem ? commandItem->text().trimmed() : QString();
                if (!name.isEmpty() && !command.isEmpty()) settings.setValue(name, command);
            }
            settings.endGroup();
        }
        const QString directory = QDir::cleanPath(gameDirectoryEdit->text().trimmed());
        if (!directory.isEmpty()) {
            const QString requestedGameInfo = QDir(directory).filePath(QStringLiteral("gameinfo.txt"));
            const bool alreadyLoaded = !loadedGameInfoPath_.isEmpty() &&
                QFileInfo(loadedGameInfoPath_).absoluteFilePath() == QFileInfo(requestedGameInfo).absoluteFilePath();
            if (!alreadyLoaded && !loadGameInfoPath(directory, true)) {
                tabs->setCurrentWidget(gamePage);
                return false;
            }
        }
        const QString requestedFgd = fgdPathEdit->text().trimmed();
        if (!requestedFgd.isEmpty()) {
            const bool alreadyLoaded = !loadedFgdPath_.isEmpty() &&
                QFileInfo(loadedFgdPath_).absoluteFilePath() == QFileInfo(requestedFgd).absoluteFilePath();
            if (!alreadyLoaded && !loadFgdPath(requestedFgd, true)) {
                tabs->setCurrentWidget(gamePage);
                return false;
            }
        }
        setMaterialRenderingEnabled(renderMaterials->isChecked());
        setMaterialEffectsEnabled(phongBox->isChecked(), specularBox->isChecked(),
                                  bumpMapsBox->isChecked(), lightWarpBox->isChecked(),
                                  selfIllumBox->isChecked(), rimLightBox->isChecked());
        updateGameStatus();
        updateFgdStatus();
        return true;
    };

    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [applySettings] {
        applySettings();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, applySettings] {
        if (applySettings()) dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

