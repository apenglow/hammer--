#include "MapDocumentWidget.hpp"

#include "Camera3D.hpp"
#include "ObjectPropertiesDialog.hpp"
#include "VmfProjectedSurfaces.hpp"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QPainter>
#include <QTimer>
#include <QPaintEvent>
#include <QPalette>
#include <QPolygonF>
#include <QListWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include "PreviewRenderGate.hpp"

#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QFormLayout>
#include <QFrame>
#include <QLayout>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
std::filesystem::path toFilesystemPath(const QString& path)
{
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}

QString objectTypeName(hammer::vmf::ObjectType type)
{
    return type == hammer::vmf::ObjectType::Solid ? QObject::tr("solid") : QObject::tr("entity");
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int parseInteger(const QString& value, int fallback = 0)
{
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? parsed : fallback;
}

QString propertyValue(const std::vector<hammer::vmf::Property>& properties, const std::string& key)
{
    const std::string wanted = lower(key);
    for (const auto& property : properties) {
        if (lower(property.key) == wanted) return QString::fromStdString(property.value);
    }
    return {};
}

std::array<double, 3> parseTriple(const QString& value)
{
    std::array<double, 3> result{0.0, 0.0, 0.0};
    const QStringList fields = value.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int index = 0; index < 3 && index < fields.size(); ++index) {
        bool ok = false;
        const double number = fields[index].toDouble(&ok);
        if (ok) result[static_cast<std::size_t>(index)] = number;
    }
    return result;
}


class ModelPreviewWidget final : public QWidget
{
public:
    explicit ModelPreviewWidget(std::shared_ptr<hammer::assets::MaterialSystem> materials,
                                std::shared_ptr<hammer::assets::StudioModelSystem> models,
                                MapViewWidget::TexturedRenderMode texturedRenderMode,
                                bool phongEnabled, bool specularEnabled, bool bumpMapsEnabled,
                                bool lightWarpEnabled, bool selfIllumEnabled, bool rimLightEnabled,
                                float phongIntensity, float specularIntensity, float bumpMapIntensity,
                                std::string skyName,
                                QWidget* parent = nullptr)
        : QWidget(parent), materials_(std::move(materials)), models_(std::move(models)),
          skyName_(std::move(skyName))
    {
        setMinimumSize(320, 320);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        viewport_ = new MapViewWidget(MapViewWidget::Kind::Perspective, this);
        viewport_->setGridVisible(false);
        viewport_->setMaterialSystem(materials_);
        viewport_->setMaterialRenderingEnabled(true);
        viewport_->setTexturedRenderMode(texturedRenderMode);
        viewport_->setMaterialEffectsEnabled(phongEnabled, specularEnabled, bumpMapsEnabled,
                                             lightWarpEnabled, selfIllumEnabled, rimLightEnabled);
        viewport_->setMaterialEffectIntensities(phongIntensity, specularIntensity, bumpMapIntensity);
        layout->addWidget(viewport_);
    }

    void setSkin(int skin)
    {
        const int normalized = std::max(0, skin);
        if (skin_ == normalized) return;
        skin_ = normalized;
        rebuildScene();
    }

    void setSequence(int sequence)
    {
        const int normalized = std::max(-1, sequence);
        if (sequence_ == normalized) return;
        sequence_ = normalized;
        rebuildScene();
    }

    void setPlaying(bool playing)
    {
        if (playing_ == playing) return;
        playing_ = playing;
        rebuildScene();
    }

    void setPlaybackRate(double rate)
    {
        const double normalized = std::clamp(rate, -16.0, 16.0);
        if (std::abs(playbackRate_ - normalized) < 1e-9) return;
        playbackRate_ = normalized;
        rebuildScene();
    }

    void setModelPath(const QString& path)
    {
        const QString normalized = path.trimmed();
        if (path_ == normalized && scene_) return;
        path_ = normalized;
        rebuildScene();
    }

private:
    void rebuildScene()
    {
        scene_ = std::make_shared<hammer::vmf::Scene>();
        scene_->invalidateLineage();
        scene_->skyName = skyName_;
        if (models_ && !path_.isEmpty()) {
            const auto model = models_->model(path_.toUtf8().toStdString());
            if (model && model->valid) {
                hammer::vmf::EntityMarker entity;
                entity.id = 1;
                entity.object = {hammer::vmf::ObjectType::Entity, 1};
                entity.classname = "prop_dynamic";
                entity.model = path_.toUtf8().toStdString();
                entity.skin = model->normalizedSkin(skin_);
                if (sequence_ >= 0 && model->sequenceCount() > 0) {
                    entity.animationSequenceIndex = model->normalizedSequence(sequence_);
                    entity.animationSequence = model->sequences[
                        static_cast<std::size_t>(entity.animationSequenceIndex)].label;
                    entity.animationPlaybackRate = playbackRate_;
                    entity.animationCycle = playing_ ? -1.0 : 0.0;
                    entity.animateModel = true;
                }
                entity.sizeMinimum = {model->minimum[0], model->minimum[1], model->minimum[2]};
                entity.sizeMaximum = {model->maximum[0], model->maximum[1], model->maximum[2]};
                scene_->entities.push_back(std::move(entity));
                scene_->minimum = {model->minimum[0], model->minimum[1], model->minimum[2]};
                scene_->maximum = {model->maximum[0], model->maximum[1], model->maximum[2]};
                scene_->hasBounds = true;
            }
        }
        viewport_->setScene(scene_, true);
    }

    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    std::shared_ptr<hammer::assets::StudioModelSystem> models_;
    std::shared_ptr<hammer::vmf::Scene> scene_;
    MapViewWidget* viewport_{nullptr};
    QString path_;
    int skin_{0};
    int sequence_{-1};
    bool playing_{true};
    double playbackRate_{1.0};
    std::string skyName_;
};

struct ModelBrowserSelection
{
    QString path;
    int skin{0};
    QString sequence;
    double playbackRate{1.0};
};

std::optional<ModelBrowserSelection> browseModel(QWidget* parent,
                                   const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                                   const std::shared_ptr<hammer::assets::StudioModelSystem>& studioModels,
                                   MapViewWidget::TexturedRenderMode texturedRenderMode,
                                   bool phongEnabled, bool specularEnabled, bool bumpMapsEnabled,
                                   bool lightWarpEnabled, bool selfIllumEnabled, bool rimLightEnabled,
                                   float phongIntensity, float specularIntensity, float bumpMapIntensity,
                                   const std::string& skyName,
                                   const QString& current, int currentSkin,
                                   const QString& currentSequence,
                                   double currentPlaybackRate)
{
    if (!materials || !materials->fileSystem()) {
        QMessageBox::information(parent, QObject::tr("Model Browser"),
                                 QObject::tr("Configure a game directory containing gameinfo.txt before browsing models."));
        return std::nullopt;
    }

    std::vector<std::string> files = materials->fileSystem()->listFiles("models/", ".mdl");
    if (files.empty()) {
        QMessageBox::information(parent, QObject::tr("Model Browser"),
                                 QObject::tr("No Source studio models were found in the mounted game content."));
        return std::nullopt;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Model Browser"));
    dialog.resize(1040, 740);
    auto* layout = new QVBoxLayout(&dialog);

    // Group-box sections, matching the Face Edit Sheet's page style.
    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* modelsBox = new QGroupBox(QObject::tr("Models"), splitter);
    auto* modelsLayout = new QVBoxLayout(modelsBox);
    auto* filter = new QLineEdit(modelsBox);
    filter->setPlaceholderText(QObject::tr("Filter mounted models…"));
    modelsLayout->addWidget(filter);
    auto* list = new QListWidget(modelsBox);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setUniformItemSizes(true);
    modelsLayout->addWidget(list, 1);
    auto* previewPage = new QGroupBox(QObject::tr("Preview"), splitter);
    auto* previewLayout = new QVBoxLayout(previewPage);
    auto* preview = new ModelPreviewWidget(materials, studioModels, texturedRenderMode,
                                           phongEnabled, specularEnabled, bumpMapsEnabled,
                                           lightWarpEnabled, selfIllumEnabled, rimLightEnabled,
                                           phongIntensity, specularIntensity, bumpMapIntensity,
                                           skyName, previewPage);

    auto* skinRow = new QWidget(previewPage);
    auto* skinLayout = new QHBoxLayout(skinRow);
    skinLayout->setContentsMargins(0, 0, 0, 0);
    skinLayout->addWidget(new QLabel(QObject::tr("Skin:"), skinRow));
    auto* skinCombo = new QComboBox(skinRow);
    skinCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    skinLayout->addWidget(skinCombo, 1);

    auto* animationRow = new QWidget(previewPage);
    auto* animationLayout = new QHBoxLayout(animationRow);
    animationLayout->setContentsMargins(0, 0, 0, 0);
    animationLayout->addWidget(new QLabel(QObject::tr("Sequence:"), animationRow));
    auto* sequenceCombo = new QComboBox(animationRow);
    sequenceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    animationLayout->addWidget(sequenceCombo, 1);
    auto* playAnimation = new QCheckBox(QObject::tr("Play"), animationRow);
    playAnimation->setChecked(true);
    animationLayout->addWidget(playAnimation);
    auto* animationRate = new QDoubleSpinBox(animationRow);
    animationRate->setRange(-16.0, 16.0);
    animationRate->setDecimals(2);
    animationRate->setSingleStep(0.1);
    animationRate->setValue(std::clamp(currentPlaybackRate, -16.0, 16.0));
    animationRate->setSuffix(QObject::tr("×"));
    animationRate->setToolTip(QObject::tr("Animation playback rate"));
    animationLayout->addWidget(animationRate);

    auto* name = new QLabel(previewPage);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    name->setWordWrap(true);
    auto* info = new QLabel(previewPage);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info->setWordWrap(true);
    previewLayout->addWidget(preview, 1);
    auto* playbackBox = new QGroupBox(QObject::tr("Playback"), previewPage);
    auto* playbackLayout = new QVBoxLayout(playbackBox);
    playbackLayout->addWidget(skinRow);
    playbackLayout->addWidget(animationRow);
    previewLayout->addWidget(playbackBox);
    previewLayout->addWidget(name);
    previewLayout->addWidget(info);
    splitter->addWidget(modelsBox);
    splitter->addWidget(previewPage);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    for (const std::string& file : files) {
        const QString path = QString::fromStdString(file);
        auto* item = new QListWidgetItem(path, list);
        item->setData(Qt::UserRole, path);
    }

    const auto selectedSkin = std::make_shared<int>(std::max(0, currentSkin));
    const auto selectedSequence = std::make_shared<QString>(currentSequence.trimmed());
    const auto updatePreview = [=](QListWidgetItem* item) {
        const QString path = item ? item->data(Qt::UserRole).toString() : QString{};
        preview->setModelPath(path);
        name->setText(path);
        if (!item || !studioModels) {
            QSignalBlocker skinBlocker(skinCombo);
            QSignalBlocker sequenceBlocker(sequenceCombo);
            skinCombo->clear();
            sequenceCombo->clear();
            skinCombo->setEnabled(false);
            sequenceCombo->setEnabled(false);
            playAnimation->setEnabled(false);
            animationRate->setEnabled(false);
            info->clear();
            return;
        }
        const auto model = studioModels->model(path.toUtf8().toStdString());
        if (!model || !model->valid) {
            QSignalBlocker skinBlocker(skinCombo);
            QSignalBlocker sequenceBlocker(sequenceCombo);
            skinCombo->clear();
            sequenceCombo->clear();
            skinCombo->setEnabled(false);
            sequenceCombo->setEnabled(false);
            playAnimation->setEnabled(false);
            animationRate->setEnabled(false);
            info->setText(model ? QString::fromStdString(model->error) : QObject::tr("Unable to load model."));
            return;
        }
        {
            QSignalBlocker blocker(skinCombo);
            skinCombo->clear();
            for (int skin = 0; skin < model->skinCount(); ++skin) {
                QStringList materialsForSkin;
                if (skin < static_cast<int>(model->skinFamilies.size())) {
                    for (const std::string& material : model->skinFamilies[static_cast<std::size_t>(skin)]) {
                        if (material.empty()) continue;
                        const QString display = QString::fromStdString(material);
                        if (!materialsForSkin.contains(display)) materialsForSkin.push_back(display);
                        if (materialsForSkin.size() >= 3) break;
                    }
                }
                const QString label = materialsForSkin.empty()
                    ? QObject::tr("Skin %1").arg(skin)
                    : QObject::tr("Skin %1 — %2").arg(skin).arg(materialsForSkin.join(QStringLiteral(", ")));
                skinCombo->addItem(label, skin);
            }
            *selectedSkin = model->normalizedSkin(*selectedSkin);
            skinCombo->setCurrentIndex(std::max(0, skinCombo->findData(*selectedSkin)));
            skinCombo->setEnabled(model->skinCount() > 1);
        }
        {
            QSignalBlocker blocker(sequenceCombo);
            sequenceCombo->clear();
            sequenceCombo->addItem(QObject::tr("Reference pose / no animation"), -1);
            for (int sequence = 0; sequence < model->sequenceCount(); ++sequence) {
                const auto& metadata = model->sequences[static_cast<std::size_t>(sequence)];
                QString label = QString::fromStdString(metadata.label);
                if (metadata.duration > 0.0f)
                    label += QObject::tr(" — %1 s, %2 fps")
                        .arg(metadata.duration, 0, 'f', 2).arg(metadata.fps, 0, 'f', 1);
                if (metadata.looping) label += QObject::tr(" — loop");
                sequenceCombo->addItem(label, sequence);
            }
            const int sequenceIndex = model->sequenceIndex(selectedSequence->toUtf8().toStdString());
            if (sequenceIndex >= 0 && model->sequenceCount() > 0) {
                *selectedSequence = QString::fromStdString(
                    model->sequences[static_cast<std::size_t>(sequenceIndex)].label);
                sequenceCombo->setCurrentIndex(sequenceIndex + 1);
            } else {
                selectedSequence->clear();
                sequenceCombo->setCurrentIndex(0);
            }
            sequenceCombo->setEnabled(model->sequenceCount() > 0);
            playAnimation->setEnabled(sequenceIndex >= 0);
            animationRate->setEnabled(sequenceIndex >= 0);
            preview->setSequence(sequenceIndex);
        }
        preview->setSkin(*selectedSkin);
        preview->setPlaying(playAnimation->isChecked());
        preview->setPlaybackRate(animationRate->value());
        std::size_t triangleCount = 0;
        for (const auto& mesh : model->meshes) triangleCount += mesh.vertices.size() / 3;
        info->setText(QObject::tr("%1 mesh(es), %2 triangle(s), %3 skin family/families, %4 sequence(s)\nBounds: %5 %6 %7 to %8 %9 %10")
            .arg(static_cast<qulonglong>(model->meshes.size()))
            .arg(static_cast<qulonglong>(triangleCount))
            .arg(model->skinCount()).arg(model->sequenceCount())
            .arg(model->minimum[0]).arg(model->minimum[1]).arg(model->minimum[2])
            .arg(model->maximum[0]).arg(model->maximum[1]).arg(model->maximum[2]));
    };
    QObject::connect(list, &QListWidget::currentItemChanged, &dialog,
                     [updatePreview](QListWidgetItem* item, QListWidgetItem*) { updatePreview(item); });
    QObject::connect(skinCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
                     [=](int index) {
        if (index < 0) return;
        *selectedSkin = skinCombo->itemData(index).toInt();
        preview->setSkin(*selectedSkin);
    });
    QObject::connect(sequenceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
                     [=](int index) {
        if (index < 0) return;
        const int sequence = sequenceCombo->itemData(index).toInt();
        if (sequence < 0) {
            selectedSequence->clear();
            playAnimation->setEnabled(false);
            animationRate->setEnabled(false);
            preview->setSequence(-1);
            return;
        }
        if (list->currentItem() && studioModels) {
            const auto model = studioModels->model(
                list->currentItem()->data(Qt::UserRole).toString().toUtf8().toStdString());
            if (model && sequence >= 0 && sequence < model->sequenceCount())
                *selectedSequence = QString::fromStdString(
                    model->sequences[static_cast<std::size_t>(sequence)].label);
        }
        playAnimation->setEnabled(true);
        animationRate->setEnabled(true);
        preview->setSequence(sequence);
    });
    QObject::connect(playAnimation, &QCheckBox::toggled, &dialog,
                     [=](bool playing) { preview->setPlaying(playing); });
    QObject::connect(animationRate, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog,
                     [=](double rate) { preview->setPlaybackRate(rate); });
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog,
                     [&dialog](QListWidgetItem*) { dialog.accept(); });
    QObject::connect(filter, &QLineEdit::textChanged, &dialog, [=](const QString& text) {
        QListWidgetItem* firstVisible = nullptr;
        for (int index = 0; index < list->count(); ++index) {
            QListWidgetItem* item = list->item(index);
            const bool visible = item->data(Qt::UserRole).toString().contains(text, Qt::CaseInsensitive);
            item->setHidden(!visible);
            if (visible && !firstVisible) firstVisible = item;
        }
        if (!list->currentItem() || list->currentItem()->isHidden()) list->setCurrentItem(firstVisible);
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QString normalizedCurrent = current.trimmed();
    normalizedCurrent.replace('\\', '/');
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->data(Qt::UserRole).toString().compare(normalizedCurrent, Qt::CaseInsensitive) == 0) {
            list->setCurrentRow(index);
            list->scrollToItem(list->item(index), QAbstractItemView::PositionAtCenter);
            break;
        }
    }
    if (!list->currentItem() && list->count() > 0) list->setCurrentRow(0);

    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return std::nullopt;
    return ModelBrowserSelection{list->currentItem()->data(Qt::UserRole).toString(),
                                 *selectedSkin, *selectedSequence, animationRate->value()};
}

std::string formatProjectedVec3(const hammer::vmf::Vec3& value)
{
    std::ostringstream stream;
    stream.precision(12);
    stream << value.x << ' ' << value.y << ' ' << value.z;
    return stream.str();
}

void projectedSurfaceBasis(const hammer::vmf::Vec3& rawNormal,
                           hammer::vmf::Vec3& axisU,
                           hammer::vmf::Vec3& axisV)
{
    auto cross = [](const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b) {
        return hammer::vmf::Vec3{a.y * b.z - a.z * b.y,
                                 a.z * b.x - a.x * b.z,
                                 a.x * b.y - a.y * b.x};
    };
    auto normalize = [](const hammer::vmf::Vec3& value,
                        const hammer::vmf::Vec3& fallback) {
        const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        return length > 1e-9 ? hammer::vmf::Vec3{value.x / length, value.y / length, value.z / length}
                             : fallback;
    };
    const hammer::vmf::Vec3 normal = normalize(rawNormal, {0.0, 0.0, 1.0});

    // Match CMapOverlay::Basis_SetInitialUAxis/Basis_BuildAxes. The decal
    // helper uses a related basis with the opposite V axis, which caused new
    // overlays to be vertically mirrored when reused here.
    const double absolute[3] = {std::abs(normal.x), std::abs(normal.y), std::abs(normal.z)};
    int majorAxis = 0;
    if (absolute[1] > absolute[majorAxis]) majorAxis = 1;
    if (absolute[2] > absolute[majorAxis]) majorAxis = 2;
    axisU = (majorAxis == 0) ? hammer::vmf::Vec3{0.0, 1.0, 0.0}
                             : hammer::vmf::Vec3{1.0, 0.0, 0.0};
    axisV = normalize(cross(normal, axisU), {0.0, 1.0, 0.0});
    axisU = normalize(cross(axisV, normal), axisU);
}

QWidget* makeSequencePropertyEditor(QWidget* parent, QDialog* dialog,
                                    const QString& key, const QString& value,
                                    const std::shared_ptr<hammer::assets::StudioModelSystem>& studioModels,
                                    const std::function<void(const QString&, const QString&)>& setRawValue,
                                    const std::function<QString(const QString&)>& rawValue)
{
    auto* combo = new QComboBox(parent);
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->addItem(QObject::tr("Reference pose / no animation"), QString{});
    const bool numericSequenceKey = key.compare(QStringLiteral("sequence"), Qt::CaseInsensitive) == 0;
    const QString modelPath = rawValue(QStringLiteral("model")).trimmed();
    if (studioModels && !modelPath.isEmpty()) {
        const auto model = studioModels->model(modelPath.toUtf8().toStdString());
        if (model && model->valid) {
            for (int sequence = 0; sequence < model->sequenceCount(); ++sequence) {
                const auto& metadata = model->sequences[static_cast<std::size_t>(sequence)];
                QString label = QString::fromStdString(metadata.label);
                if (metadata.duration > 0.0f)
                    label += QObject::tr(" — %1 s").arg(metadata.duration, 0, 'f', 2);
                combo->addItem(label, numericSequenceKey
                    ? QString::number(sequence)
                    : QString::fromStdString(metadata.label));
            }
        }
    }
    int selected = -1;
    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemData(index).toString().compare(value, Qt::CaseInsensitive) == 0) {
            selected = index;
            break;
        }
    }
    if (selected >= 0) combo->setCurrentIndex(selected);
    else combo->setEditText(value);
    QObject::connect(combo, &QComboBox::currentTextChanged, dialog, [=](const QString& text) {
        const int index = combo->currentIndex();
        setRawValue(key, index >= 0 ? combo->itemData(index).toString() : text);
    });
    return combo;
}

QWidget* makeModelPropertyEditor(QWidget* parent, QDialog* dialog,
                                 const QString& key, const QString& value,
                                 const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                                 const std::shared_ptr<hammer::assets::StudioModelSystem>& studioModels,
                                 MapViewWidget::TexturedRenderMode texturedRenderMode,
                                 bool phongEnabled, bool specularEnabled, bool bumpMapsEnabled,
                                 bool lightWarpEnabled, bool selfIllumEnabled, bool rimLightEnabled,
                                 float phongIntensity, float specularIntensity, float bumpMapIntensity,
                                 const std::string& skyName,
                                 const std::function<void(const QString&, const QString&)>& setRawValue,
                                 const std::function<QString(const QString&)>& rawValue)
{
    auto* compound = new QWidget(parent);
    auto* row = new QHBoxLayout(compound);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    auto* line = new QLineEdit(value, compound);
    auto* browse = new QPushButton(QObject::tr("Browse…"), compound);
    row->addWidget(line, 1);
    row->addWidget(browse);
    QObject::connect(line, &QLineEdit::textChanged, dialog,
                     [key, setRawValue](const QString& text) { setRawValue(key, text); });
    QObject::connect(browse, &QPushButton::clicked, dialog, [=] {
        bool playbackRateValid = false;
        double currentPlaybackRate = rawValue(QStringLiteral("playbackrate")).toDouble(&playbackRateValid);
        if (!playbackRateValid || !std::isfinite(currentPlaybackRate)) currentPlaybackRate = 1.0;
        const auto selected = browseModel(dialog, materials, studioModels,
                                          texturedRenderMode,
                                          phongEnabled, specularEnabled, bumpMapsEnabled,
                                          lightWarpEnabled, selfIllumEnabled, rimLightEnabled,
                                          phongIntensity, specularIntensity, bumpMapIntensity,
                                          skyName, line->text(),
                                          parseInteger(rawValue(QStringLiteral("skin"))),
                                          rawValue(QStringLiteral("DefaultAnim")),
                                          std::clamp(currentPlaybackRate, -16.0, 16.0));
        if (selected) {
            line->setText(selected->path);
            setRawValue(QStringLiteral("skin"), QString::number(selected->skin));
            setRawValue(QStringLiteral("DefaultAnim"), selected->sequence);
            setRawValue(QStringLiteral("playbackrate"),
                        QString::number(selected->playbackRate, 'g', 8));
        }
    });
    return compound;
}

// --------------------------------------------------------- Skybox Browser
//
// worldspawn's "skyname" picks one of the mounted skybox material sets
// (materials/skybox/<name>{ft,bk,lf,rt,up,dn}.vmt). The browser lists them with
// a panorama of their four side faces, the way the texture browser lists
// materials, and previews the selected one in a slowly turning 3D view.

constexpr std::array<const char*, 6> kSkyFaceSuffixes{{"ft", "bk", "lf", "rt", "up", "dn"}};

QStringList mountedSkyNames(const std::shared_ptr<hammer::assets::MaterialSystem>& materials)
{
    QStringList names;
    if (!materials) return names;
    std::set<QString> unique;
    for (const std::string& material : materials->materialNames()) {
        QString name = QString::fromStdString(material);
        if (!name.startsWith(QStringLiteral("skybox/"), Qt::CaseInsensitive)) continue;
        name.remove(0, 7);
        // Only the front face is enumerated, so one entry per sky set.
        if (!name.endsWith(QStringLiteral("ft"), Qt::CaseInsensitive)) continue;
        name.chop(2);
        if (!name.isEmpty()) unique.insert(name);
    }
    for (const QString& name : unique) names << name;
    return names;
}

QImage skyFaceImage(const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                    const QString& skyName, const char* suffix, int maxDimension)
{
    if (!materials) return {};
    const std::string name = "skybox/" + skyName.toStdString() + suffix;
    const auto material = materials->previewMaterial(name, maxDimension);
    if (!material || !material->image.valid()) return {};
    const hammer::assets::Image& image = material->image;
    const QImage wrapped(reinterpret_cast<const uchar*>(image.pixels.data()), image.width,
                         image.height, image.width * static_cast<int>(sizeof(std::uint32_t)),
                         QImage::Format_ARGB32);
    return wrapped.copy();
}

// The four side faces laid out left to right, which reads as a panorama of the
// sky and makes one set easy to tell from another in the list.
QPixmap skyPanoramaPixmap(const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                          const QString& skyName, int faceSize)
{
    constexpr std::array<const char*, 4> sides{{"ft", "rt", "bk", "lf"}};
    QPixmap panorama(faceSize * static_cast<int>(sides.size()), faceSize);
    panorama.fill(QColor(0, 0, 0, 0));
    QPainter painter(&panorama);
    bool any = false;
    for (std::size_t index = 0; index < sides.size(); ++index) {
        const QImage face = skyFaceImage(materials, skyName, sides[index], faceSize);
        if (face.isNull()) continue;
        any = true;
        painter.drawImage(QRect(static_cast<int>(index) * faceSize, 0, faceSize, faceSize),
                          face.scaled(faceSize, faceSize, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation));
    }
    painter.end();
    return any ? panorama : QPixmap();
}

// A perspective view of nothing but the sky, turning slowly so every face comes
// around. Dragging in the view still works, as it is the ordinary 3D viewport.
class SkyPreviewWidget final : public QWidget
{
public:
    SkyPreviewWidget(std::shared_ptr<hammer::assets::MaterialSystem> materials,
                     MapViewWidget::TexturedRenderMode texturedRenderMode,
                     bool phongEnabled, bool specularEnabled, bool bumpMapsEnabled,
                     bool lightWarpEnabled, bool selfIllumEnabled, bool rimLightEnabled,
                     float phongIntensity, float specularIntensity, float bumpMapIntensity,
                     QWidget* parent = nullptr)
        : QWidget(parent), materials_(std::move(materials))
    {
        setMinimumSize(320, 240);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        viewport_ = new MapViewWidget(MapViewWidget::Kind::Perspective, this);
        viewport_->setGridVisible(false);
        viewport_->setViewLabelVisible(false);
        // Wider than the editor's 75 degrees, so more of the sky is in frame.
        viewport_->setVerticalFieldOfView(110.0);
        viewport_->setMaterialSystem(materials_);
        viewport_->setMaterialRenderingEnabled(true);
        // The sky is drawn by the raster path in every mode, so the preview
        // never runs the ray tracer - a turning RT view would starve this
        // modal dialog for no gain.
        viewport_->setTexturedRenderMode(
            texturedRenderMode == MapViewWidget::TexturedRenderMode::RayTracedPreview
                ? MapViewWidget::TexturedRenderMode::Shaded
                : texturedRenderMode);
        viewport_->setMaterialEffectsEnabled(phongEnabled, specularEnabled, bumpMapsEnabled,
                                             lightWarpEnabled, selfIllumEnabled, rimLightEnabled);
        viewport_->setMaterialEffectIntensities(phongIntensity, specularIntensity,
                                                bumpMapIntensity);
        layout->addWidget(viewport_);

        connect(&timer_, &QTimer::timeout, this, [this] {
            yawDegrees_ = std::fmod(yawDegrees_ + 0.35, 360.0);
            pointCamera();
        });
        timer_.start(33);
    }

    void setSkyName(const QString& skyName)
    {
        if (skyName_ == skyName && scene_) return;
        skyName_ = skyName;
        scene_ = std::make_shared<hammer::vmf::Scene>();
        scene_->invalidateLineage();
        scene_->skyName = skyName_.toUtf8().toStdString();
        // No geometry to frame, so the camera is placed by hand rather than fit.
        viewport_->setScene(scene_, false);
        pointCamera();
    }

    void setRotating(bool rotating)
    {
        if (rotating == timer_.isActive()) return;
        if (rotating) timer_.start(33);
        else timer_.stop();
    }

private:
    void pointCamera()
    {
        const double radians = qDegreesToRadians(yawDegrees_);
        const hammer::vmf::Vec3 eye{0.0, 0.0, 0.0};
        const hammer::vmf::Vec3 lookAt{std::cos(radians) * 64.0, std::sin(radians) * 64.0, 6.0};
        viewport_->setCameraTransform(eye, lookAt);
        viewport_->update();
    }

    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    MapViewWidget* viewport_{nullptr};
    std::shared_ptr<hammer::vmf::Scene> scene_;
    QString skyName_;
    double yawDegrees_{0.0};
    QTimer timer_;
};

std::optional<QString> browseSkybox(QWidget* parent,
                                    const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                                    MapViewWidget::TexturedRenderMode texturedRenderMode,
                                    bool phongEnabled, bool specularEnabled, bool bumpMapsEnabled,
                                    bool lightWarpEnabled, bool selfIllumEnabled,
                                    bool rimLightEnabled, float phongIntensity,
                                    float specularIntensity, float bumpMapIntensity,
                                    const QString& current)
{
    const QStringList skies = mountedSkyNames(materials);
    if (skies.isEmpty()) {
        QMessageBox::information(parent, QObject::tr("Skybox Browser"),
                                 QObject::tr("No skybox materials were found in the mounted game "
                                             "content. Configure a game directory containing "
                                             "gameinfo.txt first."));
        return std::nullopt;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Skybox Browser"));
    dialog.resize(1040, 700);
    auto* layout = new QVBoxLayout(&dialog);

    // Group-box sections in a splitter, matching the Material and Model
    // browsers.
    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* skiesBox = new QGroupBox(QObject::tr("Skyboxes"), splitter);
    auto* skiesLayout = new QVBoxLayout(skiesBox);
    auto* filter = new QLineEdit(skiesBox);
    filter->setPlaceholderText(QObject::tr("Filter mounted skyboxes…"));
    skiesLayout->addWidget(filter);

    constexpr int FaceSize = 64;
    auto* list = new QListWidget(skiesBox);
    list->setViewMode(QListView::IconMode);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setWrapping(true);
    list->setWordWrap(true);
    list->setUniformItemSizes(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setIconSize(QSize(FaceSize * 4, FaceSize));
    list->setGridSize(QSize(FaceSize * 4 + 24, FaceSize + 46));
    skiesLayout->addWidget(list, 1);
    splitter->addWidget(skiesBox);

    auto* details = new QWidget(splitter);
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    auto* previewBox = new QGroupBox(QObject::tr("3D Preview"), details);
    auto* previewLayout = new QVBoxLayout(previewBox);
    auto* preview = new SkyPreviewWidget(materials, texturedRenderMode, phongEnabled,
                                         specularEnabled, bumpMapsEnabled, lightWarpEnabled,
                                         selfIllumEnabled, rimLightEnabled, phongIntensity,
                                         specularIntensity, bumpMapIntensity, previewBox);
    previewLayout->addWidget(preview, 1);
    auto* rotate = new QCheckBox(QObject::tr("&Rotate"), previewBox);
    rotate->setChecked(true);
    previewLayout->addWidget(rotate);
    detailsLayout->addWidget(previewBox, 1);

    auto* infoBox = new QGroupBox(QObject::tr("Skybox Info"), details);
    auto* infoLayout = new QFormLayout(infoBox);
    auto* infoName = new QLabel(infoBox);
    auto* infoSize = new QLabel(infoBox);
    auto* infoFaces = new QLabel(infoBox);
    for (QLabel* label : {infoName, infoSize, infoFaces}) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
    }
    infoLayout->addRow(QObject::tr("Name:"), infoName);
    infoLayout->addRow(QObject::tr("Face size:"), infoSize);
    infoLayout->addRow(QObject::tr("Faces:"), infoFaces);
    detailsLayout->addWidget(infoBox);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(rotate, &QCheckBox::toggled, preview,
                     [preview](bool on) { preview->setRotating(on); });

    const QPixmap placeholder(FaceSize * 4, FaceSize);
    for (const QString& sky : skies) {
        auto* item = new QListWidgetItem(sky, list);
        item->setData(Qt::UserRole, sky);
        item->setSizeHint(list->gridSize());
        const QPixmap panorama = skyPanoramaPixmap(materials, sky, FaceSize);
        if (!panorama.isNull()) item->setIcon(QIcon(panorama));
    }

    const auto updateDetails = [=](QListWidgetItem* item) {
        if (!item) return;
        const QString sky = item->data(Qt::UserRole).toString();
        preview->setSkyName(sky);
        infoName->setText(QStringLiteral("skybox/%1").arg(sky));
        QStringList present;
        QString faceSize = QObject::tr("—");
        for (const char* suffix : kSkyFaceSuffixes) {
            const auto material = materials
                ? materials->material("skybox/" + sky.toStdString() + suffix)
                : nullptr;
            if (!material || material->missing || !material->image.valid()) continue;
            present << QString::fromLatin1(suffix);
            if (faceSize == QObject::tr("—")) {
                faceSize = QStringLiteral("%1 x %2")
                               .arg(material->image.width).arg(material->image.height);
            }
        }
        infoSize->setText(faceSize);
        infoFaces->setText(present.isEmpty() ? QObject::tr("none found") : present.join(QStringLiteral(", ")));
    };
    QObject::connect(list, &QListWidget::currentItemChanged, &dialog,
                     [updateDetails](QListWidgetItem* item, QListWidgetItem*) {
                         updateDetails(item);
                     });
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog,
                     [&dialog](QListWidgetItem*) { dialog.accept(); });
    QObject::connect(filter, &QLineEdit::textChanged, &dialog, [=](const QString& text) {
        QListWidgetItem* firstVisible = nullptr;
        for (int index = 0; index < list->count(); ++index) {
            QListWidgetItem* item = list->item(index);
            const bool visible = item->data(Qt::UserRole).toString().contains(text,
                                                                             Qt::CaseInsensitive);
            item->setHidden(!visible);
            if (visible && !firstVisible) firstVisible = item;
        }
        if (!list->currentItem() || list->currentItem()->isHidden())
            list->setCurrentItem(firstVisible);
    });

    const QString normalized = current.trimmed();
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->data(Qt::UserRole).toString().compare(normalized,
                                                                    Qt::CaseInsensitive) == 0) {
            list->setCurrentRow(index);
            list->scrollToItem(list->item(index), QAbstractItemView::PositionAtCenter);
            break;
        }
    }
    if (!list->currentItem() && list->count() > 0) list->setCurrentRow(0);

    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return std::nullopt;
    return list->currentItem()->data(Qt::UserRole).toString();
}

QWidget* makeSkyPropertyEditor(QWidget* parent, QDialog* dialog, const QString& key,
                               const QString& value,
                               const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                               MapViewWidget::TexturedRenderMode texturedRenderMode,
                               bool phongEnabled, bool specularEnabled, bool bumpMapsEnabled,
                               bool lightWarpEnabled, bool selfIllumEnabled, bool rimLightEnabled,
                               float phongIntensity, float specularIntensity, float bumpMapIntensity,
                               const std::function<void(const QString&, const QString&)>& setRawValue)
{
    auto* compound = new QWidget(parent);
    auto* row = new QHBoxLayout(compound);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    auto* line = new QLineEdit(value, compound);
    auto* browse = new QPushButton(QObject::tr("Browse…"), compound);
    row->addWidget(line, 1);
    row->addWidget(browse);
    QObject::connect(line, &QLineEdit::textChanged, dialog,
                     [key, setRawValue](const QString& text) { setRawValue(key, text); });
    QObject::connect(browse, &QPushButton::clicked, dialog, [=] {
        const auto selected = browseSkybox(dialog, materials, texturedRenderMode, phongEnabled,
                                           specularEnabled, bumpMapsEnabled, lightWarpEnabled,
                                           selfIllumEnabled, rimLightEnabled, phongIntensity,
                                           specularIntensity, bumpMapIntensity, line->text());
        if (selected) line->setText(*selected);
    });
    return compound;
}
}

MapDocumentWidget::MapDocumentWidget(std::shared_ptr<hammer::fgd::Database> fgd, QWidget* parent)
    : QWidget(parent), fgd_(std::move(fgd))
{
    verticalSplitter_ = new QSplitter(Qt::Vertical, this);
    topSplitter_ = new QSplitter(Qt::Horizontal, verticalSplitter_);
    bottomSplitter_ = new QSplitter(Qt::Horizontal, verticalSplitter_);

    views_[0] = new MapViewWidget(MapViewWidget::Kind::Perspective, topSplitter_);
    views_[1] = new MapViewWidget(MapViewWidget::Kind::Top, topSplitter_);
    views_[2] = new MapViewWidget(MapViewWidget::Kind::Front, bottomSplitter_);
    views_[3] = new MapViewWidget(MapViewWidget::Kind::Side, bottomSplitter_);

    for (MapViewWidget* view : views_) {
        view->setSelectionMode(MapViewWidget::SelectionMode::Groups);
        view->setTool(tool_);
        view->setTransformMode(transformMode_);
        connect(view, &MapViewWidget::activated, this, &MapDocumentWidget::activateView);
        connect(view, &MapViewWidget::cursorPositionChanged, this, &MapDocumentWidget::coordinatesChanged);
        connect(view, &MapViewWidget::selectionRequested, this,
                [this](const hammer::vmf::ObjectRef& object, bool toggle, bool additive) {
                    // CMapGroup: a pick on a grouped object selects the whole
                    // group. Expansion happens here, where a pick becomes a
                    // selection, so nothing downstream has to know about groups.
                    const std::vector<hammer::vmf::ObjectRef> expanded =
                        expandSelectionToGroups({object});
                    if (expanded.size() == 1) {
                        editor_.select(object, toggle, additive);
                    } else {
                        std::vector<hammer::vmf::ObjectRef> selection;
                        if (additive || toggle) selection = editor_.selection();
                        // Toggling a group whose members are already selected
                        // removes the whole group, as clicking one member does.
                        const bool remove =
                            toggle && std::find(selection.begin(), selection.end(), object) !=
                                          selection.end();
                        for (const hammer::vmf::ObjectRef& member : expanded) {
                            const auto at = std::find(selection.begin(), selection.end(), member);
                            if (remove) {
                                if (at != selection.end()) selection.erase(at);
                            } else if (at == selection.end()) {
                                selection.push_back(member);
                            }
                        }
                        editor_.setSelection(std::move(selection));
                    }
                    setSelectionOnViews();
                    notifySelectionState();
                });
        connect(view, &MapViewWidget::clearSelectionRequested, this, &MapDocumentWidget::clearSelection);
        // CMapView::SelectAt fills CSelection's hit list on every pick; the
        // document owns it so Select Next/Previous Object and the 3D pick timer
        // can walk it (CSelection::SetCurrentHit).
        connect(view, &MapViewWidget::hitListChanged, this,
                [this](const std::vector<hammer::vmf::ObjectRef>& hits) {
                    hitList_ = hits;
                    currentHit_ = hits.empty() ? -1 : 0;
                });
        connect(view, &MapViewWidget::selectNextHitRequested, this,
                [this] { selectNextHit(true); });
        // Selection3D::SelectInBox (Options.view2d.bAutoSelect) on mouse up.
        connect(view, &MapViewWidget::boxSelectionRequested, this,
                [this](const std::vector<hammer::vmf::ObjectRef>& objects, bool additive) {
                    std::vector<hammer::vmf::ObjectRef> selection;
                    if (additive) selection = editor_.selection();
                    // A box that catches any member of a group takes the group.
                    for (const auto& object : expandSelectionToGroups(objects)) {
                        if (std::find(selection.begin(), selection.end(), object) == selection.end())
                            selection.push_back(object);
                    }
                    editor_.setSelection(std::move(selection));
                    hitList_.clear();
                    currentHit_ = -1;
                    setSelectionOnViews();
                    notifySelectionState();
                });
        connect(view, &MapViewWidget::moveStarted, this, &MapDocumentWidget::beginMove);
        connect(view, &MapViewWidget::moveDeltaRequested, this, &MapDocumentWidget::moveSelection);
        connect(view, &MapViewWidget::moveFinished, this, &MapDocumentWidget::finishMove);
        connect(view, &MapViewWidget::resizeStarted, this, &MapDocumentWidget::beginResize);
        connect(view, &MapViewWidget::resizeDeltaRequested, this, &MapDocumentWidget::resizeSelection);
        connect(view, &MapViewWidget::rotateStarted, this, &MapDocumentWidget::beginRotate);
        connect(view, &MapViewWidget::rotateDeltaRequested, this, &MapDocumentWidget::rotateSelection);
        connect(view, &MapViewWidget::transformFinished, this, &MapDocumentWidget::finishTransform);
        connect(view, &MapViewWidget::transformModeChangeRequested, this, &MapDocumentWidget::setTransformMode);
        connect(view, &MapViewWidget::interactionCanceled, this, [this] {
            if (!editor_.transactionActive()) return;
            editor_.cancelTransaction();
            rebuildScene(false);
            notifyDocumentState(tr("Canceled transform"));
            notifySelectionState();
        });
        connect(view, &MapViewWidget::blockCreationRequested, this, &MapDocumentWidget::createBlock);
        // The Block tool's pending box belongs to the document, not to the view
        // that drew it: every 2D view shows it (each drawing the two dimensions
        // it can) and can resize it, so the depth the drawing view cannot show
        // is editable in the others before Enter commits the brush.
        connect(view, &MapViewWidget::blockPreviewChanged, this,
                [this, view](const hammer::vmf::Bounds& bounds, int extrusionAxis) {
                    for (MapViewWidget* other : views_) {
                        // Skip the sender: it is mid-drag and owns the values.
                        if (!other || other == view) continue;
                        other->setPendingBlock(bounds, extrusionAxis);
                    }
                });
        connect(view, &MapViewWidget::entityCreationRequested, this, &MapDocumentWidget::createEntity);
        connect(view, &MapViewWidget::entityPlacementRequested, this, &MapDocumentWidget::createEntityOnSurface);
        connect(view, &MapViewWidget::decalPlacementRequested, this, &MapDocumentWidget::createDecal);
        connect(view, &MapViewWidget::overlayPlacementRequested, this, &MapDocumentWidget::createOverlay);
        connect(view, &MapViewWidget::objectPropertiesRequested, this,
                [this](const hammer::vmf::ObjectRef& object) {
                    // A solid picked in Solids mode may belong to a brush
                    // entity. Its keyvalues live on the entity, and it cannot
                    // carry VisGroups of its own (CMapDoc::
                    // VisGroups_ObjectCanBelongToVisGroup), so the double-click
                    // opens the owning entity instead.
                    hammer::vmf::ObjectRef target = object;
                    if (target.type == hammer::vmf::ObjectType::Solid && scene_) {
                        for (const auto& brush : scene_->brushes) {
                            if (brush.id != target.id || brush.ownerEntityId < 0) continue;
                            target = {hammer::vmf::ObjectType::Entity, brush.ownerEntityId};
                            break;
                        }
                    }
                    // Properties edits exactly one object, so a double-click
                    // selects just what was hit rather than expanding to its
                    // group the way a single click does.
                    editor_.setSelection({target});
                    setSelectionOnViews();
                    notifySelectionState();
                    // A world brush has no entity class, so its VisGroup
                    // membership is what a double-click is actually for.
                    showObjectProperties(window(),
                                         target.type == hammer::vmf::ObjectType::Solid);
                });
        connect(view, &MapViewWidget::nudgeRequested, this, [this](const hammer::vmf::Vec3& delta) {
            if (editor_.translateSelection(delta, "Nudge Objects")) {
                // A nudge moves exactly the selection, so it takes the same
                // incremental path as a drag rather than rebuilding the whole
                // scene and every view's GPU buffers per keypress.
                rebuildSelectedObjectsInScene();
                notifyDocumentState(tr("Moved selection by %1 %2 %3").arg(delta.x).arg(delta.y).arg(delta.z));
                notifySelectionState();
            }
        });
        connect(view, &MapViewWidget::nudgeDuplicateRequested, this,
                [this](const hammer::vmf::Vec3& delta) { duplicateSelectionBy(delta); });
        connect(view, &MapViewWidget::cameraEdited, this, &MapDocumentWidget::editCamera);
        connect(view, &MapViewWidget::cameraCycleRequested, this, &MapDocumentWidget::cycleActiveCamera);
        connect(view, &MapViewWidget::cameraDeleteRequested, this, &MapDocumentWidget::deleteActiveCamera);
        connect(view, &MapViewWidget::clipLineChanged, this, &MapDocumentWidget::updateClipLine);
        connect(view, &MapViewWidget::clipApplyRequested, this, &MapDocumentWidget::applyClip);
        connect(view, &MapViewWidget::clipCancelRequested, this, &MapDocumentWidget::clearClip);
        connect(view, &MapViewWidget::selectionToolRequested, this,
                &MapDocumentWidget::selectionToolRequested);
        connect(view, &MapViewWidget::morphSelectRequested, this,
                &MapDocumentWidget::selectMorphHandles);
        connect(view, &MapViewWidget::morphSelectionClearRequested, this,
                &MapDocumentWidget::clearMorphHandleSelection);
        connect(view, &MapViewWidget::morphMoveRequested, this,
                &MapDocumentWidget::moveMorphSelection);
        connect(view, &MapViewWidget::morphMoveFinished, this,
                &MapDocumentWidget::finishMorphMove);
        connect(view, &MapViewWidget::morphEscapePressed, this, &MapDocumentWidget::morphEscape);
        connect(view, &MapViewWidget::faceSelectRequested, this,
                [this, view](int solidId, int sideId, bool control, bool shift) {
                    handleFaceSelect(view, solidId, sideId, control, shift);
                });
        connect(view, &MapViewWidget::faceApplyRequested, this,
                [this, view](int solidId, int sideId, bool edgeAlign, bool shift) {
                    handleFaceApply(view, solidId, sideId, edgeAlign, shift);
                });
        connect(view, &MapViewWidget::displacementPaintBegin, this,
                &MapDocumentWidget::beginDisplacementPaint);
        connect(view, &MapViewWidget::displacementPaintMoved, this,
                &MapDocumentWidget::continueDisplacementPaint);
        connect(view, &MapViewWidget::displacementPaintFinished, this,
                &MapDocumentWidget::endDisplacementPaint);
    }

    topSplitter_->setChildrenCollapsible(false);
    topSplitter_->setStretchFactor(0, 1);
    topSplitter_->setStretchFactor(1, 1);
    bottomSplitter_->setChildrenCollapsible(false);
    bottomSplitter_->setStretchFactor(0, 1);
    bottomSplitter_->setStretchFactor(1, 1);
    verticalSplitter_->setChildrenCollapsible(false);
    verticalSplitter_->setStretchFactor(0, 1);
    verticalSplitter_->setStretchFactor(1, 1);
    topSplitter_->setHandleWidth(4);
    bottomSplitter_->setHandleWidth(4);
    verticalSplitter_->setHandleWidth(4);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(verticalSplitter_);

    rebuildScene();
    loadCamerasFromDocument();
    autosizeViews();
    activateView(views_[0]);
    notifyDocumentState(tr("Created a new VMF map"));
    notifySelectionState();
}

bool MapDocumentWidget::loadFromFile(const QString& path, QString* error,
                                     const std::function<void(int, const QString&)>& progress)
{
    if (error) error->clear();
    const auto report = [&progress](int percent, const QString& stage) {
        if (progress) progress(percent, stage);
    };
    report(0, tr("Reading %1…").arg(QFileInfo(path).fileName()));
    hammer::vmf::ParseError parseError;
    std::string ioError;
    auto loaded = hammer::vmf::Document::load(toFilesystemPath(path), &parseError, &ioError);
    if (!loaded) {
        if (error) {
            if (!ioError.empty()) *error = QString::fromStdString(ioError);
            else {
                *error = tr("VMF parse error at line %1, column %2: %3")
                             .arg(static_cast<qulonglong>(parseError.line))
                             .arg(static_cast<qulonglong>(parseError.column))
                             .arg(QString::fromStdString(parseError.message));
            }
        }
        return false;
    }
    report(35, tr("Parsed — building scene…"));
    // A leak trace and a portal file both belong to the map they were compiled
    // from.
    unloadPointFile();
    unloadPortalFile();
    editor_.setDocument(std::move(*loaded));
    rebuildScene();
    report(90, tr("Finalizing…"));
    loadCamerasFromDocument();
    filePath_ = QFileInfo(path).absoluteFilePath();
    notifyDocumentState(tr("Loaded %1 — %2").arg(filePath_, mapSummary()));
    notifySelectionState();
    report(100, tr("Done"));
    return true;
}

void MapDocumentWidget::setCollabIdRange(int base, int span)
{
    editor_.setIdRange(base, span);
}

void MapDocumentWidget::setCollabPeerPoses(const QList<CollabPeerPose>& poses)
{
    for (MapViewWidget* view : views_) if (view) view->setCollabPeers(poses);
}

void MapDocumentWidget::adoptCollabDocument(hammer::vmf::Document document)
{
    unloadPointFile();
    unloadPortalFile();
    editor_.setDocument(std::move(document));
    rebuildScene();
    loadCamerasFromDocument();
    notifyDocumentState(tr("Joined session — %1").arg(mapSummary()));
    notifySelectionState();
}

void MapDocumentWidget::applyRemoteDelta(const hammer::vmf::SyncDelta& delta)
{
    editor_.applyExternalEdit(
        [&delta](hammer::vmf::Document& document) { hammer::vmf::applyDelta(document, delta); });

    // Rebuild only the objects the delta names. A remote edit must never cost
    // a whole-map rebuild: buildScene alone is ~109 ms on a 6000-solid map and
    // a full publish re-resolves EVERY entity helper through the material
    // system (filesystem work per material, and a miss walks every search
    // path). Collaborator edits then arrive faster than they can be applied,
    // the event loop never catches up, and the editor appears frozen.
    // rebuildSceneObjects falls back to a full rebuild by itself whenever the
    // incremental path cannot be trusted, so this only ever saves work.
    std::vector<hammer::vmf::ObjectRef> changed;
    bool wholeMap = false;
    const auto note = [&](const std::string& kind, const std::string& key) {
        if (wholeMap) return;
        const bool solid = kind == "solid";
        if (!solid && kind != "entity") {
            // "world" (worldspawn keyvalues) and "root" (visgroup defs,
            // cameras, versioninfo) are not scene objects.
            wholeMap = true;
            return;
        }
        bool ok = false;
        const int id = QString::fromStdString(key).toInt(&ok);
        if (!ok || id < 0) {
            wholeMap = true;  // an id-less object, keyed by occurrence
            return;
        }
        changed.push_back({solid ? hammer::vmf::ObjectType::Solid
                                 : hammer::vmf::ObjectType::Entity,
                           id});
    };
    for (const hammer::vmf::SyncDelta::Upsert& upsert : delta.upserts)
        note(upsert.kind, upsert.key);
    for (const hammer::vmf::SyncDelta::Removal& removal : delta.removals)
        note(removal.kind, removal.key);

    if (wholeMap || changed.empty()) {
        rebuildScene(false);
    } else {
        // The object index and hidden set still refresh (~1 ms at 6000
        // solids): a remote upsert can change group membership or the
        // visgroupshown flags that applySceneVisibility consumes. The
        // auto-visgroup classification does NOT run here: local drags,
        // nudges and resizes never reclassify either, and deltas arrive at
        // drag-step rate while a peer moves something. It is marked stale and
        // refreshed on demand by the readers (ensureAutoVisGroupsFresh).
        objectIndex_ = hammer::vmf::indexMapObjects(editor_.document());
        autoVisGroupsStale_ = true;
        refreshHiddenObjects();
        rebuildSceneObjects(changed);
    }
    refreshMorphFromScene();
    notifyFaceSelectionState();
    notifyDocumentState(tr("Applied a collaborator's edit"));
    notifySelectionState();
}

bool MapDocumentWidget::save(QString* error)
{
    if (filePath_.isEmpty()) {
        if (error) *error = tr("This map does not have a filename yet.");
        return false;
    }
    return saveAs(filePath_, error);
}

bool MapDocumentWidget::saveAs(const QString& path, QString* error)
{
    if (error) error->clear();
    saveCamerasToDocument();
    std::string saveError;
    if (!editor_.document().save(toFilesystemPath(path), &saveError)) {
        if (error) *error = QString::fromStdString(saveError);
        return false;
    }
    filePath_ = QFileInfo(path).absoluteFilePath();
    notifyDocumentState(tr("Saved %1 — %2").arg(filePath_, mapSummary()));
    return true;
}

bool MapDocumentWidget::maybeSave(QWidget* dialogParent)
{
    if (!isModified()) return true;
    const QMessageBox::StandardButton choice = QMessageBox::warning(
        dialogParent ? dialogParent : this, tr("Save Changes"), tr("Save changes to %1?").arg(displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Discard) return true;

    QString error;
    if (filePath_.isEmpty()) {
        QString path;
        {
            // The ray-traced preview cannot see a native chooser as modal and
            // would starve it of the GUI thread; see PreviewRenderGate.hpp.
            const hammer::app::PreviewRenderSuspension suspendPreview;
            path = QFileDialog::getSaveFileName(dialogParent ? dialogParent : this,
                                                tr("Save As"), QStringLiteral("untitled.vmf"),
                                                tr("Valve Map Files (*.vmf);;All Files (*.*)"));
        }
        if (path.isEmpty()) return false;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".vmf");
        if (!saveAs(path, &error)) {
            QMessageBox::critical(dialogParent ? dialogParent : this, tr("Save VMF"), error);
            return false;
        }
    } else if (!save(&error)) {
        QMessageBox::critical(dialogParent ? dialogParent : this, tr("Save VMF"), error);
        return false;
    }
    return true;
}

QString MapDocumentWidget::displayName() const
{
    return filePath_.isEmpty() ? tr("untitled") : QFileInfo(filePath_).fileName();
}

QString MapDocumentWidget::mapSummary() const
{
    const hammer::vmf::Statistics stats = editor_.document().statistics();
    return tr("%1 entities, %2 solids, %3 sides, %4 displacements; VMF format %5, map version %6")
        .arg(static_cast<qulonglong>(stats.entities))
        .arg(static_cast<qulonglong>(stats.solids))
        .arg(static_cast<qulonglong>(stats.sides))
        .arg(static_cast<qulonglong>(stats.displacements))
        .arg(stats.formatVersion < 0 ? QStringLiteral("?") : QString::number(stats.formatVersion))
        .arg(stats.mapVersion < 0 ? QStringLiteral("?") : QString::number(stats.mapVersion));
}

QString MapDocumentWidget::objectCountSummary() const
{
    const hammer::vmf::Statistics stats = editor_.document().statistics();
    return tr("%1 solids / %2 entities")
        .arg(static_cast<qulonglong>(stats.solids))
        .arg(static_cast<qulonglong>(stats.entities));
}

QString MapDocumentWidget::selectionSummary() const
{
    const auto& selection = editor_.selection();
    if (selection.empty()) return tr("no selection");
    if (selection.size() == 1) return tr("1 %1 (#%2)").arg(objectTypeName(selection.front().type)).arg(selection.front().id);
    return tr("%1 objects").arg(static_cast<qulonglong>(selection.size()));
}

QString MapDocumentWidget::selectionSizeSummary() const
{
    const hammer::vmf::Bounds bounds = visualSelectionBounds();
    if (!bounds.valid) return QStringLiteral("0 x 0 x 0");
    return QStringLiteral("%1 x %2 x %3")
        .arg(QString::number(bounds.maximum.x - bounds.minimum.x, 'g', 6))
        .arg(QString::number(bounds.maximum.y - bounds.minimum.y, 'g', 6))
        .arg(QString::number(bounds.maximum.z - bounds.minimum.z, 'g', 6));
}

QString MapDocumentWidget::undoText() const
{
    return editor_.canUndo() ? tr("&Undo %1").arg(QString::fromStdString(editor_.undoLabel())) : tr("&Undo");
}

QString MapDocumentWidget::redoText() const
{
    return editor_.canRedo() ? tr("&Redo %1").arg(QString::fromStdString(editor_.redoLabel())) : tr("&Redo");
}

void MapDocumentWidget::rebuildScene(bool fit)
{
    scene_ = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(editor_.document()));
    // The group/visgroup index is derived from the document, so it is refreshed
    // wherever the document is re-read in full.
    objectIndex_ = hammer::vmf::indexMapObjects(editor_.document());
    refreshAutoVisGroups();
    refreshHiddenObjects();
    applySceneVisibility();
    publishScene(fit);
}

void MapDocumentWidget::rebuildSelectedObjectsInScene()
{
    rebuildSceneObjects(editor_.selection());
}

void MapDocumentWidget::rebuildSceneObjects(const std::vector<hammer::vmf::ObjectRef>& changed)
{
    if (!scene_ || changed.empty()) {
        rebuildScene(false);
        return;
    }
    std::unordered_set<int> solidIds;
    std::unordered_set<int> entityIds;
    for (const hammer::vmf::ObjectRef& object : changed) {
        if (object.id < 0) {
            rebuildScene(false);
            return;
        }
        if (object.type == hammer::vmf::ObjectType::Solid) solidIds.insert(object.id);
        else entityIds.insert(object.id);
    }
    // A brush entity moves its solids with it, so their geometry is changed too.
    if (!entityIds.empty()) {
        const auto blockId = [](const hammer::vmf::Block& block) {
            const std::string* text = block.value("id");
            bool ok = false;
            const int id = text ? QString::fromStdString(*text).toInt(&ok) : 0;
            return ok ? id : -1;
        };
        for (const hammer::vmf::Block& root : editor_.document().roots()) {
            if (QString::fromStdString(root.name).compare(QStringLiteral("entity"),
                                                          Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (!entityIds.contains(blockId(root))) continue;
            for (const hammer::vmf::Block* solid : root.children("solid")) {
                const int id = blockId(*solid);
                if (id >= 0) solidIds.insert(id);
            }
        }
    }
    // In place: the Scene keeps its address and records the changed ids in its
    // lineage, so the per-view GPU caches re-upload only these objects instead
    // of the whole map. Copying a 2000-solid Scene per mouse-move was itself
    // ~3.5 ms before this.
    const bool perfLog = qEnvironmentVariableIsSet("HAMMER_PERF");
    QElapsedTimer perfTimer;
    if (perfLog) perfTimer.start();
    hammer::vmf::rebuildSceneObjectsInPlace(editor_.document(), *scene_, solidIds, entityIds);
    const double inPlaceMs = perfLog ? perfTimer.nsecsElapsed() / 1e6 : 0.0;
    // A zero baseRevision means the incremental path bailed out and rebuilt the
    // whole scene, in which case every entity marker is new and needs its
    // helper visualization resolved again.
    const bool incremental = scene_->baseRevision != 0;
    // A fallback rebuilt from the document brings every hidden object back and
    // has to be filtered again. The incremental path carries the previous
    // scene's objects over verbatim, but it REGENERATES the changed ones from
    // the document - and a collaborator can edit an object hidden locally, so
    // the filter runs there too. It early-returns when nothing is hidden.
    applySceneVisibility();
    publishScene(false, incremental ? &entityIds : nullptr);
    if (perfLog) {
        fprintf(stderr, "perf rebuildObjects: inPlace %.2f ms (incremental=%d), publish %.2f ms\n",
                inPlaceMs, incremental ? 1 : 0, perfTimer.nsecsElapsed() / 1e6 - inPlaceMs);
    }
}

void MapDocumentWidget::rebuildSceneFaces(const std::vector<hammer::vmf::FaceRef>& faces)
{
    std::vector<hammer::vmf::ObjectRef> solids;
    for (const hammer::vmf::FaceRef& face : faces) {
        const hammer::vmf::ObjectRef solid{hammer::vmf::ObjectType::Solid, face.solidId};
        if (std::find(solids.begin(), solids.end(), solid) == solids.end()) solids.push_back(solid);
    }
    rebuildSceneObjects(solids);
}

void MapDocumentWidget::publishScene(bool fit, const std::unordered_set<int>* changedEntityIds)
{
    const bool perfLog = qEnvironmentVariableIsSet("HAMMER_PERF");
    QElapsedTimer perfTimer;
    if (perfLog) perfTimer.start();
    // The hit list refers to objects in the previous scene. Undo/redo, clip,
    // morph, delete and paste can all remove them, so drop it rather than let
    // Select Next Object walk stale ids (CSelection::RemoveDead).
    hitList_.clear();
    currentHit_ = -1;
    applyFgdEntityVisualization(changedEntityIds);
    const double fgdMs = perfLog ? perfTimer.nsecsElapsed() / 1e6 : 0.0;
    validateFaceSelection();
    const hammer::vmf::Bounds bounds = visualSelectionBounds();
    const double validateMs = perfLog ? perfTimer.nsecsElapsed() / 1e6 - fgdMs : 0.0;
    for (MapViewWidget* view : views_) {
        if (!view) continue;
        view->setScene(scene_, fit);
        view->setSelection(editor_.selection(), bounds);
        view->setFaceSelection(faceSelection_);
    }
    const double viewsMs = perfLog ? perfTimer.nsecsElapsed() / 1e6 - fgdMs - validateMs : 0.0;
    // Undo/redo can change group membership; re-read it for the tint.
    pushSmoothingGroupToViews();
    if (perfLog) {
        fprintf(stderr,
                "perf publish: fgdViz %.2f ms, validate+bounds %.2f ms, setViews %.2f ms, smoothing %.2f ms\n",
                fgdMs, validateMs, viewsMs,
                perfTimer.nsecsElapsed() / 1e6 - fgdMs - validateMs - viewsMs);
    }
}

void MapDocumentWidget::applyFgdEntityVisualization(const std::unordered_set<int>* changedEntityIds)
{
    if (!scene_) return;
    // Decal/overlay triangles are re-clipped against brush geometry here, so
    // they can change without their entity being in the changed set.
    // Re-clipping is deterministic, though, so on an incremental edit only the
    // entities whose source or nearby solids changed are re-clipped, and the
    // view caches only lose their reusable predecessor when a projected
    // surface actually came out different.
    const auto materialResolver = [this](std::string_view name)
        -> std::optional<hammer::vmf::ProjectedMaterialInfo> {
        if (!materials_) return std::nullopt;
        const auto material = materials_->material(name);
        if (!material || !material->image.valid() || material->missing) return std::nullopt;
        return hammer::vmf::ProjectedMaterialInfo{material->image.width,
                                                   material->image.height,
                                                   material->decalScale};
    };
    std::vector<std::vector<hammer::vmf::ProjectedSurface>> previousSurfaces;
    previousSurfaces.resize(scene_->entities.size());
    std::vector<bool> entityReclipped(scene_->entities.size(), false);
    if (changedEntityIds) {
        const std::unordered_set<int> changedSolids(scene_->changedSolidIds.begin(),
                                                    scene_->changedSolidIds.end());
        for (std::size_t index = 0; index < scene_->entities.size(); ++index) {
            hammer::vmf::EntityMarker& entity = scene_->entities[index];
            if (!changedEntityIds->contains(entity.id) &&
                !hammer::vmf::projectedEntityDependsOnSolids(*scene_, entity, changedSolids,
                                                             materialResolver)) {
                continue;
            }
            previousSurfaces[index] = std::move(entity.projectedSurfaces);
            hammer::vmf::rebuildEntityProjectedSurfaces(*scene_, entity, materialResolver);
            entityReclipped[index] = true;
        }
    } else {
        for (std::size_t index = 0; index < scene_->entities.size(); ++index) {
            previousSurfaces[index] = std::move(scene_->entities[index].projectedSurfaces);
            entityReclipped[index] = true;
        }
        hammer::vmf::rebuildProjectedSurfaceGeometry(*scene_, materialResolver);
    }
    const auto surfacesEqual = [](const std::vector<hammer::vmf::ProjectedSurface>& a,
                                  const std::vector<hammer::vmf::ProjectedSurface>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t surface = 0; surface < a.size(); ++surface) {
            const hammer::vmf::ProjectedSurface& x = a[surface];
            const hammer::vmf::ProjectedSurface& y = b[surface];
            if (x.material != y.material || x.kind != y.kind ||
                x.triangles.size() != y.triangles.size()) {
                return false;
            }
            for (std::size_t vertex = 0; vertex < x.triangles.size(); ++vertex) {
                const hammer::vmf::ProjectedSurfaceVertex& p = x.triangles[vertex];
                const hammer::vmf::ProjectedSurfaceVertex& q = y.triangles[vertex];
                if (p.position.x != q.position.x || p.position.y != q.position.y ||
                    p.position.z != q.position.z || p.normal.x != q.normal.x ||
                    p.normal.y != q.normal.y || p.normal.z != q.normal.z ||
                    p.u != q.u || p.v != q.v) {
                    return false;
                }
            }
        }
        return true;
    };
    bool projectedSurfacesChanged = false;
    std::vector<bool> entitySurfacesChanged(scene_->entities.size(), false);
    for (std::size_t index = 0; index < scene_->entities.size(); ++index) {
        if (!entityReclipped[index]) continue;
        if (!surfacesEqual(previousSurfaces[index], scene_->entities[index].projectedSurfaces)) {
            entitySurfacesChanged[index] = true;
            projectedSurfacesChanged = true;
        }
    }
    if (projectedSurfacesChanged) scene_->invalidateLineage();
    auto expandSceneBounds = [this](const hammer::vmf::Vec3& point) {
        if (!scene_->hasBounds) {
            scene_->minimum = scene_->maximum = point;
            scene_->hasBounds = true;
            return;
        }
        scene_->minimum.x = std::min(scene_->minimum.x, point.x);
        scene_->minimum.y = std::min(scene_->minimum.y, point.y);
        scene_->minimum.z = std::min(scene_->minimum.z, point.z);
        scene_->maximum.x = std::max(scene_->maximum.x, point.x);
        scene_->maximum.y = std::max(scene_->maximum.y, point.y);
        scene_->maximum.z = std::max(scene_->maximum.z, point.z);
    };

    for (std::size_t entityIndex = 0; entityIndex < scene_->entities.size(); ++entityIndex) {
        hammer::vmf::EntityMarker& entity = scene_->entities[entityIndex];
        // Untouched entity: its helper resolution is still valid, so only feed
        // the bounds reduction. Re-resolving every entity's FGD class, model
        // and sprite on every drag step is what this avoids.
        // Decals and overlays are re-clipped against brushes above, so their
        // selection boxes move when a brush they sit on moves even though the
        // entity itself did not change. Only the entities whose re-clip
        // actually altered a surface take the full pass; dragging a brush
        // under one overlay must not re-resolve every other entity's FGD
        // class, model and sprite on every mouse move.
        if (!entitySurfacesChanged[entityIndex] && changedEntityIds &&
            !changedEntityIds->contains(entity.id) && entity.hasSelectionCorners) {
            for (const hammer::vmf::Vec3& corner : entity.selectionCorners)
                expandSceneBounds(corner);
            continue;
        }
        const hammer::fgd::EntityVisualization visualization =
            fgd_ && !fgd_->empty() ? fgd_->effectiveVisualization(entity.classname)
                                   : hammer::fgd::EntityVisualization{};
        entity.displayColor = visualization.displayColor;
        entity.sizeMinimum = {visualization.sizeMinimum[0], visualization.sizeMinimum[1],
                              visualization.sizeMinimum[2]};
        entity.sizeMaximum = {visualization.sizeMaximum[0], visualization.sizeMaximum[1],
                              visualization.sizeMaximum[2]};
        entity.description = visualization.description;
        entity.reversePitch = visualization.modelHelper == hammer::fgd::ModelHelperKind::LightProp;
        auto propertyValue = [&entity](std::string_view key) -> std::string {
            for (const auto& [propertyKey, propertyValue] : entity.properties) {
                if (propertyKey.size() != key.size()) continue;
                bool equal = true;
                for (std::size_t index = 0; index < key.size(); ++index) {
                    if (std::tolower(static_cast<unsigned char>(propertyKey[index])) !=
                        std::tolower(static_cast<unsigned char>(key[index]))) {
                        equal = false;
                        break;
                    }
                }
                if (equal) return propertyValue;
            }
            return {};
        };
        auto resolveHelper = [&propertyValue](const std::string& helper,
                                               std::string_view expectedExtension) {
            if (helper.empty()) return std::string{};
            std::string normalized = helper;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            std::string lower = normalized;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            const bool literalPath = normalized.find('/') != std::string::npos ||
                (lower.size() >= expectedExtension.size() &&
                 lower.compare(lower.size() - expectedExtension.size(),
                               expectedExtension.size(), expectedExtension) == 0);
            if (literalPath) return normalized;
            std::string value = propertyValue(normalized);
            if (value.empty() && !normalized.empty() && normalized.front() == '$')
                value = propertyValue(std::string_view(normalized).substr(1));
            return value;
        };
        entity.model = resolveHelper(visualization.model, ".mdl");
        entity.sprite = resolveHelper(visualization.sprite, ".vmt");
        if (entity.model.empty()) entity.model = propertyValue("model");
        // Point entities whose classname the FGD does not know get Hammer's
        // obsolete-marker billboard so they are visibly flagged in the views
        // instead of rendering as an anonymous default box.
        if (entity.model.empty() && entity.sprite.empty() &&
            fgd_ && !fgd_->empty() && !fgd_->findClass(entity.classname)) {
            entity.sprite = "editor/obsolete";
        }

        // Decals and overlays select by the exact clipped projection geometry,
        // not by a generic FGD helper box or an incidental sprite helper.
        if (!entity.projectedSurfaces.empty()) {
            hammer::vmf::Vec3 minimum{};
            hammer::vmf::Vec3 maximum{};
            bool valid = false;
            for (const auto& surface : entity.projectedSurfaces) {
                for (const auto& vertex : surface.triangles) {
                    if (!valid) {
                        minimum = maximum = vertex.position;
                        valid = true;
                    } else {
                        minimum.x = std::min(minimum.x, vertex.position.x);
                        minimum.y = std::min(minimum.y, vertex.position.y);
                        minimum.z = std::min(minimum.z, vertex.position.z);
                        maximum.x = std::max(maximum.x, vertex.position.x);
                        maximum.y = std::max(maximum.y, vertex.position.y);
                        maximum.z = std::max(maximum.z, vertex.position.z);
                    }
                }
            }
            if (valid) {
                // A surface-aligned decal may be mathematically flat on one
                // axis. Hammer keeps at least a one-unit culling/selection box
                // so it remains visible and selectable in perpendicular views.
                double* minimumComponents[3] = {&minimum.x, &minimum.y, &minimum.z};
                double* maximumComponents[3] = {&maximum.x, &maximum.y, &maximum.z};
                for (int axis = 0; axis < 3; ++axis) {
                    if (*maximumComponents[axis] - *minimumComponents[axis] < 1.0) {
                        const double center = (*minimumComponents[axis] + *maximumComponents[axis]) * 0.5;
                        *minimumComponents[axis] = center - 0.5;
                        *maximumComponents[axis] = center + 0.5;
                    }
                }
                for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex) {
                    entity.selectionCorners[static_cast<std::size_t>(cornerIndex)] = {
                        (cornerIndex & 1) ? maximum.x : minimum.x,
                        (cornerIndex & 2) ? maximum.y : minimum.y,
                        (cornerIndex & 4) ? maximum.z : minimum.z};
                    expandSceneBounds(entity.selectionCorners[static_cast<std::size_t>(cornerIndex)]);
                }
                entity.hasSelectionCorners = true;
                continue;
            }
        }

        bool rotateSelectionBounds = false;
        bool modelBoundsApplied = false;
        if (!entity.model.empty() && studioModels_) {
            const auto model = studioModels_->model(entity.model);
            if (model && model->valid) {
                entity.sizeMinimum = {model->minimum[0], model->minimum[1], model->minimum[2]};
                entity.sizeMaximum = {model->maximum[0], model->maximum[1], model->maximum[2]};
                rotateSelectionBounds = true;
                modelBoundsApplied = true;
            }
        }
        if (!modelBoundsApplied && !entity.sprite.empty() && materials_) {
            const auto material = materials_->material(entity.sprite);
            if (material && material->image.valid()) {
                // Hammer's sprite helper is centered on the entity origin. Use
                // the mounted VTF dimensions so selection matches the visible
                // sprite instead of the generic FGD point-entity box.
                const double halfWidth = std::max(1, material->image.width) * 0.5;
                const double halfHeight = std::max(1, material->image.height) * 0.5;
                entity.sizeMinimum = {-halfWidth, -halfWidth, -halfHeight};
                entity.sizeMaximum = {halfWidth, halfWidth, halfHeight};
            }
        }

        const hammer::camera::SourceTransform transform =
            hammer::camera::sourceTransform(entity.origin, entity.renderAngles());
        for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex) {
            const hammer::vmf::Vec3 local{
                (cornerIndex & 1) ? entity.sizeMaximum.x : entity.sizeMinimum.x,
                (cornerIndex & 2) ? entity.sizeMaximum.y : entity.sizeMinimum.y,
                (cornerIndex & 4) ? entity.sizeMaximum.z : entity.sizeMinimum.z};
            entity.selectionCorners[static_cast<std::size_t>(cornerIndex)] = rotateSelectionBounds
                ? transform.transformPoint(local)
                : hammer::vmf::Vec3{entity.origin.x + local.x,
                                    entity.origin.y + local.y,
                                    entity.origin.z + local.z};
            expandSceneBounds(entity.selectionCorners[static_cast<std::size_t>(cornerIndex)]);
        }
        entity.hasSelectionCorners = true;
    }
}

hammer::vmf::Bounds MapDocumentWidget::visualSelectionBounds() const
{
    hammer::vmf::Bounds bounds;
    if (!scene_) return editor_.selectionBounds();

    const auto selected = [this](const hammer::vmf::ObjectRef& object) {
        return std::find(editor_.selection().begin(), editor_.selection().end(), object) !=
               editor_.selection().end();
    };
    const auto expand = [&bounds](const hammer::vmf::Vec3& point) {
        if (!bounds.valid) {
            bounds.minimum = bounds.maximum = point;
            bounds.valid = true;
            return;
        }
        bounds.minimum.x = std::min(bounds.minimum.x, point.x);
        bounds.minimum.y = std::min(bounds.minimum.y, point.y);
        bounds.minimum.z = std::min(bounds.minimum.z, point.z);
        bounds.maximum.x = std::max(bounds.maximum.x, point.x);
        bounds.maximum.y = std::max(bounds.maximum.y, point.y);
        bounds.maximum.z = std::max(bounds.maximum.z, point.z);
    };

    for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
        const hammer::vmf::ObjectRef owner = brush.ownerEntityId >= 0
            ? hammer::vmf::ObjectRef{hammer::vmf::ObjectType::Entity, brush.ownerEntityId}
            : brush.object;
        if (!selected(owner) && !selected(brush.object)) continue;
        for (const hammer::vmf::Vec3& vertex : brush.vertices) expand(vertex);
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            for (const hammer::vmf::DisplacementVertex& vertex : face.displacementVertices)
                expand(vertex.position);
        }
    }

    for (const hammer::vmf::EntityMarker& marker : scene_->entities) {
        if (!selected(marker.object)) continue;
        if (marker.hasSelectionCorners) {
            for (const hammer::vmf::Vec3& corner : marker.selectionCorners) expand(corner);
        } else {
            expand(marker.origin);
        }
    }
    return bounds.valid ? bounds : editor_.selectionBounds();
}

void MapDocumentWidget::setSelectionOnViews()
{
    const hammer::vmf::Bounds bounds = visualSelectionBounds();
    for (MapViewWidget* view : views_) if (view) view->setSelection(editor_.selection(), bounds);
    // Clipper3D::GetClipResults re-reads the selection every time it runs, so
    // changing the selection while a clip line is up re-clips the new set.
    if (clipActive_) updateClipLine(clipPoints_[0], clipPoints_[1], clipViewAxis_);
}

void MapDocumentWidget::notifyDocumentState(const QString& message)
{
    // Every command ends here, which is where the snapshot a failed command
    // would have rolled back through is released. Commands that push straight
    // onto the stack (carve, clip, transaction commit) are covered too.
    if (!editor_.undoEnabled()) editor_.clearHistory();
    emit titleChanged(displayName());
    emit modifiedChanged(isModified());
    emit editStateChanged();
    if (!message.isEmpty()) emit documentMessage(message);
}

void MapDocumentWidget::notifySelectionState()
{
    emit selectionChanged(selectionSummary(), selectionSizeSummary());
    emit editStateChanged();
}

void MapDocumentWidget::closeEvent(QCloseEvent* event)
{
    if (maybeSave(window())) event->accept();
    else event->ignore();
}

MapViewWidget* MapDocumentWidget::perspectiveView() const
{
    for (MapViewWidget* view : views_) {
        if (view && view->kind() == MapViewWidget::Kind::Perspective) return view;
    }
    return nullptr;
}

void MapDocumentWidget::activateView(MapViewWidget* view)
{
    if (!view || activeView_ == view) return;
    for (MapViewWidget* candidate : views_) candidate->setActive(candidate == view);
    activeView_ = view;
    emit activeViewChanged(view);
}

void MapDocumentWidget::setGridVisible(bool visible)
{
    for (MapViewWidget* view : views_) view->setGridVisible(visible);
}

void MapDocumentWidget::setPerspectiveRenderingPaused(bool paused)
{
    for (MapViewWidget* view : views_) {
        if (view && view->kind() == MapViewWidget::Kind::Perspective)
            view->setRenderingPaused(paused);
    }
}

void MapDocumentWidget::setGridSnapEnabled(bool enabled)
{
    for (MapViewWidget* view : views_) view->setGridSnapEnabled(enabled);
}

void MapDocumentWidget::setGridSpacing(int spacing)
{
    for (MapViewWidget* view : views_) view->setGridSpacing(spacing);
}

void MapDocumentWidget::autosizeViews()
{
    activeViewMaximized_ = false;
    for (MapViewWidget* view : views_) view->show();
    topSplitter_->setSizes({1, 1});
    bottomSplitter_->setSizes({1, 1});
    verticalSplitter_->setSizes({1, 1});
}

void MapDocumentWidget::maximizeActiveView()
{
    if (!activeView_) return;
    if (activeViewMaximized_) {
        autosizeViews();
        return;
    }
    for (MapViewWidget* view : views_) view->setVisible(view == activeView_);
    activeViewMaximized_ = true;
}

void MapDocumentWidget::setCameraProjection(ProjectionMode mode)
{
    // Any pane can be the 3D view (or several can), so the projection applies
    // to every camera view rather than to whichever pane started as one.
    for (MapViewWidget* view : views_) {
        if (view && view->kind() == MapViewWidget::Kind::Perspective)
            view->setProjectionMode(mode);
    }
}

MapDocumentWidget::ProjectionMode MapDocumentWidget::cameraProjection() const
{
    MapViewWidget* view = perspectiveView();
    return view ? view->projectionMode() : ProjectionMode::Perspective;
}

void MapDocumentWidget::setActiveViewKind(MapViewWidget::Kind kind)
{
    if (!activeView_ || activeView_->kind() == kind) return;
    activeView_->setKind(kind);
    // The window's projection and 3D render-mode commands read the camera view,
    // and the pane's title changed with its type.
    emit activeViewChanged(activeView_);
}

void MapDocumentWidget::setUndoRedoActive(bool active)
{
    if (editor_.undoEnabled() == active) return;
    editor_.setUndoEnabled(active);
    notifyDocumentState({});
}

void MapDocumentWidget::undo()
{
    const QString label = QString::fromStdString(editor_.undoLabel());
    if (!editor_.undo()) return;
    rebuildScene(false);
    refreshMorphFromScene();
    notifyFaceSelectionState();
    notifyDocumentState(tr("Undo %1").arg(label));
    notifySelectionState();
}

void MapDocumentWidget::redo()
{
    const QString label = QString::fromStdString(editor_.redoLabel());
    if (!editor_.redo()) return;
    rebuildScene(false);
    refreshMorphFromScene();
    notifyFaceSelectionState();
    notifyDocumentState(tr("Redo %1").arg(label));
    notifySelectionState();
}

void MapDocumentWidget::tieSelectionToEntity()
{
    // The original reads the game configuration's DefaultSolidEntity here;
    // the port has no per-game configuration for it, so use the near-universal
    // Source default. Object Properties opens right after for changing it.
    const auto entity = editor_.tieSelectionToEntity("func_detail");
    if (!entity) {
        notifyDocumentState(tr("There are no eligible selected objects."));
        return;
    }
    // Solids changed owners; the incremental path does not track that.
    rebuildScene(false);
    setSelectionOnViews();
    notifyDocumentState(tr("Tied %1 to entity").arg(selectionSummary()));
    notifySelectionState();
    showObjectProperties(window());
}

void MapDocumentWidget::carveSelection()
{
    if (editor_.selection().empty()) {
        notifyDocumentState(tr("Carve: nothing is selected."));
        return;
    }
    if (!editor_.carveSelection()) {
        notifyDocumentState(tr("Carve: the selection does not overlap any solids."));
        return;
    }
    // Solids were replaced wholesale; the incremental path does not track that.
    rebuildScene(false);
    setSelectionOnViews();
    notifyDocumentState(tr("Carved with %1").arg(selectionSummary()));
    notifySelectionState();
}

void MapDocumentWidget::deleteSelection()
{
    const std::size_t count = editor_.selection().size();
    if (!editor_.deleteSelection()) return;
    rebuildScene(false);
    notifyDocumentState(tr("Deleted %1 object(s)").arg(static_cast<qulonglong>(count)));
    notifySelectionState();
}

void MapDocumentWidget::clearSelection()
{
    if (editor_.selection().empty()) return;
    editor_.clearSelection();
    setSelectionOnViews();
    notifySelectionState();
}

// CSelection::SetCurrentHit(hitNext / hitPrev). The previously current hit is
// toggled off, the index moves and wraps, and the new hit is toggled on.
void MapDocumentWidget::selectNextHit(bool forward)
{
    if (hitList_.empty()) return;
    const int count = static_cast<int>(hitList_.size());
    if (currentHit_ >= 0 && currentHit_ < count) {
        editor_.select(hitList_[static_cast<std::size_t>(currentHit_)], /*toggle=*/true);
    }
    currentHit_ = currentHit_ + (forward ? 1 : -1);
    if (currentHit_ >= count) currentHit_ = 0;
    else if (currentHit_ < 0) currentHit_ = count - 1;
    editor_.select(hitList_[static_cast<std::size_t>(currentHit_)], /*toggle=*/true);
    setSelectionOnViews();
    notifySelectionState();
}

void MapDocumentWidget::selectAll()
{
    std::vector<hammer::vmf::ObjectRef> selection;
    if (scene_) {
        for (const auto& brush : scene_->brushes) {
            // Hidden objects are not selectable (CSelection::RemoveInvisibles /
            // CMapDoc::SelectRegion skip invisible objects).
            if (hammer::vmf::isBrushHiddenByToolTextures(brush, hiddenToolTextures_)) continue;
            if (selectionMode_ != SelectionMode::Solids && brush.ownerEntityId >= 0) selection.push_back({hammer::vmf::ObjectType::Entity, brush.ownerEntityId});
            else selection.push_back(brush.object);
        }
        for (const auto& entity : scene_->entities) selection.push_back(entity.object);
    }
    editor_.setSelection(std::move(selection));
    setSelectionOnViews();
    notifySelectionState();
}

void MapDocumentWidget::invertSelection()
{
    if (!scene_) return;
    const std::vector<hammer::vmf::ObjectRef> previous = editor_.selection();
    const auto wasSelected = [&previous](const hammer::vmf::ObjectRef& object) {
        return std::find(previous.begin(), previous.end(), object) != previous.end();
    };
    std::vector<hammer::vmf::ObjectRef> selection;
    for (const auto& brush : scene_->brushes) {
        // A brush entity is one object; its solids follow it.
        if (brush.ownerEntityId >= 0) continue;
        if (hammer::vmf::isBrushHiddenByToolTextures(brush, hiddenToolTextures_)) continue;
        if (!wasSelected(brush.object)) selection.push_back(brush.object);
    }
    for (const auto& entity : scene_->entities) {
        if (!wasSelected(entity.object)) selection.push_back(entity.object);
    }
    editor_.setSelection(std::move(selection));
    setSelectionOnViews();
    notifySelectionState();
}

std::size_t MapDocumentWidget::createEntityGallery()
{
    if (!fgd_) return 0;

    // Every point class in the loaded game data, in name order so the gallery
    // is reproducible across runs. Solid classes are skipped: they are brush
    // entities and have no geometry to tie until the user supplies one.
    std::vector<const hammer::fgd::EntityClass*> classes = fgd_->pointClasses();
    std::sort(classes.begin(), classes.end(),
              [](const hammer::fgd::EntityClass* a, const hammer::fgd::EntityClass* b) {
                  return a->name < b->name;
              });
    if (classes.empty()) return 0;

    // Hammer's own spacing for this command is not recoverable from the
    // reference tree — only the ID_MAP_ENTITY_GALLERY resource id survives, not
    // the handler — so the layout is derived from the data instead: each entity
    // gets a cell as wide and deep as its own FGD bounding box plus a gutter,
    // rounded up to the grid, and the cells are packed into a square-ish grid.
    // Nothing overlaps whatever its box size, which is what the command is for.
    constexpr double Gutter = 32.0;
    constexpr double CellGrid = 16.0;
    const auto cellSize = [&](const hammer::fgd::EntityClass* entityClass) {
        const hammer::fgd::EntityVisualization visualization =
            fgd_->effectiveVisualization(entityClass->name);
        const double width = visualization.sizeMaximum[0] - visualization.sizeMinimum[0];
        const double depth = visualization.sizeMaximum[1] - visualization.sizeMinimum[1];
        return QPointF(std::ceil((std::max(16.0, width) + Gutter) / CellGrid) * CellGrid,
                       std::ceil((std::max(16.0, depth) + Gutter) / CellGrid) * CellGrid);
    };

    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(classes.size()))));
    // Uniform column pitch keeps the grid readable; the widest entity in a
    // column decides that column's width.
    std::vector<double> columnWidths(static_cast<std::size_t>(columns), 0.0);
    std::vector<double> rowDepths;
    for (std::size_t index = 0; index < classes.size(); ++index) {
        const QPointF size = cellSize(classes[index]);
        const std::size_t column = index % static_cast<std::size_t>(columns);
        const std::size_t row = index / static_cast<std::size_t>(columns);
        columnWidths[column] = std::max(columnWidths[column], size.x());
        if (row >= rowDepths.size()) rowDepths.push_back(0.0);
        rowDepths[row] = std::max(rowDepths[row], size.y());
    }

    // Anchored where the 2D views are looking, snapped to the grid, so the
    // gallery lands in sight instead of at the world origin.
    const hammer::vmf::Vec3 anchor = viewsCenterWorld();
    const double startX = std::round(anchor.x / CellGrid) * CellGrid;
    const double startY = std::round(anchor.y / CellGrid) * CellGrid;
    const double startZ = std::round(anchor.z / CellGrid) * CellGrid;

    std::vector<hammer::vmf::EditorModel::PointEntitySpec> specs;
    specs.reserve(classes.size());
    double y = startY;
    for (std::size_t row = 0; row < rowDepths.size(); ++row) {
        double x = startX;
        for (std::size_t column = 0; column < static_cast<std::size_t>(columns); ++column) {
            const std::size_t index = row * static_cast<std::size_t>(columns) + column;
            if (index >= classes.size()) break;
            const hammer::fgd::EntityClass* entityClass = classes[index];
            specs.push_back({entityClass->name, {x, y, startZ},
                             entityDefaults(entityClass->name)});
            x += columnWidths[column];
        }
        // Rows march away from the anchor, so the first row stays at the top.
        y -= rowDepths[row];
    }

    const std::size_t created = editor_.createPointEntities(specs, "Entity Gallery");
    if (created == 0) return 0;
    rebuildScene(false);
    notifyDocumentState(tr("Entity gallery: created %1 entities")
                            .arg(static_cast<qulonglong>(created)));
    notifySelectionState();
    centerViewsOnSelection();
    return created;
}

std::vector<MapDocumentWidget::EntityReportEntry> MapDocumentWidget::entityReport() const
{
    std::vector<EntityReportEntry> entries;
    for (const hammer::vmf::Block& root : editor_.document().roots()) {
        if (QString::fromStdString(root.name).compare(QStringLiteral("entity"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const std::string* id = root.value("id");
        if (!id) continue;

        EntityReportEntry entry;
        entry.id = std::atoi(id->c_str());
        for (const hammer::vmf::Entry& property : root.entries) {
            if (property.kind != hammer::vmf::Entry::Kind::KeyValue) continue;
            entry.properties.emplace_back(QString::fromStdString(property.key),
                                          QString::fromStdString(property.value));
        }
        if (const std::string* classname = root.value("classname")) {
            entry.classname = QString::fromStdString(*classname);
        }
        if (const std::string* targetName = root.value("targetname")) {
            entry.targetName = QString::fromStdString(*targetName);
        }
        entry.brushEntity = !root.children("solid").empty();
        // An entity is hidden when a VisGroup or QuickHide has taken it out of
        // the scene. A brush entity additionally counts as hidden once the
        // tool-texture filter has hidden every solid it owns.
        if (hiddenEntities_.contains(entry.id)) {
            entry.hidden = true;
        } else if (entry.brushEntity && scene_) {
            bool anySolid = false;
            bool allHidden = true;
            for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
                if (brush.ownerEntityId != entry.id) continue;
                anySolid = true;
                if (!hammer::vmf::isBrushHiddenByToolTextures(brush, hiddenToolTextures_)) {
                    allHidden = false;
                    break;
                }
            }
            entry.hidden = anySolid && allHidden;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

void MapDocumentWidget::selectEntitiesById(const std::vector<int>& ids)
{
    std::vector<hammer::vmf::ObjectRef> selection;
    selection.reserve(ids.size());
    for (const int id : ids) selection.push_back({hammer::vmf::ObjectType::Entity, id});
    selectObjects(std::move(selection));
}

void MapDocumentWidget::selectObjects(std::vector<hammer::vmf::ObjectRef> objects)
{
    editor_.setSelection(std::move(objects));
    hitList_.clear();
    currentHit_ = -1;
    setSelectionOnViews();
    notifySelectionState();
}

// ---------------------------------------------------------------------------
// Map > Check for Problems (hammer/mapcheckdlg.cpp).
// ---------------------------------------------------------------------------
namespace {

using hammer::vmf::Block;
using hammer::vmf::Document;
using hammer::vmf::Entry;

bool sameName(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

int blockIntId(const Block& block)
{
    const std::string* id = block.value("id");
    return id ? std::atoi(id->c_str()) : -1;
}

// Every solid in the map paired with the entity that owns it (-1 for world
// solids), which is the list all the solid checks walk.
struct SolidRef
{
    const Block* solid{nullptr};
    int id{-1};
    int ownerEntityId{-1};
};

std::vector<SolidRef> collectSolids(const Document& document)
{
    std::vector<SolidRef> solids;
    for (const Block& root : document.roots()) {
        const bool isWorld = sameName(root.name, "world");
        if (!isWorld && !sameName(root.name, "entity")) continue;
        const int owner = isWorld ? -1 : blockIntId(root);
        for (const Block* solid : root.children("solid")) {
            solids.push_back({solid, blockIntId(*solid), owner});
        }
    }
    return solids;
}

// The highest "id" anywhere in the document. Sides, solids and entities all
// draw from one id space in a VMF, so a fresh id has to clear all of them.
int maximumBlockId(const Block& block)
{
    int maximum = 0;
    for (const Entry& entry : block.entries) {
        if (entry.kind == Entry::Kind::KeyValue) {
            if (entry.key == "id") maximum = std::max(maximum, std::atoi(entry.value.c_str()));
        } else if (entry.child) {
            maximum = std::max(maximum, maximumBlockId(*entry.child));
        }
    }
    return maximum;
}

int maximumDocumentId(const Document& document)
{
    int maximum = 0;
    for (const Block& root : document.roots()) maximum = std::max(maximum, maximumBlockId(root));
    return maximum;
}

// CEditGameClass's connection value: "target,input,parameter,delay,times",
// with \x1b as the newer separator (see rewriteConnectionTargets).
std::pair<std::string, std::string> connectionTargetAndInput(const std::string& value)
{
    const std::size_t first = value.find_first_of(",\x1b");
    if (first == std::string::npos) return {value, {}};
    const std::size_t second = value.find_first_of(",\x1b", first + 1);
    return {value.substr(0, first),
            value.substr(first + 1, second == std::string::npos ? std::string::npos
                                                                : second - first - 1)};
}

// CMapEntity's built-in keys, which no FGD class declares but every entity
// carries. Without this the unused-keyvalue check flags the whole map.
bool isIntrinsicEntityKey(std::string_view key)
{
    static constexpr std::string_view kIntrinsic[] = {"id", "classname", "origin", "angles",
                                                      "spawnflags", "hammerid"};
    return std::any_of(std::begin(kIntrinsic), std::end(kIntrinsic),
                       [&](std::string_view known) { return sameName(key, known); });
}

// CheckValidTarget's procedural names, which always resolve at runtime.
bool isProceduralTargetName(std::string_view name)
{
    static constexpr std::string_view kProcedural[] = {"!activator", "!caller", "!player", "!self"};
    return std::any_of(std::begin(kProcedural), std::end(kProcedural),
                       [&](std::string_view known) { return sameName(name, known); });
}

// GDinputvariable types ivTargetDest / ivTargetNameOrClass, matched on the raw
// FGD type so a class using either spelling is caught.
bool isTargetDestinationType(const hammer::fgd::PropertyDefinition& property)
{
    return sameName(property.rawType, "target_destination") ||
           sameName(property.rawType, "target_name_or_class");
}

double axisLength(const hammer::vmf::Vec3& axis)
{
    return std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
}

// CMapFace::IsTextureAxisValid: the mapping is broken when either axis has
// collapsed, the two are colinear (so they span no plane), or a scale is zero.
bool areTextureAxesValid(const hammer::vmf::FaceTexture& texture)
{
    const hammer::vmf::Vec3& u = texture.uAxis;
    const hammer::vmf::Vec3& v = texture.vAxis;
    if (axisLength(u) < 1e-6 || axisLength(v) < 1e-6) return false;
    const hammer::vmf::Vec3 cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                                  u.x * v.y - u.y * v.x};
    if (axisLength(cross) < 1e-6) return false;
    return texture.uScale != 0.0 && texture.vScale != 0.0;
}

// The side's plane normal, computed exactly as parseTextureAxis's neighbour
// parsePlane does in VmfScene.cpp: normal = (a - b) x (c - b).
std::optional<hammer::vmf::Vec3> sidePlaneNormal(const Block& side)
{
    const std::string* text = side.value("plane");
    if (!text) return std::nullopt;
    double values[9];
    int parsed = 0;
    const char* cursor = text->c_str();
    while (parsed < 9 && *cursor) {
        if (*cursor == '(' || *cursor == ')' || std::isspace(static_cast<unsigned char>(*cursor))) {
            ++cursor;
            continue;
        }
        char* end = nullptr;
        const double value = std::strtod(cursor, &end);
        if (!end || end == cursor) return std::nullopt;
        values[parsed++] = value;
        cursor = end;
    }
    if (parsed != 9) return std::nullopt;
    const hammer::vmf::Vec3 a{values[0], values[1], values[2]};
    const hammer::vmf::Vec3 b{values[3], values[4], values[5]};
    const hammer::vmf::Vec3 c{values[6], values[7], values[8]};
    const hammer::vmf::Vec3 first{a.x - b.x, a.y - b.y, a.z - b.z};
    const hammer::vmf::Vec3 second{c.x - b.x, c.y - b.y, c.z - b.z};
    hammer::vmf::Vec3 normal{first.y * second.z - first.z * second.y,
                             first.z * second.x - first.x * second.z,
                             first.x * second.y - first.y * second.x};
    const double magnitude = axisLength(normal);
    if (magnitude < 1e-9) return std::nullopt;
    return hammer::vmf::Vec3{normal.x / magnitude, normal.y / magnitude, normal.z / magnitude};
}

} // namespace

// CMapCheckDlg::DoCheck. Deliberately not ported, because the check is dead or
// has nothing to work on here:
//   - CheckDuplicatePlanes is commented out of the original's DoCheck.
//   - ErrorDuplicateKeys and ErrorKillInputRaceCondition are declared but never
//     raised by any check (the kill-input one is a TODO comment).
//   - CheckSolidContents only runs for mapformat == mfQuake2; this port is VMF.
//   - CheckMixedFaces tests for the Quake '*' texture prefix, which no Source
//     material name carries.
// The dialog's "Check visible objects only" box is honoured: visibility now
// comes from the per-object visgroupshown flag and QuickHide (see
// refreshHiddenObjects), not just from the tool-texture filter.
std::vector<MapDocumentWidget::MapProblem> MapDocumentWidget::checkForProblems(bool visibleOnly) const
{
    using Type = MapProblem::Type;
    const hammer::vmf::Document& document = editor_.document();
    std::vector<MapProblem> problems;

    // CMapCheckDlg's IsCheckVisible. An object is hidden when a VisGroup or
    // QuickHide has taken it out of the scene, or when the tool-texture filter
    // has hidden every one of its faces. VisGroup-hidden objects are absent
    // from the scene entirely, so they must be tested against the hidden sets
    // rather than looked for among the brushes.
    const auto solidVisible = [&](int solidId) {
        if (!visibleOnly) return true;
        if (hiddenSolids_.contains(solidId)) return false;
        if (!scene_) return true;
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            if (brush.id != solidId) continue;
            if (brush.ownerEntityId >= 0 && hiddenEntities_.contains(brush.ownerEntityId))
                return false;
            return !hammer::vmf::isBrushHiddenByToolTextures(brush, hiddenToolTextures_);
        }
        return true;
    };
    const auto entityVisible = [&](const Block& entity) {
        if (!visibleOnly) return true;
        const int entityId = blockIntId(entity);
        if (entityId >= 0 && hiddenEntities_.contains(entityId)) return false;
        const std::vector<const Block*> solids = entity.children("solid");
        if (solids.empty()) return true;  // a point entity's own flag decided it
        return std::any_of(solids.cbegin(), solids.cend(),
                           [&](const Block* solid) { return solidVisible(blockIntId(*solid)); });
    };

    const std::vector<SolidRef> solids = collectSolids(document);
    const auto solidRef = [](const SolidRef& solid) {
        return std::vector<hammer::vmf::ObjectRef>{
            {hammer::vmf::ObjectType::Solid, solid.id}};
    };
    const auto entityRef = [](int id) {
        return std::vector<hammer::vmf::ObjectRef>{{hammer::vmf::ObjectType::Entity, id}};
    };

    // --- Map validation: CheckRequirements ---------------------------------
    bool playerStart = false;
    for (const Block& root : document.roots()) {
        if (!sameName(root.name, "entity")) continue;
        const std::string* classname = root.value("classname");
        if (classname && sameName(*classname, "info_player_start") && entityVisible(root)) {
            playerStart = true;
            break;
        }
    }
    if (!playerStart) {
        MapProblem problem;
        problem.type = Type::NoPlayerStart;
        problem.text = tr("No player start");
        problem.description = tr("There is no info_player_start entity in this map, so there is "
                                 "nowhere for the player to spawn. Place one before compiling.");
        problems.push_back(std::move(problem));
    }

    // --- Solid validation --------------------------------------------------
    // CheckDuplicateFaceIDs: the first sighting of an id is the good one, every
    // later side carrying it is the error.
    std::unordered_set<int> seenSideIds;
    for (const SolidRef& solid : solids) {
        if (!solidVisible(solid.id)) continue;
        for (const Block* side : solid.solid->children("side")) {
            const int sideId = blockIntId(*side);
            if (sideId < 0) continue;
            if (seenSideIds.insert(sideId).second) continue;
            MapProblem problem;
            problem.type = Type::DuplicateFaceIds;
            problem.text = tr("Duplicate face ID %1").arg(sideId);
            problem.description = tr("Two or more faces in this map share the face ID %1. Face IDs "
                                     "must be unique; the fix assigns this face a new one.")
                                      .arg(sideId);
            problem.objects = solidRef(solid);
            problem.sideId = sideId;
            problem.canFix = true;
            problems.push_back(std::move(problem));
        }
    }

    // CheckSolidIntegrity. "Invalid" here means the planes no longer bound a
    // solid: building the winding yields no face, or a face with fewer than
    // three points.
    for (const SolidRef& solid : solids) {
        if (!solidVisible(solid.id)) continue;
        const hammer::vmf::BrushGeometry geometry =
            hammer::vmf::buildSolidGeometry(*solid.solid, solid.ownerEntityId);
        const bool degenerate =
            geometry.faces.empty() ||
            std::any_of(geometry.faces.cbegin(), geometry.faces.cend(),
                        [](const hammer::vmf::FaceGeometry& face) {
                            return !face.displacement && face.vertices.size() < 3;
                        });
        if (!degenerate) continue;
        MapProblem problem;
        problem.type = Type::SolidStructure;
        problem.text = tr("Invalid solid structure (solid %1)").arg(solid.id);
        // DEVIATION: FixSolidStructure rebuilds the solid from its planes
        // (CMapSolid::CreateFromPlanes); this port has no equivalent, so the
        // problem is reported without a fix.
        problem.description = tr("The planes of this solid do not bound a valid convex volume, so "
                                 "at least one of its faces has no shape. It will not compile, and "
                                 "must be repaired or deleted by hand.");
        problem.objects = solidRef(solid);
        problems.push_back(std::move(problem));
    }

    // CheckInvalidTextures: one error per solid, as in the original, which
    // returns after the first bad face it finds.
    for (const SolidRef& solid : solids) {
        if (!solidVisible(solid.id)) continue;
        for (const Block* side : solid.solid->children("side")) {
            const std::string* material = side->value("material");
            if (materials_ && materials_->fileSystem() && material && !material->empty()) {
                const std::string path =
                    "materials/" + hammer::vmf::normalizeMaterialPath(*material) + ".vmt";
                if (!materials_->fileSystem()->exists(path)) {
                    MapProblem problem;
                    problem.type = Type::InvalidTexture;
                    problem.text = tr("Invalid material \"%1\"")
                                       .arg(QString::fromStdString(*material));
                    problem.description =
                        tr("A face of this solid uses the material \"%1\", which is not in any of "
                           "the loaded game paths. The fix replaces every missing material on this "
                           "solid with %2.")
                            .arg(QString::fromStdString(*material),
                                 QStringLiteral("tools/toolsnodraw"));
                    problem.objects = solidRef(solid);
                    problem.canFix = true;
                    problems.push_back(std::move(problem));
                    break;
                }
            }

            const hammer::vmf::FaceTexture texture = hammer::vmf::readFaceTexture(*side);
            if (areTextureAxesValid(texture)) continue;
            MapProblem problem;
            problem.type = Type::InvalidTextureAxes;
            problem.text = tr("Invalid texture axes (solid %1)").arg(solid.id);
            problem.description = tr("A face of this solid has a degenerate texture mapping: its U "
                                     "and V axes have collapsed or its scale is zero. The fix "
                                     "reinitializes the mapping world-aligned.");
            problem.objects = solidRef(solid);
            problem.canFix = true;
            problems.push_back(std::move(problem));
            break;
        }
    }

    // --- Entity validation -------------------------------------------------
    // The name and classname index CheckValidTarget searches.
    std::unordered_set<std::string> targetNames;
    std::unordered_set<std::string> classNames;
    const auto lowered = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    for (const Block& root : document.roots()) {
        if (!sameName(root.name, "entity")) continue;
        if (visibleOnly && !entityVisible(root)) continue;
        if (const std::string* name = root.value("targetname")) targetNames.insert(lowered(*name));
        if (const std::string* classname = root.value("classname")) {
            classNames.insert(lowered(*classname));
        }
    }

    std::unordered_map<int, int> nodeIdCounts;
    for (const Block& root : document.roots()) {
        if (!sameName(root.name, "entity")) continue;
        if (const std::string* nodeId = root.value("nodeid")) {
            const int value = std::atoi(nodeId->c_str());
            if (value != 0) ++nodeIdCounts[value];
        }
    }

    for (const Block& root : document.roots()) {
        if (!sameName(root.name, "entity")) continue;
        if (!entityVisible(root)) continue;
        const int id = blockIntId(root);
        const std::string* classnameValue = root.value("classname");
        const std::string classname = classnameValue ? *classnameValue : std::string();
        const QString displayName = classname.empty() ? tr("(no classname)")
                                                      : QString::fromStdString(classname);
        const hammer::fgd::EntityClass* entityClass =
            fgd_ && !classname.empty() ? fgd_->findClass(classname) : nullptr;

        // CheckDuplicateNodeIDs.
        if (const std::string* nodeId = root.value("nodeid")) {
            const int value = std::atoi(nodeId->c_str());
            const auto count = nodeIdCounts.find(value);
            if (value != 0 && count != nodeIdCounts.end() && count->second > 1) {
                MapProblem problem;
                problem.type = Type::DuplicateNodeIds;
                problem.text = tr("Duplicate node ID %1 (%2)").arg(value).arg(displayName);
                problem.description = tr("More than one AI node in this map uses node ID %1. Node "
                                         "IDs must be unique or the node graph will be built "
                                         "wrong; the fix assigns this node a new one.")
                                          .arg(value);
                problem.objects = entityRef(id);
                problem.canFix = true;
                problems.push_back(std::move(problem));
            }
        }

        // CheckEmptyEntities: a class the game data declares as a brush entity
        // that owns no solids. (The original's test is !IsPlaceholder() &&
        // !GetChildCount(); the FGD class kind is what tells the two apart here.)
        if (entityClass && entityClass->kind == hammer::fgd::ClassKind::Solid &&
            root.children("solid").empty()) {
            MapProblem problem;
            problem.type = Type::EmptyEntity;
            problem.text = tr("Empty entity (%1)").arg(displayName);
            problem.description = tr("%1 is a brush entity but owns no solids, so it will do "
                                     "nothing in game. The fix deletes it.")
                                      .arg(displayName);
            problem.objects = entityRef(id);
            problem.canFix = true;
            problems.push_back(std::move(problem));
        }

        // CheckOverlayFaceList.
        if (sameName(classname, "info_overlay")) {
            const std::string* sides = root.value("sides");
            if (!sides || sides->find_first_not_of(" \t") == std::string::npos) {
                MapProblem problem;
                problem.type = Type::OverlayFaceList;
                problem.text = tr("Overlay with no faces (%1)").arg(displayName);
                problem.description = tr("This overlay is not applied to any face, which crashes "
                                         "the compile tools. The fix deletes it.");
                problem.objects = entityRef(id);
                problem.canFix = true;
                problems.push_back(std::move(problem));
            }
        }

        // CheckUnusedKeyvalues. Skipped for classes the game data does not
        // define (nothing to compare against) and for multi_manager, whose
        // keys are its targets, exactly as the original skips both.
        if (entityClass && !sameName(classname, "multi_manager")) {
            const std::vector<hammer::fgd::PropertyDefinition> properties =
                fgd_->effectiveProperties(classname);
            for (const Entry& entry : root.entries) {
                if (entry.kind != Entry::Kind::KeyValue) continue;
                if (isIntrinsicEntityKey(entry.key)) continue;
                const bool declared =
                    std::any_of(properties.cbegin(), properties.cend(),
                                [&](const hammer::fgd::PropertyDefinition& property) {
                                    return sameName(property.key, entry.key);
                                });
                if (declared) continue;
                MapProblem problem;
                problem.type = Type::UnusedKeyvalue;
                problem.text = tr("Unused keyvalue \"%1\" in %2")
                                   .arg(QString::fromStdString(entry.key), displayName);
                problem.description = tr("The game data for %1 does not define a \"%2\" key, so "
                                         "the game will ignore it. The fix removes the key.")
                                          .arg(displayName, QString::fromStdString(entry.key));
                problem.objects = entityRef(id);
                problem.key = QString::fromStdString(entry.key);
                problem.canFix = true;
                problems.push_back(std::move(problem));
            }
        }

        // CheckMissingTargets.
        const auto checkTarget = [&](const std::string& key, const std::string& value,
                                     bool alsoClassNames) {
            if (value.empty() || isProceduralTargetName(value)) return;
            const std::string wanted = lowered(value);
            if (targetNames.count(wanted) != 0) return;
            if (alsoClassNames && classNames.count(wanted) != 0) return;
            MapProblem problem;
            problem.type = Type::MissingTarget;
            problem.text = tr("%1 targets missing entity \"%2\"")
                               .arg(displayName, QString::fromStdString(value));
            problem.description = tr("The \"%1\" key of this entity refers to \"%2\", and no entity "
                                     "in the map has that name. The fix clears the key.")
                                      .arg(QString::fromStdString(key),
                                           QString::fromStdString(value));
            problem.objects = entityRef(id);
            problem.key = QString::fromStdString(key);
            problem.canFix = true;
            problems.push_back(std::move(problem));
        };
        if (entityClass) {
            for (const hammer::fgd::PropertyDefinition& property :
                 fgd_->effectiveProperties(classname)) {
                if (!isTargetDestinationType(property)) continue;
                if (const std::string* value = root.value(property.key)) {
                    checkTarget(property.key, *value, sameName(property.rawType,
                                                               "target_name_or_class"));
                }
            }
        } else if (const std::string* value = root.value("target")) {
            // Unknown class: all the original can do is check "target".
            checkTarget("target", *value, false);
        }

        // CheckBadConnections (CEntityConnection::ValidateOutputConnections):
        // an output is bad when nothing answers to its target name, or when the
        // target's class has no such input.
        for (const Block* connections : root.children("connections")) {
            for (const Entry& entry : connections->entries) {
                if (entry.kind != Entry::Kind::KeyValue) continue;
                const auto [target, input] = connectionTargetAndInput(entry.value);
                if (target.empty() || isProceduralTargetName(target)) continue;
                const std::string wantedTarget = lowered(target);
                const bool namedTarget = targetNames.count(wantedTarget) != 0;
                const bool classTarget = classNames.count(wantedTarget) != 0;
                QString reason;
                if (!namedTarget && !classTarget) {
                    reason = tr("no entity in the map is named \"%1\"")
                                 .arg(QString::fromStdString(target));
                } else if (fgd_ && !input.empty()) {
                    // Only classname targets can be resolved to one class here,
                    // so that is the only case where the input name is checked.
                    const hammer::fgd::EntityClass* targetClass =
                        !namedTarget ? fgd_->findClass(target) : nullptr;
                    if (targetClass) {
                        const std::vector<hammer::fgd::IoDefinition> inputs =
                            fgd_->effectiveInputs(target);
                        const bool known =
                            inputs.empty() ||
                            std::any_of(inputs.cbegin(), inputs.cend(),
                                        [&](const hammer::fgd::IoDefinition& definition) {
                                            return sameName(definition.name, input);
                                        });
                        if (!known) {
                            reason = tr("%1 has no \"%2\" input")
                                         .arg(QString::fromStdString(target),
                                              QString::fromStdString(input));
                        }
                    }
                }
                if (reason.isEmpty()) continue;
                MapProblem problem;
                problem.type = Type::BadConnection;
                problem.text = tr("Bad connection in %1 (%2)")
                                   .arg(displayName, QString::fromStdString(entry.key));
                problem.description = tr("The output \"%1\" of this entity fires \"%2\" at \"%3\", "
                                         "but %4. The connection will never do anything; the fix "
                                         "removes it.")
                                          .arg(QString::fromStdString(entry.key),
                                               QString::fromStdString(input),
                                               QString::fromStdString(target), reason);
                problem.objects = entityRef(id);
                problem.key = QString::fromStdString(entry.key);
                problem.value = QString::fromStdString(entry.value);
                problem.canFix = true;
                problems.push_back(std::move(problem));
            }
        }
    }

    return problems;
}

// CMapCheckDlg::Fix, over as many errors as the Fix / Fix all buttons hand it,
// as one undo step.
std::size_t MapDocumentWidget::fixProblems(const std::vector<MapProblem>& problems)
{
    using Type = MapProblem::Type;
    std::size_t fixed = 0;

    const bool changed = editor_.applyDocumentEdit(
        [&](hammer::vmf::Document& document) {
            int nextId = maximumDocumentId(document) + 1;
            bool any = false;

            // Finds the mutable block a problem points at.
            const auto findEntity = [&](int id) -> Block* {
                for (Block& root : document.roots()) {
                    if (sameName(root.name, "entity") && blockIntId(root) == id) return &root;
                }
                return nullptr;
            };
            const auto findSolid = [&](int id) -> Block* {
                for (Block& root : document.roots()) {
                    if (!sameName(root.name, "world") && !sameName(root.name, "entity")) continue;
                    for (Block* solid : root.children("solid")) {
                        if (blockIntId(*solid) == id) return solid;
                    }
                }
                return nullptr;
            };
            const auto eraseEntity = [&](int id) {
                std::vector<Block>& roots = document.roots();
                const auto match = std::find_if(roots.begin(), roots.end(), [&](const Block& root) {
                    return sameName(root.name, "entity") && blockIntId(root) == id;
                });
                if (match == roots.end()) return false;
                roots.erase(match);
                return true;
            };
            const auto eraseKey = [](Block& block, const QString& key) {
                const std::string wanted = key.toStdString();
                const auto match = std::find_if(
                    block.entries.begin(), block.entries.end(), [&](const Entry& entry) {
                        return entry.kind == Entry::Kind::KeyValue && sameName(entry.key, wanted);
                    });
                if (match == block.entries.end()) return false;
                block.entries.erase(match);
                return true;
            };

            for (const MapProblem& problem : problems) {
                if (!problem.canFix) continue;
                const int objectId = problem.objects.empty() ? -1 : problem.objects.front().id;
                bool done = false;

                switch (problem.type) {
                    // DEVIATION: the original's Fix() routes ErrorDuplicateFaceIDs
                    // to FixDuplicatePlanes, which deletes faces with matching
                    // normals; its FixDuplicateFaceIDs (assign a fresh id) is
                    // never called. That is a Valve bug — the intended fix is
                    // the one below.
                    case Type::DuplicateFaceIds: {
                        Block* solid = findSolid(objectId);
                        if (!solid) break;
                        for (Block* side : solid->children("side")) {
                            if (blockIntId(*side) != problem.sideId) continue;
                            side->setValue("id", std::to_string(nextId++));
                            done = true;
                            break;
                        }
                        break;
                    }
                    case Type::DuplicateNodeIds: {
                        Block* entity = findEntity(objectId);
                        if (!entity) break;
                        // A node id is its own counter in the original
                        // (CMapDoc::GetNextNodeID); one past the highest in use
                        // is the same guarantee.
                        int nextNodeId = 0;
                        for (const Block& root : document.roots()) {
                            if (!sameName(root.name, "entity")) continue;
                            if (const std::string* value = root.value("nodeid")) {
                                nextNodeId = std::max(nextNodeId, std::atoi(value->c_str()));
                            }
                        }
                        entity->setValue("nodeid", std::to_string(nextNodeId + 1));
                        done = true;
                        break;
                    }
                    case Type::InvalidTexture: {
                        Block* solid = findSolid(objectId);
                        if (!solid || !materials_ || !materials_->fileSystem()) break;
                        for (Block* side : solid->children("side")) {
                            const std::string* material = side->value("material");
                            if (!material || material->empty()) continue;
                            const std::string path =
                                "materials/" + hammer::vmf::normalizeMaterialPath(*material) +
                                ".vmt";
                            if (materials_->fileSystem()->exists(path)) continue;
                            side->setValue("material", "TOOLS/TOOLSNODRAW");
                            done = true;
                        }
                        break;
                    }
                    case Type::InvalidTextureAxes: {
                        Block* solid = findSolid(objectId);
                        if (!solid) break;
                        for (Block* side : solid->children("side")) {
                            hammer::vmf::FaceTexture texture = hammer::vmf::readFaceTexture(*side);
                            if (areTextureAxesValid(texture)) continue;
                            const std::optional<hammer::vmf::Vec3> normal = sidePlaneNormal(*side);
                            if (!normal) continue;
                            if (texture.uScale == 0.0) texture.uScale = 0.25;
                            if (texture.vScale == 0.0) texture.vScale = 0.25;
                            hammer::vmf::initializeTextureAxes(
                                texture, *normal, hammer::vmf::TextureAlignment::World);
                            hammer::vmf::writeFaceTexture(*side, texture);
                            done = true;
                        }
                        break;
                    }
                    case Type::UnusedKeyvalue:
                    case Type::MissingTarget: {
                        Block* entity = findEntity(objectId);
                        if (entity) done = eraseKey(*entity, problem.key);
                        break;
                    }
                    case Type::EmptyEntity:
                    case Type::OverlayFaceList: {
                        done = eraseEntity(objectId);
                        break;
                    }
                    case Type::BadConnection: {
                        Block* entity = findEntity(objectId);
                        if (!entity) break;
                        const std::string output = problem.key.toStdString();
                        const std::string value = problem.value.toStdString();
                        for (Block* connections : entity->children("connections")) {
                            const auto match = std::find_if(
                                connections->entries.begin(), connections->entries.end(),
                                [&](const Entry& entry) {
                                    return entry.kind == Entry::Kind::KeyValue &&
                                           sameName(entry.key, output) && entry.value == value;
                                });
                            if (match == connections->entries.end()) continue;
                            connections->entries.erase(match);
                            done = true;
                            break;
                        }
                        break;
                    }
                    case Type::NoPlayerStart:
                    case Type::SolidStructure:
                        break;
                }

                if (done) {
                    ++fixed;
                    any = true;
                }
            }
            return any;
        },
        "Fix Map Problems");

    if (!changed) return 0;
    rebuildScene(false);
    notifyDocumentState(tr("Fixed %1 problem(s)").arg(static_cast<qulonglong>(fixed)));
    notifySelectionState();
    return fixed;
}

std::size_t MapDocumentWidget::selectEntitiesByName(const QString& name)
{
    const QString wanted = name.trimmed();
    if (wanted.isEmpty()) return 0;

    // Searched against the document rather than the scene so brush entities
    // count too — the scene's entity list only carries point-entity markers.
    // Targetnames are matched the way the engine matches them, ignoring case.
    std::vector<hammer::vmf::ObjectRef> found;
    for (const hammer::vmf::Block& root : editor_.document().roots()) {
        if (QString::fromStdString(root.name).compare(QStringLiteral("entity"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const std::string* targetName = root.value("targetname");
        if (!targetName) continue;
        if (QString::fromStdString(*targetName).compare(wanted, Qt::CaseInsensitive) != 0) continue;
        const std::string* id = root.value("id");
        if (!id) continue;
        found.push_back({hammer::vmf::ObjectType::Entity, std::atoi(id->c_str())});
    }
    if (found.empty()) return 0;

    const std::size_t count = found.size();
    editor_.setSelection(std::move(found));
    setSelectionOnViews();
    notifySelectionState();
    centerViewsOnSelection();
    return count;
}

void MapDocumentWidget::centerViewsOnSelection()
{
    const hammer::vmf::Bounds bounds = editor_.selectionBounds();
    if (!bounds.valid) return;
    const hammer::vmf::Vec3 center{(bounds.minimum.x + bounds.maximum.x) * 0.5,
                                   (bounds.minimum.y + bounds.maximum.y) * 0.5,
                                   (bounds.minimum.z + bounds.maximum.z) * 0.5};
    const double radius = 0.5 * std::sqrt(
        std::pow(bounds.maximum.x - bounds.minimum.x, 2.0) +
        std::pow(bounds.maximum.y - bounds.minimum.y, 2.0) +
        std::pow(bounds.maximum.z - bounds.minimum.z, 2.0));
    for (MapViewWidget* view : views_) {
        if (view) view->centerOnWorldPoint(center, radius);
    }
}

void MapDocumentWidget::setSelectionMode(SelectionMode mode)
{
    if (selectionMode_ == mode) return;
    selectionMode_ = mode;
    const MapViewWidget::SelectionMode viewMode =
        mode == SelectionMode::Solids ? MapViewWidget::SelectionMode::Solids :
        mode == SelectionMode::Objects ? MapViewWidget::SelectionMode::Objects : MapViewWidget::SelectionMode::Groups;
    for (MapViewWidget* view : views_) view->setSelectionMode(viewMode);
    editor_.clearSelection();
    setSelectionOnViews();
    notifySelectionState();
    emit documentMessage(mode == SelectionMode::Solids ? tr("Selection mode: Solids") :
                         mode == SelectionMode::Objects ? tr("Selection mode: Objects") : tr("Selection mode: Groups"));
}

void MapDocumentWidget::setFgdDatabase(std::shared_ptr<hammer::fgd::Database> fgd)
{
    if (fgd_ == fgd) return;
    fgd_ = std::move(fgd);
    rebuildScene(false);
}

void MapDocumentWidget::setMaterialSystem(std::shared_ptr<hammer::assets::MaterialSystem> materials)
{
    materials_ = std::move(materials);
    studioModels_ = materials_ && materials_->fileSystem()
        ? std::make_shared<hammer::assets::StudioModelSystem>(materials_->fileSystem())
        : std::shared_ptr<hammer::assets::StudioModelSystem>{};
    for (MapViewWidget* view : views_) if (view) view->setMaterialSystem(materials_);
    rebuildScene(false);
}

void MapDocumentWidget::setMaterialRenderingEnabled(bool enabled)
{
    materialRenderingEnabled_ = enabled;
    for (MapViewWidget* view : views_) if (view) view->setMaterialRenderingEnabled(enabled);
}

void MapDocumentWidget::setWireframeOverlayEnabled(bool enabled)
{
    wireframeOverlayEnabled_ = enabled;
    for (MapViewWidget* view : views_) if (view) view->setWireframeOverlayEnabled(enabled);
}

void MapDocumentWidget::setDisplacementSolidMaskEnabled(bool enabled)
{
    displacementSolidMaskEnabled_ = enabled;
    for (MapViewWidget* view : views_) {
        if (view) view->setDisplacementSolidMaskEnabled(enabled);
    }
}

void MapDocumentWidget::setTexturedRenderMode(MapViewWidget::TexturedRenderMode mode)
{
    texturedRenderMode_ = mode;
    for (MapViewWidget* view : views_) if (view) view->setTexturedRenderMode(mode);
}

void MapDocumentWidget::setHdrEnabled(bool enabled)
{
    hdrEnabled_ = enabled;
    for (MapViewWidget* view : views_)
        if (view) view->setHdrEnabled(enabled);
}

void MapDocumentWidget::setRayTracedGamma(float gamma)
{
    rayTracedGamma_ = gamma;
    for (MapViewWidget* view : views_)
        if (view) view->setRayTracedGamma(gamma);
}

void MapDocumentWidget::setMaterialEffectsEnabled(bool phong, bool specular, bool bumpMaps,
                                                   bool lightWarp, bool selfIllum, bool rimLight)
{
    phongEnabled_ = phong;
    specularEnabled_ = specular;
    bumpMapsEnabled_ = bumpMaps;
    lightWarpEnabled_ = lightWarp;
    selfIllumEnabled_ = selfIllum;
    rimLightEnabled_ = rimLight;
    for (MapViewWidget* view : views_) {
        if (view) {
            view->setMaterialEffectsEnabled(phong, specular, bumpMaps,
                                            lightWarp, selfIllum, rimLight);
        }
    }
}

void MapDocumentWidget::setMaterialEffectIntensities(float phong, float specular, float bumpMaps)
{
    phongIntensity_ = std::clamp(phong, 0.0f, 4.0f);
    specularIntensity_ = std::clamp(specular, 0.0f, 4.0f);
    bumpMapIntensity_ = std::clamp(bumpMaps, 0.0f, 4.0f);
    for (MapViewWidget* view : views_) {
        if (view) view->setMaterialEffectIntensities(
            phongIntensity_, specularIntensity_, bumpMapIntensity_);
    }
}

QStringList MapDocumentWidget::toolTextureMaterials() const
{
    QStringList result;
    if (!scene_) return result;
    for (const std::string& material : hammer::vmf::toolMaterialPaths(*scene_)) {
        result.push_back(QString::fromStdString(material));
    }
    return result;
}

bool MapDocumentWidget::toolTextureVisible(const QString& material) const
{
    const std::string normalized =
        hammer::vmf::normalizeMaterialPath(material.toUtf8().toStdString());
    return hiddenToolTextures_.find(normalized) == hiddenToolTextures_.end();
}

// ---------------------------------------------------------------------------
// Texture Application tool (hammer/ToolMaterial.cpp, hammer/faceeditsheet.cpp,
// hammer/faceedit_materialpage.cpp).
//
// DEVIATION: the original also picks faces in the 2D views
// (CToolMaterial::OnLMouseDown2D -> CMapView2D::SelectAt). The port's 2D views
// are wireframe-only and have no face hit test, so face picking is 3D only.
// ---------------------------------------------------------------------------

void MapDocumentWidget::setFaceSelectionMaskHidden(bool hidden)
{
    faceSelectionMaskHidden_ = hidden;
    for (MapViewWidget* view : views_) {
        if (view) view->setFaceSelectionMaskHidden(hidden);
    }
}

void MapDocumentWidget::clearFaceSelection()
{
    if (faceSelection_.empty()) {
        notifyFaceSelectionState();
        return;
    }
    faceSelection_.clear();
    pushFaceSelectionToViews();
    notifyFaceSelectionState();
}

void MapDocumentWidget::pushFaceSelectionToViews()
{
    for (MapViewWidget* view : views_) {
        if (view) view->setFaceSelection(faceSelection_);
    }
}

void MapDocumentWidget::pushSmoothingGroupToViews()
{
    const std::vector<hammer::vmf::FaceRef> faces =
        shownSmoothingGroup_ > 0 ? editor_.facesInSmoothingGroup(shownSmoothingGroup_)
                                 : std::vector<hammer::vmf::FaceRef>{};
    for (MapViewWidget* view : views_) {
        if (view) view->setSmoothingGroupFaces(faces);
    }
}

// CSmoothingGroupMgr::AddFaceToGroup / RemoveFaceFromGroup over the face list.
void MapDocumentWidget::toggleFaceSmoothingGroup(int group, bool add)
{
    const std::uint32_t bit = hammer::vmf::smoothingGroupBit(group);
    if (bit == 0u || faceSelection_.empty()) {
        notifyFaceSelectionState();
        return;
    }
    if (editor_.applySmoothingGroups(faceSelection_, add ? bit : 0u, add ? 0u : bit)) {
        notifyDocumentState(add ? tr("Added %1 face(s) to smoothing group %2")
                                      .arg(static_cast<int>(faceSelection_.size())).arg(group)
                                : tr("Removed %1 face(s) from smoothing group %2")
                                      .arg(static_cast<int>(faceSelection_.size())).arg(group));
    }
    // Re-read from the document rather than trusting the click, so undo/redo
    // and no-op edits leave the page showing the truth.
    pushSmoothingGroupToViews();
    notifyFaceSelectionState();
}

// CFaceEditDispPage::OnButtonCreate over the face list.
void MapDocumentWidget::createFaceDisplacements(int power)
{
    if (faceSelection_.empty()) {
        notifyFaceSelectionState();
        return;
    }
    if (editor_.createDisplacements(faceSelection_, power)) {
        rebuildScene(false);
        notifyDocumentState(tr("Created %1 displacement(s) at power %2")
                                .arg(static_cast<int>(faceSelection_.size())).arg(power));
    }
    notifyFaceSelectionState();
}

// CFaceEditDispPage::OnButtonDestroy over the face list.
void MapDocumentWidget::destroyFaceDisplacements()
{
    if (faceSelection_.empty()) {
        notifyFaceSelectionState();
        return;
    }
    if (editor_.destroyDisplacements(faceSelection_)) {
        rebuildScene(false);
        notifyDocumentState(tr("Destroyed displacement(s) on %1 face(s)")
                                .arg(static_cast<int>(faceSelection_.size())));
    }
    notifyFaceSelectionState();
}

void MapDocumentWidget::setDisplacementTool(DisplacementTool tool)
{
    if (displacementPainting_) endDisplacementPaint();
    displacementTool_ = tool;
    pushDisplacementPaintToViews();
}

void MapDocumentWidget::setDisplacementPaintSettings(const DisplacementPaintSettings& settings)
{
    displacementPaintSettings_ = settings;
}

// CFaceEditDispPage::OnButtonApply over the face list.
void MapDocumentWidget::applyDisplacementAttributes(
    const hammer::vmf::EditorModel::DisplacementAttributeEdit& edit)
{
    if (faceSelection_.empty()) {
        notifyFaceSelectionState();
        return;
    }
    if (editor_.applyDisplacementAttributes(faceSelection_, edit)) {
        rebuildScene(false);
        notifyDocumentState(tr("Updated displacement attributes on %1 face(s)")
                                .arg(static_cast<int>(faceSelection_.size())));
    }
    notifyFaceSelectionState();
}

// CFaceEditDispPage::OnButtonNoise over the face list.
void MapDocumentWidget::applyDisplacementNoise(double minimum, double maximum)
{
    if (faceSelection_.empty()) {
        notifyFaceSelectionState();
        return;
    }
    // CMapDisp::ApplyNoise( min, max, 1.0f ) - the page passes a fixed rockiness.
    if (editor_.applyDisplacementNoise(faceSelection_, minimum, maximum, 1.0)) {
        rebuildScene(false);
        notifyDocumentState(tr("Applied noise to %1 displacement(s)")
                                .arg(static_cast<int>(faceSelection_.size())));
    }
    notifyFaceSelectionState();
}

// CFaceEditDispPage::OnButtonSew over the face list.
void MapDocumentWidget::sewFaceDisplacements()
{
    if (faceSelection_.size() < 2) {
        notifyDocumentState(tr("Sewing needs at least two displacement faces in the face list"));
        notifyFaceSelectionState();
        return;
    }
    if (editor_.sewDisplacementFaces(faceSelection_)) {
        rebuildScene(false);
        notifyDocumentState(tr("Sewed %1 displacement face(s)")
                                .arg(static_cast<int>(faceSelection_.size())));
    } else {
        notifyDocumentState(tr("No shared displacement edges or corners to sew"));
    }
    notifyFaceSelectionState();
}

// CFaceEditDispPage::UpdateDialogData's shared power / elevation: a value every
// displacement in the list agrees on, or nothing.
void MapDocumentWidget::displacementAttributeValues(std::optional<int>& power,
                                                    std::optional<double>& elevation) const
{
    power.reset();
    elevation.reset();
    bool first = true;
    for (const hammer::vmf::FaceRef& face : faceSelection_) {
        const auto info = editor_.faceDisplacement(face);
        if (!info) continue;
        if (first) {
            power = info->power;
            elevation = info->elevation;
            first = false;
            continue;
        }
        if (power && *power != info->power) power.reset();
        if (elevation && *elevation != info->elevation) elevation.reset();
    }
}

// ID_VIEW_3DLIGHTMAP_GRID.
void MapDocumentWidget::setLightmapGridVisible(bool visible)
{
    lightmapGridVisible_ = visible;
    for (MapViewWidget* view : views_) {
        if (view) view->setLightmapGridVisible(visible);
    }
}

// ID_VIEW_SHOWDETAILOBJECTS.
void MapDocumentWidget::setDetailPropsVisible(bool visible)
{
    detailPropsVisible_ = visible;
    for (MapViewWidget* view : views_) {
        if (view) view->setDetailPropsVisible(visible);
    }
}

void MapDocumentWidget::pushDisplacementPaintToViews()
{
    const bool painting = displacementTool_ == DisplacementTool::PaintGeometry ||
                          displacementTool_ == DisplacementTool::PaintAlpha;
    for (MapViewWidget* view : views_) {
        if (view) view->setDisplacementPaintActive(painting);
    }
}

// SpatialPaintData_t as CToolDisplace::ApplySpatialPaintTool fills it in. The
// paint axis for DISPPAINT_AXIS_FACE is the traced hit normal
// (tooldisplace.cpp: "m_vecPaintAxis = vHitNormal"), which is what the caller
// passes in as the surface normal; the axial choices are the fixed unit axes
// UpdateAxis sets.
hammer::vmf::SpatialPaintData MapDocumentWidget::paintDataFor(const hammer::vmf::Vec3& position,
                                                              bool lower) const
{
    hammer::vmf::SpatialPaintData paint;
    paint.center = position;
    paint.radius = displacementPaintSettings_.radius;
    paint.scalar = displacementPaintSettings_.distance;
    // "if ( m_bRMBDown ) { spatialData.m_flScalar = -spatialData.m_flScalar; }"
    if (lower) paint.scalar = -paint.scalar;
    paint.brushType = displacementPaintSettings_.brush;
    switch (displacementPaintSettings_.axis) {
    case hammer::vmf::PaintAxis::X: paint.paintAxis = {1.0, 0.0, 0.0}; break;
    case hammer::vmf::PaintAxis::Y: paint.paintAxis = {0.0, 1.0, 0.0}; break;
    case hammer::vmf::PaintAxis::Z:
    case hammer::vmf::PaintAxis::Subdiv: paint.paintAxis = {0.0, 0.0, 1.0}; break;
    // DISPPAINT_AXIS_FACE paints along the normal the ray hit.
    case hammer::vmf::PaintAxis::Face: paint.paintAxis = displacementPaintNormal_; break;
    }
    return paint;
}

void MapDocumentWidget::beginDisplacementPaint(const hammer::vmf::Vec3& position,
                                               const hammer::vmf::Vec3& normal, bool lower)
{
    if (faceSelection_.empty()) return;
    // "if( pFace->TraceLine(...) ) m_vecPaintAxis = vHitNormal; else ... 0,0,1".
    const double length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    displacementPaintNormal_ = length > 0.0
        ? hammer::vmf::Vec3{normal.x / length, normal.y / length, normal.z / length}
        : hammer::vmf::Vec3{0.0, 0.0, 1.0};
    // One drag is one undo entry, as CDispPaintMgr's PreUndo/PostUndo pair is.
    if (!editor_.beginTransaction("Displacement Paint", false)) return;
    displacementPainting_ = true;
    continueDisplacementPaint(position, lower);
}

void MapDocumentWidget::continueDisplacementPaint(const hammer::vmf::Vec3& position, bool lower)
{
    if (!displacementPainting_) return;
    const bool alpha = displacementTool_ == DisplacementTool::PaintAlpha;
    if (editor_.paintDisplacementsInTransaction(faceSelection_, paintDataFor(position, lower),
                                                alpha)) {
        std::vector<hammer::vmf::ObjectRef> painted;
        for (const hammer::vmf::FaceRef& face : faceSelection_) {
            const hammer::vmf::ObjectRef solid{hammer::vmf::ObjectType::Solid, face.solidId};
            if (std::find(painted.begin(), painted.end(), solid) == painted.end())
                painted.push_back(solid);
        }
        rebuildSceneObjects(painted);
    }
}

void MapDocumentWidget::endDisplacementPaint()
{
    if (!displacementPainting_) return;
    displacementPainting_ = false;
    if (editor_.commitTransaction()) {
        rebuildScene(false);
        notifyDocumentState(tr("Painted displacement"));
    }
    notifyFaceSelectionState();
}

void MapDocumentWidget::setShownSmoothingGroup(int group)
{
    shownSmoothingGroup_ = (group >= 1 && group <= hammer::vmf::MaxSmoothingGroupCount) ? group : 0;
    pushSmoothingGroupToViews();
}

void MapDocumentWidget::selectFacesInSmoothingGroup(int group)
{
    if (hammer::vmf::smoothingGroupBit(group) == 0u) return;
    faceSelection_ = editor_.facesInSmoothingGroup(group);
    pushFaceSelectionToViews();
    notifyDocumentState(tr("Selected %1 face(s) in smoothing group %2")
                            .arg(static_cast<int>(faceSelection_.size())).arg(group));
    notifyFaceSelectionState();
}

// The face list can outlive the geometry it points at (undo, clip, morph).
void MapDocumentWidget::validateFaceSelection()
{
    if (faceSelection_.empty() || !scene_) return;
    const std::size_t before = faceSelection_.size();
    std::erase_if(faceSelection_, [this](const hammer::vmf::FaceRef& face) {
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            if (brush.id != face.solidId) continue;
            for (const hammer::vmf::FaceGeometry& side : brush.faces) {
                if (side.sideId == face.sideId) return false;
            }
        }
        return true;
    });
    if (faceSelection_.size() != before) pushFaceSelectionToViews();
}

std::optional<hammer::vmf::FaceEditTarget>
MapDocumentWidget::faceTarget(const hammer::vmf::FaceRef& face) const
{
    if (!scene_) return std::nullopt;
    for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
        if (brush.id != face.solidId) continue;
        for (const hammer::vmf::FaceGeometry& side : brush.faces) {
            if (side.sideId != face.sideId) continue;
            hammer::vmf::FaceEditTarget target;
            target.face = face;
            target.normal = side.normal;
            for (const std::size_t index : side.vertices) {
                if (index < brush.vertices.size()) target.points.push_back(brush.vertices[index]);
            }
            // IEditorTexture::GetWidth/GetHeight, which every justification
            // except Fit's scale calculation needs.
            if (materials_) {
                if (const auto material = materials_->material(side.material)) {
                    if (material->image.valid()) {
                        target.textureWidth = material->image.width;
                        target.textureHeight = material->image.height;
                    }
                }
            }
            return target;
        }
    }
    return std::nullopt;
}

std::vector<hammer::vmf::FaceEditTarget> MapDocumentWidget::faceTargets() const
{
    std::vector<hammer::vmf::FaceEditTarget> targets;
    targets.reserve(faceSelection_.size());
    for (const hammer::vmf::FaceRef& face : faceSelection_) {
        if (auto target = faceTarget(face)) targets.push_back(std::move(*target));
    }
    return targets;
}

// CFaceEditMaterialPage::UpdateDialogData: the first face seeds every field and
// any later face that disagrees blanks it.
FaceEditValues MapDocumentWidget::faceEditValues() const
{
    FaceEditValues values;
    values.faceCount = static_cast<int>(faceSelection_.size());
    bool first = true;
    for (const hammer::vmf::FaceRef& face : faceSelection_) {
        const auto texture = editor_.faceTexture(face);
        if (!texture) continue;
        if (first) {
            values.shiftX = texture->uShift;
            values.shiftY = texture->vShift;
            values.scaleX = texture->uScale;
            values.scaleY = texture->vScale;
            values.rotation = texture->rotation;
            values.lightmapScale = texture->lightmapScale;
            values.material = QString::fromStdString(texture->material);
            first = false;
            continue;
        }
        if (values.shiftX && *values.shiftX != texture->uShift) values.shiftX.reset();
        if (values.shiftY && *values.shiftY != texture->vShift) values.shiftY.reset();
        if (values.scaleX && *values.scaleX != texture->uScale) values.scaleX.reset();
        if (values.scaleY && *values.scaleY != texture->vScale) values.scaleY.reset();
        if (values.rotation && *values.rotation != texture->rotation) values.rotation.reset();
        if (values.lightmapScale && *values.lightmapScale != texture->lightmapScale) {
            values.lightmapScale.reset();
        }
        if (values.material != QString::fromStdString(texture->material)) values.material.clear();
    }
    // Smoothing group membership: the intersection is what every face shares,
    // the union is what at least one face has (the page's partial state).
    bool firstGroups = true;
    for (const hammer::vmf::FaceRef& face : faceSelection_) {
        const std::uint32_t groups = editor_.faceSmoothingGroups(face);
        values.smoothingAll = firstGroups ? groups : (values.smoothingAll & groups);
        values.smoothingAny |= groups;
        firstGroups = false;
    }
    // CFaceEditDispPage::UpdateDialogData's "bAllDisps" test, which walks the
    // face list asking CMapFace::HasDisp.
    for (const hammer::vmf::FaceRef& face : faceSelection_) {
        if (editor_.faceDisplacement(face)) ++values.displacementCount;
    }
    // "if no faces selected -- get selection from texture bar"
    if (values.faceCount == 0) values.material = currentMaterial_;
    return values;
}

void MapDocumentWidget::notifyFaceSelectionState()
{
    emit faceSelectionChanged(faceEditValues());
}

// CFaceEditSheet::ClickFace's list maintenance: cfClear empties the list first
// unless CTRL is held, then cfToggle adds or removes the clicked face.
void MapDocumentWidget::toggleFaceInList(int solidId, int sideId, bool clear)
{
    if (clear) faceSelection_.clear();
    const hammer::vmf::FaceRef face{solidId, sideId};
    const auto existing = std::find(faceSelection_.begin(), faceSelection_.end(), face);
    if (existing != faceSelection_.end()) faceSelection_.erase(existing);
    else faceSelection_.push_back(face);
}

void MapDocumentWidget::liftMaterialFrom(const hammer::vmf::FaceRef& face)
{
    const auto texture = editor_.faceTexture(face);
    if (!texture || texture->material.empty()) return;
    const QString material = QString::fromStdString(texture->material);
    if (currentMaterial_.compare(material, Qt::CaseInsensitive) == 0) return;
    setCurrentMaterial(material);
    emit currentMaterialLifted(material);
}

// CToolMaterial::OnActivate: the face list starts as every face of the
// selected solids (brush entities contribute theirs), so switching to the
// face tool with brushes selected edits those brushes' faces at once. Reads
// the scene rather than the document so hidden objects stay out.
void MapDocumentWidget::seedFaceSelectionFromObjectSelection()
{
    faceSelection_.clear();
    if (scene_ && !editor_.selection().empty()) {
        std::unordered_set<int> solids;
        std::unordered_set<int> entities;
        for (const hammer::vmf::ObjectRef& object : editor_.selection()) {
            if (object.type == hammer::vmf::ObjectType::Solid) solids.insert(object.id);
            else entities.insert(object.id);
        }
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            const bool picked = solids.contains(brush.id) ||
                                (brush.ownerEntityId >= 0 && entities.contains(brush.ownerEntityId));
            if (!picked) continue;
            for (const hammer::vmf::FaceGeometry& side : brush.faces)
                faceSelection_.push_back({brush.id, side.sideId});
        }
    }
    pushFaceSelectionToViews();
    notifyFaceSelectionState();
    if (!faceSelection_.empty() && faceClickMode_ == FaceEditSheet::ClickMode::LiftSelect)
        liftMaterialFrom(faceSelection_.front());
}

void MapDocumentWidget::handleFaceSelect(MapViewWidget* view, int solidId, int sideId,
                                         bool control, bool shift)
{
    if (!scene_) return;

    // ToolMaterial::OnLMouseDown3D: SHIFT selects/applies to the whole solid.
    std::vector<int> sideIds;
    if (shift) {
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            if (brush.id != solidId) continue;
            for (const hammer::vmf::FaceGeometry& side : brush.faces) sideIds.push_back(side.sideId);
        }
    } else {
        sideIds.push_back(sideId);
    }
    if (sideIds.empty()) return;

    switch (faceClickMode_) {
    case FaceEditSheet::ClickMode::Select:
    case FaceEditSheet::ClickMode::LiftSelect: {
        bool clear = !control;
        for (const int id : sideIds) {
            toggleFaceInList(solidId, id, clear);
            clear = false;
        }
        pushFaceSelectionToViews();
        // ModeLiftSelect refreshes the fields from the whole list; ModeSelect
        // leaves the fields alone, so only the face count changes.
        notifyFaceSelectionState();
        // The lift half: the clicked face's material becomes the current
        // texture (Textures bar and sheet follow via currentMaterialLifted).
        if (faceClickMode_ == FaceEditSheet::ClickMode::LiftSelect)
            liftMaterialFrom({solidId, sideIds.front()});
        break;
    }
    case FaceEditSheet::ClickMode::Lift: {
        // ModeLift: UpdateDialogData( pFace ) - show the clicked face's values
        // without touching the face list.
        FaceEditValues values;
        values.faceCount = static_cast<int>(faceSelection_.size());
        if (const auto texture = editor_.faceTexture({solidId, sideIds.front()})) {
            values.shiftX = texture->uShift;
            values.shiftY = texture->vShift;
            values.scaleX = texture->uScale;
            values.scaleY = texture->vScale;
            values.rotation = texture->rotation;
            values.lightmapScale = texture->lightmapScale;
            values.material = QString::fromStdString(texture->material);
        }
        emit faceSelectionChanged(values);
        liftMaterialFrom({solidId, sideIds.front()});
        break;
    }
    case FaceEditSheet::ClickMode::Apply:
    case FaceEditSheet::ClickMode::ApplyAll: {
        std::vector<hammer::vmf::FaceRef> faces;
        for (const int id : sideIds) faces.push_back({solidId, id});
        hammer::vmf::FaceTextureEdit edit;
        edit.material = currentMaterial_.toStdString();
        if (faceClickMode_ == FaceEditSheet::ClickMode::ApplyAll) {
            // FACE_APPLY_ALL: material, mapping and lightmap scale.
            edit.shiftX = pendingFaceEdit_.shiftX;
            edit.shiftY = pendingFaceEdit_.shiftY;
            edit.scaleX = pendingFaceEdit_.scaleX;
            edit.scaleY = pendingFaceEdit_.scaleY;
            edit.rotation = pendingFaceEdit_.rotation;
            edit.lightmapScale = pendingFaceEdit_.lightmapScale;
        }
        if (editor_.applyFaceTextures(faces, edit, "Apply texture")) {
            rebuildSceneFaces(faces);
            notifyDocumentState(tr("Applied texture to %1 face(s)")
                                    .arg(static_cast<int>(faces.size())));
        }
        break;
    }
    case FaceEditSheet::ClickMode::AlignToView: {
        if (!view) return;
        std::vector<hammer::vmf::FaceEditTarget> targets;
        for (const int id : sideIds) {
            if (auto target = faceTarget({solidId, id})) targets.push_back(std::move(*target));
        }
        if (editor_.alignFacesToView(targets, view->viewRight(), view->viewUp(),
                                     view->viewPoint(), "Apply texture")) {
            rebuildScene(false);
            notifyDocumentState(tr("Aligned %1 face(s) to the 3D view")
                                    .arg(static_cast<int>(targets.size())));
        }
        break;
    }
    }
}

// CToolMaterial::OnRMouseDown3D: right-click always applies, in ModeApplyAll,
// with cfEdgeAlign when ALT is held.
void MapDocumentWidget::handleFaceApply(MapViewWidget* view, int solidId, int sideId,
                                        bool edgeAlign, bool shift)
{
    Q_UNUSED(view);
    if (!scene_) return;

    std::vector<int> sideIds;
    if (shift) {
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            if (brush.id != solidId) continue;
            for (const hammer::vmf::FaceGeometry& side : brush.faces) sideIds.push_back(side.sideId);
        }
    } else {
        sideIds.push_back(sideId);
    }
    if (sideIds.empty()) return;

    // CToolMaterial::OnRMouseDown3D: "If we're in a lightmap grid preview
    // window, only apply the lightmap scale" (ModeApplyLightmapScale ->
    // Apply( pFace, FACE_APPLY_LIGHTMAP_SCALE )). Everything else - material
    // included - is left alone.
    if (lightmapGridVisible_) {
        std::vector<hammer::vmf::FaceRef> faces;
        for (const int id : sideIds) faces.push_back({solidId, id});
        hammer::vmf::FaceTextureEdit edit;
        edit.lightmapScale = pendingFaceEdit_.lightmapScale;
        if (!edit.lightmapScale) return;
        if (editor_.applyFaceTextures(faces, edit, "Apply lightmap scale")) {
            rebuildSceneFaces(faces);
            notifyDocumentState(tr("Applied lightmap scale to %1 face(s)")
                                    .arg(static_cast<int>(faces.size())));
        }
        return;
    }

    if (edgeAlign) {
        // FACE_APPLY_ALIGN_EDGE: the reference is the LAST face in the face
        // list (CopyTCoordSystem( GetFaceListDataFace( faceCount - 1 ), pFace )).
        // The mapping fields are deliberately NOT applied in this branch.
        // With an empty face list the original's faceCount >= 1 guard skips
        // CopyTCoordSystem, but FACE_APPLY_MATERIAL still runs, so the clicked
        // face still receives the material. Fall through to that below.
        if (!faceSelection_.empty()) {
            const auto reference = faceTarget(faceSelection_.back());
            if (!reference) return;
            std::vector<hammer::vmf::FaceEditTarget> targets;
            for (const int id : sideIds) {
                if (auto target = faceTarget({solidId, id})) targets.push_back(std::move(*target));
            }
            if (editor_.edgeAlignFaces(targets, *reference, "Apply texture")) {
                rebuildScene(false);
                notifyDocumentState(tr("Aligned %1 face(s) to the reference face")
                                        .arg(static_cast<int>(targets.size())));
            }
            return;
        }
    }

    std::vector<hammer::vmf::FaceRef> faces;
    for (const int id : sideIds) faces.push_back({solidId, id});
    // FACE_APPLY_ALIGN_EDGE suppresses the mapping fields; only the material
    // (and the lightmap scale) reach the face in that branch.
    hammer::vmf::FaceTextureEdit edit = edgeAlign ? hammer::vmf::FaceTextureEdit{} : pendingFaceEdit_;
    edit.material = currentMaterial_.toStdString();
    if (editor_.applyFaceTextures(faces, edit, "Apply texture")) {
        rebuildSceneFaces(faces);
        notifyDocumentState(tr("Applied texture to %1 face(s)").arg(static_cast<int>(faces.size())));
    }
}

void MapDocumentWidget::applyFaceEdit(const hammer::vmf::FaceTextureEdit& edit)
{
    // Remember the sheet's fields so a later click in an Apply mode uses them,
    // exactly as CFaceEditMaterialPage::Apply reads its own controls.
    pendingFaceEdit_ = edit;
    if (faceSelection_.empty()) return;
    if (editor_.applyFaceTextures(faceSelection_, edit, "Apply Face Attributes")) {
        rebuildScene(false);
        notifyDocumentState(tr("Applied face attributes to %1 face(s)")
                                .arg(static_cast<int>(faceSelection_.size())));
        notifyFaceSelectionState();
    }
}

// CFaceEditMaterialPage::Apply( pFace, FACE_APPLY_LIGHTMAP_SCALE ).
void MapDocumentWidget::applyLightmapScale(int scale)
{
    if (scale < 1) return;
    // Remember it for the lightmap-grid right-click, without disturbing the
    // mapping fields the Apply click modes push.
    pendingFaceEdit_.lightmapScale = scale;
    if (faceSelection_.empty()) return;
    hammer::vmf::FaceTextureEdit edit;
    edit.lightmapScale = scale;
    if (editor_.applyFaceTextures(faceSelection_, edit, "Apply lightmap scale")) {
        rebuildSceneFaces(faceSelection_);
        notifyDocumentState(tr("Applied lightmap scale %1 to %2 face(s)")
                                .arg(scale).arg(static_cast<int>(faceSelection_.size())));
        notifyFaceSelectionState();
    }
}

void MapDocumentWidget::justifyFaceSelection(int justification)
{
    if (faceSelection_.empty()) return;
    if (editor_.justifyFaces(faceTargets(),
                             static_cast<hammer::vmf::TextureJustification>(justification),
                             treatFacesAsOne_, "Justify texture")) {
        rebuildScene(false);
        notifyDocumentState(tr("Justified %1 face(s)")
                                .arg(static_cast<int>(faceSelection_.size())));
        notifyFaceSelectionState();
    }
}

void MapDocumentWidget::alignFaceSelection(int alignment)
{
    if (faceSelection_.empty()) return;
    if (editor_.alignFaceTextures(faceTargets(),
                                  static_cast<hammer::vmf::TextureAlignment>(alignment),
                                  "Align texture")) {
        rebuildScene(false);
        notifyDocumentState(tr("Aligned %1 face(s)").arg(static_cast<int>(faceSelection_.size())));
        notifyFaceSelectionState();
    }
}

// "Apply current texture" (Shift+T). A momentary command: the active tool is
// unaffected. When the face list is non-empty (Texture Application is up) it
// applies to those faces, otherwise to every face of the selected solids.
void MapDocumentWidget::applyCurrentTexture()
{
    const QString material = currentMaterial_.trimmed();
    if (material.isEmpty()) {
        notifyDocumentState(tr("No current texture."));
        return;
    }

    if (!faceSelection_.empty()) {
        hammer::vmf::FaceTextureEdit edit;
        edit.material = material.toStdString();
        if (editor_.applyFaceTextures(faceSelection_, edit, "Apply texture")) {
            rebuildScene(false);
            notifyDocumentState(tr("Applied %1 to %2 face(s)")
                                    .arg(material).arg(static_cast<int>(faceSelection_.size())));
            notifyFaceSelectionState();
        }
        return;
    }

    if (editor_.selection().empty()) {
        notifyDocumentState(tr("Select one or more solids first."));
        return;
    }
    if (editor_.applyMaterialToSelection(material.toStdString(), "Apply texture")) {
        rebuildScene(false);
        notifyDocumentState(tr("Applied %1 to the selection").arg(material));
        notifySelectionState();
    }
}

hammer::vmf::ReplaceTexturesResult MapDocumentWidget::replaceTextures(
    const hammer::vmf::ReplaceTexturesRequest& request,
    const std::function<bool(const std::string&, int&, int&)>& materialSize)
{
    const hammer::vmf::ReplaceTexturesResult result =
        editor_.replaceTextures(request, materialSize, "Replace Textures");

    if (request.markOnly) {
        rebuildScene(false);
        notifySelectionState();
        notifyDocumentState(tr("Marked %1 face(s) on %2 solid(s)")
                                .arg(result.facesMatched).arg(static_cast<int>(editor_.selection().size())));
        return result;
    }

    if (result.facesChanged > 0) {
        rebuildScene(false);
        notifyDocumentState(tr("Replaced %1 of %2 matching face(s)")
                                .arg(result.facesChanged).arg(result.facesMatched));
    } else {
        notifyDocumentState(tr("%1 matching face(s) found; nothing to replace")
                                .arg(result.facesMatched));
    }
    return result;
}

void MapDocumentWidget::setToolTextureVisible(const QString& material, bool visible)
{
    const std::string normalized =
        hammer::vmf::normalizeMaterialPath(material.toUtf8().toStdString());
    if (normalized.rfind("tools/", 0) != 0) return;
    const bool changed = visible ? hiddenToolTextures_.erase(normalized) > 0
                                 : hiddenToolTextures_.insert(normalized).second;
    if (changed) applyToolTextureVisibility();
}

void MapDocumentWidget::setAllToolTexturesVisible(bool visible)
{
    bool changed = false;
    for (const QString& material : toolTextureMaterials()) {
        const std::string normalized =
            hammer::vmf::normalizeMaterialPath(material.toUtf8().toStdString());
        if (visible) changed = hiddenToolTextures_.erase(normalized) > 0 || changed;
        else changed = hiddenToolTextures_.insert(normalized).second || changed;
    }
    if (changed) applyToolTextureVisibility();
}

void MapDocumentWidget::applyToolTextureVisibility()
{
    emit toolTextureVisibilityChanged();
    for (MapViewWidget* view : views_) {
        if (view) view->setHiddenToolTextures(hiddenToolTextures_);
    }
    // CSelection::RemoveInvisibles: an object that just became invisible must
    // leave the selection, so no later move/resize/delete acts on something the
    // user cannot see. Only fully hidden solids qualify (see the semantics note
    // in VmfScene.hpp); a solid with only some hidden tool faces stays visible.
    if (!scene_) return;
    std::vector<hammer::vmf::ObjectRef> hidden;
    for (const auto& brush : scene_->brushes) {
        if (hammer::vmf::isBrushHiddenByToolTextures(brush, hiddenToolTextures_)) {
            hidden.push_back(brush.object);
            if (brush.ownerEntityId >= 0)
                hidden.push_back({hammer::vmf::ObjectType::Entity, brush.ownerEntityId});
        }
    }
    if (hidden.empty()) return;
    // A brush entity only leaves the selection when every one of its solids is
    // hidden; a partly visible brush entity is still visible.
    for (const auto& brush : scene_->brushes) {
        if (brush.ownerEntityId < 0) continue;
        if (hammer::vmf::isBrushHiddenByToolTextures(brush, hiddenToolTextures_)) continue;
        const hammer::vmf::ObjectRef owner{hammer::vmf::ObjectType::Entity, brush.ownerEntityId};
        hidden.erase(std::remove(hidden.begin(), hidden.end(), owner), hidden.end());
    }
    std::vector<hammer::vmf::ObjectRef> selection = editor_.selection();
    const std::size_t before = selection.size();
    selection.erase(std::remove_if(selection.begin(), selection.end(),
                                   [&hidden](const hammer::vmf::ObjectRef& object) {
                                       return std::find(hidden.begin(), hidden.end(), object) !=
                                              hidden.end();
                                   }),
                    selection.end());
    hitList_.erase(std::remove_if(hitList_.begin(), hitList_.end(),
                                  [&hidden](const hammer::vmf::ObjectRef& object) {
                                      return std::find(hidden.begin(), hidden.end(), object) !=
                                             hidden.end();
                                  }),
                   hitList_.end());
    if (currentHit_ >= static_cast<int>(hitList_.size())) currentHit_ = -1;
    if (selection.size() == before) return;
    editor_.setSelection(std::move(selection));
    setSelectionOnViews();
    notifySelectionState();
}

void MapDocumentWidget::refreshAutoVisGroups()
{
    // The NPCs category is keyed on the FGD's @NPCClass declarations.
    if (npcClasses_.empty() && fgd_) {
        for (const hammer::fgd::EntityClass* entity : fgd_->pointClasses()) {
            if (!entity || !entity->npc) continue;
            std::string lowered = entity->name;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            npcClasses_.insert(std::move(lowered));
        }
    }
    // Water classification must never trigger a material load: this runs over
    // every face of every solid on each full rebuild, and material() reads the
    // VMT and decodes VTFs on a miss. Only already-resident materials are
    // consulted, so an unmounted or not-yet-loaded material simply is not
    // water yet and picks up the category once the renderer has loaded it.
    //
    // Deviation: the reference tests the %compileWater material var; this port
    // uses MaterialSystem's own water-shader detection, which is what every
    // other water path here already keys on.
    const auto isWaterMaterial = [this](std::string_view material) {
        if (!materials_) return false;
        const auto resident = materials_->residentMaterial(material);
        return resident && resident->water;
    };
    autoVisGroupIndex_ = hammer::vmf::indexAutoVisGroups(editor_.document(), objectIndex_,
                                                        npcClasses_, isWaterMaterial);
    autoVisGroupsStale_ = false;
}

void MapDocumentWidget::ensureAutoVisGroupsFresh() const
{
    if (!autoVisGroupsStale_) return;
    // Rebuild without the material-load guard changing: same predicate as
    // refreshAutoVisGroups, which is non-const only for npcClasses_ seeding.
    const auto isWaterMaterial = [this](std::string_view material) {
        if (!materials_) return false;
        const auto resident = materials_->residentMaterial(material);
        return resident && resident->water;
    };
    autoVisGroupIndex_ = hammer::vmf::indexAutoVisGroups(editor_.document(), objectIndex_,
                                                        npcClasses_, isWaterMaterial);
    autoVisGroupsStale_ = false;
}

std::vector<MapDocumentWidget::AutoVisGroupNode> MapDocumentWidget::autoVisGroups() const
{
    ensureAutoVisGroupsFresh();
    std::vector<AutoVisGroupNode> nodes;
    for (const hammer::vmf::AutoVisGroupDef& def : hammer::vmf::autoVisGroupTable()) {
        // Only categories this map populates are shown, exactly as the original
        // creates a visgroup only when something lands in it.
        if (!autoVisGroupIndex_.contains(def.id)) continue;
        AutoVisGroupNode node;
        node.id = def.id;
        node.parent = def.parent;
        node.name = QString::fromLatin1(def.name);
        node.memberCount = autoVisGroupIndex_.objectsIn(def.id).size();
        nodes.push_back(std::move(node));
    }
    return nodes;
}

hammer::vmf::VisGroupState MapDocumentWidget::autoVisGroupState(hammer::vmf::AutoVisGroup id) const
{
    if (showAllVisGroups_) return hammer::vmf::VisGroupState::Shown;
    ensureAutoVisGroupsFresh();
    const std::vector<hammer::vmf::MapObjectKey> members = autoVisGroupIndex_.objectsIn(id);
    if (members.empty()) return hammer::vmf::VisGroupState::Shown;
    bool anyShown = false;
    bool anyHidden = false;
    for (const hammer::vmf::MapObjectKey& key : members) {
        const hammer::vmf::MapObjectEntry* entry = objectIndex_.find(key);
        if (!entry) continue;
        // The auto half of the flag pair is what an auto visgroup owns.
        if (entry->visGroupAutoShown) anyShown = true;
        else anyHidden = true;
    }
    if (anyShown && anyHidden) return hammer::vmf::VisGroupState::Partial;
    return anyHidden ? hammer::vmf::VisGroupState::Hidden : hammer::vmf::VisGroupState::Shown;
}

void MapDocumentWidget::setAutoVisGroupVisible(hammer::vmf::AutoVisGroup id, bool visible)
{
    // Like its user counterpart, this is a document change with no undo entry
    // (CMapDoc::VisGroups_ShowVisGroup never marks an undo position).
    ensureAutoVisGroupsFresh();  // must act on the current member list
    const std::vector<hammer::vmf::MapObjectKey> members = autoVisGroupIndex_.objectsIn(id);
    if (!hammer::vmf::showAutoVisGroup(editor_.document(), members, visible)) return;
    rebuildScene(false);
    dropHiddenFromSelection();
    notifyDocumentState();
}

void MapDocumentWidget::refreshHiddenObjects()
{
    hiddenSolids_ = quickHiddenSolids_;
    hiddenEntities_ = quickHiddenEntities_;
    // CVisGroup::IsShowAllActive overrides every visgroup's state without
    // touching the per-object flags, so Show All is applied here rather than by
    // rewriting the document.
    if (!showAllVisGroups_) objectIndex_.hiddenObjects(hiddenSolids_, hiddenEntities_);
}

void MapDocumentWidget::applySceneVisibility()
{
    if (!scene_) return;
    if (hiddenSolids_.empty() && hiddenEntities_.empty()) return;
    const std::size_t brushesBefore = scene_->brushes.size();
    const std::size_t entitiesBefore = scene_->entities.size();
    // A hidden brush entity takes its solids with it; a hidden solid of a
    // visible brush entity goes on its own, as Hammer hides per object.
    scene_->brushes.erase(
        std::remove_if(scene_->brushes.begin(), scene_->brushes.end(),
                       [this](const hammer::vmf::BrushGeometry& brush) {
                           return hiddenSolids_.contains(brush.id) ||
                                  (brush.ownerEntityId >= 0 &&
                                   hiddenEntities_.contains(brush.ownerEntityId));
                       }),
        scene_->brushes.end());
    scene_->entities.erase(std::remove_if(scene_->entities.begin(), scene_->entities.end(),
                                          [this](const hammer::vmf::EntityMarker& entity) {
                                              return hiddenEntities_.contains(entity.id);
                                          }),
                           scene_->entities.end());
    if (scene_->brushes.size() == brushesBefore && scene_->entities.size() == entitiesBefore)
        return;
    // The render caches key off the lineage, and removing objects is not
    // something a changed-id list can describe, so force a full rebuild.
    scene_->invalidateLineage();
}

void MapDocumentWidget::collectQuickHideTargets(
    const std::vector<hammer::vmf::ObjectRef>& objects, std::unordered_set<int>& solids,
    std::unordered_set<int>& entities) const
{
    for (const hammer::vmf::ObjectRef& object : objects) {
        if (object.id < 0) continue;
        if (object.type == hammer::vmf::ObjectType::Solid) solids.insert(object.id);
        else entities.insert(object.id);
    }
}

void MapDocumentWidget::applyQuickHide()
{
    // The hidden sets must reflect the new quickhide state before the selection
    // is filtered against them.
    objectIndex_ = hammer::vmf::indexMapObjects(editor_.document());
    refreshHiddenObjects();
    // CSelection::RemoveInvisibles: nothing the user cannot see may stay
    // selected, or a later move/delete would act on it unseen.
    std::vector<hammer::vmf::ObjectRef> selection = editor_.selection();
    const auto stillHidden = [this](const hammer::vmf::ObjectRef& object) {
        return object.type == hammer::vmf::ObjectType::Solid ? hiddenSolids_.contains(object.id)
                                                             : hiddenEntities_.contains(object.id);
    };
    const std::size_t before = selection.size();
    selection.erase(std::remove_if(selection.begin(), selection.end(), stillHidden),
                    selection.end());
    hitList_.erase(std::remove_if(hitList_.begin(), hitList_.end(), stillHidden), hitList_.end());
    if (currentHit_ >= static_cast<int>(hitList_.size())) currentHit_ = -1;
    if (selection.size() != before) editor_.setSelection(std::move(selection));
    // Rebuilt from the document so unhidden objects come back, then refiltered.
    rebuildScene(false);
    setSelectionOnViews();
    notifySelectionState();
}

void MapDocumentWidget::quickHideSelected()
{
    if (editor_.selection().empty()) return;
    collectQuickHideTargets(editor_.selection(), quickHiddenSolids_, quickHiddenEntities_);
    applyQuickHide();
}

void MapDocumentWidget::quickHideUnselected()
{
    if (!scene_) return;
    std::unordered_set<int> selectedSolids;
    std::unordered_set<int> selectedEntities;
    collectQuickHideTargets(editor_.selection(), selectedSolids, selectedEntities);
    for (const auto& brush : scene_->brushes) {
        // A brush entity is hidden as one object, so its solids are never
        // listed individually - the entity id alone takes them out of the
        // scene, and keeps the hidden-object count one per visible object.
        if (brush.ownerEntityId >= 0) continue;
        if (selectedSolids.contains(brush.id)) continue;
        quickHiddenSolids_.insert(brush.id);
    }
    for (const auto& entity : scene_->entities) {
        if (selectedEntities.contains(entity.id)) continue;
        quickHiddenEntities_.insert(entity.id);
    }
    applyQuickHide();
}

void MapDocumentWidget::quickHideUnhideAll()
{
    if (quickHiddenSolids_.empty() && quickHiddenEntities_.empty()) return;
    quickHiddenSolids_.clear();
    quickHiddenEntities_.clear();
    applyQuickHide();
}

std::size_t MapDocumentWidget::quickHideConvertToVisGroup(const QString& name)
{
    if (quickHiddenSolids_.empty() && quickHiddenEntities_.empty()) return 0;
    std::vector<hammer::vmf::ObjectRef> objects;
    for (const int id : quickHiddenSolids_) objects.push_back({hammer::vmf::ObjectType::Solid, id});
    for (const int id : quickHiddenEntities_)
        objects.push_back({hammer::vmf::ObjectType::Entity, id});
    const std::string visGroupName = name.trimmed().toStdString();
    std::size_t tagged = 0;
    const bool changed = editor_.applyDocumentEdit(
        [&](hammer::vmf::Document& document) {
            const int visGroupId =
                hammer::vmf::createVisGroup(document, visGroupName,
                                            hammer::vmf::maximumVisGroupId(document) + 1);
            if (visGroupId == 0) return false;
            if (!hammer::vmf::addObjectsToVisGroup(document, objects, visGroupId, false)) {
                hammer::vmf::deleteVisGroup(document, visGroupId);
                return false;
            }
            // The whole point of the conversion: the new visgroup takes over
            // the hiding, so the objects carry visgroupshown 0 and the visgroup
            // shows up unchecked in the Filter Control tree.
            hammer::vmf::setObjectsVisGroupShown(document, objects, false);
            tagged = objects.size();
            return true;
        },
        "Convert QuickHide to VisGroup");
    if (!changed) return 0;
    // Quickhide has handed these objects over. They stay hidden - now because
    // the visgroup is unchecked - and Unhide QuickHide Objects no longer
    // touches them, matching Hammer.
    quickHiddenSolids_.clear();
    quickHiddenEntities_.clear();
    applyQuickHide();
    notifyDocumentState();
    return tagged;
}

// --- Groups -----------------------------------------------------------------

std::vector<hammer::vmf::ObjectRef> MapDocumentWidget::expandSelectionToGroups(
    const std::vector<hammer::vmf::ObjectRef>& objects) const
{
    if (!groupsActive()) return objects;
    std::vector<hammer::vmf::ObjectRef> result;
    const auto add = [&result](const hammer::vmf::ObjectRef& object) {
        if (std::find(result.begin(), result.end(), object) == result.end())
            result.push_back(object);
    };
    for (const hammer::vmf::ObjectRef& object : objects) {
        const hammer::vmf::MapObjectKey key{object.type == hammer::vmf::ObjectType::Solid
                                                ? hammer::vmf::MapObjectKind::Solid
                                                : hammer::vmf::MapObjectKind::Entity,
                                            object.id};
        const int group = objectIndex_.topLevelGroup(key);
        if (group == 0) {
            add(object);
            continue;
        }
        // Clicking any member selects the whole group (CMapGroup): the group
        // itself is never an ObjectRef, it just expands to its members.
        for (const hammer::vmf::ObjectRef& member : objectIndex_.groupMembers(group)) add(member);
    }
    return result;
}

bool MapDocumentWidget::groupsActive() const
{
    // Ignore Groups (CMapDoc's selectSolids / Options.general.bIgnoreGroups)
    // turns group expansion off; so does picking in Objects or Solids mode.
    return selectionMode_ == SelectionMode::Groups && !ignoreGroups_;
}

std::vector<int> MapDocumentWidget::selectedGroups() const
{
    // A group counts as selected when every object in it is selected, which is
    // always true of a group-mode pick and stays true after a box select.
    std::vector<int> groups;
    const std::vector<hammer::vmf::ObjectRef>& selection = editor_.selection();
    for (const hammer::vmf::MapObjectEntry& entry : objectIndex_.objects) {
        if (entry.key.kind != hammer::vmf::MapObjectKind::Group) continue;
        // Ungroup strips the outermost level, so a nested group is left alone -
        // ungrouping its parent is what re-parents it, and it stays a group.
        // A group is outermost exactly when it is in no group itself.
        if (entry.groupId != 0) continue;
        const std::vector<hammer::vmf::ObjectRef> members =
            objectIndex_.groupMembers(entry.key.id);
        if (members.empty()) continue;
        const bool all = std::all_of(members.begin(), members.end(),
                                     [&selection](const hammer::vmf::ObjectRef& member) {
                                         return std::find(selection.begin(), selection.end(),
                                                          member) != selection.end();
                                     });
        if (all) groups.push_back(entry.key.id);
    }
    return groups;
}

bool MapDocumentWidget::selectionCrossesExistingGroups() const
{
    for (const hammer::vmf::ObjectRef& object : editor_.selection()) {
        const hammer::vmf::MapObjectKey key{object.type == hammer::vmf::ObjectType::Solid
                                                ? hammer::vmf::MapObjectKind::Solid
                                                : hammer::vmf::MapObjectKind::Entity,
                                            object.id};
        if (objectIndex_.topLevelGroup(key) != 0) return true;
    }
    return false;
}

bool MapDocumentWidget::groupSelection()
{
    // CMapDoc::OnToolsGroup refuses to group in Solids ("ignore groups") mode.
    if (editor_.selection().size() < 2 || !groupsActive()) return false;
    const std::vector<hammer::vmf::ObjectRef> objects = editor_.selection();
    const bool changed = editor_.applyDocumentEdit(
        [&objects](hammer::vmf::Document& document) {
            const int id = hammer::vmf::maximumObjectId(document) + 1;
            if (hammer::vmf::createGroup(document, objects, id) == 0) return false;
            // Grouping can strand the group the members came out of.
            hammer::vmf::purgeEmptyGroups(document);
            return true;
        },
        "Group Objects");
    if (!changed) return false;
    rebuildScene(false);
    notifyDocumentState(tr("Grouped %1 objects").arg(objects.size()));
    return true;
}

bool MapDocumentWidget::ungroupSelection()
{
    if (!groupsActive()) return false;
    const std::vector<int> groups = selectedGroups();
    if (groups.empty()) return false;
    const bool changed = editor_.applyDocumentEdit(
        [&groups](hammer::vmf::Document& document) {
            return hammer::vmf::ungroup(document, groups);
        },
        "Ungroup");
    if (!changed) return false;
    rebuildScene(false);
    notifyDocumentState(tr("Ungrouped %1 group(s)").arg(groups.size()));
    return true;
}

void MapDocumentWidget::setIgnoreGroups(bool ignore)
{
    if (ignoreGroups_ == ignore) return;
    ignoreGroups_ = ignore;
}

// --- VisGroups --------------------------------------------------------------

std::vector<hammer::vmf::VisGroupDef> MapDocumentWidget::visGroups() const
{
    return objectIndex_.visGroups;
}

hammer::vmf::VisGroupState MapDocumentWidget::visGroupState(int visGroupId) const
{
    if (showAllVisGroups_) return hammer::vmf::VisGroupState::Shown;
    return objectIndex_.visGroupState(visGroupId);
}

std::size_t MapDocumentWidget::visGroupMemberCount(int visGroupId) const
{
    return objectIndex_.visGroupMembers(visGroupId).size();
}

void MapDocumentWidget::setVisGroupVisible(int visGroupId, bool visible)
{
    // CMapDoc::VisGroups_ShowVisGroup calls SetModifiedFlag but never
    // MarkUndoPosition: toggling visibility is a document change with no undo
    // entry, so this goes straight at the document rather than through
    // applyDocumentEdit.
    if (!hammer::vmf::showVisGroup(editor_.document(), visGroupId, visible)) return;
    rebuildScene(false);
    dropHiddenFromSelection();
    notifyDocumentState();
}

void MapDocumentWidget::setShowAllVisGroups(bool showAll)
{
    if (showAllVisGroups_ == showAll) return;
    showAllVisGroups_ = showAll;
    rebuildScene(false);
    dropHiddenFromSelection();
}

void MapDocumentWidget::dropHiddenFromSelection()
{
    std::vector<hammer::vmf::ObjectRef> selection = editor_.selection();
    const std::size_t before = selection.size();
    selection.erase(std::remove_if(selection.begin(), selection.end(),
                                   [this](const hammer::vmf::ObjectRef& object) {
                                       return object.type == hammer::vmf::ObjectType::Solid
                                                  ? hiddenSolids_.contains(object.id)
                                                  : hiddenEntities_.contains(object.id);
                                   }),
                    selection.end());
    hitList_.erase(std::remove_if(hitList_.begin(), hitList_.end(),
                                  [this](const hammer::vmf::ObjectRef& object) {
                                      return object.type == hammer::vmf::ObjectType::Solid
                                                 ? hiddenSolids_.contains(object.id)
                                                 : hiddenEntities_.contains(object.id);
                                  }),
                   hitList_.end());
    if (currentHit_ >= static_cast<int>(hitList_.size())) currentHit_ = -1;
    if (selection.size() == before) return;
    editor_.setSelection(std::move(selection));
    setSelectionOnViews();
    notifySelectionState();
}

int MapDocumentWidget::createVisGroupFromSelection(const QString& name, bool hide,
                                                   bool removeFromOtherVisGroups)
{
    if (editor_.selection().empty()) return 0;
    const std::vector<hammer::vmf::ObjectRef> objects = editor_.selection();
    const std::string visGroupName = name.trimmed().toStdString();
    int created = 0;
    const bool changed = editor_.applyDocumentEdit(
        [&](hammer::vmf::Document& document) {
            const int id = hammer::vmf::createVisGroup(document, visGroupName,
                                                       hammer::vmf::maximumVisGroupId(document) + 1);
            if (id == 0) return false;
            if (!hammer::vmf::addObjectsToVisGroup(document, objects, id,
                                                   removeFromOtherVisGroups)) {
                hammer::vmf::deleteVisGroup(document, id);
                return false;
            }
            if (hide) hammer::vmf::setObjectsVisGroupShown(document, objects, false);
            hammer::vmf::purgeEmptyVisGroups(document);
            created = id;
            return true;
        },
        "Move Selection to VisGroup");
    if (!changed) return 0;
    rebuildScene(false);
    if (hide) dropHiddenFromSelection();
    notifyDocumentState();
    return created;
}

bool MapDocumentWidget::addSelectionToVisGroup(int visGroupId, bool hide,
                                               bool removeFromOtherVisGroups)
{
    if (editor_.selection().empty() || visGroupId <= 0) return false;
    const std::vector<hammer::vmf::ObjectRef> objects = editor_.selection();
    const bool changed = editor_.applyDocumentEdit(
        [&](hammer::vmf::Document& document) {
            bool any =
                hammer::vmf::addObjectsToVisGroup(document, objects, visGroupId,
                                                  removeFromOtherVisGroups);
            if (hide) any = hammer::vmf::setObjectsVisGroupShown(document, objects, false) || any;
            if (any) hammer::vmf::purgeEmptyVisGroups(document);
            return any;
        },
        "Move Selection to VisGroup");
    if (!changed) return false;
    rebuildScene(false);
    if (hide) dropHiddenFromSelection();
    notifyDocumentState();
    return true;
}

bool MapDocumentWidget::renameVisGroup(int visGroupId, const QString& name)
{
    const std::string text = name.trimmed().toStdString();
    if (text.empty()) return false;
    const bool changed = editor_.applyDocumentEdit(
        [visGroupId, &text](hammer::vmf::Document& document) {
            return hammer::vmf::renameVisGroup(document, visGroupId, text);
        },
        "Rename VisGroup");
    if (!changed) return false;
    objectIndex_ = hammer::vmf::indexMapObjects(editor_.document());
    notifyDocumentState();
    return true;
}

bool MapDocumentWidget::setVisGroupColor(int visGroupId, const QColor& color)
{
    const bool changed = editor_.applyDocumentEdit(
        [visGroupId, &color](hammer::vmf::Document& document) {
            return hammer::vmf::setVisGroupColor(document, visGroupId, color.red(), color.green(),
                                                 color.blue());
        },
        "Change VisGroup Color");
    if (!changed) return false;
    objectIndex_ = hammer::vmf::indexMapObjects(editor_.document());
    notifyDocumentState();
    return true;
}

bool MapDocumentWidget::deleteVisGroup(int visGroupId)
{
    const bool changed = editor_.applyDocumentEdit(
        [visGroupId](hammer::vmf::Document& document) {
            // Deleting a visgroup must never leave its members invisible with
            // no way to get them back.
            const hammer::vmf::MapObjectIndex index = hammer::vmf::indexMapObjects(document);
            std::vector<hammer::vmf::ObjectRef> members;
            for (const hammer::vmf::MapObjectKey& key : index.visGroupMembers(visGroupId)) {
                if (key.kind == hammer::vmf::MapObjectKind::Solid)
                    members.push_back({hammer::vmf::ObjectType::Solid, key.id});
                else if (key.kind == hammer::vmf::MapObjectKind::Entity)
                    members.push_back({hammer::vmf::ObjectType::Entity, key.id});
            }
            bool any = hammer::vmf::deleteVisGroup(document, visGroupId);
            if (!members.empty())
                any = hammer::vmf::setObjectsVisGroupShown(document, members, true) || any;
            return any;
        },
        "Delete VisGroup");
    if (!changed) return false;
    rebuildScene(false);
    notifyDocumentState();
    return true;
}

int MapDocumentWidget::createEmptyVisGroup(const QString& name)
{
    const std::string text = name.trimmed().toStdString();
    if (text.empty()) return 0;
    int created = 0;
    const bool changed = editor_.applyDocumentEdit(
        [&](hammer::vmf::Document& document) {
            created = hammer::vmf::createVisGroup(document, text,
                                                  hammer::vmf::maximumVisGroupId(document) + 1);
            return created != 0;
        },
        "New VisGroup");
    if (!changed) return 0;
    objectIndex_ = hammer::vmf::indexMapObjects(editor_.document());
    notifyDocumentState();
    return created;
}

bool MapDocumentWidget::moveVisGroup(int visGroupId, bool up)
{
    const bool changed = editor_.applyDocumentEdit(
        [visGroupId, up](hammer::vmf::Document& document) {
            return hammer::vmf::moveVisGroup(document, visGroupId, up);
        },
        "Reorder VisGroups");
    if (!changed) return false;
    objectIndex_ = hammer::vmf::indexMapObjects(editor_.document());
    notifyDocumentState();
    return true;
}

std::size_t MapDocumentWidget::markVisGroup(int visGroupId)
{
    // The Filter Control "Mark" button: select everything in the visgroup that
    // is currently visible.
    std::vector<hammer::vmf::ObjectRef> selection;
    // A negative id is an auto visgroup, whose membership is derived rather
    // than stored on the objects.
    if (hammer::vmf::isAutoVisGroupId(visGroupId)) ensureAutoVisGroupsFresh();
    const std::vector<hammer::vmf::MapObjectKey> members =
        hammer::vmf::isAutoVisGroupId(visGroupId)
            ? autoVisGroupIndex_.objectsIn(static_cast<hammer::vmf::AutoVisGroup>(visGroupId))
            : objectIndex_.visGroupMembers(visGroupId);
    for (const hammer::vmf::MapObjectKey& key : members) {
        if (key.kind == hammer::vmf::MapObjectKind::Solid) {
            if (hiddenSolids_.contains(key.id)) continue;
            selection.push_back({hammer::vmf::ObjectType::Solid, key.id});
        } else if (key.kind == hammer::vmf::MapObjectKind::Entity) {
            if (hiddenEntities_.contains(key.id)) continue;
            selection.push_back({hammer::vmf::ObjectType::Entity, key.id});
        }
    }
    if (selection.empty()) return 0;
    const std::size_t count = selection.size();
    editor_.setSelection(std::move(selection));
    setSelectionOnViews();
    notifySelectionState();
    return count;
}

std::vector<int> MapDocumentWidget::selectionVisGroups(bool* mixed) const
{
    // The Object Properties VisGroup tab (hammer/op_groups.cpp): the ids every
    // selected object shares, plus a flag when they differ.
    std::vector<int> shared;
    bool first = true;
    bool differ = false;
    for (const hammer::vmf::ObjectRef& object : editor_.selection()) {
        const hammer::vmf::MapObjectKey key{object.type == hammer::vmf::ObjectType::Solid
                                                ? hammer::vmf::MapObjectKind::Solid
                                                : hammer::vmf::MapObjectKind::Entity,
                                            object.id};
        const hammer::vmf::MapObjectEntry* entry = objectIndex_.find(key);
        const std::vector<int> ids = entry ? entry->visGroupIds : std::vector<int>{};
        if (first) {
            shared = ids;
            first = false;
            continue;
        }
        std::vector<int> intersection;
        for (const int id : shared) {
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) intersection.push_back(id);
        }
        if (intersection.size() != shared.size() || ids.size() != shared.size()) differ = true;
        shared = std::move(intersection);
    }
    if (mixed) *mixed = differ;
    return shared;
}

bool MapDocumentWidget::setSelectionVisGroupMembership(int visGroupId, bool member)
{
    if (editor_.selection().empty() || visGroupId <= 0) return false;
    const std::vector<hammer::vmf::ObjectRef> objects = editor_.selection();
    const bool changed = editor_.applyDocumentEdit(
        [&](hammer::vmf::Document& document) {
            if (member)
                return hammer::vmf::addObjectsToVisGroup(document, objects, visGroupId, false);
            const bool any = hammer::vmf::removeObjectsFromVisGroup(document, objects, visGroupId);
            if (any) hammer::vmf::purgeEmptyVisGroups(document);
            return any;
        },
        "Change VisGroup Membership");
    if (!changed) return false;
    rebuildScene(false);
    notifyDocumentState();
    return true;
}

void MapDocumentWidget::setCurrentMaterial(const QString& material)
{
    if (!material.trimmed().isEmpty()) currentMaterial_ = material.trimmed();
}

void MapDocumentWidget::setEntityClass(const QString& classname)
{
    if (!classname.trimmed().isEmpty()) entityClass_ = classname.trimmed();
}

void MapDocumentWidget::setTool(MapViewWidget::Tool tool)
{
    const bool leavingClipper = tool_ == MapViewWidget::Tool::Clipper &&
                                tool != MapViewWidget::Tool::Clipper;
    const bool leavingMorph = tool_ == MapViewWidget::Tool::Morph &&
                              tool != MapViewWidget::Tool::Morph;
    const bool enteringMorph = tool == MapViewWidget::Tool::Morph &&
                               tool_ != MapViewWidget::Tool::Morph;
    // CToolMaterial::OnDeactivate clears the face list. The original keeps it
    // only when the displacement face-edit tool takes over; that tool has no
    // port yet, so the list is always cleared here.
    const bool leavingMaterial = tool_ == MapViewWidget::Tool::TextureApplication &&
                                 tool != MapViewWidget::Tool::TextureApplication;
    // Transition only: re-clicking the active tool button must not re-seed
    // and clobber a hand-picked face list (OnActivate is single-shot).
    const bool enteringMaterial = tool == MapViewWidget::Tool::TextureApplication &&
                                  tool_ != MapViewWidget::Tool::TextureApplication;
    // Morph3D::OnDeactivate runs BEFORE the new tool takes over, so the meshes
    // are committed and the solids re-selected while the morph is still up.
    if (leavingMorph) endMorph();
    tool_ = tool;
    for (MapViewWidget* view : views_) view->setTool(tool);
    // Clipper3D::OnDeactivate -> SetEmpty.
    if (leavingClipper) {
        clipActive_ = false;
        clipPreview_ = {};
        pushClipToViews();
    }
    // Morph3D::OnActivate puts the current selection into morph mode.
    if (enteringMorph) beginMorph();
    if (leavingMaterial) clearFaceSelection();
    if (enteringMaterial) seedFaceSelectionFromObjectSelection();
}

QString MapDocumentWidget::morphHandleModeName() const
{
    switch (morphHandleMode_) {
    case hammer::vmf::MorphHandleMode::VerticesAndEdges: return tr("vertices and edges");
    case hammer::vmf::MorphHandleMode::Vertices: return tr("vertices");
    case hammer::vmf::MorphHandleMode::Edges: return tr("edges");
    }
    return {};
}

QString MapDocumentWidget::cycleMorphHandleMode()
{
    // Morph3D::ToggleMode, reached by activating the tool while it is already
    // active (Morph3D::OnActivate's IsActiveTool branch).
    morphHandleMode_ = hammer::vmf::nextMorphHandleMode(morphHandleMode_);
    // Handles that the new mode does not show cannot stay selected, exactly as
    // CSSolid::ShowHandles drops the hidden handles' selection state.
    const bool keepVertices = morphHandleMode_ != hammer::vmf::MorphHandleMode::Edges;
    const bool keepEdges = morphHandleMode_ != hammer::vmf::MorphHandleMode::Vertices;
    std::vector<hammer::vmf::MorphHandleRef> kept;
    for (const hammer::vmf::MorphHandleRef& ref : morphSelection_) {
        if (ref.edge ? keepEdges : keepVertices) kept.push_back(ref);
    }
    morphSelection_ = std::move(kept);
    pushMorphToViews();
    return morphHandleModeName();
}

void MapDocumentWidget::beginMorph()
{
    // Morph3D::OnActivate: every selected solid becomes a CSSolid, and the
    // document selection is cleared because the solids now live in the tool.
    morphSelection_.clear();
    morphSolids_.clear();
    morphSolidIds_.clear();
    if (!scene_) return;
    morphSolids_ = hammer::vmf::buildMorphSolids(*scene_, editor_.selection());
    for (const hammer::vmf::MorphSolid& solid : morphSolids_) morphSolidIds_.push_back(solid.solidId);
    if (morphSolids_.empty()) {
        notifyDocumentState(tr("Select solids before using vertex manipulation"));
        pushMorphToViews();
        return;
    }
    editor_.clearSelection();
    setSelectionOnViews();
    notifySelectionState();
    pushMorphToViews();
    notifyDocumentState(tr("Vertex manipulation: %1 solid(s), showing %2")
                            .arg(static_cast<qulonglong>(morphSolids_.size()), 0, 10)
                            .arg(morphHandleModeName()));
}

void MapDocumentWidget::endMorph()
{
    // Morph3D::OnDeactivate: save the meshes (SetEmpty) and re-select the
    // solids that were being morphed.
    if (morphSolids_.empty()) {
        pushMorphToViews();
        return;
    }
    finishMorphMove();
    std::vector<hammer::vmf::ObjectRef> selection;
    for (int id : morphSolidIds_) selection.push_back({hammer::vmf::ObjectType::Solid, id});
    morphSolids_.clear();
    morphSolidIds_.clear();
    morphSelection_.clear();
    morphHandleRefs_.clear();
    pushMorphToViews();
    editor_.setSelection(std::move(selection));
    setSelectionOnViews();
    notifySelectionState();
}

void MapDocumentWidget::refreshMorphFromScene()
{
    // Plane intersection can nudge a vertex by a fraction of a unit, and an
    // undo can change the solids outright, so the meshes are rebuilt from the
    // document rather than kept across a commit.
    if (morphSolidIds_.empty() || !scene_) return;
    std::vector<hammer::vmf::MorphSolid> rebuilt =
        hammer::vmf::buildMorphSolidsById(*scene_, morphSolidIds_);
    // Keep the handle selection only where the topology is unchanged.
    std::vector<hammer::vmf::MorphHandleRef> kept;
    for (const hammer::vmf::MorphHandleRef& ref : morphSelection_) {
        if (ref.solid >= rebuilt.size() || ref.solid >= morphSolids_.size()) continue;
        if (rebuilt[ref.solid].solidId != morphSolids_[ref.solid].solidId ||
            rebuilt[ref.solid].vertices.size() != morphSolids_[ref.solid].vertices.size() ||
            rebuilt[ref.solid].edges.size() != morphSolids_[ref.solid].edges.size() ||
            rebuilt[ref.solid].dispFaces.size() != morphSolids_[ref.solid].dispFaces.size()) {
            continue;
        }
        if (ref.dispFace >= 0) {
            const std::size_t dispIndex = static_cast<std::size_t>(ref.dispFace);
            if (dispIndex >= rebuilt[ref.solid].dispFaces.size() ||
                rebuilt[ref.solid].dispFaces[dispIndex].positions.size() !=
                    morphSolids_[ref.solid].dispFaces[dispIndex].positions.size()) {
                continue;
            }
        }
        kept.push_back(ref);
    }
    morphSolids_ = std::move(rebuilt);
    morphSelection_ = std::move(kept);
    morphSolidIds_.clear();
    for (const hammer::vmf::MorphSolid& solid : morphSolids_) morphSolidIds_.push_back(solid.solidId);
    pushMorphToViews();
}

void MapDocumentWidget::selectMorphHandles(const QList<int>& handles, bool toggle)
{
    // Morph3D::SelectHandle. A plain click clears the selection first
    // (scClear|scSelect); Ctrl-click toggles (scToggle).
    if (morphSolids_.empty()) return;
    if (!toggle) morphSelection_.clear();
    for (int index : handles) {
        if (index < 0 || index >= static_cast<int>(morphHandleRefs_.size())) continue;
        const hammer::vmf::MorphHandleRef ref = morphHandleRefs_[static_cast<std::size_t>(index)];
        const auto existing = std::find(morphSelection_.begin(), morphSelection_.end(), ref);
        if (existing != morphSelection_.end()) {
            if (toggle) morphSelection_.erase(existing);
            continue;
        }
        // Vertex and edge handles cannot be selected together: SelectHandle
        // clears the selection when hi.Type != m_SelectedType.
        if (!morphSelection_.empty() && morphSelection_.front().edge != ref.edge) {
            morphSelection_.clear();
        }
        morphSelection_.push_back(ref);
    }
    pushMorphToViews();
}

void MapDocumentWidget::clearMorphHandleSelection()
{
    if (morphSelection_.empty()) return;
    morphSelection_.clear();
    pushMorphToViews();
}

void MapDocumentWidget::moveMorphSelection(const hammer::vmf::Vec3& delta)
{
    // Morph3D::MoveSelectedHandles -> CSSolid::MoveSelectedHandles: an edge
    // handle drags both of its vertices along.
    if (morphSolids_.empty() || morphSelection_.empty()) return;
    for (std::size_t i = 0; i < morphSolids_.size(); ++i) {
        std::vector<std::size_t> vertices;
        std::vector<std::size_t> edges;
        std::map<int, std::vector<std::size_t>> dispVertices;
        for (const hammer::vmf::MorphHandleRef& ref : morphSelection_) {
            if (ref.solid != i) continue;
            if (ref.dispFace >= 0) dispVertices[ref.dispFace].push_back(ref.index);
            else (ref.edge ? edges : vertices).push_back(ref.index);
        }
        if (!vertices.empty() || !edges.empty())
            hammer::vmf::moveMorphHandles(morphSolids_[i], vertices, edges, delta);
        for (const auto& [dispFace, indices] : dispVertices) {
            hammer::vmf::moveMorphDispHandles(morphSolids_[i],
                                              static_cast<std::size_t>(dispFace), indices, delta);
        }
    }
    pushMorphToViews();
}

void MapDocumentWidget::finishMorphMove()
{
    // Morph3D::FinishTranslation( true ) followed by the tool's single
    // "Morphing" undo entry (Morph3D::SetEmpty).
    if (morphSolids_.empty()) return;
    const bool changed = editor_.applyMorph(morphSolids_);
    if (!changed) {
        // Nothing moved, or the move would have collapsed the solid; drop the
        // pending vertex positions so the handles snap back.
        refreshMorphFromScene();
        return;
    }
    // Morphing only reshapes the solids the tool is holding.
    std::vector<hammer::vmf::ObjectRef> morphed;
    for (const int id : morphSolidIds_) morphed.push_back({hammer::vmf::ObjectType::Solid, id});
    rebuildSceneObjects(morphed);
    refreshMorphFromScene();
    notifyDocumentState(tr("Morphed %1 solid(s)").arg(static_cast<qulonglong>(morphSolidIds_.size())));
}

void MapDocumentWidget::morphEscape()
{
    // Morph3D::OnEscape: clear the selected handles first, leave the tool on
    // the second press.
    if (!morphSelection_.empty()) {
        clearMorphHandleSelection();
        return;
    }
    emit selectionToolRequested();
}

void MapDocumentWidget::pushMorphToViews()
{
    const std::vector<hammer::vmf::MorphHandle> handles =
        hammer::vmf::morphHandles(morphSolids_, morphHandleMode_, morphSelection_, &morphHandleRefs_);
    const std::vector<hammer::vmf::FacePolygons> preview =
        hammer::vmf::morphFacePolygons(morphSolids_);
    const std::vector<hammer::vmf::MorphDispGrid> dispGrids =
        hammer::vmf::morphDispGrids(morphSolids_);
    for (MapViewWidget* view : views_) {
        if (view) view->setMorphState(!morphSolids_.empty(), handles, preview, dispGrids);
    }
}

QString MapDocumentWidget::clipModeName() const
{
    switch (clipMode_) {
    case hammer::vmf::EditorModel::ClipMode::Front: return tr("keep front");
    case hammer::vmf::EditorModel::ClipMode::Back: return tr("keep back");
    case hammer::vmf::EditorModel::ClipMode::Both: return tr("keep both");
    }
    return {};
}

QString MapDocumentWidget::cycleClipMode()
{
    // Clipper3D::IterateClipMode: front -> back -> both -> front, followed by
    // GetClipResults so the preview reflects the new mode immediately.
    switch (clipMode_) {
    case hammer::vmf::EditorModel::ClipMode::Front:
        clipMode_ = hammer::vmf::EditorModel::ClipMode::Back;
        break;
    case hammer::vmf::EditorModel::ClipMode::Back:
        clipMode_ = hammer::vmf::EditorModel::ClipMode::Both;
        break;
    case hammer::vmf::EditorModel::ClipMode::Both:
        clipMode_ = hammer::vmf::EditorModel::ClipMode::Front;
        break;
    }
    updateClipLine(clipPoints_[0], clipPoints_[1], clipViewAxis_);
    return clipModeName();
}

void MapDocumentWidget::updateClipLine(const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                                       const hammer::vmf::Vec3& viewAxis)
{
    clipPoints_[0] = first;
    clipPoints_[1] = second;
    clipViewAxis_ = viewAxis;
    clipActive_ = true;
    // Clipper3D::BuildClipPlane + GetClipResults.
    const hammer::vmf::ClipPlane plane = hammer::vmf::clipPlaneFromLine(first, second, viewAxis);
    clipPreview_ = plane.valid() ? editor_.previewClip(plane, clipMode_)
                                 : hammer::vmf::EditorModel::ClipPreview{};
    pushClipToViews();
}

void MapDocumentWidget::clearClip()
{
    // Clipper3D::OnEscape with a clip in progress: SetEmpty, tool stays active.
    if (!clipActive_) return;
    clipActive_ = false;
    clipPreview_ = {};
    pushClipToViews();
    notifyDocumentState(tr("Cleared the clip plane"));
}

void MapDocumentWidget::applyClip()
{
    // Clipper3D::SaveClipResults, as one "Clip Objects" undo step.
    if (!clipActive_) return;
    const hammer::vmf::ClipPlane plane =
        hammer::vmf::clipPlaneFromLine(clipPoints_[0], clipPoints_[1], clipViewAxis_);
    // Deliberate deviation: the original builds a degenerate plane from a
    // zero-length clip line and then deletes every selected solid. Refuse.
    if (!plane.valid()) {
        notifyDocumentState(tr("Drag a clip line before clipping"));
        return;
    }
    if (!editor_.clipSelection(plane, clipMode_)) {
        notifyDocumentState(tr("Nothing to clip"));
        return;
    }
    clipActive_ = false;
    clipPreview_ = {};
    rebuildScene(false);
    pushClipToViews();
    notifyDocumentState(tr("Clipped objects (%1)").arg(clipModeName()));
    notifySelectionState();
}

void MapDocumentWidget::pushClipToViews()
{
    for (MapViewWidget* view : views_) {
        if (!view) continue;
        view->setClipState(clipActive_, clipPoints_[0], clipPoints_[1], clipViewAxis_, clipMode_,
                           clipPreview_.kept, clipPreview_.discarded);
    }
}

void MapDocumentWidget::setTransformMode(MapViewWidget::TransformMode mode)
{
    if (transformMode_ == mode) return;
    transformMode_ = mode;
    for (MapViewWidget* view : views_) view->setTransformMode(mode);
    switch (mode) {
    case MapViewWidget::TransformMode::Scale:
        emit documentMessage(tr("Transform handles: Resize")); break;
    case MapViewWidget::TransformMode::Translate:
        emit documentMessage(tr("Transform handles: Move (drag an edge handle to move along one axis)")); break;
    case MapViewWidget::TransformMode::Rotate:
        emit documentMessage(tr("Transform handles: Rotate (15° snap, Alt for free rotation)")); break;
    }
    emit transformModeChanged(mode);
}

hammer::vmf::ClipboardData MapDocumentWidget::copySelection() const
{
    return editor_.copySelection();
}

bool MapDocumentWidget::cutSelection(hammer::vmf::ClipboardData& clipboard)
{
    clipboard = editor_.copySelection();
    if (clipboard.empty() || !editor_.deleteSelection("Cut Objects")) return false;
    rebuildScene(false);
    notifyDocumentState(tr("Cut %1 object(s)").arg(static_cast<qulonglong>(clipboard.objects.size())));
    notifySelectionState();
    return true;
}

bool MapDocumentWidget::paste(const hammer::vmf::ClipboardData& clipboard)
{
    if (!editor_.paste(clipboard)) return false;
    rebuildScene(false);
    notifyDocumentState(tr("Pasted %1 object(s)").arg(static_cast<qulonglong>(editor_.selection().size())));
    notifySelectionState();
    return true;
}

std::size_t MapDocumentWidget::pasteSpecial(const hammer::vmf::ClipboardData& clipboard,
                                            hammer::vmf::PasteSpecialOptions options)
{
    if (!options.startAtOriginal) options.viewCenter = viewsCenterWorld();
    if (!editor_.pasteSpecial(clipboard, options)) return 0;
    const std::size_t created = editor_.selection().size();
    rebuildScene(false);
    notifyDocumentState(tr("Pasted %1 object(s) in %2 cop%3")
                            .arg(static_cast<qulonglong>(created))
                            .arg(std::max(1, options.copies))
                            .arg(std::max(1, options.copies) == 1 ? tr("y") : tr("ies")));
    notifySelectionState();
    return created;
}

hammer::vmf::Vec3 MapDocumentWidget::viewsCenterWorld() const
{
    hammer::vmf::Vec3 center{};
    const auto take = [&](MapViewWidget::Kind kind) -> std::optional<hammer::vmf::Vec3> {
        for (MapViewWidget* view : views_) {
            if (view && view->kind() == kind) return view->viewCenterWorld();
        }
        return std::nullopt;
    };
    const auto top = take(MapViewWidget::Kind::Top);
    const auto front = take(MapViewWidget::Kind::Front);
    const auto side = take(MapViewWidget::Kind::Side);
    if (top) {
        center.x = top->x;
        center.y = top->y;
    } else if (side) {
        center.x = side->x;
    }
    if (front) {
        center.y = top ? center.y : front->y;
        center.z = front->z;
    } else if (side) {
        center.z = side->z;
    }
    return center;
}

bool MapDocumentWidget::duplicateSelection()
{
    if (!editor_.duplicateSelection()) return false;
    rebuildScene(false);
    notifyDocumentState(tr("Duplicated selection"));
    notifySelectionState();
    return true;
}

bool MapDocumentWidget::duplicateSelectionBy(const hammer::vmf::Vec3& delta)
{
    // paste() selects the copy, so a chain of these walks copies out from
    // the newest one - each its own undo step, as each is its own action.
    // New objects cannot take the incremental scene path (their ids are not
    // in the previous scene), so this is a full rebuild per press; the view
    // suppresses autorepeat for it precisely so that stays a per-press cost.
    if (!editor_.duplicateSelection(delta, "Clone Objects")) return false;
    rebuildScene(false);
    notifyDocumentState(tr("Cloned selection by %1 %2 %3").arg(delta.x).arg(delta.y).arg(delta.z));
    notifySelectionState();
    return true;
}

QString MapDocumentWidget::defaultPointFilePath() const
{
    // CMapDoc::OnMapLoadpointfile swaps the map's extension for the compiler's
    // pointfile extension: .lin for Source (mfHalfLife2), .pts otherwise. Both
    // are offered here, newest-of-what-exists first.
    if (filePath_.isEmpty()) return {};
    const QFileInfo mapFile(filePath_);
    const QString stem = mapFile.dir().filePath(mapFile.completeBaseName());
    const QString lin = stem + QStringLiteral(".lin");
    if (QFileInfo::exists(lin)) return lin;
    const QString pts = stem + QStringLiteral(".pts");
    if (QFileInfo::exists(pts)) return pts;
    return lin;
}

bool MapDocumentWidget::loadPointFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }

    std::vector<hammer::vmf::Vec3> points;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const QStringList fields = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 3) {
            // The original stops at the first line that is not three numbers
            // rather than skipping it, so a truncated file loads the part that
            // is intact. Blank leading lines are not skipped either.
            break;
        }
        bool okX = false;
        bool okY = false;
        bool okZ = false;
        const hammer::vmf::Vec3 point{fields[0].toDouble(&okX), fields[1].toDouble(&okY),
                                      fields[2].toDouble(&okZ)};
        if (!okX || !okY || !okZ) break;
        points.push_back(point);
    }

    if (points.size() < 2) {
        if (error) {
            *error = points.empty() ? tr("The file contains no pointfile coordinates.")
                                    : tr("The file contains a single point, so there is no line "
                                         "to trace.");
        }
        return false;
    }

    pointFile_ = std::move(points);
    pointFilePath_ = path;
    for (MapViewWidget* view : views_) {
        if (view) view->setPointFile(pointFile_);
    }
    return true;
}

void MapDocumentWidget::unloadPointFile()
{
    if (pointFile_.empty()) return;
    pointFile_.clear();
    pointFilePath_.clear();
    for (MapViewWidget* view : views_) {
        if (view) view->setPointFile({});
    }
}

QString MapDocumentWidget::defaultPortalFilePath() const
{
    // vbsp names the portal file after the map, beside it, with a .prt
    // extension, the same way the pointfile is named.
    if (filePath_.isEmpty()) return {};
    const QFileInfo mapFile(filePath_);
    return mapFile.dir().filePath(mapFile.completeBaseName() + QStringLiteral(".prt"));
}

bool MapDocumentWidget::loadPortalFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }

    std::vector<std::vector<hammer::vmf::Vec3>> portals;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (!line.contains(QLatin1Char('('))) {
            // The PRT1/PRT2 header and its count lines come first; a PRT2 file
            // also carries a cluster table after the portals. Skip the leading
            // ones, stop at anything that follows the windings.
            if (portals.empty()) continue;
            break;
        }
        // "<points> <cluster> <cluster> (x y z ) (x y z ) ..." — the
        // parentheses are pure punctuation, so drop them and read numbers.
        line.replace(QLatin1Char('('), QLatin1Char(' '));
        line.replace(QLatin1Char(')'), QLatin1Char(' '));
        const QStringList fields = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 3) break;
        bool okCount = false;
        const int pointCount = fields[0].toInt(&okCount);
        if (!okCount || pointCount < 3) break;
        if (fields.size() < 3 + pointCount * 3) break;

        std::vector<hammer::vmf::Vec3> winding;
        winding.reserve(static_cast<std::size_t>(pointCount));
        bool okWinding = true;
        for (int index = 0; index < pointCount && okWinding; ++index) {
            const int base = 3 + index * 3;
            bool okX = false;
            bool okY = false;
            bool okZ = false;
            const hammer::vmf::Vec3 point{fields[base].toDouble(&okX),
                                          fields[base + 1].toDouble(&okY),
                                          fields[base + 2].toDouble(&okZ)};
            okWinding = okX && okY && okZ;
            winding.push_back(point);
        }
        if (!okWinding) break;
        portals.push_back(std::move(winding));
    }

    if (portals.empty()) {
        if (error) *error = tr("The file contains no portal windings.");
        return false;
    }

    portalFile_ = std::move(portals);
    portalFilePath_ = path;
    for (MapViewWidget* view : views_) {
        if (view) view->setPortalFile(portalFile_);
    }
    return true;
}

void MapDocumentWidget::unloadPortalFile()
{
    if (portalFile_.empty()) return;
    portalFile_.clear();
    portalFilePath_.clear();
    for (MapViewWidget* view : views_) {
        if (view) view->setPortalFile({});
    }
}

void MapDocumentWidget::showObjectProperties(QWidget* dialogParent, bool openVisGroupPage)
{
    if (editor_.selection().size() != 1) {
        QMessageBox::information(dialogParent ? dialogParent : this, tr("Object Properties"),
                                 tr("Select exactly one solid or entity to edit its properties."));
        return;
    }

    const auto object = editor_.selection().front();
    ObjectPropertiesDialog::Setup setup;
    setup.object = object;
    setup.typeName = objectTypeName(object.type);
    setup.properties = editor_.selectedProperties();
    setup.connections = editor_.selectedConnections();
    setup.fgd = fgd_;
    setup.document = &editor_.document();
    // The VisGroup page (hammer/op_groups.cpp).
    for (const hammer::vmf::VisGroupDef& group : objectIndex_.visGroups) {
        if (group.automatic) continue;
        setup.visGroups.emplace_back(group.id, QString::fromStdString(group.name));
    }
    setup.memberVisGroups = selectionVisGroups(&setup.visGroupsMixed);
    setup.setVisGroupMembership = [this](int visGroupId, bool member) {
        setSelectionVisGroupMembership(visGroupId, member);
    };
    if (openVisGroupPage) setup.initialPage = ObjectPropertiesDialog::Setup::Page::VisGroups;
    // A brush entity (an entity block with solid children) can only become
    // another solid class; a point entity only another point class.
    if (object.type == hammer::vmf::ObjectType::Entity) {
        for (const hammer::vmf::Block& root : editor_.document().roots()) {
            if (QString::fromStdString(root.name).compare(QStringLiteral("entity"),
                                                          Qt::CaseInsensitive) != 0) {
                continue;
            }
            const std::string* idValue = root.value("id");
            if (idValue && QString::fromStdString(*idValue).toInt() == object.id) {
                setup.brushEntity = !root.children("solid").empty();
                break;
            }
        }
    }
    setup.modelEditorFactory =
        [this](QWidget* parent, QDialog* dialog, const QString& key, const QString& value,
               const std::function<void(const QString&, const QString&)>& setRawValue,
               const std::function<QString(const QString&)>& rawValue) {
            return makeModelPropertyEditor(parent, dialog, key, value, materials_, studioModels_,
                                           texturedRenderMode_, phongEnabled_, specularEnabled_,
                                           bumpMapsEnabled_, lightWarpEnabled_, selfIllumEnabled_,
                                           rimLightEnabled_, phongIntensity_, specularIntensity_,
                                           bumpMapIntensity_,
                                           scene_ ? scene_->skyName : std::string{},
                                           setRawValue, rawValue);
        };
    setup.sequenceEditorFactory =
        [this](QWidget* parent, QDialog* dialog, const QString& key, const QString& value,
               const std::function<void(const QString&, const QString&)>& setRawValue,
               const std::function<QString(const QString&)>& rawValue) {
            return makeSequencePropertyEditor(parent, dialog, key, value, studioModels_,
                                              setRawValue, rawValue);
        };
    setup.skyEditorFactory =
        [this](QWidget* parent, QDialog* dialog, const QString& key, const QString& value,
               const std::function<void(const QString&, const QString&)>& setRawValue,
               const std::function<QString(const QString&)>&) {
            return makeSkyPropertyEditor(parent, dialog, key, value, materials_,
                                         texturedRenderMode_, phongEnabled_, specularEnabled_,
                                         bumpMapsEnabled_, lightWarpEnabled_, selfIllumEnabled_,
                                         rimLightEnabled_, phongIntensity_, specularIntensity_,
                                         bumpMapIntensity_, setRawValue);
        };

    ObjectPropertiesDialog dialog(std::move(setup), dialogParent ? dialogParent : this);
    // Apply and OK take the same path; Apply just leaves the dialog open, so
    // each one is its own undo step (CObjectProperties::OnApply).
    const auto commit = [this, &dialog, object] {
        std::vector<hammer::vmf::Property> updated;
        updated.reserve(dialog.properties().size());
        for (const auto& property : dialog.properties()) {
            if (QString::fromStdString(property.key).trimmed().isEmpty()) continue;
            updated.push_back(property);
        }
        const bool changed =
            dialog.isEntity()
                ? editor_.replaceSelectedPropertiesAndConnections(updated, dialog.connections())
                : editor_.replaceSelectedProperties(updated);
        if (changed) {
            rebuildScene(false);
            notifyDocumentState(tr("Changed properties for %1 #%2")
                                    .arg(objectTypeName(object.type))
                                    .arg(object.id));
            notifySelectionState();
        }
    };
    connect(&dialog, &ObjectPropertiesDialog::applyRequested, &dialog, [&dialog, commit] {
        commit();
        dialog.markApplied();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    commit();
}

void MapDocumentWidget::showMapProperties(QWidget* dialogParent)
{
    std::vector<hammer::vmf::Property> current = editor_.worldProperties();
    if (current.empty()) {
        QMessageBox::warning(dialogParent ? dialogParent : this, tr("Map Properties"),
                             tr("This document does not contain a worldspawn block."));
        return;
    }

    // Map Properties is the same sheet as Object Properties, run over the
    // worldspawn block (CMapDoc::OnMapProperties opens the same pages).
    ObjectPropertiesDialog::Setup setup;
    setup.world = true;
    setup.windowTitle = tr("Map Properties - worldspawn");
    setup.typeName = tr("worldspawn");
    setup.properties = std::move(current);
    setup.fgd = fgd_;
    setup.document = &editor_.document();
    setup.modelEditorFactory =
        [this](QWidget* parent, QDialog* dialog, const QString& key, const QString& value,
               const std::function<void(const QString&, const QString&)>& setRawValue,
               const std::function<QString(const QString&)>& rawValue) {
            return makeModelPropertyEditor(parent, dialog, key, value, materials_, studioModels_,
                                           texturedRenderMode_, phongEnabled_, specularEnabled_,
                                           bumpMapsEnabled_, lightWarpEnabled_, selfIllumEnabled_,
                                           rimLightEnabled_, phongIntensity_, specularIntensity_,
                                           bumpMapIntensity_,
                                           scene_ ? scene_->skyName : std::string{},
                                           setRawValue, rawValue);
        };
    setup.sequenceEditorFactory =
        [this](QWidget* parent, QDialog* dialog, const QString& key, const QString& value,
               const std::function<void(const QString&, const QString&)>& setRawValue,
               const std::function<QString(const QString&)>& rawValue) {
            return makeSequencePropertyEditor(parent, dialog, key, value, studioModels_,
                                              setRawValue, rawValue);
        };
    setup.skyEditorFactory =
        [this](QWidget* parent, QDialog* dialog, const QString& key, const QString& value,
               const std::function<void(const QString&, const QString&)>& setRawValue,
               const std::function<QString(const QString&)>&) {
            return makeSkyPropertyEditor(parent, dialog, key, value, materials_,
                                         texturedRenderMode_, phongEnabled_, specularEnabled_,
                                         bumpMapsEnabled_, lightWarpEnabled_, selfIllumEnabled_,
                                         rimLightEnabled_, phongIntensity_, specularIntensity_,
                                         bumpMapIntensity_, setRawValue);
        };

    ObjectPropertiesDialog dialog(std::move(setup), dialogParent ? dialogParent : this);
    const auto commit = [this, &dialog] {
        std::vector<hammer::vmf::Property> updated;
        updated.reserve(dialog.properties().size());
        for (const auto& property : dialog.properties()) {
            if (QString::fromStdString(property.key).trimmed().isEmpty()) continue;
            updated.push_back(property);
        }
        if (editor_.replaceWorldProperties(updated)) {
            rebuildScene(false);
            notifyDocumentState(tr("Changed map properties"));
        }
    };
    connect(&dialog, &ObjectPropertiesDialog::applyRequested, &dialog, [&dialog, commit] {
        commit();
        dialog.markApplied();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    commit();
}

void MapDocumentWidget::beginMove() { editor_.beginTransaction("Move Objects"); }

void MapDocumentWidget::moveSelection(const hammer::vmf::Vec3& delta)
{
    if (editor_.translateSelectionInTransaction(delta)) {
        rebuildSelectedObjectsInScene();
        notifyDocumentState();
        notifySelectionState();
    }
}

void MapDocumentWidget::finishMove()
{
    if (editor_.commitTransaction()) {
        notifyDocumentState(tr("Moved selection"));
        notifySelectionState();
    }
}

void MapDocumentWidget::beginResize() { editor_.beginTransaction("Resize Objects"); }

void MapDocumentWidget::resizeSelection(const hammer::vmf::Vec3& factors, const hammer::vmf::Vec3& pivot)
{
    const bool perfLog = qEnvironmentVariableIsSet("HAMMER_PERF");
    QElapsedTimer perfTimer;
    if (perfLog) perfTimer.start();
    double editMs = 0.0;
    double rebuildMs = 0.0;
    if (editor_.scaleSelectionInTransaction(factors, pivot)) {
        if (perfLog) editMs = perfTimer.nsecsElapsed() / 1e6;
        rebuildSelectedObjectsInScene();
        if (perfLog) rebuildMs = perfTimer.nsecsElapsed() / 1e6 - editMs;
        notifyDocumentState();
        notifySelectionState();
        if (perfLog) {
            fprintf(stderr, "perf resize step: edit %.2f ms, rebuild+publish %.2f ms, notify %.2f ms\n",
                    editMs, rebuildMs, perfTimer.nsecsElapsed() / 1e6 - editMs - rebuildMs);
        }
    }
}

void MapDocumentWidget::beginRotate() { editor_.beginTransaction("Rotate Objects"); }

void MapDocumentWidget::rotateSelection(double radians, hammer::vmf::RotationAxis axis, const hammer::vmf::Vec3& pivot)
{
    if (editor_.rotateSelectionInTransaction(radians, axis, pivot)) {
        rebuildSelectedObjectsInScene();
        notifyDocumentState();
        notifySelectionState();
    }
}

void MapDocumentWidget::finishTransform()
{
    if (editor_.commitTransaction()) {
        notifyDocumentState(tr("Transformed selection"));
        notifySelectionState();
    }
}

std::optional<hammer::vmf::Vec3> MapDocumentWidget::selectedPointEntityOrigin() const
{
    if (!scene_ || editor_.selection().size() != 1) return std::nullopt;
    const hammer::vmf::ObjectRef object = editor_.selection().front();
    if (object.type != hammer::vmf::ObjectType::Entity) return std::nullopt;
    for (const hammer::vmf::EntityMarker& entity : scene_->entities) {
        if (entity.object == object) return entity.origin;
    }
    return std::nullopt;
}

void MapDocumentWidget::setPrimitiveKindByName(const QString& name)
{
    using Kind = hammer::vmf::EditorModel::PrimitiveKind;
    if (name.compare(QStringLiteral("Wedge"), Qt::CaseInsensitive) == 0) primitiveKind_ = Kind::Wedge;
    else if (name.compare(QStringLiteral("Cylinder"), Qt::CaseInsensitive) == 0) primitiveKind_ = Kind::Cylinder;
    else if (name.compare(QStringLiteral("Spike"), Qt::CaseInsensitive) == 0) primitiveKind_ = Kind::Spike;
    else primitiveKind_ = Kind::Block;
}

void MapDocumentWidget::setPrimitiveFaces(int faces)
{
    primitiveFaces_ = std::clamp(faces, 3, 64);
}

void MapDocumentWidget::createBlock(const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                                    int extrusionAxis)
{
    // Committing consumes the shared preview everywhere, including the views
    // that only received it through the relay.
    for (MapViewWidget* view : views_) {
        if (view) view->setPendingBlock({}, -1);
    }
    if (!editor_.createPrimitive(primitiveKind_, first, second, extrusionAxis, primitiveFaces_,
                                 currentMaterial_.toUtf8().toStdString())) {
        return;
    }
    rebuildScene(false);
    notifyDocumentState(tr("Created brush with %1").arg(currentMaterial_));
    notifySelectionState();
}

void MapDocumentWidget::createEntity(const hammer::vmf::Vec3& origin)
{
    const std::string classname = entityClass_.isEmpty() ? std::string("info_target") : entityClass_.toUtf8().toStdString();
    if (!editor_.createPointEntity(classname, origin, entityDefaults(classname))) return;
    rebuildScene(false);
    notifyDocumentState(tr("Created %1 at %2 %3 %4").arg(QString::fromStdString(classname)).arg(origin.x).arg(origin.y).arg(origin.z));
    notifySelectionState();
}

void MapDocumentWidget::createEntityOnSurface(const hammer::vmf::Vec3& position,
                                              const hammer::vmf::Vec3& normal)
{
    // CToolEntity::OnLMouseDown3D: traces the click ray against the clicked
    // solid's face and places the entity at the impact point, then calls
    // CMapEntity::AlignOnPlane(HitPos, &pFace->plane, ALIGN_BOTTOM/ALIGN_TOP)
    // to offset it so its bounding box sits flush against the surface rather
    // than being centered in the wall. We don't have AlignOnPlane's original
    // source (it isn't present anywhere in this reference tree), so this
    // reimplements its effect in general form: push the entity's origin out
    // along the hit normal by the box corner that's nearest the surface, for
    // whichever axes the normal touches (floor/ceiling/wall all fall out of
    // the same formula that the original special-cased on HitNormal.z).
    const std::string classname = entityClass_.isEmpty() ? std::string("info_target") : entityClass_.toUtf8().toStdString();
    hammer::fgd::EntityVisualization visualization;
    if (fgd_ && !fgd_->empty()) visualization = fgd_->effectiveVisualization(classname);
    const auto pick = [](double n, double lo, double hi) { return n >= 0.0 ? lo : hi; };
    const hammer::vmf::Vec3 corner{pick(normal.x, visualization.sizeMinimum[0], visualization.sizeMaximum[0]),
                                   pick(normal.y, visualization.sizeMinimum[1], visualization.sizeMaximum[1]),
                                   pick(normal.z, visualization.sizeMinimum[2], visualization.sizeMaximum[2])};
    const double projected = normal.x * corner.x + normal.y * corner.y + normal.z * corner.z;
    const hammer::vmf::Vec3 origin{position.x - normal.x * projected, position.y - normal.y * projected,
                                   position.z - normal.z * projected};
    if (!editor_.createPointEntity(classname, origin, entityDefaults(classname))) return;
    rebuildScene(false);
    notifyDocumentState(tr("Created %1 at %2 %3 %4").arg(QString::fromStdString(classname)).arg(origin.x).arg(origin.y).arg(origin.z));
    notifySelectionState();
}

void MapDocumentWidget::createDecal(const hammer::vmf::Vec3& position,
                                    const hammer::vmf::Vec3& normal, int sideId)
{
    Q_UNUSED(sideId);
    const hammer::vmf::Vec3 origin{position.x + normal.x * 0.25,
                                   position.y + normal.y * 0.25,
                                   position.z + normal.z * 0.25};
    std::vector<hammer::vmf::Property> properties = entityDefaults("infodecal");
    const std::string material = currentMaterial_.toUtf8().toStdString();
    auto texture = std::find_if(properties.begin(), properties.end(), [](const auto& property) {
        return QString::fromStdString(property.key).compare(QStringLiteral("texture"), Qt::CaseInsensitive) == 0;
    });
    if (texture == properties.end()) properties.push_back({"texture", material});
    else texture->value = material;
    if (!editor_.createPointEntity("infodecal", origin, properties, "Create Decal")) return;
    rebuildScene(false);
    setSelectionOnViews();
    notifyDocumentState(tr("Applied decal %1").arg(currentMaterial_));
    notifySelectionState();
}

void MapDocumentWidget::createOverlay(const hammer::vmf::Vec3& position,
                                      const hammer::vmf::Vec3& normal, int sideId)
{
    hammer::vmf::Vec3 axisU, axisV;
    projectedSurfaceBasis(normal, axisU, axisV);
    double halfWidth = 32.0;
    double halfHeight = 32.0;
    if (materials_) {
        const auto material = materials_->material(currentMaterial_.toUtf8().toStdString());
        if (material && material->image.valid()) {
            // CMapOverlay::Handles_Init uses one eighth of each image
            // dimension as the half extent (one quarter full-size preview).
            halfWidth = std::clamp(material->image.width / 8.0, 1.0, 512.0);
            halfHeight = std::clamp(material->image.height / 8.0, 1.0, 512.0);
        }
    }

    const auto created = editor_.createPointEntity("info_overlay", position,
                                                   entityDefaults("info_overlay"),
                                                   "Create Overlay");
    if (!created) return;
    for (hammer::vmf::Block& root : editor_.document().roots()) {
        const std::string* id = root.value("id");
        if ((root.name != "entity" && root.name != "ENTITY") || !id ||
            std::atoi(id->c_str()) != created->id) continue;
        hammer::vmf::Block& data = root.appendChild("overlaydata");
        auto setOverlayValue = [&](std::string key, std::string value) {
            // VBSP consumes the entity keyvalues; Hammer's editor helper also
            // persists the same state in overlaydata. Save both representations.
            root.setValue(key, value);
            data.setValue(std::move(key), std::move(value));
        };
        setOverlayValue("material", currentMaterial_.toUtf8().toStdString());
        setOverlayValue("StartU", "0");
        setOverlayValue("EndU", "1");
        setOverlayValue("StartV", "0");
        setOverlayValue("EndV", "1");
        setOverlayValue("BasisOrigin", formatProjectedVec3(position));
        setOverlayValue("BasisU", formatProjectedVec3(axisU));
        setOverlayValue("BasisV", formatProjectedVec3(axisV));
        setOverlayValue("BasisNormal", formatProjectedVec3(normal));
        setOverlayValue("uv0", formatProjectedVec3({-halfWidth, -halfHeight, 0.0}));
        setOverlayValue("uv1", formatProjectedVec3({-halfWidth, halfHeight, 0.0}));
        setOverlayValue("uv2", formatProjectedVec3({halfWidth, halfHeight, 0.0}));
        setOverlayValue("uv3", formatProjectedVec3({halfWidth, -halfHeight, 0.0}));
        setOverlayValue("sides", std::to_string(sideId));
        editor_.document().markDirty();
        break;
    }
    rebuildScene(false);
    setSelectionOnViews();
    notifyDocumentState(tr("Created overlay with %1").arg(currentMaterial_));
    notifySelectionState();
}

std::vector<hammer::vmf::Property> MapDocumentWidget::entityDefaults(const std::string& classname) const
{
    std::vector<hammer::vmf::Property> defaults;
    if (!fgd_) return defaults;
    for (const auto& definition : fgd_->effectiveProperties(classname)) {
        std::string value = definition.defaultValue;
        if (value.empty() && definition.type == hammer::fgd::PropertyType::Flags) {
            int flags = 0;
            for (const auto& choice : definition.choices) {
                if (!choice.defaultOn) continue;
                int bit = 0;
                const auto result = std::from_chars(choice.value.data(), choice.value.data() + choice.value.size(), bit);
                if (result.ec == std::errc{}) flags |= bit;
            }
            if (flags != 0) value = std::to_string(flags);
        }
        if (!value.empty()) defaults.push_back({definition.key, value});
    }
    return defaults;
}

namespace {
QString formatCameraVec3(const hammer::vmf::Vec3& value)
{
    // Matches CChunkFile::WriteKeyValueVector3's "x y z" layout used by
    // Camera3D::SaveVMF for the "position"/"look" keys.
    return QStringLiteral("%1 %2 %3").arg(value.x, 0, 'g', 8).arg(value.y, 0, 'g', 8).arg(value.z, 0, 'g', 8);
}

bool parseCameraVec3(const std::string* text, hammer::vmf::Vec3& value)
{
    if (!text) return false;
    std::string cleaned(*text);
    for (char& ch : cleaned) {
        if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ',') ch = ' ';
    }
    std::istringstream stream(cleaned);
    double x = 0.0, y = 0.0, z = 0.0;
    if (!(stream >> x >> y >> z)) return false;
    value = {x, y, z};
    return true;
}
}

void MapDocumentWidget::pushCamerasToViews()
{
    for (MapViewWidget* view : views_) view->setCameras(cameras_, activeCamera_);
    if (activeCamera_ >= 0 && activeCamera_ < static_cast<int>(cameras_.size())) {
        views_[0]->setCameraTransform(cameras_[activeCamera_].eye, cameras_[activeCamera_].lookAt);
    }
}

void MapDocumentWidget::loadCamerasFromDocument()
{
    // Camera3D::LoadVMF: read the "cameras" root block's activecamera key and
    // its child "camera" blocks' position/look keys.
    cameras_.clear();
    activeCamera_ = -1;
    if (const hammer::vmf::Block* block = editor_.document().firstRoot("cameras")) {
        if (const std::string* active = block->value("activecamera")) activeCamera_ = std::atoi(active->c_str());
        for (const hammer::vmf::Block* camera : block->children("camera")) {
            hammer::vmf::CameraDef def;
            parseCameraVec3(camera->value("position"), def.eye);
            parseCameraVec3(camera->value("look"), def.lookAt);
            cameras_.push_back(def);
        }
        if (cameras_.empty()) activeCamera_ = -1;
        else if (activeCamera_ < 0 || activeCamera_ >= static_cast<int>(cameras_.size())) activeCamera_ = 0;
    }
    pushCamerasToViews();
}

void MapDocumentWidget::saveCamerasToDocument()
{
    // Camera3D::SaveVMF: rewrite the whole "cameras" block from the current
    // in-memory camera list so it round-trips through save/load.
    hammer::vmf::Block rebuilt("cameras");
    rebuilt.setValue("activecamera", std::to_string(activeCamera_));
    for (const hammer::vmf::CameraDef& camera : cameras_) {
        hammer::vmf::Block& child = rebuilt.appendChild("camera");
        child.setValue("position", formatCameraVec3(camera.eye).toStdString());
        child.setValue("look", formatCameraVec3(camera.lookAt).toStdString());
    }

    std::vector<hammer::vmf::Block>& roots = editor_.document().roots();
    const auto it = std::find_if(roots.begin(), roots.end(), [](const hammer::vmf::Block& root) {
        return root.name == "cameras";
    });
    if (it != roots.end()) *it = std::move(rebuilt);
    else roots.push_back(std::move(rebuilt));
    editor_.document().markDirty();
}

void MapDocumentWidget::editCamera(int index, const hammer::vmf::Vec3& eye,
                                   const hammer::vmf::Vec3& lookAt, bool created)
{
    if (created && index == static_cast<int>(cameras_.size())) cameras_.push_back({});
    if (index < 0 || index >= static_cast<int>(cameras_.size())) return;
    cameras_[index] = {eye, lookAt};
    activeCamera_ = index;
    editor_.document().markDirty();
    pushCamerasToViews();
    notifyDocumentState(tr("Moved camera %1").arg(index + 1));
}

void MapDocumentWidget::cycleActiveCamera(bool forward)
{
    // Camera3D::SetNextCamera(sncNext/sncPrev).
    if (cameras_.empty()) { activeCamera_ = -1; return; }
    if (forward) {
        ++activeCamera_;
        if (activeCamera_ >= static_cast<int>(cameras_.size())) activeCamera_ = 0;
    } else {
        --activeCamera_;
        if (activeCamera_ < 0) activeCamera_ = static_cast<int>(cameras_.size()) - 1;
    }
    pushCamerasToViews();
    notifyDocumentState(tr("Active camera %1 of %2").arg(activeCamera_ + 1).arg(cameras_.size()));
}

void MapDocumentWidget::deleteActiveCamera()
{
    // Camera3D::DeleteActiveCamera.
    if (activeCamera_ < 0 || activeCamera_ >= static_cast<int>(cameras_.size())) return;
    cameras_.erase(cameras_.begin() + activeCamera_);
    if (activeCamera_ >= static_cast<int>(cameras_.size())) activeCamera_ = static_cast<int>(cameras_.size()) - 1;
    editor_.document().markDirty();
    pushCamerasToViews();
    notifyDocumentState(tr("Deleted camera"));
}
