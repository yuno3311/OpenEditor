// Offline render: runs the preview compositor and mixer at full quality and pipes to ffmpeg.
#pragma once
#include "../core/model.h"
#include <QJsonObject>
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
    bool chapters = false;         // write sequence markers as chapters (MP4, MOV, MKV, WebM)
};

struct ExportFormat {
    QString name, ext;
    bool video, audio;
    QStringList videoArgs;
    QStringList audioArgs;
};
const QVector<ExportFormat> &exportFormats();
bool isImageSequence(const QString &formatName);
// Where frame n of an image sequence export goes: <name>_00001.png beside the chosen path.
QString imageSequencePattern(const QString &path);

// A named bundle of export settings the user saved from the Export page.
namespace ExportPresets {
QStringList names();
bool load(const QString &name, ExportSettings *out);
void save(const QString &name, const ExportSettings &s);
void remove(const QString &name);
QJsonObject toJson(const ExportSettings &s);
ExportSettings fromJson(const QJsonObject &o, ExportSettings base = {});
}

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
