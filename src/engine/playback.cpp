#include "playback.h"
#include "../core/mediacache.h"
#include <QMediaDevices>
#include <QThread>
#include <cmath>

qint64 MixDevice::readData(char *data, qint64 max)
{
    int frames = int(max / 8);
    if (frames <= 0) return 0;
    double r = rate.load();
    if (r == 1.0) {
        mixer->mix(reinterpret_cast<float *>(data), frames, pos.load());
        pos += frames;
    } else {
        // Shuttle: read proportionally more (or backwards) and resample by nearest neighbour.
        double ar = std::fabs(r);
        int src = qMax(1, int(frames * ar));
        tmp.resize(src * 2);
        qint64 start = r > 0 ? pos.load() : pos.load() - src;
        if (start < 0) start = 0;
        mixer->mix(tmp.data(), src, start);
        auto *o = reinterpret_cast<float *>(data);
        for (int i = 0; i < frames; ++i) {
            int k = int(i * ar);
            if (r < 0) k = src - 1 - k;
            k = qBound(0, k, src - 1);
            o[i * 2] = tmp[k * 2]; o[i * 2 + 1] = tmp[k * 2 + 1];
        }
        pos = qMax<qint64>(0, pos.load() + (r > 0 ? src : -src));
    }
    return frames * 8;
}

PlaybackEngine::PlaybackEngine(QObject *parent) : QObject(parent)
{
    dev = new MixDevice(&mixer, this);
    timer.setInterval(10);
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, &PlaybackEngine::tick);
    th = std::thread([this] { renderLoop(); });
}
PlaybackEngine::~PlaybackEngine()
{
    stopAudio();
    { std::lock_guard<std::mutex> l(m); quit = true; }
    cv.notify_all();
    space.notify_all();
    th.join();
}

void PlaybackEngine::setInput(const ProjectState &st, int sid)
{
    auto snap = std::make_shared<const ProjectState>(st);
    mixer.setState(snap, sid);
    {
        std::lock_guard<std::mutex> l(m);
        state = snap;
        bool changedSeq = seqId != sid;
        seqId = sid;
        dirty = true;
        if (auto q = snap->seqById(sid)) { fps = q->fps(); seqDur = q->duration(); }
        if (changedSeq) queue.clear();
        else if (isPlaying) { // drop queued frames rendered from the stale state
            queue.clear();
            nextRender = Frame(clockFrames());
        }
    }
    cv.notify_all();
    space.notify_all();
}

void PlaybackEngine::setScale(double s)
{
    { std::lock_guard<std::mutex> l(m); resScale = s; dirty = true; queue.clear(); }
    cv.notify_all();
}

void PlaybackEngine::setUseProxies(bool on)
{
    { std::lock_guard<std::mutex> l(m); proxiesOn = on; dirty = true; queue.clear(); }
    cv.notify_all();
}

void PlaybackEngine::seek(Frame f)
{
    if (isPlaying) pause();
    curFrame = f;
    { std::lock_guard<std::mutex> l(m); wantFrame = f; }
    cv.notify_all();
}

void PlaybackEngine::setRange(Frame first, Frame last, bool loop) { firstF = first; lastF = last; looping = loop; }

void PlaybackEngine::startAudio()
{
    QAudioFormat fmt;
    fmt.setSampleRate(MediaCache::kRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    QAudioDevice out = QMediaDevices::defaultAudioOutput();
    if (out.isNull() || !out.isFormatSupported(fmt)) return;
    sink = new QAudioSink(out, fmt, this);
    sink->setBufferSize(48000 * 8 / 10);
    dev->pos = qint64(startFrame / fps * MediaCache::kRate);
    dev->rate = playSpeed;
    mixer.reset();
    sink->start(dev);
}
void PlaybackEngine::stopAudio()
{
    if (sink) { sink->stop(); sink->deleteLater(); sink = nullptr; }
}


double PlaybackEngine::clockFrames() const
{
    double ms = double(elapsed.elapsed());
    if (sink && sink->state() != QAudio::StoppedState) {
        const double heard = sink->processedUSecs() / 1000.0;
        if (heard > 0) ms = heard;
        else if (ms < 2500) ms = 0; // the device can take most of a second to open
    }
    return startFrame + ms / 1000.0 * fps * playSpeed;
}

void PlaybackEngine::play(double speed)
{
    if (isPlaying) pause();
    playSpeed = speed;
    startFrame = curFrame;
    Frame endF = lastF >= 0 ? lastF : seqDur;
    if (speed > 0 && curFrame >= endF - 1) { curFrame = startFrame = firstF; }
    elapsed.restart();
    {
        std::lock_guard<std::mutex> l(m);
        queue.clear();
        nextRender = startFrame;
        clockA = double(startFrame);
        isPlayingA = true;
    }
    isPlaying = true;
    shown = dropped = 0;
    if (qFuzzyCompare(std::fabs(speed), 1.0) && speed > 0) startAudio();
    else if (std::fabs(speed) <= 4) startAudio();
    cv.notify_all();
    timer.start();
}

void PlaybackEngine::pause()
{
    if (!isPlaying) return;
    isPlaying = false;
    timer.stop();
    stopAudio();
    {
        std::lock_guard<std::mutex> l(m);
        isPlayingA = false;
        queue.clear();
        wantFrame = curFrame;
        doneFrame = -1;
    }
    cv.notify_all();
    space.notify_all();
    emit stopped();
}

void PlaybackEngine::tick()
{
    double cf = clockFrames();
    if (sink && sink->processedUSecs() > 0 && clockLead == 0) clockLead = elapsed.elapsed();
    clockA = cf;
    Frame pos = Frame(std::floor(cf));
    Frame endF = lastF >= 0 ? lastF : seqDur;
    if (playSpeed > 0 && pos >= endF) {
        if (looping) {
            pause();
            curFrame = firstF;
            play(playSpeed);
            return;
        }
        pos = qMax<Frame>(0, endF - 1);
        curFrame = pos;
        pause();
        emit positionChanged(pos);
        return;
    }
    if (playSpeed < 0 && pos <= 0) {
        curFrame = 0;
        pause();
        emit positionChanged(0);
        return;
    }
    if (pos != curFrame) {
        curFrame = pos;
        emit positionChanged(pos);
    }
    Q best{-1, {}};
    {
        std::lock_guard<std::mutex> l(m);
        while (!queue.empty()) {
            const Q &h = queue.front();
            bool ready = playSpeed > 0 ? h.f <= pos : h.f >= pos;
            if (!ready) break;
            if (best.f >= 0) ++dropped;
            best = h;
            queue.pop_front();
        }
    }
    if (best.f >= 0) {
        ++shown;
        dropRatio = double(dropped) / qMax(1, shown + dropped);
        lastImage = best.img;
        emit frameReady(best.img, best.f);
        space.notify_all();
    }
}

void PlaybackEngine::renderLoop()
{
    const int kQueue = 6;
    std::unique_lock<std::mutex> l(m);
    while (!quit) {
        if (isPlayingA) {
            space.wait_for(l, std::chrono::milliseconds(20), [&] { return quit || !isPlayingA || int(queue.size()) < kQueue; });
            if (quit) break;
            if (!isPlayingA || int(queue.size()) >= kQueue) continue;
            double sp = playSpeed;
            int dir = sp > 0 ? 1 : -1;
            Frame clock = Frame(std::floor(clockA.load()));
            Frame n = nextRender.load();
            if ((dir > 0 && n <= clock) || (dir < 0 && n >= clock)) n = clock + dir; // late: skip ahead to stay in sync
            auto snap = state;
            int sid = seqId;
            double sc = resScale;
            l.unlock();
            comp.useProxies = proxiesOn;
            QImage img = comp.render(*snap, sid, qMax<Frame>(0, n), sc, false);
            comp.preroll(*snap, sid, n, sc);
            l.lock();
            if (isPlayingA && snap == state) {
                queue.push_back({n, img});
                nextRender = n + Frame(dir * qMax(1.0, std::fabs(sp)));
            }
            continue;
        }
        cv.wait(l, [&] { return quit || isPlayingA || dirty || wantFrame != doneFrame; });
        if (quit) break;
        if (isPlayingA) continue;
        Frame f = wantFrame;
        dirty = false;
        doneFrame = f;
        auto snap = state;
        int sid = seqId;
        double sc = resScale;
        if (!snap) continue;
        l.unlock();
        comp.useProxies = proxiesOn;
        QImage img = comp.render(*snap, sid, f, sc, false);
        l.lock();
        if (wantFrame == f && !img.isNull()) {
            lastImage = img;
            QMetaObject::invokeMethod(this, [this, img, f] { emit frameReady(img, f); }, Qt::QueuedConnection);
        }
    }
    l.unlock();
    comp.releaseAll();
}
