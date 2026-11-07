#include "pipeproc.h"
#include <QDir>
#ifdef Q_OS_WIN
#include <windows.h>

// A job object makes sure no ffmpeg child can outlive the editor, even after a crash.
static HANDLE sharedJob()
{
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info));
        return j;
    }();
    return job;
}

static QString quoteArg(const QString &a)
{
    if (!a.isEmpty() && !a.contains(' ') && !a.contains('"') && !a.contains('\t')) return a;
    QString r = "\"";
    int slashes = 0;
    for (QChar c : a) {
        if (c == '\\') { ++slashes; r += c; continue; }
        if (c == '"') { r += QString(slashes + 1, '\\'); }
        slashes = 0;
        r += c;
    }
    r += QString(slashes, '\\');
    return r + '"';
}

struct PipeProc::Impl {
    HANDLE proc = nullptr, rd = nullptr, wr = nullptr;
};
PipeProc::PipeProc() : d(new Impl) {}
PipeProc::~PipeProc() { kill(); delete d; }

bool PipeProc::start(const QString &program, const QStringList &args, bool wantStdin)
{
    kill();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, inR = nullptr, inW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 1 << 20)) return false;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (wantStdin) {
        if (!CreatePipe(&inR, &inW, &sa, 1 << 20)) return false;
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = wantStdin ? inR : nul;
    si.hStdOutput = outW;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    QString cmd = quoteArg(QDir::toNativeSeparators(program));
    for (auto &a : args) cmd += ' ' + quoteArg(a);
    std::wstring wcmd = cmd.toStdWString();
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outW);
    CloseHandle(nul);
    if (inR) CloseHandle(inR);
    if (!ok) {
        CloseHandle(outR);
        if (inW) CloseHandle(inW);
        return false;
    }
    AssignProcessToJobObject(sharedJob(), pi.hProcess);
    CloseHandle(pi.hThread);
    d->proc = pi.hProcess;
    d->rd = outR;
    d->wr = inW;
    return true;
}
bool PipeProc::read(void *buf, qint64 n)
{
    if (!d->rd) return false;
    char *p = static_cast<char *>(buf);
    while (n > 0) {
        DWORD got = 0;
        if (!ReadFile(d->rd, p, DWORD(qMin<qint64>(n, 1 << 24)), &got, nullptr) || got == 0) return false;
        p += got; n -= got;
    }
    return true;
}
bool PipeProc::write(const void *buf, qint64 n)
{
    if (!d->wr) return false;
    const char *p = static_cast<const char *>(buf);
    while (n > 0) {
        DWORD put = 0;
        if (!WriteFile(d->wr, p, DWORD(qMin<qint64>(n, 1 << 24)), &put, nullptr)) return false;
        p += put; n -= put;
    }
    return true;
}
void PipeProc::closeStdin()
{
    if (d->wr) { CloseHandle(d->wr); d->wr = nullptr; }
}
int PipeProc::wait()
{
    if (!d->proc) return -1;
    WaitForSingleObject(d->proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(d->proc, &code);
    return int(code);
}
void PipeProc::kill()
{
    if (d->proc) {
        TerminateProcess(d->proc, 1);
        CloseHandle(d->proc);
        d->proc = nullptr;
    }
    if (d->rd) { CloseHandle(d->rd); d->rd = nullptr; }
    closeStdin();
}
bool PipeProc::running() const { return d->proc && WaitForSingleObject(d->proc, 0) == WAIT_TIMEOUT; }

#else
#include <QProcess>
struct PipeProc::Impl { QProcess p; };
PipeProc::PipeProc() : d(new Impl) {}
PipeProc::~PipeProc() { kill(); delete d; }
bool PipeProc::start(const QString &program, const QStringList &args, bool)
{
    d->p.setProcessChannelMode(QProcess::SeparateChannels);
    d->p.start(program, args);
    return d->p.waitForStarted(5000);
}
bool PipeProc::read(void *buf, qint64 n)
{
    char *p = static_cast<char *>(buf);
    while (n > 0) {
        if (!d->p.bytesAvailable() && !d->p.waitForReadyRead(5000)) return false;
        qint64 got = d->p.read(p, n);
        if (got <= 0) return false;
        p += got; n -= got;
    }
    return true;
}
bool PipeProc::write(const void *buf, qint64 n)
{
    d->p.write(static_cast<const char *>(buf), n);
    return d->p.waitForBytesWritten(30000);
}
void PipeProc::closeStdin() { d->p.closeWriteChannel(); }
int PipeProc::wait() { d->p.waitForFinished(-1); return d->p.exitCode(); }
void PipeProc::kill() { if (d->p.state() != QProcess::NotRunning) { d->p.kill(); d->p.waitForFinished(1000); } }
bool PipeProc::running() const { return d->p.state() == QProcess::Running; }
#endif
