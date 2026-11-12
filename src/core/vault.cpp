#include "vault.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace Vault {
static QString g_root;
static const int kFormatVersion = 1;

Options options()
{
    QSettings s;
    Options o;
    o.autosave = s.value("autosave/enabled", o.autosave).toBool();
    o.minutes = qBound(1, s.value("autosave/minutes", o.minutes).toInt(), 120);
    o.maxVersions = qBound(1, s.value("autosave/max", o.maxVersions).toInt(), 500);
    o.backup = s.value("autosave/backup", o.backup).toBool();
    o.recovery = s.value("autosave/recovery", o.recovery).toBool();
    o.reopenLast = s.value("autosave/reopenLast", o.reopenLast).toBool();
    return o;
}
void setOptions(const Options &o)
{
    QSettings s;
    s.setValue("autosave/enabled", o.autosave);
    s.setValue("autosave/minutes", o.minutes);
    s.setValue("autosave/max", o.maxVersions);
    s.setValue("autosave/backup", o.backup);
    s.setValue("autosave/recovery", o.recovery);
    s.setValue("autosave/reopenLast", o.reopenLast);
}

QString root() { return g_root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : g_root; }
void setRoot(const QString &d) { g_root = d; }

bool writeAtomic(const QString &path, const QByteArray &data, QString *err)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    if (f.write(data) != data.size() || !f.commit()) {
        if (err) *err = f.errorString();
        f.cancelWriting();
        return false;
    }
    return true;
}
qint64 freeBytes(const QString &path)
{
    QFileInfo fi(path);
    QStorageInfo st(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
    return st.isValid() ? st.bytesAvailable() : -1;
}

QByteArray encode(const ProjectState &s)
{
    QJsonObject o = stateToJson(s);
    o["app"] = QCoreApplication::applicationVersion();
    o["saved"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
LoadResult decode(const QByteArray &data, ProjectState *out, QString *savedBy, QDateTime *savedAt)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError) return LoadResult::Corrupt;
    if (!doc.isObject() || doc.object()["format"].toString() != "OpenEditorProject") return LoadResult::NotProject;
    const QJsonObject o = doc.object();
    if (savedBy) *savedBy = o["app"].toString();
    if (savedAt) *savedAt = QDateTime::fromString(o["saved"].toString(), Qt::ISODate);
    if (o["version"].toInt(1) > kFormatVersion) return LoadResult::Newer;
    if (out) *out = stateFromJson(o);
    return LoadResult::Ok;
}
LoadResult read(const QString &path, ProjectState *out, QString *savedBy, QDateTime *savedAt)
{
    QFile f(path);
    if (!f.exists()) return LoadResult::Missing;
    if (!f.open(QIODevice::ReadOnly)) return LoadResult::Unreadable;
    const QByteArray data = f.readAll();
    if (data.trimmed().isEmpty()) return LoadResult::Corrupt;
    return decode(data, out, savedBy, savedAt);
}
QString describe(LoadResult r)
{
    switch (r) {
    case LoadResult::Ok: return QStringLiteral("The project opened.");
    case LoadResult::Missing: return QStringLiteral("The file does not exist.");
    case LoadResult::Unreadable: return QStringLiteral("The file could not be read. It may be open in another program or you may not have permission.");
    case LoadResult::Corrupt: return QStringLiteral("The file is damaged or incomplete.");
    case LoadResult::NotProject: return QStringLiteral("This is not an Open Editor project.");
    case LoadResult::Newer: return QStringLiteral("This project was saved by a newer version of Open Editor.");
    }
    return {};
}

QString backupPath(const QString &p) { return p + ".bak"; }

bool saveProject(const QString &path, const ProjectState &s, bool keepBackup, QString *err)
{
    // Only a file that still opens is worth keeping as the backup.
    if (keepBackup && QFileInfo::exists(path) && read(path, nullptr) == LoadResult::Ok) {
        const QString bak = backupPath(path), tmp = bak + ".tmp";
        QFile::remove(tmp);
        if (QFile::copy(path, tmp)) {
            QFile::remove(bak);
            if (!QFile::rename(tmp, bak)) QFile::remove(tmp);
        }
    }
    return writeAtomic(path, encode(s), err);
}

// ------------------------------------------------------------------------------- versions
static QString safeName(QString n)
{
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    for (QChar &c : n) if (bad.contains(c) || c.unicode() < 32) c = '_';
    n = n.trimmed();
    return n.isEmpty() ? QStringLiteral("Untitled") : n.left(60);
}
QString versionDir(const QString &projectPath, const QString &name)
{
    // Two projects with the same file name in different folders keep separate histories.
    const QString key = projectPath.isEmpty() ? QStringLiteral("unsaved")
        : QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(projectPath).toLower().toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return root() + "/AutoSave/" + safeName(name) + "-" + key;
}
QString saveVersion(const ProjectState &s, const QString &projectPath, const QString &name, int maxVersions, QString *err)
{
    const QString dir = versionDir(projectPath, name);
    if (!QDir().mkpath(dir)) { if (err) *err = QStringLiteral("Cannot create %1").arg(dir); return {}; }
    const QByteArray data = encode(s);
    const qint64 free_ = freeBytes(dir);
    if (free_ >= 0 && free_ < data.size() * 2 + (8 << 20)) { if (err) *err = QStringLiteral("The disk holding the auto-save folder is almost full."); return {}; }
    // The time down to the millisecond: names sort in the order they were written and are never
    // reused, which is what pruning and the version list rely on.
    QString path;
    do {
        path = dir + "/" + safeName(name) + " " + QDateTime::currentDateTime().toString("yyyy-MM-dd HH.mm.ss.zzz") + ".oep";
        if (QFileInfo::exists(path)) QThread::msleep(1);
    } while (QFileInfo::exists(path));
    if (!writeAtomic(path, data, err)) return {};
    // "name stamp.oep" must come before "name stamp (002).oep", so compare without the extension.
    QStringList all = QDir(dir).entryList({"*.oep"}, QDir::Files);
    std::sort(all.begin(), all.end(), [](const QString &a, const QString &b) { return a.chopped(4) < b.chopped(4); });
    while (all.size() > qMax(1, maxVersions)) QFile::remove(dir + "/" + all.takeFirst());
    return path;
}
QVector<Version> versions(const QString &projectPath, const QString &name)
{
    QVector<Version> v;
    for (const QFileInfo &fi : QDir(versionDir(projectPath, name)).entryInfoList({"*.oep"}, QDir::Files))
        v << Version{fi.absoluteFilePath(), "autosave", fi.lastModified(), fi.size()};
    if (!projectPath.isEmpty()) {
        QFileInfo b(backupPath(projectPath));
        if (b.exists()) v << Version{b.absoluteFilePath(), "backup", b.lastModified(), b.size()};
    }
    std::sort(v.begin(), v.end(), [](const Version &a, const Version &b) { return a.time != b.time ? a.time > b.time : a.path.chopped(4) > b.path.chopped(4); });
    return v;
}
bool newestGood(const QString &projectPath, const QString &name, Version *out)
{
    for (const Version &v : versions(projectPath, name))
        if (read(v.path, nullptr) == LoadResult::Ok) { if (out) *out = v; return true; }
    return false;
}

// ------------------------------------------------------------------------------- sessions
QString Session::dir() { return root() + "/Sessions"; }
Session::Session() {}
Session::~Session() {}

bool Session::begin()
{
    if (lock) return true;
    QDir().mkpath(dir());
    sid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto l = std::make_shared<QLockFile>(dir() + "/" + sid + ".lock");
    l->setStaleLockTime(0);
    if (!l->tryLock(0)) return false;
    lock = l;
    writeInfo();
    return true;
}
void Session::writeInfo()
{
    if (!lock) return;
    const QJsonObject o{{"id", sid}, {"pid", double(QCoreApplication::applicationPid())}, {"project", projectPath}, {"name", name},
                        {"app", QCoreApplication::applicationVersion()}, {"time", QDateTime::currentDateTime().toString(Qt::ISODate)}};
    writeAtomic(dir() + "/" + sid + ".json", QJsonDocument(o).toJson(QJsonDocument::Compact));
}
void Session::setProject(const QString &p, const QString &n)
{
    if (p == projectPath && n == name) return;
    projectPath = p; name = n;
    writeInfo();
}
QString Session::recoverFile() const { return dir() + "/" + sid + ".recover.oep"; }
bool Session::writeRecoveryData(const QByteArray &data, QString *err)
{
    if (!lock) { if (err) *err = QStringLiteral("No session."); return false; }
    return writeAtomic(recoverFile(), data, err);
}
bool Session::writeRecovery(const ProjectState &s, QString *err) { return writeRecoveryData(encode(s), err); }
void Session::clearRecovery() { if (lock) QFile::remove(recoverFile()); }
void Session::end()
{
    if (!lock) return;
    QFile::remove(recoverFile());
    QFile::remove(dir() + "/" + sid + ".json");
    lock->unlock();
    lock.reset();
}

// A session is dead when its lock can be taken: QLockFile checks that the owning process is gone.
static bool sessionDead(const QString &id)
{
    QLockFile l(Session::dir() + "/" + id + ".lock");
    l.setStaleLockTime(0);
    if (!l.tryLock(0)) return false;
    l.unlock();
    return true;
}
static QJsonObject sessionInfo(const QString &id)
{
    QFile f(Session::dir() + "/" + id + ".json");
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}
QVector<Orphan> Session::orphans()
{
    QVector<Orphan> out;
    QDir d(dir());
    for (const QFileInfo &fi : d.entryInfoList({"*.json"}, QDir::Files)) {
        const QString id = fi.completeBaseName();
        if (!sessionDead(id)) continue;
        Orphan o;
        o.id = id;
        o.recoverFile = d.filePath(id + ".recover.oep");
        if (!QFileInfo::exists(o.recoverFile) || read(o.recoverFile, nullptr) != LoadResult::Ok) { discard(o); continue; }
        const QJsonObject j = sessionInfo(id);
        o.projectPath = j["project"].toString();
        o.name = j["name"].toString();
        o.appVersion = j["app"].toString();
        o.time = QFileInfo(o.recoverFile).lastModified();
        QFile cr(d.filePath(id + ".crashed"));
        if (cr.open(QIODevice::ReadOnly)) o.crashReport = QString::fromLocal8Bit(cr.readAll()).trimmed();
        out << o;
    }
    std::sort(out.begin(), out.end(), [](const Orphan &a, const Orphan &b) { return a.time > b.time; });
    return out;
}
void Session::discard(const Orphan &o)
{
    for (const char *ext : {".recover.oep", ".json", ".lock", ".crashed"}) QFile::remove(dir() + "/" + o.id + ext);
}
bool Session::openElsewhere(const QString &projectPath, const QString &exceptId)
{
    if (projectPath.isEmpty()) return false;
    const QString want = QDir::cleanPath(projectPath).toLower();
    for (const QFileInfo &fi : QDir(dir()).entryInfoList({"*.json"}, QDir::Files)) {
        const QString id = fi.completeBaseName();
        if (id == exceptId || sessionDead(id)) continue;
        if (QDir::cleanPath(sessionInfo(id)["project"].toString()).toLower() == want) return true;
    }
    return false;
}
} // namespace Vault

// ------------------------------------------------------------------------------- crash reports
namespace Crash {
static QString g_dir;
#ifdef Q_OS_WIN
// Everything the handler needs is prepared up front: a crashed process must not allocate.
static wchar_t g_dump[1024], g_text[1024], g_mark[1024];
static char g_head[512];

static void writeAll(const wchar_t *path, const char *text)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, text, DWORD(std::strlen(text)), &n, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}
static void report(const char *what, EXCEPTION_POINTERS *ep)
{
    static LONG once = 0;
    if (InterlockedExchange(&once, 1)) return;
    char buf[1024];
    if (ep && ep->ExceptionRecord)
        std::snprintf(buf, sizeof buf, "%s%s\r\nException 0x%08lX at address %p\r\n", g_head, what,
                      (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    else
        std::snprintf(buf, sizeof buf, "%s%s\r\n", g_head, what);
    writeAll(g_text, buf);
    if (g_mark[0]) {
        char m[1100];
        WideCharToMultiByte(CP_ACP, 0, g_text, -1, m, sizeof m, nullptr, nullptr);
        writeAll(g_mark, m);
    }
    HANDLE h = CreateFileW(g_dump, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(h);
    }
}
static LONG WINAPI onException(EXCEPTION_POINTERS *ep)
{
    report("The program stopped because of an unhandled exception.", ep);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void onAbort(int)
{
    report("The program aborted (fatal error or failed assertion).", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}
#endif

QString dir() { return g_dir; }

void install(const QString &d, const QString &version, const QString &sessionId)
{
    g_dir = d;
    QDir().mkpath(d);
    // Keep the newest 10 reports.
    QFileInfoList old = QDir(d).entryInfoList({"crash-*.txt"}, QDir::Files, QDir::Name);
    while (old.size() > 10) {
        const QString base = old.takeFirst().absoluteFilePath().chopped(4);
        QFile::remove(base + ".txt");
        QFile::remove(base + ".dmp");
    }
#ifdef Q_OS_WIN
    const QString base = QDir::toNativeSeparators(d + "/crash-" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + QString("-%1").arg(QCoreApplication::applicationPid()));
    (base + ".dmp").left(1000).toWCharArray(g_dump); g_dump[qMin(1000, base.size() + 4)] = 0;
    (base + ".txt").left(1000).toWCharArray(g_text); g_text[qMin(1000, base.size() + 4)] = 0;
    g_mark[0] = 0;
    if (!sessionId.isEmpty()) {
        const QString m = QDir::toNativeSeparators(Vault::Session::dir() + "/" + sessionId + ".crashed").left(1000);
        m.toWCharArray(g_mark); g_mark[m.size()] = 0;
    }
    std::snprintf(g_head, sizeof g_head, "Open Editor %s crash report\r\nStarted %s\r\n", qPrintable(version),
                  qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(onException);
    std::signal(SIGABRT, onAbort);
#else
    Q_UNUSED(version); Q_UNUSED(sessionId);
#endif
}
QStringList reports()
{
    QStringList l;
    for (const QFileInfo &fi : QDir(g_dir).entryInfoList({"crash-*.txt"}, QDir::Files, QDir::Name | QDir::Reversed)) l << fi.absoluteFilePath();
    return l;
}
void crashNow()
{
    volatile int *p = nullptr;
    *p = 42;
    std::abort();
}
}
