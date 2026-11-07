// Signal processing shared by the audio effects, loudness measurement and export.
#pragma once
#include <QVector>
#include <complex>

namespace Dsp {
void fft(std::complex<float> *a, int n, bool inverse); // n is a power of two

// ITU-R BS.1770-4 / EBU R128 integrated loudness of 48 kHz stereo. Feed any block sizes in order.
class LoudnessMeter {
public:
    LoudnessMeter();
    void add(const float *stereo, qint64 frames);
    double integrated() const; // LUFS, or -99 when there is nothing above the absolute gate
private:
    struct Stage { double b0, b1, b2, a1, a2, z1[2] = {0, 0}, z2[2] = {0, 0}; };
    Stage shelf, hp;
    double acc = 0;
    int accN = 0;
    QVector<double> sub; // mean square of each 100 ms sub-block
};
}

// Streaming STFT noise reduction for one channel. The noise floor in each frequency bin is tracked
// as a falling minimum that rises slowly, so steady hiss, hum and room tone are learned without a
// separate noise print, while speech, which keeps changing, passes. Delays the signal by kLatency.
class SpectralDenoiser {
public:
    static constexpr int kN = 512, kHop = 128, kBins = kN / 2 + 1, kLatency = kN;
    SpectralDenoiser();
    // reductionDb: deepest cut in noise-only bins. sensitivity: 1 gentle to 4 aggressive.
    float process(float x, float reductionDb, float sensitivity);
private:
    void frame(float reductionDb, float sensitivity);
    float in[kN] = {}, acc[kN] = {}, outQ[kHop] = {};
    float win[kN];
    // Minimum statistics: the noise floor is the lowest smoothed power over the last ~1 s, kept as
    // the minima of kBlocks sub-windows so it follows rising noise as well as falling noise.
    static constexpr int kBlocks = 6, kBlockFrames = 64;
    float power[kBins] = {}, slow[kBins] = {}, noise[kBins] = {}, gain[kBins];
    float blockMin[kBlocks][kBins], curMin[kBins];
    int blockFrame = 0, blockIdx = 0;
    int k = 0;
    bool primed = false;
};
