// Transport for one viewer. Audio is the master clock; a render thread keeps a short queue of
// composited frames ahead of it and drops frames rather than letting picture fall behind sound.
#pragma once
#include "audiomix.h"
#include "compositor.h"
#include <QAudioFormat>
#include <QAudioSink>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class MixDevice : public QIODevice {
    Q_OBJECT
public:
    MixDevice(AudioMixer *m, QObject *p = nullptr) : QIODevice(p), mixer(m) { open(ReadOnly); }
    std::atomic<qint64> pos{0};
    std::atomic<double> rate{1};
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (qint64(1) << 24) + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 max) override;
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    AudioMixer *mixer;
    QVector<float> tmp;
};

class PlaybackEngine : public QObject {
    Q_OBJECT
public:
    explicit PlaybackEngine(QObject *parent = nullptr);
    ~PlaybackEngine() override;
    void setInput(const ProjectState &st, int seqId);
    void setScale(double s);
    void setUseProxies(bool on);
    double scale() const { return resScale; }
    void seek(Frame f);
    void play(double speed = 1);
    void pause();
    bool playing() const { return isPlaying; }
    double speed() const { return playSpeed; }
    Frame position() const { return curFrame; }
    void setRange(Frame first, Frame last, bool loop); // playback bounds, last<0 = end of sequence
    AudioMixer mixer;
    QImage currentImage() const { return lastImage; }
    double droppedRatio() const { return dropRatio; }
    double audioClockLeadMs() const { return clockLead; }
signals:
    void frameReady(const QImage &img, Frame f);
    void positionChanged(Frame f);
    void stopped();
private:
    void renderLoop();
    void tick();
    void startAudio();
    void stopAudio();
    double clockFrames() const;
    Compositor comp;
    std::thread th;
    std::mutex m;
    std::condition_variable cv, space;
    std::shared_ptr<const ProjectState> state;
    int seqId = 0;
    double resScale = .5;
    bool quit = false, dirty = true;
    Frame wantFrame = 0, doneFrame = -1;
    std::atomic<bool> isPlayingA{false};
    std::atomic<bool> proxiesOn{false};
    std::atomic<Frame> nextRender{0};
    std::atomic<double> clockA{0};
    struct Q { Frame f; QImage img; };
    std::deque<Q> queue;
    bool isPlaying = false;
    double playSpeed = 1;
    Frame curFrame = 0, startFrame = 0, firstF = 0, lastF = -1;
    bool looping = false;
    double fps = 30;
    Frame seqDur = 0;
    QTimer timer;
    QElapsedTimer elapsed;
    QPointer<QAudioSink> sink;
    MixDevice *dev = nullptr;
    QImage lastImage;
    double dropRatio = 0;
    double clockLead = 0;
    int shown = 0, dropped = 0;
};
