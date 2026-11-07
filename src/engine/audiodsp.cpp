#include "audiodsp.h"
#include <QtMath>
#include <algorithm>
#include <cmath>

void Dsp::fft(std::complex<float> *a, int n, bool inverse)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / len * (inverse ? 1 : -1);
        const std::complex<float> wl(float(std::cos(ang)), float(std::sin(ang)));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (int j = 0; j < len / 2; ++j) {
                std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; ++i) a[i] /= float(n);
}

// K-weighting coefficients for 48 kHz, from BS.1770-4.
Dsp::LoudnessMeter::LoudnessMeter()
    : shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585},
      hp{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621}
{
}

void Dsp::LoudnessMeter::add(const float *s, qint64 frames)
{
    auto run = [](Stage &q, int c, double x) {
        double y = q.b0 * x + q.z1[c];
        q.z1[c] = q.b1 * x - q.a1 * y + q.z2[c];
        q.z2[c] = q.b2 * x - q.a2 * y;
        return y;
    };
    for (qint64 i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            double y = run(hp, c, run(shelf, c, s[i * 2 + c]));
            acc += y * y;
        }
        if (++accN == 4800) { sub << acc / accN; acc = 0; accN = 0; }
    }
}

double Dsp::LoudnessMeter::integrated() const
{
    // 400 ms blocks with 75 % overlap are four consecutive 100 ms sub-blocks.
    QVector<double> blocks;
    for (int i = 0; i + 4 <= sub.size(); ++i) blocks << (sub[i] + sub[i + 1] + sub[i + 2] + sub[i + 3]) / 4;
    auto lufs = [](double ms) { return -0.691 + 10 * std::log10(qMax(ms, 1e-12)); };
    double sum = 0;
    int n = 0;
    for (double b : blocks) if (lufs(b) > -70) { sum += b; ++n; }
    if (!n) return -99;
    const double rel = lufs(sum / n) - 10;
    sum = 0; n = 0;
    for (double b : blocks) if (lufs(b) > -70 && lufs(b) > rel) { sum += b; ++n; }
    return n ? lufs(sum / n) : -99;
}

SpectralDenoiser::SpectralDenoiser()
{
    // Hann analysis and synthesis at 75 % overlap sum to 1.5; folding that in keeps unity gain.
    for (int i = 0; i < kN; ++i) win[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / kN)) / std::sqrt(1.5f);
    std::fill(gain, gain + kBins, 1.f);
    std::fill(curMin, curMin + kBins, 1e30f);
    for (auto &b : blockMin) std::fill(b, b + kBins, 1e30f);
}

float SpectralDenoiser::process(float x, float reductionDb, float sensitivity)
{
    const float y = outQ[k];
    in[kN - kHop + k] = x;
    if (++k == kHop) {
        frame(reductionDb, sensitivity);
        k = 0;
    }
    return y;
}

void SpectralDenoiser::frame(float reductionDb, float sensitivity)
{
    std::complex<float> X[kN];
    for (int i = 0; i < kN; ++i) X[i] = in[i] * win[i];
    Dsp::fft(X, kN, false);
    const float floorG = std::pow(10.f, -qMax(0.f, reductionDb) / 20.f);
    const bool newBlock = ++blockFrame >= kBlockFrames;
    for (int b = 0; b < kBins; ++b) {
        const float p = std::norm(X[b]);
        power[b] = primed ? 0.6f * power[b] + 0.4f * p : p;
        slow[b] = primed ? 0.95f * slow[b] + 0.05f * p : p;
        curMin[b] = qMin(curMin[b], slow[b]);
        float m = curMin[b];
        for (auto &blk : blockMin) m = qMin(m, blk[b]);
        noise[b] = m;
        if (newBlock) { blockMin[blockIdx][b] = curMin[b]; curMin[b] = slow[b]; }
        // The minimum of a smoothed noise power sits about 2 dB under its mean; 1.6 puts it back.
        // Wiener-style gain on the smoothed power; opening fast and closing slowly avoids warble.
        float g = power[b] > 1e-20f ? 1 - sensitivity * 1.6f * noise[b] / power[b] : 0.f;
        g = qMax(floorG, g);
        gain[b] = g > gain[b] ? g : 0.75f * gain[b] + 0.25f * g;
    }
    primed = true;
    if (newBlock) { blockFrame = 0; blockIdx = (blockIdx + 1) % kBlocks; }
    // Neighbouring bins share a little of their gain so isolated bins do not sing.
    float sm[kBins];
    for (int b = 0; b < kBins; ++b) sm[b] = (gain[qMax(0, b - 1)] + 2 * gain[b] + gain[qMin(kBins - 1, b + 1)]) / 4;
    for (int b = 0; b < kBins; ++b) {
        X[b] *= sm[b];
        if (b > 0 && b < kN / 2) X[kN - b] = std::conj(X[b]);
    }
    Dsp::fft(X, kN, true);
    for (int i = 0; i < kN; ++i) acc[i] += X[i].real() * win[i];
    std::copy(acc, acc + kHop, outQ);
    std::move(acc + kHop, acc + kN, acc);
    std::fill(acc + kN - kHop, acc + kN, 0.f);
    std::move(in + kHop, in + kN, in);
}
