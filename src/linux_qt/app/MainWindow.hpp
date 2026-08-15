#pragma once

#include "FgdDatabase.hpp"
#include "MaterialSystem.hpp"
#include "MapViewWidget.hpp"
#include "VmfEditor.hpp"

#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <memory>

class QAction;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QLabel;
class QMdiArea;
class QMenu;
class QPlainTextEdit;
class QLineEdit;
class QResizeEvent;
class QShowEvent;
class QToolBar;
class QToolButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class MapDocumentWidget;
class CollabSession;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const QStringList& paths = {}, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    bool strippingMdiTitleSuffix_{false};
    QAction* command(const QString& id, const QString& text = {}, const QKeySequence& shortcut = {});
    QAction* addMenuCommand(QMenu* menu, const QString& id, const QString& text,
                            const QKeySequence& shortcut = {}, bool checkable = false,
                            bool checked = false);
    QAction* addDrawnCommand(QToolBar* bar, const QString& id, const QString& text,
                             const QString& iconName, bool checkable = false);

    QMenu* addScrollMenu(const QString& title);
    void createMenus();
    void createToolbars();
    void createMdiSystemButton();
    void updateMdiSystemButtonGeometry();
    void updateMdiSystemButtonState();
    void applyGridSettings();
    void createRightControlBars();
    void createMessageWindow();
    void createStatusBar();
    void restoreWindowLayout();
    void saveWindowLayout() const;
    void scheduleResizableLayoutRefresh();
    void normalizeResizableLayout();
    // "recentPath" is the file the user actually chose, which differs from
    // "path" when a BSP was decompiled into a temporary VMF: the Recent list
    // must remember the BSP, not a file in /tmp. Empty means "same as path".
    MapDocumentWidget* createDocument(const QString& path = {}, const QString& recentPath = {});
    void openDocument();
    // Opens a map the user picked, decompiling first when it is a BSP. Shared
    // by the Open dialog and the Recent Files menu so both routes record the
    // same file and handle BSPs the same way.
    void openMapPath(const QString& path);
    void refreshRecentFilesMenu();
    QString convertBspToVmf(const QString& bspPath, QString* error,
                            QString* extractedContentDir = nullptr);
    void offerExtractedBspContent(const QString& bspPath, const QString& contentDir);
    void runMapCompile();
    void saveDocument();
    void saveDocumentAs();
    void appendMessage(const QString& message);
    void showAbout();
    void showOptions();
    void setPrompt(const QString& text);
    void selectTool(QAction* action);
    void updateTransformHandlesActionText();
    void updateEditActions();
    void updateProjectionActions();
    void rebuildToolTexturesMenu();
    // The "Show nodraw faces" toolbar button is a shortcut for the
    // View > Tool Textures entry for tools/toolsnodraw, so its checked state is
    // read back from the document rather than kept on the side.
    void updateNodrawActionState();
    void loadFgd();
    bool loadFgdPath(const QString& path, bool reportErrors = true);
    void buildCubemaps();
    void clearShaderCache();
    void configureGameDirectory();
    bool loadGameInfoPath(const QString& path, bool reportErrors = true);
    void setMaterialRenderingEnabled(bool enabled);
    void setWireframeOverlayEnabled(bool enabled);
    void setHdrEnabled(bool enabled);
    void setUndoRedoActive(bool active);
    void setRayTracedGamma(float gamma);
    void showRayTracedGammaDialog();
    void showQuickHideToVisGroupDialog();
    // View > Move Selection to VisGroup (hammer/NewVisGroupDlg.cpp).
    void showNewVisGroupDialog();
    // The Filter Control "Edit" button (hammer/editgroups.cpp).
    void showEditVisGroupsDialog();
    // Requests a Filter Control tree rebuild on the next event-loop turn.
    // ALWAYS use this rather than rebuildVisGroupTree() from a signal handler:
    // the rebuild deletes every QTreeWidgetItem, and doing that inside
    // QTreeWidget's own itemChanged emission frees the item Qt is still
    // working on (a confirmed crash in QTreeWidgetItem::setData).
    void scheduleVisGroupTreeRebuild();
    // Repopulates the Filter Control tree from the active document. Safe to
    // call directly only from code that is not inside a tree signal.
    void rebuildVisGroupTree();
    void handleVisGroupItemChanged(QTreeWidgetItem* item, int column);
    void handleAutoVisGroupItemChanged(QTreeWidgetItem* item, int column);
    // Repopulates the Auto tab. Runs in the same deferred pass as the User
    // tree, for the same reentrancy reason.
    void rebuildAutoVisGroupTree();
    // The visgroupid of the tree's current item, or 0.
    int selectedVisGroupId() const;
    // Enables the Filter Control buttons that need a selected VisGroup.
    void updateVisGroupButtonStates();
    void setDisplacementSolidMaskEnabled(bool enabled);
    void setTexturedRenderMode(MapViewWidget::TexturedRenderMode mode);
    void setMaterialEffectsEnabled(bool phong, bool specular, bool bumpMaps,
                                   bool lightWarp, bool selfIllum, bool rimLight);
    void refreshMaterialList();
    void updateTexturePreview(const QString& materialName);
    void showMaterialBrowser();
    // Modal material picker shared by the Textures bar Browse button and the
    // Replace Textures dialog's Find/Replace Browse buttons
    // (hammer/replacetexdlg.cpp CReplaceTexDlg::BrowseTex). Returns the picked
    // material's short name, or an empty string if cancelled.
    QString pickMaterial(const QString& initialMaterial);
    void showReplaceTexturesDialog();
    void showPasteSpecialDialog();
    void showFindEntitiesDialog();
    void showEntityReportDialog();
    void showCheckForProblemsDialog();
    void loadPointFile();
    void loadPortalFile();
    void setActiveViewKind(MapViewWidget::Kind kind);
    // Face Edit sheet (hammer/faceeditsheet.cpp). One floating sheet per main
    // frame, as CMainFrame::m_pFaceEditSheet is.
    // ID_VIEW_3DLIGHTMAP_GRID: the 3D lightmap grid draw mode, driven from both
    // the View menu and the Face Edit sheet's Lightmap page.
    void setLightmapGridVisible(bool visible);
    void setDetailPropsVisible(bool visible);
    void ensureFaceEditSheet();
    void setFaceEditSheetVisible(bool visible);
    void applyCurrentTextureCommand();
    void refreshEntityClasses();
    void applyObjectBarSettings(MapDocumentWidget* document);

    MapDocumentWidget* activeDocument() const;

    // Collaborative editing (Collaborate menu). One session at a time, bound
    // to one document widget for its lifetime.
    CollabSession* ensureCollabSession();
    void hostCollabSession();
    void joinCollabSession();
    void leaveCollabSession(const QString& reason = {});
    void bindCollabDocument(MapDocumentWidget* document);
    void mountCollabDownloads(const QString& directory);
    // Prompts for (and remembers) the display name shown on this editor's
    // presence avatar. Empty means the user cancelled.
    QString promptCollabName();
    void kickCollaborator();
    void ensureCollabChatDock();
    void updateCollabActions();

    QMdiArea* mdiArea_{nullptr};
    CollabSession* collabSession_{nullptr};
    QPointer<MapDocumentWidget> collabDocument_;
    QAction* collabLeaveAction_{nullptr};
    QAction* collabHostAction_{nullptr};
    QAction* collabJoinAction_{nullptr};
    QAction* collabKickAction_{nullptr};
    QAction* collabChatAction_{nullptr};
    QTimer* collabPoseTimer_{nullptr};
    QDockWidget* collabChatDock_{nullptr};
    QPlainTextEdit* collabChatLog_{nullptr};
    QLineEdit* collabChatInput_{nullptr};
    // Re-entry guard: a remote edit runs the same editStateChanged hook local
    // edits do; while it is applied there is nothing to diff.
    bool collabApplyingRemote_{false};
    // Only these two connections come down when a session ends; the document
    // keeps its ordinary MDI wiring.
    QMetaObject::Connection collabEditConnection_;
    QMetaObject::Connection collabDestroyedConnection_;
    QHash<QString, QAction*> commands_;
    QHash<QString, QDockWidget*> controlBars_;
    QHash<QString, QToolBar*> toolBars_;
    QDockWidget* messageDock_{nullptr};
    QPlainTextEdit* messageOutput_{nullptr};
    QComboBox* objectCombo_{nullptr};
    class FaceEditSheet* faceEditSheet_{nullptr};
    QAction* lightmapGridAction_{nullptr};
    bool lightmapGridVisible_{false};
    bool detailPropsVisible_{true};
    QAction* detailObjectsAction_{nullptr};
    QAction* nodrawAction_{nullptr};
    // Filter Control (hammer/filtercontrol.cpp).
    QTreeWidget* visGroupTree_{nullptr};
    QTreeWidget* autoVisGroupTree_{nullptr};
    QPushButton* showAllVisGroupsButton_{nullptr};
    QPushButton* editVisGroupsButton_{nullptr};
    QPushButton* markVisGroupButton_{nullptr};
    QPushButton* moveVisGroupUpButton_{nullptr};
    QPushButton* moveVisGroupDownButton_{nullptr};
    // Guards rebuildVisGroupTree against its own itemChanged signals.
    bool updatingVisGroupTree_{false};
    // Coalesces the deferred rebuilds scheduleVisGroupTreeRebuild posts.
    bool visGroupTreeRebuildPending_{false};
    QComboBox* textureCombo_{nullptr};
    QLabel* texturePreview_{nullptr};
    QLabel* textureSizeLabel_{nullptr};
    QStringList mountedMaterialNames_;
    QAction* perspectiveProjectionAction_{nullptr};
    QAction* orthographicProjectionAction_{nullptr};
    QAction* texturedViewAction_{nullptr};
    QAction* shadedTexturedViewAction_{nullptr};
    QAction* shadedMaterialPolygonsViewAction_{nullptr};
    QAction* rayTracedPreviewAction_{nullptr};
    QAction* wireframeOverlayAction_{nullptr};
    QAction* hdrAction_{nullptr};
    QAction* undoRedoActiveAction_{nullptr};
    QAction* rayTracedGammaAction_{nullptr};
    QAction* displacementSolidMaskAction_{nullptr};
    bool displacementSolidMaskEnabled_{true};
    QMenu* toolTexturesMenu_{nullptr};
    QMenu* recentFilesMenu_{nullptr};
    QToolButton* mdiSystemButton_{nullptr};
    QMenu* mdiSystemMenu_{nullptr};
    QTimer* layoutRefreshTimer_{nullptr};

    QLabel* promptPane_{nullptr};
    QLabel* selectionPane_{nullptr};
    QLabel* coordinatesPane_{nullptr};
    QLabel* sizePane_{nullptr};
    QLabel* gridPane_{nullptr};
    QLabel* snapPane_{nullptr};

    std::shared_ptr<hammer::fgd::Database> fgd_;
    std::shared_ptr<hammer::assets::GameFileSystem> gameFileSystem_;
    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    hammer::vmf::ClipboardData clipboard_;
    // Paste Special reopens showing whatever was pasted last, as the original
    // dialog does. Session-scoped, like the real one — nothing is persisted.
    hammer::vmf::PasteSpecialOptions pasteSpecialOptions_;
    // Find Entities reopens showing the last name searched for.
    QString lastEntitySearch_;
    QString loadedFgdPath_;
    QString loadedGameInfoPath_;
    QString currentToolId_{QStringLiteral("tool.pointer")};
    MapViewWidget::TransformMode transformMode_{MapViewWidget::TransformMode::Scale};
    QAction* transformHandlesAction_{nullptr};
    bool gridSnapEnabled_{true};
    int gridSpacing_{16};
    QString primitiveName_{QStringLiteral("Block")};
    int primitiveFaces_{8};
    bool materialRenderingEnabled_{true};
    bool wireframeOverlayEnabled_{false};
    bool hdrEnabled_{true};
    bool undoRedoActive_{true};
    float rayTracedGamma_{2.2f};
    MapViewWidget::TexturedRenderMode texturedRenderMode_{MapViewWidget::TexturedRenderMode::Unlit};
    bool phongEnabled_{true};
    bool specularEnabled_{true};
    bool bumpMapsEnabled_{true};
    bool lightWarpEnabled_{true};
    bool selfIllumEnabled_{true};
    bool rimLightEnabled_{true};
    float phongIntensity_{1.0f};
    float specularIntensity_{1.0f};
    float bumpMapIntensity_{1.0f};
};
