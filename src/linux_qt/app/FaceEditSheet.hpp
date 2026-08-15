#pragma once

// Hammer's Face Edit sheet (hammer/faceeditsheet.cpp, IDD_FACEEDIT), reduced to
// the Material page that the Texture Application tool puts on screen.
//
// The original is a CPropertySheet floating over the main frame with a
// "Material" and a "Displacement" page; both are ported here, alongside a
// Smoothing Groups page that stands in for the original's separate dialog.

#include "MaterialSystem.hpp"
#include "VmfDisplacement.hpp"
#include "VmfFaceEdit.hpp"

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

// CFaceEditMaterialPage::UpdateDialogData's result: the values shared by every
// face in the face list. A field with no value is one the selected faces
// disagree about, and is shown blank exactly as the original leaves it.
struct FaceEditValues
{
    int faceCount{0};
    std::optional<double> shiftX;
    std::optional<double> shiftY;
    std::optional<double> scaleX;
    std::optional<double> scaleY;
    std::optional<double> rotation;
    std::optional<int> lightmapScale;
    QString material;
    // Smoothing group membership across the face list: a bit set in
    // smoothingAll is a group every selected face is in, a bit set only in
    // smoothingAny is one they disagree about (the page's partial state).
    std::uint32_t smoothingAll{0};
    std::uint32_t smoothingAny{0};
    // CFaceEditDispPage::UpdateDialogData's "bAllDisps": how many of the faces
    // in the list already carry a dispinfo chunk.
    int displacementCount{0};
};

// CFaceEditDispPage's tool enum (faceedit_disppage.h FACEEDITTOOL_*), in the
// same order. Create and Destroy act immediately and drop back to Select, as
// the original's handlers do.
enum class DisplacementTool
{
    Select = 0,
    Create,
    Destroy,
    PaintGeometry,
    PaintAlpha,
    Sew,
    Subdivide,
    Noise
};

// CDispPaintDistDlg's state, which CToolDisplace reads at paint time.
struct DisplacementPaintSettings
{
    double distance{hammer::vmf::DefaultPaintDistance};
    double radius{hammer::vmf::DefaultPaintRadius};
    hammer::vmf::PaintBrushType brush{hammer::vmf::PaintBrushType::Soft};
    hammer::vmf::PaintAxis axis{hammer::vmf::PaintAxis::Face};
};

class FaceEditSheet final : public QDialog
{
    Q_OBJECT

public:
    // CFaceEditSheet's click modes, in the order the original's Mode combo
    // lists them (faceeditsheet.h id_SwitchModeStart..id_SwitchModeEnd).
    enum class ClickMode { LiftSelect, Lift, Select, Apply, ApplyAll, AlignToView };

    explicit FaceEditSheet(QWidget* parent = nullptr);

    void setMaterialSystem(std::shared_ptr<hammer::assets::MaterialSystem> materials);
    void setMaterialNames(const QStringList& names);

    ClickMode clickMode() const;
    bool hideMask() const;
    bool treatAsOne() const;

    QString currentMaterial() const;
    void setCurrentMaterial(const QString& material);

    DisplacementTool displacementTool() const { return displacementTool_; }
    DisplacementPaintSettings displacementPaintSettings() const;

    // Pushes the face list's values into the fields.
    void setFaceValues(const FaceEditValues& values);

    // CFaceEditDispPage::UpdateDialogData's Attributes fields, filled from the
    // displacements the face list actually holds.
    void setDisplacementAttributes(std::optional<int> power, std::optional<double> elevation);

    // Keeps the sheet's lightmap grid checkbox in step with the View menu.
    void setLightmapGridVisible(bool visible);

    // CFaceEditMaterialPage::Apply's dialog read-back. Blank fields stay unset
    // and are therefore left alone on every face (the NOT_INIT sentinel).
    hammer::vmf::FaceTextureEdit currentEdit(bool includeMaterial) const;

signals:
    // ID_FACEEDIT_APPLY.
    void applyRequested();
    // A mapping field was committed. Applies to the selected faces immediately,
    // which is what the original's spin controls and Apply button both end in.
    void mappingEdited();
    void justifyRequested(int justification); // hammer::vmf::TextureJustification
    void alignRequested(int alignment);       // hammer::vmf::TextureAlignment
    void hideMaskChanged(bool hidden);
    // IDC_MODE: CFaceEditSheet::SetClickMode.
    void clickModeChanged(int mode);
    void materialChanged(const QString& material);
    void browseRequested();
    // IDC_REPLACE: CFaceEditMaterialPage::OnReplace opens the Replace Textures
    // dialog (hammer/replacetexdlg.cpp).
    void replaceRequested();
    void sheetClosed();

    // Smoothing Groups page (hammer/resource.h ID_SMOOTHING_GROUP_1..32).
    // A group button was clicked: add the selected faces to the group, or
    // remove them from it (CSmoothingGroupMgr::AddFaceToGroup /
    // RemoveFaceFromGroup).
    void smoothingGroupToggled(int group, bool add);
    // The group whose faces the 3D view tints (IDD_SMOOTHING_GROUP_VISUAL).
    // 0 means "no group".
    void smoothingVisualGroupChanged(int group);
    // Put every face of the group into the face selection.
    void smoothingGroupSelectRequested(int group);

    // Displacement page (hammer/faceedit_disppage.cpp).
    void displacementToolChanged(int tool); // DisplacementTool
    void displacementCreateRequested(int power);
    void displacementDestroyRequested();
    void displacementPaintSettingsChanged();
    // ID_DISP_APPLY: the Attributes group's power / elevation / scale.
    // Unset fields are left alone on every face (see the note on
    // EditorModel::DisplacementAttributeEdit).
    void displacementAttributesApplied(std::optional<int> power,
                                       std::optional<double> elevation,
                                       std::optional<double> scale, double previousScale);
    // ID_DISP_NOISE, after CDispNoiseDlg returned min/max.
    void displacementNoiseRequested(double minimum, double maximum);
    // ID_DISP_SEW.
    void displacementSewRequested();

    // Lightmap page. Applies the lightmap scale to the face list, and toggles
    // the 3D lightmap grid view (ID_VIEW_3DLIGHTMAP_GRID).
    void lightmapScaleApplied(int scale);
    void lightmapGridToggled(bool visible);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void updatePreview();
    void emitMappingEdited();
    void buildSmoothingPage(QWidget* page);
    void buildDisplacementPage(QWidget* page);
    void buildLightmapPage(QWidget* page);
    void setDisplacementTool(DisplacementTool tool);
    void updateDisplacementControls();
    void updateSmoothingButtons();
    int visualGroup() const;

    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    bool updating_{false};

    // One arrow click on a mapping field's spinner: steps, clamps, applies.
    void spinField(QLineEdit* edit, double delta, double minimum, double maximum, int decimals);
    QLineEdit* scaleX_{nullptr};
    QLineEdit* scaleY_{nullptr};
    QLineEdit* shiftX_{nullptr};
    QLineEdit* shiftY_{nullptr};
    QLineEdit* rotation_{nullptr};
    QLineEdit* lightmapScale_{nullptr};
    QCheckBox* treatAsOne_{nullptr};
    QCheckBox* hideMask_{nullptr};
    QComboBox* mode_{nullptr};
    QComboBox* textureGroup_{nullptr};
    QComboBox* textureName_{nullptr};
    QLabel* preview_{nullptr};
    QLabel* faceCountLabel_{nullptr};
    QPushButton* apply_{nullptr};
    QStringList materialNames_;

    QVector<QToolButton*> smoothingButtons_;
    QSpinBox* visualGroup_{nullptr};
    QLabel* smoothingSummary_{nullptr};
    std::uint32_t smoothingAll_{0};
    std::uint32_t smoothingAny_{0};
    int smoothingFaceCount_{0};

    // Displacement page.
    DisplacementTool displacementTool_{DisplacementTool::Select};
    int displacementFaceCount_{0};
    int displacementDispCount_{0};
    QVector<QToolButton*> displacementToolButtons_;
    QSpinBox* displacementPower_{nullptr};
    QLineEdit* displacementElevation_{nullptr};
    QPushButton* displacementApply_{nullptr};
    QComboBox* paintAxis_{nullptr};
    QComboBox* paintBrush_{nullptr};
    QSpinBox* paintDistance_{nullptr};
    QSpinBox* paintRadius_{nullptr};
    QLabel* displacementSummary_{nullptr};
    QWidget* paintGroup_{nullptr};
    QLineEdit* displacementScale_{nullptr};
    // CMapDisp::m_Scale is not part of the VMF dispinfo chunk, so the sheet
    // holds the scale the surfaces were last set to, exactly as the original's
    // edit box is the only place the value lives between Applies.
    double displacementPreviousScale_{1.0};

    // Lightmap page.
    QLineEdit* lightmapPageScale_{nullptr};
    QCheckBox* lightmapGrid_{nullptr};
    QLabel* lightmapSummary_{nullptr};
};
