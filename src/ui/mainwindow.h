#pragma once
#include "assistant.h"
#include "captionspanel.h"
#include "colorpanel.h"
#include "effectcontrols.h"
#include "monitor.h"
#include "pages.h"
#include "panels.h"
#include "projectpanel.h"
#include "sidepanels.h"
#include "timeline.h"
#include <QActionGroup>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    enum Page { PageHome, PageImport, PageEdit, PageExport };
    void showPage(Page p);
    void setWorkspace(const QString &name);
    // Exposed for the UI smoke test
    TimelinePanel *timeline;
    MonitorPanel *source, *program;
    ProjectPanel *project;
    PanelManager *panels;
    ScopesPanel *scopes;
    CaptionsPanel *captions;
    MulticamPanel *multicam;
    AssistantPanel *assistant;
    QWidget *pageHost() const { return stack; }
protected:
    void closeEvent(QCloseEvent *e) override;
private:
    void buildHeader(class QVBoxLayout *v);
    void buildMenus();
    QAction *act(QMenu *m, const QString &text, const QString &key, const std::function<void()> &fn);
    MonitorPanel *activeMonitor() const;
    bool confirmClose();
    bool saveProject(bool as);
    void openProject(const QString &path = {});
    void newProject();
    void addRecent(const QString &path);
    void updateTitle();
    void shortcutsDialog();
    void prefsDialog();
    void buildAiMenu(QMenu *m);
    void runTool(const QString &name, const QJsonObject &args);
    void setProxies(bool on);
    void createMulticam();
    void mcpDialog();
    void silenceDialog();
    void duckDialog();
    void loudnessDialog();
    void watchJob(const QJsonObject &startResult, const QString &label);
    QList<int> selectedClipIds() const;
    QAction *proxyAction = nullptr;
    void nextMarker(bool fwd);
    void loadShortcuts();
    QStackedWidget *stack;
    HomePage *home;
    ImportPage *importPage;
    ExportPage *exportPage;
    QWidget *editor;
    QWidget *header;
    QHash<QString, QPushButton *> pageTabs, wsTabs;
    QLabel *titleLabel, *jobLabel;
    QString workspace;
    QHash<QString, QString> wsLayouts;
    QList<QAction *> allActions;
};
