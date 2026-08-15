#include "ObjectPropertiesDialog.hpp"

#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>
#include <QtMath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

constexpr int kKeyRole = Qt::UserRole + 1;

int parseInteger(const QString& value, int fallback = 0)
{
    bool ok = false;
    const int parsed = value.trimmed().toInt(&ok);
    return ok ? parsed : fallback;
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

// A color keyvalue of "-1 -1 -1" is Source's "no color set" marker, drawn as a
// rainbow rather than as a black swatch.
bool isRainbowColor(const QStringList& tokens)
{
    if (tokens.size() < 3) return false;
    for (int index = 0; index < 3; ++index) {
        bool ok = false;
        if (tokens[index].toDouble(&ok) != -1.0 || !ok) return false;
    }
    return true;
}

QColor colorFromText(const QString& text, bool scaled)
{
    const QStringList tokens = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto component = [&tokens, scaled](int index) {
        if (index >= tokens.size()) return 255;
        return scaled ? static_cast<int>(std::lround(tokens[index].toDouble() * 255.0))
                      : parseInteger(tokens[index], 255);
    };
    return QColor(std::clamp(component(0), 0, 255), std::clamp(component(1), 0, 255),
                  std::clamp(component(2), 0, 255));
}

QPixmap colorSwatch(const QString& text, bool scaled, QSize size)
{
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    const QRectF bounds = QRectF(QPointF(0.5, 0.5), QSizeF(size.width() - 1, size.height() - 1));
    if (isRainbowColor(text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts))) {
        QLinearGradient gradient(bounds.topLeft(), bounds.topRight());
        for (int step = 0; step <= 6; ++step)
            gradient.setColorAt(step / 6.0, QColor::fromHsvF(step / 6.0 * 0.999, 0.9, 1.0));
        painter.setBrush(gradient);
    } else {
        painter.setBrush(colorFromText(text, scaled));
    }
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
    painter.drawRect(bounds);
    return pixmap;
}

// Entity IO is stored either comma separated or, in newer maps, separated by
// the 0x1b unit separator (hammer/entityconnection.cpp).
QStringList splitConnection(const std::string& value)
{
    const QChar unitSeparator(0x1b);
    const QString text = QString::fromStdString(value);
    QStringList fields = text.contains(unitSeparator) ? text.split(unitSeparator)
                                                      : text.split(QLatin1Char(','));
    while (fields.size() < 5) fields << QString();
    return fields;
}

} // namespace

// ------------------------------------------------------------ angle control

HammerAngleBox::HammerAngleBox(QWidget* parent) : QWidget(parent)
{
    setFixedSize(sizeHint());
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::ClickFocus);
}

QString HammerAngleBox::angles() const
{
    return QStringLiteral("%1 %2 %3")
        .arg(QString::number(pitch_, 'g', 6), QString::number(yaw_, 'g', 6),
             QString::number(roll_, 'g', 6));
}

void HammerAngleBox::setAngles(const QString& text)
{
    const auto parsed = parseTriple(text);
    pitch_ = parsed[0];
    // CAngleBox::SetAnglesInternal normalizes a negative yaw into [0, 360).
    yaw_ = parsed[1];
    while (yaw_ < 0.0) yaw_ += 360.0;
    roll_ = parsed[2];
    update();
}

QString HammerAngleBox::editText() const
{
    if (pitch_ == 90.0 && yaw_ == 0.0 && roll_ == 0.0) return QStringLiteral("Down");
    if (pitch_ == -90.0 && yaw_ == 0.0 && roll_ == 0.0) return QStringLiteral("Up");
    if (yaw_ >= 0.0) return QString::number(static_cast<int>(yaw_));
    return {};
}

QString HammerAngleBox::anglesForEditText(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return {};
    if (trimmed.compare(QStringLiteral("down"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("90 0 0");
    if (trimmed.compare(QStringLiteral("up"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("-90 0 0");
    bool ok = false;
    const int yaw = trimmed.toInt(&ok);
    if (!ok) return {};
    return QStringLiteral("0 %1 0").arg(yaw);
}

void HammerAngleBox::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF bounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    painter.setBrush(palette().brush(QPalette::Base));
    painter.drawEllipse(bounds);

    // CAngleBox::DrawAngleLine draws nothing when the angles are not a plain
    // yaw, which is how Up/Down read on the control.
    if (pitch_ != 0.0 || roll_ != 0.0 || yaw_ < 0.0 || yaw_ > 359.0) return;

    const QPointF center = bounds.center();
    const double radius = bounds.width() / 2.0 - 3.0;
    const double radians = qDegreesToRadians(yaw_);
    const QPointF tip(center.x() + std::cos(radians) * radius,
                      center.y() - std::sin(radians) * radius);
    painter.setPen(QPen(palette().color(QPalette::WindowText), 1.5));
    painter.drawLine(center, tip);
}

void HammerAngleBox::mousePressEvent(QMouseEvent* event)
{
    setYawFromPoint(event->position());
}

void HammerAngleBox::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton) setYawFromPoint(event->position());
}

void HammerAngleBox::setYawFromPoint(const QPointF& point)
{
    const QPointF center = QRectF(rect()).center();
    double yaw = qRadiansToDegrees(std::atan2(center.y() - point.y(), point.x() - center.x()));
    while (yaw < 0.0) yaw += 360.0;
    // Dragging sets a pure yaw, as CAngleBox::OnMouseMove does.
    pitch_ = 0.0;
    roll_ = 0.0;
    yaw_ = std::round(yaw);
    if (yaw_ >= 360.0) yaw_ = 0.0;
    update();
    emit anglesChanged(angles());
}

ObjectPropertiesDialog::ObjectPropertiesDialog(Setup setup, QWidget* parent)
    : QDialog(parent), setup_(std::move(setup)), properties_(setup_.properties)
{
    setWindowTitle(setup_.windowTitle.isEmpty()
                       ? tr("Object Properties - %1 #%2").arg(setup_.typeName).arg(setup_.object.id)
                       : setup_.windowTitle);
    resize(860, 600);

    auto* layout = new QVBoxLayout(this);
    buildClassRow(layout);

    if (setup_.fgd && !classname().isEmpty())
        definitions_ = setup_.fgd->effectiveProperties(classname().toUtf8().toStdString());

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildKeyvaluesPage(), tr("Class Info"));
    if (isEntity()) {
        tabs_->addTab(buildOutputsPage(), tr("Outputs"));
        tabs_->addTab(buildInputsPage(), tr("Inputs"));
    }
    int visGroupTabIndex = -1;
    if (QWidget* visGroupPage = buildVisGroupPage())
        visGroupTabIndex = tabs_->addTab(visGroupPage, tr("VisGroup"));
    // Flags sits last, after the IO pages.
    rebuildFlagsPage();
    // A map with no VisGroups has no page to open, in which case the dialog
    // falls back to its usual first tab rather than opening on nothing.
    if (setup_.initialPage == Setup::Page::VisGroups && visGroupTabIndex >= 0)
        tabs_->setCurrentIndex(visGroupTabIndex);
    layout->addWidget(tabs_, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    applyButton_ = buttons->button(QDialogButtonBox::Apply);
    connect(applyButton_, &QPushButton::clicked, this,
            [this] { emit applyRequested(); });

    reloadKeyvalueList();
    // Whatever the pages materialized while building (spawnflag defaults and
    // the like) counts as the starting state, not as an unapplied edit.
    markApplied();
}

void ObjectPropertiesDialog::markApplied()
{
    appliedProperties_ = properties_;
    appliedConnections_ = connections();
    refreshApplyState();
}

void ObjectPropertiesDialog::refreshApplyState()
{
    if (!applyButton_) return;
    applyButton_->setEnabled(properties_ != appliedProperties_ ||
                             connections() != appliedConnections_);
}

bool ObjectPropertiesDialog::isEntity() const
{
    return !setup_.world && setup_.object.type == hammer::vmf::ObjectType::Entity;
}

bool ObjectPropertiesDialog::hasClassData() const
{
    return isEntity() || setup_.world;
}

QString ObjectPropertiesDialog::classname() const
{
    const QString stored = value(QStringLiteral("classname"));
    if (!stored.isEmpty()) return stored;
    return setup_.world ? QStringLiteral("worldspawn") : QString{};
}

QString ObjectPropertiesDialog::value(const QString& key) const
{
    for (const auto& property : properties_) {
        if (QString::fromStdString(property.key).compare(key, Qt::CaseInsensitive) == 0)
            return QString::fromStdString(property.value);
    }
    return {};
}

void ObjectPropertiesDialog::setValue(const QString& key, const QString& valueText)
{
    for (auto& property : properties_) {
        if (QString::fromStdString(property.key).compare(key, Qt::CaseInsensitive) == 0) {
            property.value = valueText.toUtf8().toStdString();
            updateListRow(key, valueText);
            if (key.compare(QStringLiteral("angles"), Qt::CaseInsensitive) == 0)
                pushAnglesToControls();
            refreshApplyState();
            return;
        }
    }
    properties_.push_back({key.toUtf8().toStdString(), valueText.toUtf8().toStdString()});
    updateListRow(key, valueText);
    if (key.compare(QStringLiteral("angles"), Qt::CaseInsensitive) == 0)
        pushAnglesToControls();
    refreshApplyState();
}

void ObjectPropertiesDialog::removeKey(const QString& key)
{
    properties_.erase(std::remove_if(properties_.begin(), properties_.end(),
                                     [&key](const hammer::vmf::Property& property) {
                                         return QString::fromStdString(property.key)
                                                    .compare(key, Qt::CaseInsensitive) == 0;
                                     }),
                      properties_.end());
    refreshApplyState();
}

const hammer::fgd::PropertyDefinition* ObjectPropertiesDialog::definitionFor(const QString& key) const
{
    for (const auto& definition : definitions_) {
        if (QString::fromStdString(definition.key).compare(key, Qt::CaseInsensitive) == 0)
            return &definition;
    }
    return nullptr;
}

// ---------------------------------------------------------------- class row

void ObjectPropertiesDialog::buildClassRow(QVBoxLayout* layout)
{
    if (!hasClassData()) return;

    auto* row = new QHBoxLayout;
    // No accelerator in world mode: there is no combo for it to focus.
    auto* classLabel = new QLabel(setup_.world ? tr("Class:") : tr("&Class:"), this);
    row->addWidget(classLabel);
    if (setup_.world) {
        // worldspawn cannot be re-classed, so its class is shown, not offered.
        auto* fixedClass = new QLabel(classname(), this);
        QFont font = fixedClass->font();
        font.setBold(true);
        fixedClass->setFont(font);
        row->addWidget(fixedClass, 1);
    } else {
        classCombo_ = new QComboBox(this);
        classCombo_->setEditable(true);
        classCombo_->setInsertPolicy(QComboBox::NoInsert);
        if (setup_.fgd && !setup_.fgd->empty()) {
            for (const hammer::fgd::EntityClass& entityClass : setup_.fgd->classes()) {
                if (entityClass.kind == hammer::fgd::ClassKind::Base) continue;
                if ((entityClass.kind == hammer::fgd::ClassKind::Solid) != setup_.brushEntity)
                    continue;
                classCombo_->addItem(QString::fromStdString(entityClass.name));
            }
        }
        classCombo_->setCurrentText(classname());
        classCombo_->setEnabled(setup_.fgd && !setup_.fgd->empty());
        classLabel->setBuddy(classCombo_);
        row->addWidget(classCombo_, 1);
    }

    // BS_PUSHLIKE checkbox in the original; a checkable button keeps it looking
    // like the rest of this port's toggles.
    smartEditButton_ = new QPushButton(tr("&SmartEdit"), this);
    smartEditButton_->setCheckable(true);
    smartEditButton_->setChecked(true);
    row->addWidget(smartEditButton_);

    auto* help = new QPushButton(tr("&Help"), this);
    row->addWidget(help);

    // IDC_ANGLEEDIT + IDC_ANGLEBOX: the main angle control, which always edits
    // the "angles" key whatever the list selection is.
    row->addSpacing(8);
    auto* anglesLabel = new QLabel(tr("A&ngles:"), this);
    row->addWidget(anglesLabel);
    angleCombo_ = new QComboBox(this);
    angleCombo_->setEditable(true);
    angleCombo_->setInsertPolicy(QComboBox::NoInsert);
    angleCombo_->addItem(tr("Up"));
    angleCombo_->addItem(tr("Down"));
    angleCombo_->setFixedWidth(80);
    anglesLabel->setBuddy(angleCombo_);
    row->addWidget(angleCombo_);
    angleBox_ = new HammerAngleBox(this);
    row->addWidget(angleBox_);
    layout->addLayout(row);

    connect(angleBox_, &HammerAngleBox::anglesChanged, this, [this](const QString& angles) {
        if (loadingAngles_) return;
        setValue(QStringLiteral("angles"), angles);
        const QSignalBlocker blocker(angleCombo_);
        angleCombo_->setCurrentText(angleBox_->editText());
    });
    connect(angleCombo_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        if (loadingAngles_) return;
        // CAngleCombo::UpdateAngleBox: a number is a yaw, "Down" is a pitch of
        // 90 and anything else points straight up.
        const QString angles = HammerAngleBox::anglesForEditText(text);
        if (angles.isEmpty()) return;
        angleBox_->setAngles(angles);
        setValue(QStringLiteral("angles"), angles);
    });
    pushAnglesToControls();

    if (classCombo_) connect(classCombo_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        setValue(QStringLiteral("classname"), text);
        definitions_ = setup_.fgd && !text.isEmpty()
                           ? setup_.fgd->effectiveProperties(text.toUtf8().toStdString())
                           : std::vector<hammer::fgd::PropertyDefinition>{};
        reloadKeyvalueList();
        // The new class brings its own spawnflags, so the Flags page is rebuilt
        // rather than left describing the old one.
        rebuildFlagsPage();
    });
    connect(smartEditButton_, &QPushButton::toggled, this,
            [this](bool smart) { setSmartEdit(smart); });
    connect(help, &QPushButton::clicked, this, [this] {
        // hammer/entityhelpdlg.cpp: the class description plus its IO list.
        const hammer::fgd::EntityClass* entityClass =
            setup_.fgd ? setup_.fgd->findClass(classname().toUtf8().toStdString()) : nullptr;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Entity Help - %1").arg(classname()));
        dialog.resize(560, 480);
        auto* dialogLayout = new QVBoxLayout(&dialog);
        auto* text = new QPlainTextEdit(&dialog);
        text->setReadOnly(true);
        QString body;
        if (!entityClass) {
            body = tr("No game data is loaded for this class.");
        } else {
            body = QString::fromStdString(entityClass->description);
            if (!entityClass->inputs.empty()) {
                body += tr("\n\nInputs:\n");
                for (const auto& io : entityClass->inputs)
                    body += QStringLiteral("  %1 - %2\n").arg(QString::fromStdString(io.name),
                                                              QString::fromStdString(io.description));
            }
            if (!entityClass->outputs.empty()) {
                body += tr("\nOutputs:\n");
                for (const auto& io : entityClass->outputs)
                    body += QStringLiteral("  %1 - %2\n").arg(QString::fromStdString(io.name),
                                                              QString::fromStdString(io.description));
            }
        }
        text->setPlainText(body);
        dialogLayout->addWidget(text, 1);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        dialogLayout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    });
}

void ObjectPropertiesDialog::pushAnglesToControls()
{
    if (!angleBox_) return;
    loadingAngles_ = true;
    angleBox_->setAngles(value(QStringLiteral("angles")));
    angleCombo_->setCurrentText(angleBox_->editText());
    loadingAngles_ = false;
    setAnglesEnabled();
}

void ObjectPropertiesDialog::setAnglesEnabled()
{
    if (!angleBox_) return;
    // COP_Entity::UpdateKeyValues only enables the control when the class
    // actually defines "angles" (or when SmartEdit is off).
    const bool enabled = !smartEdit_ || definitionFor(QStringLiteral("angles")) != nullptr;
    angleBox_->setEnabled(enabled);
    angleCombo_->setEnabled(enabled);
}

// ----------------------------------------------------------- Class Info page

QWidget* ObjectPropertiesDialog::buildKeyvaluesPage()
{
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    auto* splitter = new QSplitter(Qt::Horizontal, page);
    splitter->setChildrenCollapsible(false);

    auto* listBox = new QGroupBox(tr("Keyvalues"), splitter);
    auto* listLayout = new QVBoxLayout(listBox);
    keyvalueList_ = new QTreeWidget(listBox);
    keyvalueList_->setColumnCount(2);
    keyvalueList_->setHeaderLabels({tr("Property Name"), tr("Value")});
    keyvalueList_->setRootIsDecorated(false);
    keyvalueList_->setAlternatingRowColors(true);
    keyvalueList_->setUniformRowHeights(true);
    keyvalueList_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    keyvalueList_->header()->setStretchLastSection(true);
    keyvalueList_->setColumnWidth(0, 180);
    listLayout->addWidget(keyvalueList_, 1);
    splitter->addWidget(listBox);

    auto* side = new QWidget(splitter);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);

    editorBox_ = new QGroupBox(tr("Selected Keyvalue"), side);
    auto* editorLayout = new QVBoxLayout(editorBox_);
    auto* keyRow = new QHBoxLayout;
    keyLabel_ = new QLabel(tr("&Key:"), editorBox_);
    keyEdit_ = new QLineEdit(editorBox_);
    keyLabel_->setBuddy(keyEdit_);
    keyRow->addWidget(keyLabel_);
    keyRow->addWidget(keyEdit_, 1);
    editorLayout->addLayout(keyRow);

    editorLayout->addWidget(new QLabel(tr("Value:"), editorBox_));
    auto* valueHost = new QWidget(editorBox_);
    valueEditorLayout_ = new QVBoxLayout(valueHost);
    valueEditorLayout_->setContentsMargins(0, 0, 0, 0);
    editorLayout->addWidget(valueHost);

    auto* buttonRow = new QHBoxLayout;
    addButton_ = new QPushButton(tr("&Add"), editorBox_);
    removeButton_ = new QPushButton(tr("&Delete"), editorBox_);
    buttonRow->addWidget(addButton_);
    buttonRow->addWidget(removeButton_);
    buttonRow->addStretch(1);
    editorLayout->addLayout(buttonRow);
    sideLayout->addWidget(editorBox_);

    auto* helpBox = new QGroupBox(tr("Help"), side);
    auto* helpLayout = new QVBoxLayout(helpBox);
    helpText_ = new QPlainTextEdit(helpBox);
    helpText_->setReadOnly(true);
    helpLayout->addWidget(helpText_, 1);
    sideLayout->addWidget(helpBox, 1);

    auto* commentsBox = new QGroupBox(tr("Comments"), side);
    auto* commentsLayout = new QVBoxLayout(commentsBox);
    comments_ = new QLineEdit(value(QStringLiteral("comments")), commentsBox);
    commentsLayout->addWidget(comments_);
    sideLayout->addWidget(commentsBox);
    // A solid carries no comments keyvalue in the original dialog.
    commentsBox->setVisible(hasClassData());

    splitter->addWidget(side);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    // Even split: the list and the editor each get half the page.
    splitter->setSizes({1000, 1000});
    pageLayout->addWidget(splitter, 1);

    connect(keyvalueList_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) { showKeyvalue(item); });
    connect(comments_, &QLineEdit::textChanged, this, [this](const QString& text) {
        setValue(QStringLiteral("comments"), text);
    });
    connect(keyEdit_, &QLineEdit::editingFinished, this, [this] {
        if (loadingKeyvalue_ || smartEdit_) return;
        QTreeWidgetItem* item = keyvalueList_->currentItem();
        if (!item) return;
        const QString oldKey = item->data(0, kKeyRole).toString();
        const QString newKey = keyEdit_->text().trimmed();
        if (newKey.isEmpty() || newKey.compare(oldKey, Qt::CaseInsensitive) == 0) return;
        for (auto& property : properties_) {
            if (QString::fromStdString(property.key).compare(oldKey, Qt::CaseInsensitive) == 0) {
                property.key = newKey.toUtf8().toStdString();
                break;
            }
        }
        reloadKeyvalueList(newKey);
        refreshApplyState();
    });
    connect(addButton_, &QPushButton::clicked, this, [this] {
        int suffix = 0;
        QString key = QStringLiteral("newkey");
        while (!value(key).isEmpty() || definitionFor(key))
            key = QStringLiteral("newkey%1").arg(++suffix);
        setValue(key, QString{});
        reloadKeyvalueList(key);
        keyEdit_->setFocus();
        keyEdit_->selectAll();
    });
    connect(removeButton_, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* item = keyvalueList_->currentItem();
        if (!item) return;
        removeKey(item->data(0, kKeyRole).toString());
        reloadKeyvalueList();
    });

    // A solid has no class and so no SmartEdit toggle; it is always edited raw.
    setSmartEdit(hasClassData());
    return page;
}

void ObjectPropertiesDialog::setSmartEdit(bool smart)
{
    smartEdit_ = smart;
    if (keyEdit_) keyEdit_->setReadOnly(smart);
    if (addButton_) addButton_->setEnabled(!smart);
    if (removeButton_) removeButton_->setEnabled(!smart);
    if (editorBox_)
        editorBox_->setTitle(smart ? tr("Selected Keyvalue") : tr("Selected Keyvalue (raw)"));
    if (keyvalueList_) reloadKeyvalueList();
    setAnglesEnabled();
}

void ObjectPropertiesDialog::reloadKeyvalueList(const QString& keyToSelect)
{
    if (!keyvalueList_) return;
    const QString wanted = keyToSelect.isEmpty() && keyvalueList_->currentItem()
                               ? keyvalueList_->currentItem()->data(0, kKeyRole).toString()
                               : keyToSelect;

    const QSignalBlocker blocker(keyvalueList_);
    keyvalueList_->clear();

    const auto addRow = [this](const QString& key, const QString& label, const QString& valueText) {
        auto* item = new QTreeWidgetItem(keyvalueList_);
        item->setText(0, label);
        item->setText(1, valueText);
        item->setData(0, kKeyRole, key);
        decorateListRow(item, key, valueText);
    };

    if (smartEdit_) {
        // SmartEdit lists the class's properties under their FGD display names,
        // exactly as CObjectPage does; keys the FGD does not know are only
        // visible (and editable) with SmartEdit off, but they are preserved.
        for (const auto& definition : definitions_) {
            const QString key = QString::fromStdString(definition.key);
            if (key.compare(QStringLiteral("spawnflags"), Qt::CaseInsensitive) == 0) continue;
            QString current = value(key);
            if (current.isEmpty()) current = QString::fromStdString(definition.defaultValue);
            addRow(key,
                   QString::fromStdString(definition.displayName.empty() ? definition.key
                                                                         : definition.displayName),
                   current);
        }
        if (definitions_.empty()) {
            auto* item = new QTreeWidgetItem(keyvalueList_);
            item->setText(0, tr("No game data loaded"));
            item->setText(1, tr("Switch SmartEdit off to edit raw VMF keys"));
            item->setFlags(Qt::ItemIsEnabled);
        }
    } else {
        for (const auto& property : properties_) {
            const QString key = QString::fromStdString(property.key);
            addRow(key, key, QString::fromStdString(property.value));
        }
    }

    QTreeWidgetItem* selected = nullptr;
    for (int index = 0; index < keyvalueList_->topLevelItemCount(); ++index) {
        QTreeWidgetItem* item = keyvalueList_->topLevelItem(index);
        if (item->data(0, kKeyRole).toString().compare(wanted, Qt::CaseInsensitive) == 0) {
            selected = item;
            break;
        }
    }
    if (!selected && keyvalueList_->topLevelItemCount() > 0)
        selected = keyvalueList_->topLevelItem(0);
    keyvalueList_->setCurrentItem(selected);
    showKeyvalue(selected);
}

void ObjectPropertiesDialog::updateListRow(const QString& key, const QString& valueText)
{
    if (!keyvalueList_) return;
    for (int index = 0; index < keyvalueList_->topLevelItemCount(); ++index) {
        QTreeWidgetItem* item = keyvalueList_->topLevelItem(index);
        if (item->data(0, kKeyRole).toString().compare(key, Qt::CaseInsensitive) == 0) {
            item->setText(1, valueText);
            decorateListRow(item, key, valueText);
            return;
        }
    }
}

void ObjectPropertiesDialog::decorateListRow(QTreeWidgetItem* item, const QString& key,
                                             const QString& valueText)
{
    // A color property shows its swatch beside the value, in either list mode
    // as long as the FGD says the key is a color.
    const hammer::fgd::PropertyDefinition* definition = definitionFor(key);
    const bool isColor = definition &&
                         (definition->type == hammer::fgd::PropertyType::Color255 ||
                          definition->type == hammer::fgd::PropertyType::Color1);
    if (!isColor) {
        item->setData(1, Qt::DecorationRole, {});
        return;
    }
    item->setData(1, Qt::DecorationRole,
                  QIcon(colorSwatch(valueText,
                                    definition->type == hammer::fgd::PropertyType::Color1,
                                    QSize(24, 14))));
}

void ObjectPropertiesDialog::showKeyvalue(QTreeWidgetItem* item)
{
    if (!valueEditorLayout_) return;
    loadingKeyvalue_ = true;
    if (valueEditor_) {
        // Taking it out of the layout is not enough: until deleteLater runs it
        // is still a child of the group box and keeps painting at its old
        // geometry, showing as a sliver under the new editor.
        valueEditorLayout_->removeWidget(valueEditor_);
        valueEditor_->hide();
        valueEditor_->setParent(nullptr);
        valueEditor_->deleteLater();
        valueEditor_ = nullptr;
    }

    const QString key = item ? item->data(0, kKeyRole).toString() : QString{};
    keyEdit_->setText(key);
    const hammer::fgd::PropertyDefinition* definition = smartEdit_ ? definitionFor(key) : nullptr;
    helpText_->setPlainText(definition ? QString::fromStdString(definition->description)
                                       : QString{});

    if (!key.isEmpty()) {
        QString current = value(key);
        if (current.isEmpty() && definition)
            current = QString::fromStdString(definition->defaultValue);
        valueEditor_ = makeValueEditor(definition, key, current);
        valueEditorLayout_->addWidget(valueEditor_);
        if (definition && definition->readOnly) valueEditor_->setEnabled(false);
    }
    editorBox_->setEnabled(!key.isEmpty());
    loadingKeyvalue_ = false;
}

QWidget* ObjectPropertiesDialog::makeValueEditor(const hammer::fgd::PropertyDefinition* definition,
                                                 const QString& key, const QString& valueText)
{
    const auto store = [this](const QString& storeKey, const QString& storeValue) {
        setValue(storeKey, storeValue);
    };
    const auto read = [this](const QString& readKey) { return value(readKey); };

    if (definition || !smartEdit_) {
        // Model and animation keys get the browser-backed editors even with no
        // FGD definition, matching how the port has always treated them.
        if (key.compare(QStringLiteral("model"), Qt::CaseInsensitive) == 0 ||
            (definition && definition->type == hammer::fgd::PropertyType::Model)) {
            if (setup_.modelEditorFactory)
                return setup_.modelEditorFactory(editorBox_, this, key, valueText, store, read);
        }
        if (key.compare(QStringLiteral("DefaultAnim"), Qt::CaseInsensitive) == 0 ||
            key.compare(QStringLiteral("sequence"), Qt::CaseInsensitive) == 0) {
            if (setup_.sequenceEditorFactory)
                return setup_.sequenceEditorFactory(editorBox_, this, key, valueText, store, read);
        }
        if (key.compare(QStringLiteral("skyname"), Qt::CaseInsensitive) == 0) {
            if (setup_.skyEditorFactory)
                return setup_.skyEditorFactory(editorBox_, this, key, valueText, store, read);
        }
    }

    if (!definition) {
        auto* line = new QLineEdit(valueText, editorBox_);
        connect(line, &QLineEdit::textChanged, this,
                [this, key](const QString& text) { setValue(key, text); });
        return line;
    }

    switch (definition->type) {
    case hammer::fgd::PropertyType::Choices:
    case hammer::fgd::PropertyType::Boolean: {
        auto* combo = new QComboBox(editorBox_);
        if (definition->type == hammer::fgd::PropertyType::Boolean) {
            combo->addItem(tr("No"), QStringLiteral("0"));
            combo->addItem(tr("Yes"), QStringLiteral("1"));
        } else {
            for (const auto& choice : definition->choices)
                combo->addItem(QString::fromStdString(choice.label),
                               QString::fromStdString(choice.value));
        }
        int selected = combo->findData(valueText);
        if (selected < 0 && !valueText.isEmpty()) {
            combo->addItem(valueText, valueText);
            selected = combo->count() - 1;
        }
        if (selected >= 0) combo->setCurrentIndex(selected);
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key](int) {
            setValue(key, combo->currentData().toString());
        });
        return combo;
    }
    case hammer::fgd::PropertyType::Vector:
    case hammer::fgd::PropertyType::Angle:
    case hammer::fgd::PropertyType::Color255:
    case hammer::fgd::PropertyType::Color1: {
        // COP_Entity's smart control is a single edit box holding the raw
        // value, with the angle control or the Pick color button beside it.
        const bool isAngle = definition->type == hammer::fgd::PropertyType::Angle;
        const bool isColor = definition->type == hammer::fgd::PropertyType::Color255 ||
                             definition->type == hammer::fgd::PropertyType::Color1;
        auto* compound = new QWidget(editorBox_);
        auto* outerLayout = new QVBoxLayout(compound);
        outerLayout->setContentsMargins(0, 0, 0, 0);
        outerLayout->setSpacing(4);
        auto* valueRow = new QHBoxLayout;
        valueRow->setContentsMargins(0, 0, 0, 0);
        valueRow->setSpacing(6);
        auto* line = new QLineEdit(valueText, compound);
        valueRow->addWidget(line, 1);
        outerLayout->addLayout(valueRow);

        // Shared by the sub-controls so writing the edit box does not bounce
        // back through them while they are driving it.
        auto updating = std::make_shared<bool>(false);
        connect(line, &QLineEdit::textChanged, this,
                [this, key](const QString& text) { setValue(key, text); });

        if (isColor) {
            const bool scaled = definition->type == hammer::fgd::PropertyType::Color1;
            // A swatch of the current value, so the color is readable without
            // opening the picker.
            auto* preview = new QLabel(compound);
            preview->setFixedSize(28, 22);
            preview->setToolTip(tr("Current color"));
            valueRow->addWidget(preview);
            auto* pick = new QPushButton(tr("Pick color"), compound);
            valueRow->addWidget(pick);

            const auto updatePreview = [preview, scaled](const QString& text) {
                preview->setPixmap(colorSwatch(text, scaled, QSize(28, 22)));
            };
            updatePreview(valueText);
            connect(line, &QLineEdit::textChanged, this,
                    [updatePreview](const QString& text) { updatePreview(text); });

            connect(pick, &QPushButton::clicked, this, [this, line, scaled] {
                // COP_Entity::OnPickColor: only the first three components are
                // replaced; a fourth (a light's brightness) is carried over.
                const QStringList tokens =
                    line->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
                const QColor initial = isRainbowColor(tokens)
                                           ? QColor(Qt::white)
                                           : colorFromText(line->text(), scaled);
                const QColor chosen = QColorDialog::getColor(initial, this, tr("Pick color"));
                if (!chosen.isValid()) return;
                QString text;
                if (scaled) {
                    text = QStringLiteral("%1 %2 %3")
                               .arg(chosen.redF(), 0, 'f', 3)
                               .arg(chosen.greenF(), 0, 'f', 3)
                               .arg(chosen.blueF(), 0, 'f', 3);
                } else {
                    text = QStringLiteral("%1 %2 %3")
                               .arg(chosen.red()).arg(chosen.green()).arg(chosen.blue());
                }
                if (tokens.size() >= 4) text += QStringLiteral(" %1").arg(tokens[3]);
                line->setText(text);
            });
        }

        if (isAngle) {
            // IDC_SMART_ANGLEEDIT / IDC_SMART_ANGLEBOX, which the original
            // places below the smart control (CreateSmartControls_Angle).
            auto* combo = new QComboBox(compound);
            combo->setEditable(true);
            combo->setInsertPolicy(QComboBox::NoInsert);
            combo->addItem(tr("Up"));
            combo->addItem(tr("Down"));
            combo->setFixedWidth(80);
            auto* box = new HammerAngleBox(compound);
            auto* angleRow = new QHBoxLayout;
            angleRow->setContentsMargins(0, 0, 0, 0);
            angleRow->setSpacing(6);
            angleRow->addWidget(combo);
            angleRow->addWidget(box);
            angleRow->addStretch(1);
            outerLayout->addLayout(angleRow);

            const auto pushToControls = [box, combo, updating](const QString& angles) {
                if (*updating) return;
                *updating = true;
                box->setAngles(angles);
                combo->setCurrentText(box->editText());
                *updating = false;
            };
            pushToControls(valueText);
            connect(line, &QLineEdit::textChanged, this,
                    [pushToControls](const QString& text) { pushToControls(text); });
            connect(box, &HammerAngleBox::anglesChanged, this,
                    [line, combo, box, updating](const QString& angles) {
                        if (*updating) return;
                        *updating = true;
                        combo->setCurrentText(box->editText());
                        line->setText(angles);
                        *updating = false;
                    });
            connect(combo, &QComboBox::currentTextChanged, this,
                    [line, box, updating](const QString& text) {
                        if (*updating) return;
                        const QString angles = HammerAngleBox::anglesForEditText(text);
                        if (angles.isEmpty()) return;
                        *updating = true;
                        box->setAngles(angles);
                        line->setText(angles);
                        *updating = false;
                    });
        }
        return compound;
    }
    case hammer::fgd::PropertyType::Integer: {
        auto* spin = new QSpinBox(editorBox_);
        spin->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        spin->setValue(parseInteger(valueText));
        connect(spin, &QSpinBox::valueChanged, this,
                [this, key](int number) { setValue(key, QString::number(number)); });
        return spin;
    }
    case hammer::fgd::PropertyType::Float: {
        auto* spin = new QDoubleSpinBox(editorBox_);
        spin->setRange(-1000000000.0, 1000000000.0);
        spin->setDecimals(6);
        spin->setValue(valueText.toDouble());
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double number) {
            setValue(key, QString::number(number, 'g', 12));
        });
        return spin;
    }
    case hammer::fgd::PropertyType::Flags: {
        // A non-spawnflags flags property; spawnflags has its own page.
        auto* button = new QToolButton(editorBox_);
        auto* menu = new QMenu(button);
        button->setMenu(menu);
        button->setPopupMode(QToolButton::InstantPopup);
        const int initialFlags = parseInteger(valueText);
        for (const auto& choice : definition->choices) {
            bool ok = false;
            const int bit = QString::fromStdString(choice.value).toInt(&ok);
            QAction* action = menu->addAction(QString::fromStdString(choice.label));
            action->setCheckable(true);
            action->setData(ok ? bit : 0);
            action->setChecked(ok && (initialFlags & bit) != 0);
        }
        const auto updateFlags = [this, button, menu, key] {
            int flags = 0;
            QStringList names;
            for (QAction* action : menu->actions()) {
                if (!action->isChecked()) continue;
                flags |= action->data().toInt();
                names.push_back(action->text());
            }
            button->setText(names.isEmpty()
                                ? tr("None (%1)").arg(flags)
                                : tr("%1 (%2)").arg(names.join(QStringLiteral(", "))).arg(flags));
            setValue(key, QString::number(flags));
        };
        for (QAction* action : menu->actions())
            connect(action, &QAction::toggled, this, [updateFlags](bool) { updateFlags(); });
        updateFlags();
        return button;
    }
    default:
        break;
    }

    auto* line = new QLineEdit(valueText, editorBox_);
    line->setToolTip(QString::fromStdString(definition->description));
    connect(line, &QLineEdit::textChanged, this,
            [this, key](const QString& text) { setValue(key, text); });
    return line;
}

// ---------------------------------------------------------------- Flags page

void ObjectPropertiesDialog::rebuildFlagsPage()
{
    if (flagsPage_) {
        const int index = tabs_->indexOf(flagsPage_);
        if (index >= 0) tabs_->removeTab(index);
        flagsPage_->deleteLater();
        flagsPage_ = nullptr;
    }
    spawnFlagChecks_.clear();
    spawnFlagsValue_ = nullptr;
    spawnFlagsKey_.clear();
    unknownSpawnFlags_ = 0;
    flagsPage_ = buildFlagsPage();
    if (flagsPage_) tabs_->addTab(flagsPage_, tr("Flags"));
}

QWidget* ObjectPropertiesDialog::buildVisGroupPage()
{
    // hammer/op_groups.cpp COP_Groups: one check box per visgroup, ticked for
    // the ones this object is in. "To remove the VisGrouping from this object,
    // simply uncheck all of the boxes."
    // The page is built even when the map has no VisGroups yet, so a
    // double-click on a world brush always lands somewhere meaningful; it just
    // explains that there is nothing to tick.
    if (!setup_.setVisGroupMembership) return nullptr;
    // Page style of the Flags and Outputs pages: content inside one titled
    // group box on a plain page.
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    auto* box = new QGroupBox(tr("VisGroups"), page);
    auto* layout = new QVBoxLayout(box);
    pageLayout->addWidget(box, 1);
    auto* note = new QLabel(
        setup_.visGroups.empty()
            ? tr("This map has no VisGroups yet. Create one with View > Move Selection "
                 "To Visgroup, or the Edit button under the Filter Control list.")
            : tr("Select the VisGroups this object belongs to. Uncheck them all "
                 "to remove it from every VisGroup."),
        box);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* list = new QListWidget(box);
    list->setAlternatingRowColors(true);
    layout->addWidget(list, 1);
    for (const auto& [id, name] : setup_.visGroups) {
        auto* item = new QListWidgetItem(name, list);
        item->setData(Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool member = std::find(setup_.memberVisGroups.begin(),
                                      setup_.memberVisGroups.end(),
                                      id) != setup_.memberVisGroups.end();
        // A multi-object selection that disagrees shows the shared memberships
        // ticked and everything else clear; ticking a clear box adds the whole
        // selection to that visgroup.
        item->setCheckState(member ? Qt::Checked : Qt::Unchecked);
    }
    if (setup_.visGroupsMixed) {
        auto* mixed = new QLabel(tr("The selected objects are not all in the same VisGroups; "
                                    "only the ones they share are ticked."),
                                 box);
        mixed->setWordWrap(true);
        mixed->setEnabled(false);
        layout->addWidget(mixed);
    }
    connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!item || !setup_.setVisGroupMembership) return;
        setup_.setVisGroupMembership(item->data(Qt::UserRole).toInt(),
                                     item->checkState() == Qt::Checked);
    });
    return page;
}

QWidget* ObjectPropertiesDialog::buildFlagsPage()
{
    if (!isEntity()) return nullptr;
    const hammer::fgd::PropertyDefinition* definition = nullptr;
    for (const auto& candidate : definitions_) {
        if (candidate.type == hammer::fgd::PropertyType::Flags &&
            QString::fromStdString(candidate.key)
                    .compare(QStringLiteral("spawnflags"), Qt::CaseInsensitive) == 0) {
            definition = &candidate;
            break;
        }
    }
    if (!definition) return nullptr;
    spawnFlagsKey_ = QString::fromStdString(definition->key);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget(scroll);
    auto* pageLayout = new QVBoxLayout(page);
    auto* box = new QGroupBox(tr("Flags"), page);
    auto* boxLayout = new QVBoxLayout(box);
    pageLayout->addWidget(box);
    pageLayout->addStretch(1);
    scroll->setWidget(page);

    QString current = value(spawnFlagsKey_);
    int initialFlags = parseInteger(current);
    if (current.trimmed().isEmpty()) {
        // An unset spawnflags takes the FGD's default-on bits.
        for (const auto& choice : definition->choices) {
            if (!choice.defaultOn) continue;
            bool ok = false;
            const int bit = QString::fromStdString(choice.value).toInt(&ok);
            if (ok) initialFlags |= bit;
        }
    }

    int knownMask = 0;
    for (const auto& choice : definition->choices) {
        bool ok = false;
        const int bit = QString::fromStdString(choice.value).toInt(&ok);
        if (!ok || bit == 0) continue;
        knownMask |= bit;
        auto* checkbox = new QCheckBox(
            tr("%1  (%2)").arg(QString::fromStdString(choice.label)).arg(bit), box);
        checkbox->setChecked((initialFlags & bit) != 0);
        boxLayout->addWidget(checkbox);
        spawnFlagChecks_.push_back({checkbox, bit});
    }
    // Bits the FGD does not describe belong to the map and must survive.
    unknownSpawnFlags_ = initialFlags & ~knownMask;

    spawnFlagsValue_ = new QLabel(box);
    spawnFlagsValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    boxLayout->addSpacing(8);
    boxLayout->addWidget(spawnFlagsValue_);

    const auto update = [this] {
        int flags = unknownSpawnFlags_;
        for (const auto& [checkbox, bit] : spawnFlagChecks_) {
            if (checkbox->isChecked()) flags |= bit;
        }
        spawnFlagsValue_->setText(tr("Combined value: %1").arg(flags));
        setValue(spawnFlagsKey_, QString::number(flags));
    };
    for (const auto& [checkbox, bit] : spawnFlagChecks_) {
        Q_UNUSED(bit);
        connect(checkbox, &QCheckBox::toggled, this, [update](bool) { update(); });
    }
    update();
    return scroll;
}

// -------------------------------------------------------------- Outputs page

QStringList ObjectPropertiesDialog::targetNames() const
{
    QStringList names;
    if (!setup_.document) return names;
    for (const hammer::vmf::Block& root : setup_.document->roots()) {
        if (QString::fromStdString(root.name).compare(QStringLiteral("entity"),
                                                      Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (const std::string* targetname = root.value("targetname")) {
            const QString name = QString::fromStdString(*targetname);
            if (!name.isEmpty() && !names.contains(name)) names << name;
        }
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString ObjectPropertiesDialog::classOfTarget(const QString& targetName) const
{
    if (!setup_.document) return {};
    for (const hammer::vmf::Block& root : setup_.document->roots()) {
        if (QString::fromStdString(root.name).compare(QStringLiteral("entity"),
                                                      Qt::CaseInsensitive) != 0) {
            continue;
        }
        const std::string* targetname = root.value("targetname");
        if (!targetname ||
            QString::fromStdString(*targetname).compare(targetName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const std::string* entityClass = root.value("classname");
        return entityClass ? QString::fromStdString(*entityClass) : QString{};
    }
    return {};
}

QStringList ObjectPropertiesDialog::inputNamesFor(const QString& targetClass) const
{
    QStringList names;
    if (setup_.fgd && !setup_.fgd->empty() && !targetClass.isEmpty()) {
        for (const auto& io : setup_.fgd->effectiveInputs(targetClass.toUtf8().toStdString()))
            names << QString::fromStdString(io.name);
    }
    return names;
}

QWidget* ObjectPropertiesDialog::buildOutputsPage()
{
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);

    outputsList_ = new QTreeWidget(page);
    outputsList_->setColumnCount(6);
    outputsList_->setHeaderLabels({tr("My Output"), tr("Target Entity"), tr("Target Input"),
                                   tr("Parameter"), tr("Delay"), tr("Only Once")});
    outputsList_->setRootIsDecorated(false);
    outputsList_->setAlternatingRowColors(true);
    outputsList_->setUniformRowHeights(true);
    outputsList_->header()->setSectionResizeMode(QHeaderView::Stretch);
    pageLayout->addWidget(outputsList_, 1);

    // The edit panel of IDD_OBJPAGE_OUTPUT: one sentence spread over five
    // fields, editing whichever connection is selected above.
    outputPanel_ = new QGroupBox(page);
    auto* form = new QFormLayout(outputPanel_);
    outputName_ = new QComboBox(outputPanel_);
    outputName_->setEditable(true);
    if (setup_.fgd && !setup_.fgd->empty()) {
        for (const auto& io : setup_.fgd->effectiveOutputs(classname().toUtf8().toStdString()))
            outputName_->addItem(QString::fromStdString(io.name));
    }
    outputTarget_ = new QComboBox(outputPanel_);
    outputTarget_->setEditable(true);
    outputTarget_->addItems(targetNames());
    outputInput_ = new QComboBox(outputPanel_);
    outputInput_->setEditable(true);
    outputParameter_ = new QLineEdit(outputPanel_);
    outputDelay_ = new QLineEdit(outputPanel_);
    outputOnce_ = new QCheckBox(tr("&Fire once only"), outputPanel_);
    form->addRow(tr("My &output named"), outputName_);
    form->addRow(tr("&Targets entities named"), outputTarget_);
    form->addRow(tr("Via this &input"), outputInput_);
    form->addRow(tr("With a pa&rameter override of"), outputParameter_);
    form->addRow(tr("After a de&lay in seconds of"), outputDelay_);
    form->addRow(QString{}, outputOnce_);
    pageLayout->addWidget(outputPanel_);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("&Add"), page);
    auto* remove = new QPushButton(tr("&Delete"), page);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    pageLayout->addLayout(buttons);

    for (const auto& connection : setup_.connections) {
        auto* item = new QTreeWidgetItem(outputsList_);
        item->setText(0, QString::fromStdString(connection.output));
        item->setText(1, QString::fromStdString(connection.target));
        item->setText(2, QString::fromStdString(connection.input));
        item->setText(3, QString::fromStdString(connection.parameter));
        item->setText(4, QString::number(connection.delay));
        item->setText(5, connection.timesToFire >= 0 ? tr("Yes") : tr("No"));
    }

    connect(outputsList_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) { showOutput(item); });
    connect(outputTarget_, &QComboBox::currentTextChanged, this, [this](const QString& target) {
        // "Via this input" only offers the inputs of whatever class the typed
        // targetname resolves to (op_output.cpp does the same lookup).
        const QString kept = outputInput_->currentText();
        const QSignalBlocker blocker(outputInput_);
        outputInput_->clear();
        outputInput_->addItems(inputNamesFor(classOfTarget(target)));
        outputInput_->setCurrentText(kept);
        commitOutputEdits();
    });
    for (QComboBox* combo : {outputName_, outputInput_}) {
        connect(combo, &QComboBox::currentTextChanged, this,
                [this](const QString&) { commitOutputEdits(); });
    }
    for (QLineEdit* line : {outputParameter_, outputDelay_}) {
        connect(line, &QLineEdit::textChanged, this,
                [this](const QString&) { commitOutputEdits(); });
    }
    connect(outputOnce_, &QCheckBox::toggled, this, [this](bool) { commitOutputEdits(); });
    connect(add, &QPushButton::clicked, this, [this] {
        auto* item = new QTreeWidgetItem(outputsList_);
        item->setText(4, QStringLiteral("0"));
        item->setText(5, tr("No"));
        outputsList_->setCurrentItem(item);
        outputName_->setFocus();
        refreshApplyState();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        delete outputsList_->currentItem();
        showOutput(outputsList_->currentItem());
        refreshApplyState();
    });

    if (outputsList_->topLevelItemCount() > 0)
        outputsList_->setCurrentItem(outputsList_->topLevelItem(0));
    else
        showOutput(nullptr);
    return page;
}

void ObjectPropertiesDialog::showOutput(QTreeWidgetItem* item)
{
    loadingOutput_ = true;
    outputPanel_->setEnabled(item != nullptr);
    const QString target = item ? item->text(1) : QString{};
    {
        const QSignalBlocker blockInput(outputInput_);
        outputInput_->clear();
        outputInput_->addItems(inputNamesFor(classOfTarget(target)));
        outputInput_->setCurrentText(item ? item->text(2) : QString{});
    }
    {
        const QSignalBlocker blockName(outputName_);
        outputName_->setCurrentText(item ? item->text(0) : QString{});
    }
    {
        const QSignalBlocker blockTarget(outputTarget_);
        outputTarget_->setCurrentText(target);
    }
    outputParameter_->setText(item ? item->text(3) : QString{});
    outputDelay_->setText(item ? item->text(4) : QStringLiteral("0"));
    outputOnce_->setChecked(item && item->text(5) == tr("Yes"));
    loadingOutput_ = false;
}

void ObjectPropertiesDialog::commitOutputEdits()
{
    if (loadingOutput_) return;
    QTreeWidgetItem* item = outputsList_ ? outputsList_->currentItem() : nullptr;
    if (!item) return;
    item->setText(0, outputName_->currentText().trimmed());
    item->setText(1, outputTarget_->currentText().trimmed());
    item->setText(2, outputInput_->currentText().trimmed());
    item->setText(3, outputParameter_->text());
    item->setText(4, outputDelay_->text());
    item->setText(5, outputOnce_->isChecked() ? tr("Yes") : tr("No"));
    refreshApplyState();
}

std::vector<hammer::vmf::EditorModel::EntityConnection> ObjectPropertiesDialog::connections() const
{
    std::vector<hammer::vmf::EditorModel::EntityConnection> result;
    if (!outputsList_) return result;
    for (int index = 0; index < outputsList_->topLevelItemCount(); ++index) {
        const QTreeWidgetItem* item = outputsList_->topLevelItem(index);
        hammer::vmf::EditorModel::EntityConnection connection;
        connection.output = item->text(0).trimmed().toUtf8().toStdString();
        if (connection.output.empty()) continue;
        connection.target = item->text(1).trimmed().toUtf8().toStdString();
        connection.input = item->text(2).trimmed().toUtf8().toStdString();
        connection.parameter = item->text(3).toUtf8().toStdString();
        connection.delay = item->text(4).toDouble();
        connection.timesToFire = item->text(5) == tr("Yes") ? 1 : -1;
        result.push_back(std::move(connection));
    }
    return result;
}

// --------------------------------------------------------------- Inputs page

QWidget* ObjectPropertiesDialog::buildInputsPage()
{
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    auto* table = new QTreeWidget(page);
    table->setColumnCount(6);
    table->setHeaderLabels({tr("Source Entity"), tr("Its Output"), tr("My Input"), tr("Parameter"),
                            tr("Delay"), tr("Only Once")});
    table->setRootIsDecorated(false);
    table->setAlternatingRowColors(true);
    table->setUniformRowHeights(true);
    table->header()->setSectionResizeMode(QHeaderView::Stretch);
    pageLayout->addWidget(table, 1);
    fillInputsList(table);
    auto* note = new QLabel(
        tr("Incoming connections are owned by the sending entity and are shown read-only."), page);
    note->setWordWrap(true);
    pageLayout->addWidget(note);
    return page;
}

void ObjectPropertiesDialog::fillInputsList(QTreeWidget* table)
{
    if (!setup_.document) return;
    const QString myTargetName = value(QStringLiteral("targetname"));
    const QString myClassName = classname();
    for (const hammer::vmf::Block& root : setup_.document->roots()) {
        if (QString::fromStdString(root.name).compare(QStringLiteral("entity"),
                                                      Qt::CaseInsensitive) != 0) {
            continue;
        }
        const std::string* idValue = root.value("id");
        if (idValue && QString::fromStdString(*idValue).toInt() == setup_.object.id) continue;
        const std::string* sourceName = root.value("targetname");
        const std::string* sourceClass = root.value("classname");
        QString sourceLabel = sourceName ? QString::fromStdString(*sourceName) : QString{};
        if (sourceLabel.isEmpty() && sourceClass) sourceLabel = QString::fromStdString(*sourceClass);
        else if (sourceClass)
            sourceLabel += QStringLiteral(" (%1)").arg(QString::fromStdString(*sourceClass));
        for (const hammer::vmf::Block* connections : root.children("connections")) {
            for (const auto& entry : connections->entries) {
                if (entry.kind != hammer::vmf::Entry::Kind::KeyValue) continue;
                const QStringList fields = splitConnection(entry.value);
                const QString target = fields[0].trimmed();
                const bool matches =
                    (!myTargetName.isEmpty() &&
                     target.compare(myTargetName, Qt::CaseInsensitive) == 0) ||
                    (!myClassName.isEmpty() && target.compare(myClassName, Qt::CaseInsensitive) == 0);
                if (!matches) continue;
                auto* item = new QTreeWidgetItem(table);
                item->setText(0, sourceLabel);
                item->setText(1, QString::fromStdString(entry.key));
                item->setText(2, fields[1]);
                item->setText(3, fields[2]);
                item->setText(4, fields[3]);
                const QString times = fields[4].trimmed();
                item->setText(5, !times.isEmpty() && times.toInt() >= 0 ? tr("Yes") : tr("No"));
            }
        }
    }
}
