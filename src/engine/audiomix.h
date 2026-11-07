// Sample-accurate stereo mixer over conformed PCM: clip fx, keyframed gain, crossfades, track and
// master faders, and meter tapping. The same mix() call serves playback and export.
#pragma once
#include "../core/model.h"
#include <QHash>
#include <QMutex>
#include <array>
#include <atomic>
#include <memory>

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    inline float run(int ch, float x)
    {
        double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return float(y);
    }
};

struct FxState {
    Biquad b[3];
    QVector<float> delay;
    int dpos = 0;
    float env = 0;
    QVector<float> comb[4][2];
    int cpos[4] = {0, 0, 0, 0};
    float lp[4][2] = {};
    std::shared_ptr<void> plugin;
};

class AudioMixer {
public:
    static constexpr int kMaxTracks = 32;
    void setState(std::shared_ptr<const ProjectState> st, int seqId);
    void reset();
    void mix(float *out, int frames, qint64 pos); // pos in samples at 48 kHz; out is stereo interleaved
    std::atomic<float> masterPeak[2]{};
    std::atomic<float> trackPeak[kMaxTracks][2]{};
    std::atomic<int> trackIds[kMaxTracks]{};
    bool tapMeters = true;
private:
    void mixSeq(const ProjectState &st, const Sequence &q, float *out, int frames, qint64 pos, int depth, bool meters, int solo = -1);
    void mixClip(const ProjectState &st, const Sequence &q, const Track &t, const Clip &c, float *out, int frames, qint64 pos, int depth);
    void runFx(const Effect &e, quint64 key, float *buf, int frames, Frame kt);
    QMutex mx;
    std::shared_ptr<const ProjectState> state;
    int seqId = 0;
    QHash<quint64, FxState> fx;
    QVector<float> scratchClip;
};
