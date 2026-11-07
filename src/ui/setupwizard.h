#pragma once
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// First-run configuration. Runs after the installer, and again whenever FFmpeg goes missing.
namespace Setup {
struct Status {
    QString ffmpeg, ffprobe, ffmpegNote;
    bool ffmpegOk = false, ffprobeOk = false, whisper = false, model = false, bridge = false, encoders = false;
    bool requiredOk() const { return ffmpegOk && ffprobeOk; }
};
Status check();
bool needed();   // first run, after an upgrade, or when FFmpeg has gone missing
void markDone(); // records that setup finished for this version
bool useFfmpegDir(const QString &dir);
// False means the app cannot run: only possible at startup, when the user left FFmpeg missing.
bool run(bool atStartup);
int selfTest(const QStringList &args);
}

// Fetches a pinned FFmpeg release into the app data folder. The executables are only unpacked once
// the archive matches the expected hash, because the editor runs them.
class FfmpegInstaller : public QObject {
    Q_OBJECT
public:
    FfmpegInstaller(const QString &url, const QByteArray &sha256Hex, QObject *p = nullptr);
    explicit FfmpegInstaller(QObject *p = nullptr);
    void start();
    void cancel();
signals:
    void progress(qint64 got, qint64 total);
    void finished();
    void failed(const QString &why);
private:
    void unpack();
    void fail(const QString &why);
    QString url;
    QByteArray sha;
    QString dir, zip, staging;
    QNetworkAccessManager *net;
    QNetworkReply *reply = nullptr;
    bool cancelled = false;
};
