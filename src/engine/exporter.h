// Offline render: runs the preview compositor and mixer at full quality and pipes to ffmpeg.
#pragma once
#include "../core/model.h"
#include <QStringList>
#include <QThread>
#include <atomic>

struct ExportSettings {
    QString path;
    QString format = "H.264";
    int width = 1920, height = 1080;
    int crf = 20;
    int kbps = 0;                  // 0 = quality (CRF) mode
    QString preset = "medium";
    int audioKbps = 320;
    bool video = true, audio = true;
    Frame first = 0, last = -1;
    bool sidecarSrt = false;
    double loudnessLufs = 0;       // below zero: normalise the mix to this integrated loudness
};

struct ExportFormat {
    QString name, ext;
    bool video, audio;
    QStringList videoArgs;
    QStringList audioArgs;
};
const QVector<ExportFormat> &exportFormats();

class Exporter : public QThread {
    Q_OBJECT
public:
    Exporter(const ProjectState &st, int seqId, const ExportSettings &s, QObject *p = nullptr);
    void cancel() { stop = true; }
    double measuredLufs = -99, appliedGainDb = 0; // set when a loudness target was given
signals:
    void progress(double fraction, const QString &status);
    void finishedOk(const QString &path);
    void failed(const QString &why);
protected:
    void run() override;
private:
    bool normalizeLoudness(const QString &f32Path);
    ProjectState st;
    int seqId;
    ExportSettings cfg;
    std::atomic<bool> stop{false};
};
