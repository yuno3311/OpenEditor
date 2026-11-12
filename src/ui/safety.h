// The running editor's safety net: timed auto-save versions, the crash recovery file, and the
// dialogs that give lost work back.
#pragma once
#include "../core/vault.h"
#include <QDateTime>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

class QWidget;

class SafetyNet : public QObject {
    Q_OBJECT
public:
    static SafetyNet *instance();
    void start();                              // begins the session and the timers; call once
    void shutdown();                           // clean exit: nothing to recover next time
    void applyOptions();                       // after Preferences change
    void setAutosaveIntervalMs(int ms) { autoTimer.start(ms); } // tests: do not wait minutes
    // Writes an auto-save version now. Skips an unchanged or clean project unless force is set.
    bool autosaveNow(bool force = false, QString *path = nullptr);
    bool writeRecoveryNow();                   // synchronous, for shutdown and tests
    void projectSwitched();                    // a project was opened, created or saved
    Vault::Session session;
    QString lastAutosave, lastError;
    QDateTime lastAutosaveTime;
    int recoveryDelayMs = 2000;

    // Recover / restore without the dialog (tests and tools use these too).
    bool recover(const Vault::Orphan &o, QString *err = nullptr);
    bool restoreVersion(const Vault::Version &v, QString *err = nullptr);
    // Dialogs
    bool offerRecovery(QWidget *parent);       // true if a project was recovered
    bool restoreVersionDialog(QWidget *parent);
    // When a project file will not open: offers the newest backup or auto-save that does.
    bool offerFallback(QWidget *parent, const QString &projectPath, Vault::LoadResult why);
signals:
    void autosaved(const QString &path);
    void problem(const QString &text);
private:
    explicit SafetyNet(QObject *p = nullptr);
    void scheduleRecovery();
    void recoveryTick();
    QTimer autoTimer, recoveryTimer;
    QFutureWatcher<bool> writer;
    quint64 autosavedRev = 0, recoveredRev = 0;
    bool started = false, again = false;
};
#define SAFETY (SafetyNet::instance())
