#pragma once

#include "FgdDatabase.hpp"
#include "VmfEditor.hpp"

#include <QDialog>
#include <QSize>
#include <QString>
#include <QWidget>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

// The angle control of hammer/anglebox.cpp (IDC_ANGLEBOX): a circle with a
// line showing yaw, dragged with the mouse. Pitch and roll are cleared by a
// drag, exactly as CAngleBox::OnMouseMove does, and the line is hidden
// whenever the angles cannot be drawn as a yaw (a pitch of +/-90 for
// Up/Down, or a non-normalized yaw).
class HammerAngleBox final : public QWidget
{
    Q_OBJECT

public:
    explicit HammerAngleBox(QWidget* parent = nullptr);

    // "pitch yaw roll", the raw form of an "angles" keyvalue.
    QString angles() const;
    void setAngles(const QString& text);
    // The companion combo's text: "Up", "Down" or the yaw (CAngleBox::
    // GetAngleEditText).
    QString editText() const;
    // The angles a combo entry stands for; empty when the text is neither a
    // number nor Up/Down.
    static QString anglesForEditText(const QString& text);

    QSize sizeHint() const override { return {34, 34}; }

signals:
    void anglesChanged(const QString& angles);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void setYawFromPoint(const QPointF& point);

    double pitch_{0.0};
    double yaw_{0.0};
    double roll_{0.0};
};

// Object Properties (hammer/objectproperties.cpp + op_entity.cpp). The layout
// follows IDD_OBJPAGE_ENTITYKV: a class row with a SmartEdit push-toggle above
// a keyvalue list on the left and the key/value editor, help text and comments
// on the right, with the Flags, Outputs and Inputs pages of IDD_OBJPAGE_FLAGS /
// _OUTPUT / _INPUT beside it.
class ObjectPropertiesDialog final : public QDialog
{
    Q_OBJECT

public:
    // A per-property editor widget, built by the caller so the model and
    // sequence browsers stay where their helpers already live. The widget
    // commits through the store callbacks it is handed.
    using EditorFactory = std::function<QWidget*(QWidget* parent, QDialog* dialog,
                                                 const QString& key, const QString& value,
                                                 const std::function<void(const QString&, const QString&)>& setValue,
                                                 const std::function<QString(const QString&)>& readValue)>;

    struct Setup
    {
        hammer::vmf::ObjectRef object{};
        // "Solid" / "Entity", for the window title.
        QString typeName;
        // A brush entity can only be re-classed to another solid class, and a
        // point entity only to another point class.
        bool brushEntity{false};
        // Map Properties: the worldspawn block, which has a fixed class and no
        // entity IO but is otherwise edited exactly like an entity.
        bool world{false};
        // Replaces the "Object Properties - <type> #<id>" title when set.
        QString windowTitle;
        std::vector<hammer::vmf::Property> properties;
        std::vector<hammer::vmf::EditorModel::EntityConnection> connections;
        std::shared_ptr<hammer::fgd::Database> fgd;
        // Read for the target-name list of the Outputs page and the incoming
        // connections of the Inputs page. Must outlive the dialog.
        const hammer::vmf::Document* document{nullptr};
        EditorFactory modelEditorFactory;
        EditorFactory sequenceEditorFactory;
        // worldspawn's "skyname": the Skybox Browser.
        EditorFactory skyEditorFactory;

        // --- VisGroup page (hammer/op_groups.cpp) --------------------------
        // The map's user visgroups as (id, name). Empty means no page: a map
        // with no visgroups has nothing to tick.
        std::vector<std::pair<int, QString>> visGroups;
        // The ids EVERY selected object belongs to. With "mixed" set, the
        // selection disagrees about the rest, which the page shows as
        // partially checked boxes.
        std::vector<int> memberVisGroups;
        bool visGroupsMixed{false};
        // Applied immediately, as the original's page does - the VisGroup page
        // is not gated behind Apply.
        std::function<void(int visGroupId, bool member)> setVisGroupMembership;

        // Which page opens first. A double-click on a plain world brush goes
        // straight to VisGroups, because membership is the only thing a solid
        // with no entity class has worth editing there.
        enum class Page { Default, VisGroups };
        Page initialPage{Page::Default};
    };

    ObjectPropertiesDialog(Setup setup, QWidget* parent = nullptr);

signals:
    // The Apply button (CObjectProperties's IDC_APPLY): the caller commits the
    // current keyvalues and connections without the dialog closing.
    void applyRequested();

public:
    // Called by the caller once it has committed an Apply, so the button goes
    // back to disabled until something else is edited.
    void markApplied();

    // The edited keyvalues and outbound connections, valid after accept().
    const std::vector<hammer::vmf::Property>& properties() const { return properties_; }
    std::vector<hammer::vmf::EditorModel::EntityConnection> connections() const;
    bool isEntity() const;

private:
    // True where the FGD drives the page: entities and worldspawn, but not a
    // bare solid, which is always edited raw.
    bool hasClassData() const;

    void buildClassRow(QVBoxLayout* layout);
    QWidget* buildKeyvaluesPage();
    QWidget* buildVisGroupPage();
    QWidget* buildFlagsPage();
    void rebuildFlagsPage();
    QWidget* buildOutputsPage();
    QWidget* buildInputsPage();
    void fillInputsList(QTreeWidget* table);

    // The keyvalue store. Everything - the class combo, both list modes, the
    // flags page and the value editors - reads and writes these, and the order
    // is the document's so saving does not shuffle the VMF.
    QString value(const QString& key) const;
    void setValue(const QString& key, const QString& value);
    void removeKey(const QString& key);

    void reloadKeyvalueList(const QString& keyToSelect = {});
    void showKeyvalue(QTreeWidgetItem* item);
    // The SmartEdit editor for one FGD property, or a plain line edit when the
    // key has no definition (or SmartEdit is off).
    QWidget* makeValueEditor(const hammer::fgd::PropertyDefinition* definition,
                             const QString& key, const QString& valueText);
    void setSmartEdit(bool smart);
    void updateListRow(const QString& key, const QString& valueText);
    // Color rows carry a swatch of their value in the list.
    void decorateListRow(QTreeWidgetItem* item, const QString& key, const QString& valueText);

    const hammer::fgd::PropertyDefinition* definitionFor(const QString& key) const;
    QString classname() const;
    QStringList targetNames() const;
    QString classOfTarget(const QString& targetName) const;
    QStringList inputNamesFor(const QString& targetClass) const;

    // Keeps the header angle control in step with the "angles" keyvalue.
    void pushAnglesToControls();
    void setAnglesEnabled();

    // Enables Apply only while there is something uncommitted, which is how
    // the original property sheet drives it.
    void refreshApplyState();

    void reloadOutputRow(QTreeWidgetItem* item);
    void commitOutputEdits();
    void showOutput(QTreeWidgetItem* item);

    Setup setup_;
    std::vector<hammer::vmf::Property> properties_;
    std::vector<hammer::fgd::PropertyDefinition> definitions_;

    // The last state handed to the caller, for the Apply button's enabled state.
    std::vector<hammer::vmf::Property> appliedProperties_;
    std::vector<hammer::vmf::EditorModel::EntityConnection> appliedConnections_;
    QPushButton* applyButton_{nullptr};

    bool smartEdit_{true};
    QTabWidget* tabs_{nullptr};
    QComboBox* classCombo_{nullptr};
    QPushButton* smartEditButton_{nullptr};

    HammerAngleBox* angleBox_{nullptr};
    QComboBox* angleCombo_{nullptr};
    // Guards the angles keyvalue <-> angle control round trip.
    bool loadingAngles_{false};

    QTreeWidget* keyvalueList_{nullptr};
    QLabel* keyLabel_{nullptr};
    QLineEdit* keyEdit_{nullptr};
    QGroupBox* editorBox_{nullptr};
    QWidget* valueEditor_{nullptr};
    QVBoxLayout* valueEditorLayout_{nullptr};
    QPlainTextEdit* helpText_{nullptr};
    QPushButton* addButton_{nullptr};
    QPushButton* removeButton_{nullptr};
    QLineEdit* comments_{nullptr};
    // Guards the list -> editor -> list round trip while a row is being loaded.
    bool loadingKeyvalue_{false};

    QWidget* flagsPage_{nullptr};
    QString spawnFlagsKey_;
    int unknownSpawnFlags_{0};
    std::vector<std::pair<QCheckBox*, int>> spawnFlagChecks_;
    QLabel* spawnFlagsValue_{nullptr};

    QTreeWidget* outputsList_{nullptr};
    QComboBox* outputName_{nullptr};
    QComboBox* outputTarget_{nullptr};
    QComboBox* outputInput_{nullptr};
    QLineEdit* outputParameter_{nullptr};
    QLineEdit* outputDelay_{nullptr};
    QCheckBox* outputOnce_{nullptr};
    QGroupBox* outputPanel_{nullptr};
    bool loadingOutput_{false};
};
