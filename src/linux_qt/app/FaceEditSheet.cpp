#include "FaceEditSheet.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// The original's Justify block is a 3x2 grid of bitmap buttons plus a "Fit"
// button and the "Treat as one" checkbox underneath.
struct JustifyButton
{
    const char* text;
    hammer::vmf::TextureJustification justification;
};

const JustifyButton JustifyButtons[] = {
    {"L", hammer::vmf::TextureJustification::Left},
    {"C", hammer::vmf::TextureJustification::Center},
    {"R", hammer::vmf::TextureJustification::Right},
    {"T", hammer::vmf::TextureJustification::Top},
    {"B", hammer::vmf::TextureJustification::Bottom},
    {"Fit", hammer::vmf::TextureJustification::Fit},
};

const char* const JustifyToolTips[] = {
    "Justify left", "Justify center", "Justify right",
    "Justify top", "Justify bottom", "Fit to face",
};

QString formatValue(const std::optional<double>& value)
{
    if (!value) return {};
    QString text = QString::number(*value, 'f', 4);
    while (text.contains('.') && (text.endsWith('0'))) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}

std::optional<double> readValue(const QLineEdit* edit)
{
    const QString text = edit->text().trimmed();
    if (text.isEmpty()) return std::nullopt;
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok) return std::nullopt;
    return value;
}

std::optional<int> readInteger(const QLineEdit* edit)
{
    const QString text = edit->text().trimmed();
    if (text.isEmpty()) return std::nullopt;
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok) return std::nullopt;
    return value;
}

QPixmap previewPixmap(const hammer::assets::Image& image, const QSize& size)
{
    if (!image.valid()) {
        QPixmap empty(size);
        empty.fill(QColor(48, 48, 48));
        return empty;
    }
    const QImage wrapped(reinterpret_cast<const uchar*>(image.pixels.data()),
                         image.width, image.height,
                         image.width * static_cast<int>(sizeof(std::uint32_t)),
                         QImage::Format_ARGB32);
    return QPixmap::fromImage(wrapped.copy()).scaled(size, Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation);
}

} // namespace

FaceEditSheet::FaceEditSheet(QWidget* parent)
    : QDialog(parent, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
{
    // CFaceEditSheet( "Face Edit Sheet", ... ) - the caption Hammer shows.
    setWindowTitle(tr("Face Edit Sheet"));
    setObjectName(QStringLiteral("FaceEditSheet"));

    auto* tabs = new QTabWidget(this);
    auto* material = new QWidget(tabs);
    auto* layout = new QVBoxLayout(material);

    // ---------------------------------------------------------------------
    // Texture group: preview, name, browse/replace, and the texture group
    // filter combo (IDC_TEXTUREPIC, IDC_TEXTURES, IDC_BROWSE, IDC_TEXTUREGROUPS).
    // ---------------------------------------------------------------------
    auto* textureGroupBox = new QGroupBox(tr("Texture"), material);
    auto* textureLayout = new QGridLayout(textureGroupBox);

    preview_ = new QLabel(textureGroupBox);
    preview_->setFixedSize(96, 96);
    preview_->setFrameShape(QFrame::StyledPanel);
    preview_->setAlignment(Qt::AlignCenter);
    textureLayout->addWidget(preview_, 0, 0, 3, 1);

    textureName_ = new QComboBox(textureGroupBox);
    textureName_->setEditable(true);
    textureName_->setMinimumWidth(220);
    textureLayout->addWidget(textureName_, 0, 1, 1, 2);

    auto* browse = new QPushButton(tr("Browse..."), textureGroupBox);
    textureLayout->addWidget(browse, 1, 1);
    // IDC_REPLACE. CFaceEditMaterialPage::OnReplace opens CReplaceTexDlg with
    // the page's current texture pre-filled as the "find" material; the port's
    // dialog reads the same current material from the main window.
    auto* replace = new QPushButton(tr("Replace..."), textureGroupBox);
    replace->setToolTip(tr("Replace textures throughout the map or the selection"));
    textureLayout->addWidget(replace, 1, 2);

    textureGroup_ = new QComboBox(textureGroupBox);
    textureGroup_->addItem(tr("All Textures"));
    textureLayout->addWidget(textureGroup_, 2, 1, 1, 2);

    layout->addWidget(textureGroupBox);

    // ---------------------------------------------------------------------
    // Mapping fields (IDC_SCALEX/Y, IDC_SHIFTX/Y, IDC_ROTATE, lightmap scale).
    // ---------------------------------------------------------------------
    auto* mappingBox = new QGroupBox(tr("Texture Mapping"), material);
    auto* mappingLayout = new QGridLayout(mappingBox);

    // Each field carries the increment arrows of its IDC_SPIN* buddy in the
    // original (hammer.rc + CFaceEditMaterialPage::OnInitDialog /
    // OnDeltaPosFloatSpin), and like the original's OnVScroll every arrow
    // click applies the mapping to the face list immediately.
    struct SpinSpec
    {
        double step;
        double minimum;
        double maximum;
        int decimals;  // %.2f for the float spinners, integer text otherwise
    };
    const auto makeField = [&](const QString& label, int row, int column, bool integer,
                               const SpinSpec& spin) {
        auto* edit = new QLineEdit(mappingBox);
        edit->setMaximumWidth(90);
        if (integer) edit->setValidator(new QIntValidator(1, 1024, edit));
        else edit->setValidator(new QDoubleValidator(edit));
        mappingLayout->addWidget(new QLabel(label, mappingBox), row, column * 3);
        mappingLayout->addWidget(edit, row, column * 3 + 1);
        connect(edit, &QLineEdit::editingFinished, this, &FaceEditSheet::emitMappingEdited);

        // The up/down arrow pair (msctls_updown32's two halves), stacked.
        auto* arrows = new QWidget(mappingBox);
        auto* arrowLayout = new QVBoxLayout(arrows);
        arrowLayout->setContentsMargins(0, 0, 0, 0);
        arrowLayout->setSpacing(0);
        const auto makeArrow = [&](Qt::ArrowType direction, int delta) {
            auto* button = new QToolButton(arrows);
            button->setArrowType(direction);
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(400);
            button->setAutoRepeatInterval(60);
            button->setFixedSize(16, 11);
            button->setFocusPolicy(Qt::NoFocus);
            connect(button, &QToolButton::clicked, this, [this, edit, spin, delta] {
                spinField(edit, spin.step * delta, spin.minimum, spin.maximum, spin.decimals);
            });
            arrowLayout->addWidget(button);
            return button;
        };
        makeArrow(Qt::UpArrow, +1);
        makeArrow(Qt::DownArrow, -1);
        mappingLayout->addWidget(arrows, row, column * 3 + 2);
        return edit;
    };

    // Ranges from OnInitDialog's UDM_SETRANGE calls; steps from the spin
    // style (UDS_SETBUDDYINT integer buddies step 1) and OnDeltaPosFloatSpin
    // (scale steps 0.1f, "%.2f"). Shift's +/-MAX_TEXTUREWIDTH/HEIGHT is the
    // engine's 2048 texture cap.
    scaleX_ = makeField(tr("Scale X:"), 0, 0, false, {0.1, -32767.0, 32767.0, 2});
    scaleY_ = makeField(tr("Scale Y:"), 0, 1, false, {0.1, -32767.0, 32767.0, 2});
    shiftX_ = makeField(tr("Shift X:"), 1, 0, false, {1.0, -2048.0, 2048.0, 0});
    shiftY_ = makeField(tr("Shift Y:"), 1, 1, false, {1.0, -2048.0, 2048.0, 0});
    rotation_ = makeField(tr("Rotation:"), 2, 0, false, {1.0, -359.0, 359.0, 0});
    lightmapScale_ = makeField(tr("Lightmap Scale:"), 2, 1, true, {1.0, 1.0, 32767.0, 0});

    layout->addWidget(mappingBox);

    // ---------------------------------------------------------------------
    // Justify block (IDC_JUSTIFY_*, IDC_TREAT_AS_ONE) and the alignment
    // checkboxes (IDC_ALIGN_WORLD, IDC_ALIGN_FACE).
    // ---------------------------------------------------------------------
    auto* justifyBox = new QGroupBox(tr("Justify"), material);
    auto* justifyLayout = new QGridLayout(justifyBox);
    int index = 0;
    for (const JustifyButton& entry : JustifyButtons) {
        auto* button = new QPushButton(tr(entry.text), justifyBox);
        button->setFixedWidth(38);
        button->setToolTip(tr(JustifyToolTips[index]));
        justifyLayout->addWidget(button, index / 3, index % 3);
        const int justification = static_cast<int>(entry.justification);
        connect(button, &QPushButton::clicked, this,
                [this, justification] { emit justifyRequested(justification); });
        ++index;
    }
    treatAsOne_ = new QCheckBox(tr("Treat as one"), justifyBox);
    justifyLayout->addWidget(treatAsOne_, 2, 0, 1, 3);

    auto* alignBox = new QGroupBox(tr("Align"), material);
    auto* alignLayout = new QVBoxLayout(alignBox);
    auto* alignWorld = new QPushButton(tr("World"), alignBox);
    auto* alignFace = new QPushButton(tr("Face"), alignBox);
    alignLayout->addWidget(alignWorld);
    alignLayout->addWidget(alignFace);
    alignLayout->addStretch(1);
    connect(alignWorld, &QPushButton::clicked, this, [this] {
        emit alignRequested(static_cast<int>(hammer::vmf::TextureAlignment::World));
    });
    connect(alignFace, &QPushButton::clicked, this, [this] {
        emit alignRequested(static_cast<int>(hammer::vmf::TextureAlignment::Face));
    });

    auto* middleRow = new QHBoxLayout;
    middleRow->addWidget(justifyBox);
    middleRow->addWidget(alignBox);
    layout->addLayout(middleRow);

    // ---------------------------------------------------------------------
    // Mode combo (IDC_MODE), Hide Mask (IDC_HIDEMASK), Apply
    // (ID_FACEEDIT_APPLY), Smoothing Groups (ID_BUTTON_SMOOTHING_GROUPS).
    // ---------------------------------------------------------------------
    auto* modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel(tr("Mode:"), material));
    mode_ = new QComboBox(material);
    mode_->addItem(tr("Lift+Select"));
    mode_->addItem(tr("Lift"));
    mode_->addItem(tr("Select"));
    mode_->addItem(tr("Apply (texture only)"));
    mode_->addItem(tr("Apply (texture+values)"));
    mode_->addItem(tr("Align to View"));
    modeRow->addWidget(mode_, 1);
    hideMask_ = new QCheckBox(tr("Hide Mask"), material);
    modeRow->addWidget(hideMask_);
    layout->addLayout(modeRow);

    auto* buttonRow = new QHBoxLayout;
    faceCountLabel_ = new QLabel(tr("0 faces selected"), material);
    buttonRow->addWidget(faceCountLabel_, 1);
    auto* smoothing = new QPushButton(tr("Smoothing Groups"), material);
    smoothing->setToolTip(tr("Show the Smoothing Groups page"));
    buttonRow->addWidget(smoothing);
    apply_ = new QPushButton(tr("Apply"), material);
    buttonRow->addWidget(apply_);
    layout->addLayout(buttonRow);

    tabs->addTab(material, tr("Material"));

    // ---------------------------------------------------------------------
    // Smoothing Groups page.
    //
    // DEVIATION: the original reaches its smoothing groups UI through
    // ID_BUTTON_SMOOTHING_GROUPS on the Material page, which opens the
    // IDD_SMOOTHING_GROUPS dialog; its implementation is not in this tree
    // (hammer/resource.h carries the ids, hammer.rc has no such dialog and
    // there is no dialog class), so the port puts the same controls on a page
    // of the face edit sheet. The Smoothing Groups button on the Material page
    // switches to it, so the entry point still reads the same.
    // ---------------------------------------------------------------------
    auto* smoothingPage = new QWidget(tabs);
    buildSmoothingPage(smoothingPage);
    const int smoothingIndex = tabs->addTab(smoothingPage, tr("Smoothing Groups"));
    connect(smoothing, &QPushButton::clicked, this, [tabs, smoothingIndex] {
        tabs->setCurrentIndex(smoothingIndex);
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this, smoothingIndex](int index) {
        // Leaving the page drops the group tint, as closing the original's
        // dialog does.
        emit smoothingVisualGroupChanged(index == smoothingIndex ? visualGroup() : 0);
    });

    // ---------------------------------------------------------------------
    // Displacement page (IDD_FACEEDIT_DISP, hammer/faceedit_disppage.cpp).
    // ---------------------------------------------------------------------
    auto* displacement = new QWidget(tabs);
    buildDisplacementPage(displacement);
    const int displacementIndex = tabs->addTab(displacement, tr("Displacement"));
    connect(tabs, &QTabWidget::currentChanged, this, [this, displacementIndex](int index) {
        // OnKillActive drops back to the selection tool, which is also what
        // closing the paint dialogs does (CloseAllDialogs).
        if (index != displacementIndex && displacementTool_ != DisplacementTool::Select) {
            setDisplacementTool(DisplacementTool::Select);
        }
    });

    // ---------------------------------------------------------------------
    // Lightmap page.
    //
    // DEVIATION: the original has no lightmap page. Its lightmap controls are
    // IDC_LIGHTMAP_SCALE / IDC_SPIN_LIGHTMAP_SCALE on the Material page (which
    // this port already carries) and the ID_VIEW_3DLIGHTMAP_GRID draw mode on
    // the View menu; the "apply the lightmap scale only" behaviour is the
    // implicit ModeApplyLightmapScale that CToolMaterial::OnRMouseDown3D
    // selects while that draw mode is up. The page gathers those into one place
    // without changing what any of them do.
    // ---------------------------------------------------------------------
    auto* lightmap = new QWidget(tabs);
    buildLightmapPage(lightmap);
    tabs->addTab(lightmap, tr("Lightmap"));

    auto* sheetLayout = new QVBoxLayout(this);
    sheetLayout->addWidget(tabs);

    connect(apply_, &QPushButton::clicked, this, &FaceEditSheet::applyRequested);
    connect(browse, &QPushButton::clicked, this, &FaceEditSheet::browseRequested);
    connect(replace, &QPushButton::clicked, this, &FaceEditSheet::replaceRequested);
    connect(hideMask_, &QCheckBox::toggled, this, &FaceEditSheet::hideMaskChanged);
    connect(mode_, &QComboBox::currentIndexChanged, this, &FaceEditSheet::clickModeChanged);
    connect(textureName_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        updatePreview();
        if (!updating_) emit materialChanged(name);
    });
}

// The 32 numbered group buttons (ID_SMOOTHING_GROUP_1..ID_SMOOTHING_GROUP_32),
// the group whose faces the 3D view tints, and a control that selects every
// face of that group.
void FaceEditSheet::buildSmoothingPage(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);

    auto* groupBox = new QGroupBox(tr("Groups"), page);
    auto* grid = new QGridLayout(groupBox);
    grid->setSpacing(2);
    smoothingButtons_.reserve(hammer::vmf::MaxSmoothingGroupCount);
    for (int group = 1; group <= hammer::vmf::MaxSmoothingGroupCount; ++group) {
        auto* button = new QToolButton(groupBox);
        button->setText(QString::number(group));
        button->setCheckable(true);
        button->setMinimumWidth(28);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(button, &QToolButton::clicked, this, [this, group, button](bool checked) {
            if (updating_) return;
            // The partial state has no "checked" of its own: clicking a
            // partial group adds every selected face to it, which is what
            // AddFaceToGroup does for the faces that are not in it yet.
            emit smoothingGroupToggled(group, checked);
            // Clicking a group is also how its faces get looked at.
            if (visualGroup_) visualGroup_->setValue(group);
        });
        smoothingButtons_.push_back(button);
        grid->addWidget(button, (group - 1) / 8, (group - 1) % 8);
    }
    layout->addWidget(groupBox);

    smoothingSummary_ = new QLabel(tr("0 faces selected"), page);
    layout->addWidget(smoothingSummary_);

    auto* visualRow = new QHBoxLayout;
    visualRow->addWidget(new QLabel(tr("Show group:"), page));
    visualGroup_ = new QSpinBox(page);
    visualGroup_->setRange(0, hammer::vmf::MaxSmoothingGroupCount);
    visualGroup_->setSpecialValueText(tr("None"));
    visualGroup_->setToolTip(tr("Tints every face in this group in the 3D view"));
    visualRow->addWidget(visualGroup_);
    auto* selectGroup = new QPushButton(tr("Select Faces in Group"), page);
    visualRow->addWidget(selectGroup, 1);
    layout->addLayout(visualRow);
    layout->addStretch(1);

    connect(visualGroup_, &QSpinBox::valueChanged, this, [this](int group) {
        if (updating_) return;
        emit smoothingVisualGroupChanged(group);
    });
    connect(selectGroup, &QPushButton::clicked, this, [this] {
        const int group = visualGroup();
        if (group > 0) emit smoothingGroupSelectRequested(group);
    });
}

int FaceEditSheet::visualGroup() const { return visualGroup_ ? visualGroup_->value() : 0; }

// The tri-state fill, matching UpdateDialogData's rule for the mapping fields:
// agreement shows the value, disagreement shows the indeterminate state.
void FaceEditSheet::updateSmoothingButtons()
{
    const bool wasUpdating = updating_;
    updating_ = true;
    for (int i = 0; i < smoothingButtons_.size(); ++i) {
        QToolButton* button = smoothingButtons_[i];
        const std::uint32_t bit = hammer::vmf::smoothingGroupBit(i + 1);
        const bool all = smoothingFaceCount_ > 0 && (smoothingAll_ & bit) != 0u;
        const bool any = (smoothingAny_ & bit) != 0u;
        button->setChecked(all);
        // Qt has no indeterminate push button; a partial group is drawn with a
        // trailing dot and says so in its tooltip.
        button->setText(any && !all ? QStringLiteral("%1·").arg(i + 1)
                                    : QString::number(i + 1));
        button->setToolTip(any && !all ? tr("Group %1: some selected faces").arg(i + 1)
                                       : tr("Group %1").arg(i + 1));
        button->setEnabled(smoothingFaceCount_ > 0);
    }
    if (smoothingSummary_) {
        smoothingSummary_->setText(tr("%1 faces selected").arg(smoothingFaceCount_));
    }
    updating_ = wasUpdating;
}

void FaceEditSheet::setMaterialSystem(std::shared_ptr<hammer::assets::MaterialSystem> materials)
{
    materials_ = std::move(materials);
    updatePreview();
}

void FaceEditSheet::setMaterialNames(const QStringList& names)
{
    materialNames_ = names;
    const QString current = textureName_->currentText();
    const bool wasUpdating = updating_;
    updating_ = true;
    textureName_->clear();
    textureName_->addItems(names);
    textureName_->setCurrentText(current);
    updating_ = wasUpdating;
    updatePreview();
}

FaceEditSheet::ClickMode FaceEditSheet::clickMode() const
{
    return static_cast<ClickMode>(mode_->currentIndex());
}

bool FaceEditSheet::hideMask() const { return hideMask_->isChecked(); }

bool FaceEditSheet::treatAsOne() const { return treatAsOne_->isChecked(); }

QString FaceEditSheet::currentMaterial() const { return textureName_->currentText().trimmed(); }

void FaceEditSheet::setCurrentMaterial(const QString& material)
{
    if (currentMaterial().compare(material, Qt::CaseInsensitive) == 0) return;
    const bool wasUpdating = updating_;
    updating_ = true;
    textureName_->setCurrentText(material);
    updating_ = wasUpdating;
    updatePreview();
}

void FaceEditSheet::setFaceValues(const FaceEditValues& values)
{
    updating_ = true;
    scaleX_->setText(formatValue(values.scaleX));
    scaleY_->setText(formatValue(values.scaleY));
    shiftX_->setText(formatValue(values.shiftX));
    shiftY_->setText(formatValue(values.shiftY));
    rotation_->setText(formatValue(values.rotation));
    lightmapScale_->setText(values.lightmapScale ? QString::number(*values.lightmapScale) : QString{});
    if (lightmapPageScale_) {
        lightmapPageScale_->setText(values.lightmapScale ? QString::number(*values.lightmapScale)
                                                         : QString{});
    }
    if (lightmapSummary_) {
        lightmapSummary_->setText(
            values.faceCount == 0
                ? tr("No faces selected.")
                : (values.lightmapScale
                       ? tr("%1 face(s) selected, lightmap scale %2.")
                             .arg(values.faceCount).arg(*values.lightmapScale)
                       : tr("%1 face(s) selected with differing lightmap scales.")
                             .arg(values.faceCount)));
    }
    // CToolMaterial::UpdateStatusBar writes exactly this string.
    faceCountLabel_->setText(tr("%1 faces selected").arg(values.faceCount));
    if (!values.material.isEmpty()) textureName_->setCurrentText(values.material);
    smoothingAll_ = values.smoothingAll;
    smoothingAny_ = values.smoothingAny;
    smoothingFaceCount_ = values.faceCount;
    displacementFaceCount_ = values.faceCount;
    displacementDispCount_ = values.displacementCount;
    updating_ = false;
    updateSmoothingButtons();
    updateDisplacementControls();
    updatePreview();
}

hammer::vmf::FaceTextureEdit FaceEditSheet::currentEdit(bool includeMaterial) const
{
    hammer::vmf::FaceTextureEdit edit;
    edit.shiftX = readValue(shiftX_);
    edit.shiftY = readValue(shiftY_);
    edit.scaleX = readValue(scaleX_);
    edit.scaleY = readValue(scaleY_);
    edit.rotation = readValue(rotation_);
    edit.lightmapScale = readInteger(lightmapScale_);
    if (includeMaterial) {
        const QString material = currentMaterial();
        if (!material.isEmpty()) edit.material = material.toStdString();
    }
    return edit;
}

void FaceEditSheet::emitMappingEdited()
{
    if (updating_) return;
    emit mappingEdited();
}

void FaceEditSheet::spinField(QLineEdit* edit, double delta, double minimum, double maximum,
                              int decimals)
{
    // A blank field (mixed values across the face list) starts from 0 for
    // shift/rotation and from the smallest legal value otherwise, which is
    // what atof("") gave the original.
    double value = readValue(edit).value_or(0.0);
    value = std::clamp(value + delta, minimum, maximum);
    if (decimals > 0) {
        edit->setText(QString::number(value, 'f', decimals));
    } else {
        edit->setText(QString::number(static_cast<long long>(std::llround(value))));
    }
    // OnVScroll: every spin click applies the mapping to the face list.
    emitMappingEdited();
}

void FaceEditSheet::updatePreview()
{
    if (!preview_) return;
    const QString name = currentMaterial();
    if (!materials_ || name.isEmpty()) {
        preview_->setPixmap(previewPixmap({}, preview_->size()));
        return;
    }
    const auto material = materials_->material(name.toStdString());
    preview_->setPixmap(previewPixmap(material ? material->image : hammer::assets::Image{},
                                      preview_->size()));
    if (material && material->image.valid()) {
        preview_->setToolTip(tr("%1\n%2 x %3").arg(name)
                                 .arg(material->image.width).arg(material->image.height));
    } else {
        preview_->setToolTip(name);
    }
}

void FaceEditSheet::closeEvent(QCloseEvent* event)
{
    // CFaceEditSheet::OnClose returns the editor to the Selection tool, and
    // CFaceEditDispPage::CloseAllDialogs drops the displacement paint tools.
    if (displacementTool_ != DisplacementTool::Select) {
        displacementTool_ = DisplacementTool::Select;
        updateDisplacementControls();
    }
    QDialog::closeEvent(event);
    emit sheetClosed();
}

// -----------------------------------------------------------------------------
// Displacement page (IDD_FACEEDIT_DISP / hammer/faceedit_disppage.cpp).
//
// The original's page is a Tools group of push-like check buttons, an
// Attributes group (power / elevation / scale + Apply), and a Masks group. The
// paint settings live in a separate floating dialog (IDD_DISP_PAINT_DIST) that
// OnButtonPaintGeo creates.
//
// DEVIATION: the port keeps the paint settings on the page itself instead of a
// second floating window, and shows/hides them with the paint tools, so the
// workflow is the same without a second top-level window over the 3D view.
// -----------------------------------------------------------------------------
namespace {

// IDD_DISP_CREATE: a power edit with a spin control, OK and Cancel. The power
// is clamped to [2..4] by CDispCreateDlg.
int askDisplacementPower(QWidget* parent, int initialPower)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Displacement Create"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QObject::tr("Power"), &dialog));
    auto* power = new QSpinBox(&dialog);
    power->setRange(hammer::vmf::MinDisplacementPower, hammer::vmf::MaxDisplacementPower);
    power->setValue(initialPower);
    row->addWidget(power);
    layout->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted ? power->value() : -1;
}

// IDD_DISP_NOISE / CDispNoiseDlg: a min and a max edit (both starting at 0.0),
// each with a spin control, plus OK and Cancel. OnButtonNoise treats IDCANCEL as
// "drop back to the selection tool and do nothing".
bool askDisplacementNoise(QWidget* parent, double& minimum, double& maximum)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Displacement Noise"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* grid = new QGridLayout;
    auto* minEdit = new QLineEdit(QStringLiteral("0.0"), &dialog);
    auto* maxEdit = new QLineEdit(QStringLiteral("0.0"), &dialog);
    minEdit->setValidator(new QDoubleValidator(minEdit));
    maxEdit->setValidator(new QDoubleValidator(maxEdit));
    grid->addWidget(new QLabel(QObject::tr("Min"), &dialog), 0, 0);
    grid->addWidget(minEdit, 0, 1);
    grid->addWidget(new QLabel(QObject::tr("Max"), &dialog), 1, 0);
    grid->addWidget(maxEdit, 1, 1);
    layout->addLayout(grid);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    minimum = minEdit->text().trimmed().toDouble();
    maximum = maxEdit->text().trimmed().toDouble();
    return true;
}

} // namespace

void FaceEditSheet::buildDisplacementPage(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);

    // Tools group, in the resource's button order.
    struct ToolButtonSpec
    {
        DisplacementTool tool;
        const char* text;
        bool implemented;
    };
    static const ToolButtonSpec specs[] = {
        {DisplacementTool::Select, QT_TR_NOOP("Select"), true},
        {DisplacementTool::PaintGeometry, QT_TR_NOOP("Paint Geometry"), true},
        {DisplacementTool::PaintAlpha, QT_TR_NOOP("Paint Alpha"), true},
        {DisplacementTool::Create, QT_TR_NOOP("Create"), true},
        {DisplacementTool::Destroy, QT_TR_NOOP("Destroy"), true},
        {DisplacementTool::Subdivide, QT_TR_NOOP("Subdivide"), false},
        {DisplacementTool::Noise, QT_TR_NOOP("Noise"), true},
        {DisplacementTool::Sew, QT_TR_NOOP("Sew"), true},
    };

    auto* toolsBox = new QGroupBox(tr("Tools"), page);
    auto* toolsGrid = new QGridLayout(toolsBox);
    int column = 0;
    int row = 0;
    for (const ToolButtonSpec& spec : specs) {
        auto* button = new QToolButton(toolsBox);
        button->setText(tr(spec.text));
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("displacementTool", static_cast<int>(spec.tool));
        button->setProperty("implemented", spec.implemented);
        if (!spec.implemented) {
            // CatmullClarkSubdivide (hammer/dispsubdiv.cpp, driven through
            // IWorldEditDispMgr) is not ported, so the button stays greyed.
            button->setToolTip(tr("Subdivide is not ported: it needs the "
                                  "Catmull-Clark subdivision manager."));
        }
        toolsGrid->addWidget(button, row, column);
        displacementToolButtons_.push_back(button);
        const DisplacementTool tool = spec.tool;
        connect(button, &QToolButton::clicked, this, [this, tool] {
            if (updating_) return;
            setDisplacementTool(tool);
        });
        if (++column == 4) {
            column = 0;
            ++row;
        }
    }
    layout->addWidget(toolsBox);

    // Attributes group (ID_DISP_POWER / ID_DISP_ELEVATION / ID_DISP_SCALE +
    // ID_DISP_APPLY). OnButtonApply pushes all three onto every displacement in
    // the face list: UpdatePower -> CMapDisp::Resample, UpdateElevation ->
    // Elevate, UpdateScale -> Scale.
    auto* attributes = new QGroupBox(tr("Attributes"), page);
    auto* attributesGrid = new QGridLayout(attributes);
    attributesGrid->addWidget(new QLabel(tr("Power"), attributes), 0, 0);
    displacementPower_ = new QSpinBox(attributes);
    displacementPower_->setRange(hammer::vmf::MinDisplacementPower, hammer::vmf::MaxDisplacementPower);
    displacementPower_->setValue(3);
    attributesGrid->addWidget(displacementPower_, 0, 1);
    attributesGrid->addWidget(new QLabel(tr("Elev"), attributes), 1, 0);
    displacementElevation_ = new QLineEdit(attributes);
    displacementElevation_->setValidator(new QDoubleValidator(displacementElevation_));
    attributesGrid->addWidget(displacementElevation_, 1, 1);
    attributesGrid->addWidget(new QLabel(tr("Scale"), attributes), 2, 0);
    displacementScale_ = new QLineEdit(attributes);
    displacementScale_->setValidator(new QDoubleValidator(displacementScale_));
    displacementScale_->setText(QStringLiteral("1"));
    attributesGrid->addWidget(displacementScale_, 2, 1);
    displacementApply_ = new QPushButton(tr("Apply"), attributes);
    attributesGrid->addWidget(displacementApply_, 1, 2, 2, 1);
    layout->addWidget(attributes);

    connect(displacementApply_, &QPushButton::clicked, this, [this] {
        std::optional<int> power;
        std::optional<double> elevation;
        std::optional<double> scale;
        // The original reads these boxes with atoi/atof, so an empty box is a
        // zero; the port keeps the sheet's blank-means-leave-alone rule instead
        // (see EditorModel::DisplacementAttributeEdit).
        if (displacementPower_ && displacementPower_->isEnabled()) {
            power = displacementPower_->value();
        }
        if (displacementElevation_) elevation = readValue(displacementElevation_);
        if (displacementScale_) scale = readValue(displacementScale_);
        const double previous = displacementPreviousScale_;
        if (scale) displacementPreviousScale_ = *scale;
        emit displacementAttributesApplied(power, elevation, scale, previous);
    });

    // The paint settings dialog, inlined (IDD_DISP_PAINT_DIST).
    auto* paint = new QGroupBox(tr("Paint"), page);
    paintGroup_ = paint;
    auto* paintGrid = new QGridLayout(paint);
    paintGrid->addWidget(new QLabel(tr("Axis"), paint), 0, 0);
    paintAxis_ = new QComboBox(paint);
    // CDispPaintDistDlg::InitComboBoxAxis, in its "defined" order.
    paintAxis_->addItems({tr("X-Axis"), tr("Y-Axis"), tr("Z-Axis"), tr("Subdiv Normal"),
                          tr("Face Normal")});
    paintAxis_->setCurrentIndex(static_cast<int>(hammer::vmf::PaintAxis::Face));
    // Subdiv painting needs subdivision normals the port does not keep.
    if (auto* model = qobject_cast<QStandardItemModel*>(paintAxis_->model())) {
        if (QStandardItem* item = model->item(static_cast<int>(hammer::vmf::PaintAxis::Subdiv))) {
            item->setEnabled(false);
        }
    }
    paintGrid->addWidget(paintAxis_, 0, 1);

    paintGrid->addWidget(new QLabel(tr("Brush"), paint), 1, 0);
    paintBrush_ = new QComboBox(paint);
    paintBrush_->addItems({tr("Soft-Edge"), tr("Hard-Edge")});
    paintGrid->addWidget(paintBrush_, 1, 1);

    paintGrid->addWidget(new QLabel(tr("Distance"), paint), 2, 0);
    paintDistance_ = new QSpinBox(paint);
    paintDistance_->setRange(static_cast<int>(hammer::vmf::PaintDistanceMin),
                             static_cast<int>(hammer::vmf::PaintDistanceMax));
    paintDistance_->setValue(static_cast<int>(hammer::vmf::DefaultPaintDistance));
    paintGrid->addWidget(paintDistance_, 2, 1);

    paintGrid->addWidget(new QLabel(tr("Radius"), paint), 3, 0);
    paintRadius_ = new QSpinBox(paint);
    paintRadius_->setRange(static_cast<int>(hammer::vmf::PaintRadiusMin),
                           static_cast<int>(hammer::vmf::PaintRadiusMax));
    paintRadius_->setSingleStep(static_cast<int>(hammer::vmf::PaintRadiusStep));
    paintRadius_->setValue(static_cast<int>(hammer::vmf::DefaultPaintRadius));
    paintGrid->addWidget(paintRadius_, 3, 1);
    layout->addWidget(paint);

    displacementSummary_ = new QLabel(page);
    displacementSummary_->setWordWrap(true);
    layout->addWidget(displacementSummary_);
    layout->addStretch(1);

    for (QComboBox* box : {paintAxis_, paintBrush_}) {
        connect(box, &QComboBox::currentIndexChanged, this,
                [this] { if (!updating_) emit displacementPaintSettingsChanged(); });
    }
    for (QSpinBox* box : {paintDistance_, paintRadius_}) {
        connect(box, &QSpinBox::valueChanged, this,
                [this] { if (!updating_) emit displacementPaintSettingsChanged(); });
    }

    updateDisplacementControls();
}

// -----------------------------------------------------------------------------
// Lightmap page.
//
// The original's lightmap controls, gathered: the lightmap scale edit + spin
// (IDC_LIGHTMAP_SCALE / IDC_SPIN_LIGHTMAP_SCALE on IDD_FACEEDIT_MATERIAL) and
// the 3D Lightmap Grid draw mode (ID_VIEW_3DLIGHTMAP_GRID on the View menu).
// While the grid is up, CToolMaterial::OnRMouseDown3D applies the lightmap
// scale only (ModeApplyLightmapScale), which the port reproduces on the
// document side.
// -----------------------------------------------------------------------------
void FaceEditSheet::buildLightmapPage(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);

    auto* scaleBox = new QGroupBox(tr("Lightmap Scale"), page);
    auto* scaleGrid = new QGridLayout(scaleBox);
    scaleGrid->addWidget(new QLabel(tr("Luxels per world unit:"), scaleBox), 0, 0);
    lightmapPageScale_ = new QLineEdit(scaleBox);
    lightmapPageScale_->setMaximumWidth(90);
    // CFaceEditMaterialPage::Apply clamps with max( nLightmapScale, 1 ).
    lightmapPageScale_->setValidator(new QIntValidator(1, 1024, lightmapPageScale_));
    scaleGrid->addWidget(lightmapPageScale_, 0, 1);
    auto* applyScale = new QPushButton(tr("Apply to Selected Faces"), scaleBox);
    scaleGrid->addWidget(applyScale, 0, 2);

    // DEVIATION: the original has no preset buttons (hammer.rc gives
    // IDC_LIGHTMAP_SCALE an edit box and a spin control only). These are a
    // convenience that write the same field; the powers of two are the values
    // vrad's lightmap allocator actually likes.
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel(tr("Presets:"), scaleBox));
    for (const int preset : {4, 8, 16, 32, 64}) {
        auto* button = new QPushButton(QString::number(preset), scaleBox);
        button->setFixedWidth(38);
        presetRow->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, preset] {
            if (lightmapPageScale_) lightmapPageScale_->setText(QString::number(preset));
            if (lightmapScale_) lightmapScale_->setText(QString::number(preset));
            emit lightmapScaleApplied(preset);
        });
    }
    presetRow->addStretch(1);
    scaleGrid->addLayout(presetRow, 1, 0, 1, 3);
    layout->addWidget(scaleBox);

    auto* viewBox = new QGroupBox(tr("View"), page);
    auto* viewLayout = new QVBoxLayout(viewBox);
    lightmapGrid_ = new QCheckBox(tr("3D Lightmap Grid"), viewBox);
    lightmapGrid_->setToolTip(tr("Draws each face's luxel grid in the 3D view. While it is on, "
                                 "right-clicking a face applies the lightmap scale only."));
    viewLayout->addWidget(lightmapGrid_);
    layout->addWidget(viewBox);

    lightmapSummary_ = new QLabel(page);
    lightmapSummary_->setWordWrap(true);
    layout->addWidget(lightmapSummary_);
    layout->addStretch(1);

    connect(applyScale, &QPushButton::clicked, this, [this] {
        const auto scale = readInteger(lightmapPageScale_);
        if (!scale || *scale < 1) return;
        if (lightmapScale_) lightmapScale_->setText(QString::number(*scale));
        emit lightmapScaleApplied(*scale);
    });
    connect(lightmapPageScale_, &QLineEdit::editingFinished, this, [this] {
        if (updating_) return;
        // Keep the Material page's field in step, since both write the same
        // CMapFace::texture.nLightmapScale.
        const auto scale = readInteger(lightmapPageScale_);
        if (scale && lightmapScale_) lightmapScale_->setText(QString::number(*scale));
    });
    connect(lightmapGrid_, &QCheckBox::toggled, this, [this](bool visible) {
        if (updating_) return;
        emit lightmapGridToggled(visible);
    });
}

void FaceEditSheet::setLightmapGridVisible(bool visible)
{
    if (!lightmapGrid_ || lightmapGrid_->isChecked() == visible) return;
    const bool wasUpdating = updating_;
    updating_ = true;
    lightmapGrid_->setChecked(visible);
    updating_ = wasUpdating;
}

void FaceEditSheet::setDisplacementAttributes(std::optional<int> power,
                                              std::optional<double> elevation)
{
    const bool wasUpdating = updating_;
    updating_ = true;
    if (displacementPower_ && power) displacementPower_->setValue(*power);
    if (displacementElevation_) displacementElevation_->setText(formatValue(elevation));
    updating_ = wasUpdating;
}

DisplacementPaintSettings FaceEditSheet::displacementPaintSettings() const
{
    DisplacementPaintSettings settings;
    if (paintDistance_) settings.distance = paintDistance_->value();
    if (paintRadius_) settings.radius = paintRadius_->value();
    if (paintBrush_) {
        settings.brush = paintBrush_->currentIndex() == 1 ? hammer::vmf::PaintBrushType::Hard
                                                          : hammer::vmf::PaintBrushType::Soft;
    }
    if (paintAxis_) settings.axis = static_cast<hammer::vmf::PaintAxis>(paintAxis_->currentIndex());
    return settings;
}

// CFaceEditDispPage::SetTool plus the immediate action OnButtonCreate /
// OnButtonDestroy take before dropping back to Select.
void FaceEditSheet::setDisplacementTool(DisplacementTool tool)
{
    if (tool == DisplacementTool::Create) {
        const int power = askDisplacementPower(this, displacementPower_ ? displacementPower_->value() : 3);
        // "if( m_CreateDlg.DoModal() == IDCANCEL ) { SetTool( FACEEDITTOOL_SELECT ); return; }"
        if (power > 0) {
            if (displacementPower_) displacementPower_->setValue(power);
            emit displacementCreateRequested(power);
        }
        displacementTool_ = DisplacementTool::Select;
        emit displacementToolChanged(static_cast<int>(displacementTool_));
        updateDisplacementControls();
        return;
    }
    // OnButtonNoise / OnButtonSew set their tool, act on the face list, and then
    // "reset selection tool" - so they behave as momentary commands too.
    if (tool == DisplacementTool::Noise) {
        double minimum = 0.0;
        double maximum = 0.0;
        if (askDisplacementNoise(this, minimum, maximum)) {
            emit displacementNoiseRequested(minimum, maximum);
        }
        displacementTool_ = DisplacementTool::Select;
        emit displacementToolChanged(static_cast<int>(displacementTool_));
        updateDisplacementControls();
        return;
    }
    if (tool == DisplacementTool::Sew) {
        emit displacementSewRequested();
        displacementTool_ = DisplacementTool::Select;
        emit displacementToolChanged(static_cast<int>(displacementTool_));
        updateDisplacementControls();
        return;
    }
    if (tool == DisplacementTool::Destroy) {
        emit displacementDestroyRequested();
        displacementTool_ = DisplacementTool::Select;
        emit displacementToolChanged(static_cast<int>(displacementTool_));
        updateDisplacementControls();
        return;
    }

    displacementTool_ = tool;
    emit displacementToolChanged(static_cast<int>(displacementTool_));
    updateDisplacementControls();
}

// CFaceEditDispPage::UpdateEditControls, button for button:
//   - Select is always enabled.
//   - No faces in the list: only Select.
//   - Not every face is a displacement: Create, Destroy, Sew.
//   - Every face is a displacement: Destroy, Paint Geo, Paint Alpha, Subdiv,
//     Sew, Noise - but not Create.
//   - The attributes are only live in the Select tool.
void FaceEditSheet::updateDisplacementControls()
{
    if (displacementToolButtons_.isEmpty()) return;

    const bool hasFace = displacementFaceCount_ > 0;
    const bool allDisps = hasFace && displacementDispCount_ == displacementFaceCount_;

    const bool wasUpdating = updating_;
    updating_ = true;
    for (QToolButton* button : displacementToolButtons_) {
        const auto tool = static_cast<DisplacementTool>(button->property("displacementTool").toInt());
        const bool implemented = button->property("implemented").toBool();
        bool enabled = false;
        switch (tool) {
        case DisplacementTool::Select:
            enabled = true;
            break;
        case DisplacementTool::Create:
            enabled = hasFace && !allDisps;
            break;
        case DisplacementTool::Destroy:
        case DisplacementTool::Sew:
            enabled = hasFace;
            break;
        case DisplacementTool::PaintGeometry:
        case DisplacementTool::PaintAlpha:
        case DisplacementTool::Subdivide:
        case DisplacementTool::Noise:
            enabled = allDisps;
            break;
        }
        button->setEnabled(enabled && implemented);
        button->setChecked(tool == displacementTool_);
    }

    // CFaceEditDispPage::UpdateEditControls: the attributes are only live in
    // the Select tool, and only when every face in the list is a displacement.
    const bool attributes = displacementTool_ == DisplacementTool::Select && allDisps;
    if (displacementPower_) displacementPower_->setEnabled(attributes);
    if (displacementElevation_) displacementElevation_->setEnabled(attributes);
    if (displacementScale_) displacementScale_->setEnabled(attributes);
    if (displacementApply_) displacementApply_->setEnabled(attributes);

    const bool painting = displacementTool_ == DisplacementTool::PaintGeometry ||
                          displacementTool_ == DisplacementTool::PaintAlpha;
    if (paintGroup_) paintGroup_->setEnabled(painting);

    if (displacementSummary_) {
        if (!hasFace) {
            displacementSummary_->setText(tr("No faces selected."));
        } else if (painting) {
            displacementSummary_->setText(
                tr("%1 displacement(s): click in the 3D view to raise, right-click or Ctrl-click "
                   "to lower.").arg(displacementDispCount_));
        } else {
            displacementSummary_->setText(tr("%1 of %2 face(s) are displacements.")
                                              .arg(displacementDispCount_)
                                              .arg(displacementFaceCount_));
        }
    }
    updating_ = wasUpdating;
}
