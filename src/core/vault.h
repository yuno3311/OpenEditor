// Everything that keeps work from being lost: atomic project writes with a backup of the previous
// save, a folder of timestamped auto-save versions per project, a recovery file for the running
// session that survives a crash or power cut, and crash reports with a minidump.
#pragma once
#include "model.h"
#include <QDateTime>
#include <QLockFile>
#include <QStringList>
#include <memory>

namespace Vault {
struct Options {
    bool autosave = true;
    int minutes = 3;        // time between auto-save versions
    int maxVersions = 20;   // per project, oldest removed first
    bool backup = true;     // keep the previous save as <project>.oep.bak
    bool recovery = true;   // write the session recovery file while there are unsaved changes
    bool reopenLast = false;
};
Options options();
void setOptions(const Options &o);

QString root();                       // the app data folder; tests point it elsewhere
void setRoot(const QString &dir);

// Writes through a temporary file and renames it into place, so a crash or full disk mid-write
// leaves the old file intact.
bool writeAtomic(const QString &path, const QByteArray &data, QString *err = nullptr);
qint64 freeBytes(const QString &path); // free space on the volume holding path, -1 if unknown

enum class LoadResult { Ok, Missing, Unreadable, Corrupt, NotProject, Newer };
QByteArray encode(const ProjectState &s);
LoadResult decode(const QByteArray &data, ProjectState *out, QString *savedBy = nullptr, QDateTime *savedAt = nullptr);
LoadResult read(const QString &path, ProjectState *out, QString *savedBy = nullptr, QDateTime *savedAt = nullptr);
QString describe(LoadResult r);

QString backupPath(const QString &projectPath);
bool saveProject(const QString &path, const ProjectState &s, bool keepBackup, QString *err = nullptr);

struct Version {
    QString path, kind; // kind: "autosave" or "backup"
    QDateTime time;
    qint64 size = 0;
};
QString versionDir(const QString &projectPath, const QString &name);
QString saveVersion(const ProjectState &s, const QString &projectPath, const QString &name, int maxVersions, QString *err = nullptr);
QVector<Version> versions(const QString &projectPath, const QString &name); // newest first, backup included
// The newest version that still opens, for when the project file itself is damaged.
bool newestGood(const QString &projectPath, const QString &name, Version *out);

struct Orphan {
    QString id, name, projectPath, recoverFile, appVersion, crashReport;
    QDateTime time;
};

// One running editor. While it lives it holds a lock; if the process dies without end(), the next
// start finds the session's recovery file and offers it back.
class Session {
public:
    Session();
    ~Session();
    bool begin();
    void setProject(const QString &projectPath, const QString &name);
    bool writeRecovery(const ProjectState &s, QString *err = nullptr);
    bool writeRecoveryData(const QByteArray &encoded, QString *err = nullptr);
    void clearRecovery();
    void end();                               // clean exit: nothing to recover
    QString id() const { return sid; }
    QString recoverFile() const;
    bool active() const { return bool(lock); }
    static QVector<Orphan> orphans();         // sessions that died with unsaved work, newest first
    static void discard(const Orphan &o);
    static bool openElsewhere(const QString &projectPath, const QString &exceptId = {});
    static QString dir();
private:
    void writeInfo();
    QString sid, projectPath, name;
    std::shared_ptr<QLockFile> lock;
};
}

namespace Crash {
// Installs handlers that write <dir>/crash-<time>.txt (and .dmp on Windows) when the process dies.
void install(const QString &dir, const QString &appVersion, const QString &sessionId = {});
QString dir();
QStringList reports();            // report text files, newest first
[[noreturn]] void crashNow();     // tests: a real access violation
}
