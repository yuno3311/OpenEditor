// OpenGL 3.3 implementation of the per-pixel effects. Consecutive supported effects share one
// upload and readback. Anything unsupported, or any GL failure, falls back to the CPU code path, so
// a machine without a usable GPU renders the same picture, only slower.
#pragma once
#include "imagefx.h"
#include <atomic>
#include <memory>

class GpuFx {
public:
    static void prepareOnMainThread();          // the offscreen surface must come from the GUI thread
    static std::atomic<bool> enabled;
    static QString rendererName();
    static bool supports(const Effect &e);
    GpuFx();
    ~GpuFx();
    bool ok() const;
    // Runs effects in order on img. Returns false, leaving img unchanged, if the GPU path failed.
    bool applyChain(const QVector<const Effect *> &chain, QImage &img, const ImageFx::Ctx &c);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
