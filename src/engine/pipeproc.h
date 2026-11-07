// Minimal child process with a blocking stdout pipe. QProcess buffers without bound on Windows,
// which would defeat the decoder's back-pressure; a raw pipe blocks ffmpeg when we stop reading.
#pragma once
#include <QString>
#include <QStringList>

class PipeProc {
public:
    PipeProc();
    ~PipeProc();
    bool start(const QString &program, const QStringList &args, bool wantStdin = false);
    bool read(void *buf, qint64 n);   // blocks until n bytes arrived; false on EOF
    bool write(const void *buf, qint64 n);
    void closeStdin();
    int wait();                       // wait for exit, returns exit code
    void kill();
    bool running() const;
private:
    struct Impl;
    Impl *d;
};
