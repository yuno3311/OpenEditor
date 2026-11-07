#include "setupwizard.h"
#include "../core/mediacache.h"
#include "../engine/analysis.h"
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWizard>
#include <cstdio>

// A build known to include the whisper filter. Updating it means updating the hash too.
static const char *const kFfmpegFolder = "ffmpeg-9.0.2-full_build";
static const char *const kFfmpegUrl = "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-full_build.zip";
static const char *const kFfmpegSha256 = "759d0a9831c436a0eb331ad36f236c06cb04aaa0005da46571f0e9d3d9206f6b";
static const int kFfmpegMB = 246;
static const int kFfmpegMinMajor = 8;

static bool runTool(const QString &exe, const QStringList &args, QString *out)
{
    if (exe.isEmpty()) return false;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, args);
    if (!p.waitForFinished(30000)) { p.kill(); p.waitForFinished(1000); return false; }
    *out = QString::fromUtf8(p.readAll());
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

Setup::Status Setup::check()
{
    Status s;
    s.ffmpeg = CACHE->ffmpeg();
    s.ffprobe = CACHE->ffprobe();
    QString out;
    if (s.ffmpeg.isEmpty()) {
        s.ffmpegNote = QObject::tr("Not found");
    } else if (!runTool(s.ffmpeg, {"-hide_banner", "-version"}, &out)) {
        s.ffmpegNote = QObject::tr("Found, but it does not run: %1").arg(QDir::toNativeSeparators(s.ffmpeg));
    } else {
        // Development builds report a git revision instead of a number, and are recent by nature.
        auto m = QRegularExpression("version n?(\\d+)\\.").match(out);
        if (m.hasMatch() && m.captured(1).toInt() < kFfmpegMinMajor) s.ffmpegNote = QObject::tr("Version %1 is too old: %2 or newer is required").arg(m.captured(1)).arg(kFfmpegMinMajor);
        else s.ffmpegOk = true;
    }
    s.ffprobeOk = runTool(s.ffprobe, {"-hide_banner", "-version"}, &out);
    s.whisper = s.ffmpegOk && runTool(s.ffmpeg, {"-hide_banner", "-filters"}, &out) && out.contains(QRegularExpression("\\swhisper\\s"));
    s.encoders = s.ffmpegOk && runTool(s.ffmpeg, {"-hide_banner", "-encoders"}, &out) && out.contains(QRegularExpression("\\slibx264\\s")) && out.contains(QRegularExpression("\\saac\\s"));
    for (auto &n : Analysis::whisperModels()) s.model |= QFileInfo::exists(Analysis::whisperModelPath(n));
    s.bridge = QFileInfo::exists(QCoreApplication::applicationDirPath() + "/oe-mcp.exe");
    return s;
}

bool Setup::needed()
{
    QSettings s;
    return !s.value("setup/done", false).toBool() || s.value("setup/version").toString() != QCoreApplication::applicationVersion()
        || CACHE->ffmpeg().isEmpty() || CACHE->ffprobe().isEmpty();
}

void Setup::markDone()
{
    QSettings s;
    s.setValue("setup/done", true);
    s.setValue("setup/version", QCoreApplication::applicationVersion());
}

bool Setup::useFfmpegDir(const QString &dir)
{
    if (!QFileInfo::exists(dir + "/ffmpeg.exe") || !QFileInfo::exists(dir + "/ffprobe.exe")) return false;
    QSettings().setValue("tools/ffmpegDir", dir);
    CACHE->reloadTools();
    return true;
}

FfmpegInstaller::FfmpegInstaller(const QString &u, const QByteArray &s, QObject *p)
    : QObject(p), url(u), sha(s), dir(MediaCache::downloadedToolsDir()), zip(dir + "/download.zip"), staging(dir + "/unpacking"), net(new QNetworkAccessManager(this))
{
    net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}
FfmpegInstaller::FfmpegInstaller(QObject *p) : FfmpegInstaller(kFfmpegUrl, kFfmpegSha256, p) {}

void FfmpegInstaller::fail(const QString &why)
{
    QFile::remove(zip);
    QDir(staging).removeRecursively();
    emit failed(why);
}

void FfmpegInstaller::cancel()
{
    cancelled = true;
    if (reply) reply->abort();
}

void FfmpegInstaller::start()
{
    QDir().mkpath(dir);
    auto *out = new QFile(zip, this);
    if (!out->open(QIODevice::WriteOnly)) { emit failed(tr("Cannot write %1").arg(QDir::toNativeSeparators(zip))); return; }
    QNetworkRequest rq{QUrl(url)};
    rq.setTransferTimeout(60000);
    reply = net->get(rq);
    auto *hash = new QCryptographicHash(QCryptographicHash::Sha256);
    connect(reply, &QNetworkReply::readyRead, this, [this, out, hash] { QByteArray b = reply->readAll(); hash->addData(b); out->write(b); });
    connect(reply, &QNetworkReply::downloadProgress, this, &FfmpegInstaller::progress);
    connect(reply, &QNetworkReply::finished, this, [this, out, hash] {
        QByteArray rest = reply->readAll();
        hash->addData(rest);
        out->write(rest);
        const bool wrote = out->error() == QFile::NoError;
        out->close();
        out->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString err = reply->errorString();
        const QByteArray got = hash->result().toHex();
        delete hash;
        reply->deleteLater();
        reply = nullptr;
        if (cancelled) fail(tr("Download cancelled."));
        else if (!ok) fail(err);
        else if (!wrote) fail(tr("Could not save the download. Check the free space on this drive."));
        else if (got != sha) fail(tr("The download does not match the expected FFmpeg release, so it was discarded."));
        else unpack();
    });
}

void FfmpegInstaller::unpack()
{
    QDir(staging).removeRecursively();
    QDir().mkpath(staging);
    // The tar in System32 reads zip archives. Git and MSYS install other tars that do not.
    const QString tar = qEnvironmentVariable("SystemRoot") + "/System32/tar.exe";
    const QString root = QString(kFfmpegFolder) + "/bin/";
    auto *p = new QProcess(this);
    p->setProgram(tar);
    p->setArguments({"-xf", QDir::toNativeSeparators(zip), "--strip-components=2", "-C", QDir::toNativeSeparators(staging), root + "ffmpeg.exe", root + "ffprobe.exe"});
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater();
        fail(tr("Could not start the Windows tar tool to unpack FFmpeg."));
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        p->deleteLater();
        if (st != QProcess::NormalExit || code != 0) { fail(tr("Could not unpack FFmpeg.")); return; }
        for (const char *n : {"ffmpeg.exe", "ffprobe.exe"}) {
            QFile::remove(dir + "/" + n);
            if (!QFile::rename(staging + "/" + n, dir + "/" + n)) { fail(tr("Could not install %1. It may be in use.").arg(n)); return; }
        }
        QFile::remove(zip);
        QDir(staging).removeRecursively();
        // A folder picked earlier would otherwise keep winning over this copy.
        QSettings().remove("tools/ffmpegDir");
        CACHE->reloadTools();
        emit finished();
    });
    p->start();
}

namespace {

const char *const kGood = "#5fbf6f", *const kBad = "#e5534b", *const kWarn = "#d6a93a";

QString mbProgress(qint64 got, qint64 total)
{
    return QObject::tr("Downloading: %1 of %2 MB").arg(got >> 20).arg(total >> 20);
}

QString heading(const QString &t) { return "<span style='font-size:14pt;font-weight:600'>" + t + "</span>"; }

class DepsPage : public QWizardPage {
public:
    DepsPage()
    {
        setTitle(heading(tr("Check what Open Editor needs")));
        auto *v = new QVBoxLayout(this);
        banner = new QLabel(this);
        banner->setWordWrap(true);
        v->addWidget(banner);
        auto *g = new QGridLayout;
        g->setColumnStretch(2, 1);
        g->setHorizontalSpacing(14);
        g->setVerticalSpacing(8);
        for (int i = 0; i < 6; ++i) {
            for (int c = 0; c < 3; ++c) { rows[i][c] = new QLabel(this); g->addWidget(rows[i][c], i, c); }
            rows[i][2]->setWordWrap(true);
            rows[i][2]->setStyleSheet("color:#8a8a8a;");
        }
        v->addLayout(g);
        v->addSpacing(8);
        auto *h = new QHBoxLayout;
        download = new QPushButton(tr("Download FFmpeg"), this);
        download->setProperty("primary", true);
        locate = new QPushButton(tr("Locate FFmpeg..."), this);
        recheck = new QPushButton(tr("Check Again"), this);
        stop = new QPushButton(tr("Cancel Download"), this);
        stop->hide();
        h->addWidget(download);
        h->addWidget(locate);
        h->addWidget(recheck);
        h->addWidget(stop);
        h->addStretch(1);
        v->addLayout(h);
        bar = new QProgressBar(this);
        bar->setRange(0, 1000);
        bar->setTextVisible(false);
        bar->hide();
        v->addWidget(bar);
        note = new QLabel(tr("Download gets FFmpeg 9.0.2 (%1 MB) from github.com. Locate uses a folder that already has ffmpeg.exe and ffprobe.exe.").arg(kFfmpegMB), this);
        note->setWordWrap(true);
        note->setStyleSheet("color:#8a8a8a;");
        v->addWidget(note);
        v->addStretch(1);
        connect(download, &QPushButton::clicked, this, [this] { startDownload(); });
        connect(locate, &QPushButton::clicked, this, [this] { chooseFolder(); });
        connect(recheck, &QPushButton::clicked, this, [this] { refresh(); });
        connect(stop, &QPushButton::clicked, this, [this] { if (job) job->cancel(); });
    }
    void initializePage() override { refresh(); }
    bool isComplete() const override { return st.requiredOk() && !job; }

    void row(int i, const QString &name, bool ok, bool required, const QString &detail)
    {
        rows[i][0]->setText(name);
        rows[i][1]->setText(QString("<span style='color:%1'>%2</span>").arg(ok ? kGood : required ? kBad : kWarn, ok ? tr("Ready") : required ? tr("Missing (required)") : tr("Missing (optional)")));
        rows[i][2]->setText(detail);
    }
    void refresh()
    {
        CACHE->reloadTools();
        st = Setup::check();
        auto path = [](const QString &p) { return QDir::toNativeSeparators(p); };
        row(0, "FFmpeg", st.ffmpegOk, true, st.ffmpegOk ? path(st.ffmpeg) : st.ffmpegNote);
        row(1, "FFprobe", st.ffprobeOk, true, st.ffprobeOk ? path(st.ffprobe) : tr("Not found"));
        row(2, tr("Whisper in FFmpeg"), st.whisper, false, st.whisper ? tr("Transcription is available") : tr("Without it, transcription does not work. Download FFmpeg to get it."));
        row(3, tr("Speech model"), st.model, false, st.model ? tr("Installed") : tr("Choose one on the next page"));
        row(4, tr("MCP bridge"), st.bridge, false, st.bridge ? tr("Installed") : tr("oe-mcp.exe is missing. Reinstall Open Editor if you use MCP clients."));
        row(5, tr("Export encoders"), st.encoders, false, st.encoders ? tr("H.264 and AAC are available") : tr("This FFmpeg lacks libx264 or AAC, so the default H.264 export fails. Download FFmpeg to fix it."));
        int missing = !st.ffmpegOk + !st.ffprobeOk;
        banner->setText(missing ? tr("<span style='color:%1'><b>%2 required item(s) missing.</b></span> Open Editor cannot import, play or export media until FFmpeg and FFprobe are ready. Download them or add them below to continue.").arg(kBad).arg(missing)
                                : tr("<span style='color:%1'><b>Everything required is ready.</b></span>").arg(kGood));
        download->setVisible(!job);
        locate->setVisible(!job);
        recheck->setVisible(!job);
        stop->setVisible(job);
        emit completeChanged();
    }
    void chooseFolder()
    {
        QString d = QFileDialog::getExistingDirectory(this, tr("Folder with ffmpeg.exe and ffprobe.exe"));
        if (d.isEmpty()) return;
        if (!Setup::useFfmpegDir(d)) { QMessageBox::warning(this, tr("Locate FFmpeg"), tr("That folder needs both ffmpeg.exe and ffprobe.exe.")); return; }
        refresh();
    }
    void startDownload()
    {
        job = new FfmpegInstaller(this);
        bar->setValue(0);
        bar->show();
        connect(job, &FfmpegInstaller::progress, this, [this](qint64 g, qint64 t) {
            if (t > 0) { bar->setValue(int(g * 1000 / t)); note->setText(mbProgress(g, t)); }
            if (t > 0 && g >= t) note->setText(tr("Unpacking FFmpeg..."));
        });
        connect(job, &FfmpegInstaller::finished, this, [this] { done(tr("FFmpeg is installed.")); });
        connect(job, &FfmpegInstaller::failed, this, [this](const QString &e) { done(tr("Download failed: %1").arg(e)); });
        refresh();
        job->start();
    }
    void done(const QString &msg)
    {
        job->deleteLater();
        job = nullptr;
        bar->hide();
        note->setText(msg);
        refresh();
    }
    Setup::Status st;
    QLabel *banner, *note, *rows[6][3];
    QPushButton *download, *locate, *recheck, *stop;
    QProgressBar *bar;
    FfmpegInstaller *job = nullptr;
};

class ModelPage : public QWizardPage {
public:
    ModelPage()
    {
        setTitle(heading(tr("Speech model for transcription")));
        auto *v = new QVBoxLayout(this);
        auto *intro = new QLabel(tr("Transcription runs on this computer and needs a speech model from huggingface.co. Larger models are slower and more accurate. You can skip this and download one the first time you transcribe."), this);
        intro->setWordWrap(true);
        v->addWidget(intro);
        box = new QComboBox(this);
        for (auto &n : Analysis::whisperModels()) box->addItem(QString("%1 (%2 MB)").arg(Analysis::whisperModelLabel(n)).arg(Analysis::whisperModelSizeMB(n)), n);
        box->setCurrentIndex(qMax(0, box->findData(QSettings().value("transcribe/model"))));
        v->addWidget(box);
        auto *h = new QHBoxLayout;
        download = new QPushButton(tr("Download Model"), this);
        download->setProperty("primary", true);
        h->addWidget(download);
        h->addStretch(1);
        v->addLayout(h);
        bar = new QProgressBar(this);
        bar->setRange(0, 1000);
        bar->setTextVisible(false);
        bar->hide();
        v->addWidget(bar);
        note = new QLabel(this);
        note->setWordWrap(true);
        note->setStyleSheet("color:#8a8a8a;");
        v->addWidget(note);
        v->addStretch(1);
        connect(box, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
        connect(download, &QPushButton::clicked, this, [this] { startDownload(); });
    }
    void initializePage() override { refresh(); }
    bool isComplete() const override { return !job; }
    void refresh()
    {
        QString n = box->currentData().toString();
        QSettings().setValue("transcribe/model", n);
        bool have = QFileInfo::exists(Analysis::whisperModelPath(n));
        download->setEnabled(!have && !job);
        box->setEnabled(!job);
        if (!job) note->setText(have ? tr("This model is installed.") : tr("This model is not installed."));
    }
    void startDownload()
    {
        job = new ModelDownloader(this);
        bar->setValue(0);
        bar->show();
        connect(job, &ModelDownloader::progress, this, [this](qint64 g, qint64 t) { if (t > 0) { bar->setValue(int(g * 1000 / t)); note->setText(mbProgress(g, t)); } });
        auto end = [this] { job->deleteLater(); job = nullptr; bar->hide(); refresh(); emit completeChanged(); };
        connect(job, &ModelDownloader::finished, this, end);
        connect(job, &ModelDownloader::failed, this, [this, end](const QString &e) { end(); note->setText(tr("Download failed: %1").arg(e)); });
        job->start(box->currentData().toString());
        refresh();
        emit completeChanged();
    }
    QComboBox *box;
    QPushButton *download;
    QProgressBar *bar;
    QLabel *note;
    ModelDownloader *job = nullptr;
};

class DonePage : public QWizardPage {
public:
    DonePage()
    {
        setTitle(heading(tr("Setup is complete")));
        text = new QLabel(this);
        text->setWordWrap(true);
        auto *v = new QVBoxLayout(this);
        v->addWidget(text);
        v->addStretch(1);
        setFinalPage(true);
    }
    void initializePage() override
    {
        auto s = Setup::check();
        QStringList later;
        if (!s.whisper) later << tr("Transcription needs an FFmpeg build with the whisper filter.");
        if (!s.model) later << tr("A speech model downloads the first time you transcribe.");
        if (!s.bridge) later << tr("MCP clients need oe-mcp.exe: reinstall Open Editor to restore it.");
        if (!s.encoders) later << tr("H.264 export needs an FFmpeg build with libx264 and AAC.");
        const QStringList news = {
            tr("<b>Enhance Speech</b>, <b>DeNoise</b>, <b>DeEsser</b>, <b>DeHum</b>, <b>Noise Gate</b> and <b>Chorus/Flanger</b> audio effects"),
            tr("<b>Auto Ducking</b> and <b>Match Loudness</b> in the AI menu, and a loudness target on the Export page"),
            tr("<b>Remove Filler Words</b> from the transcript and the footage"),
            tr("<b>Import fonts</b> (File > Import Font) and many more title options: weights, gradients, glow, rounded backgrounds, style presets and in/out animations"),
            tr("<b>Warp Stabilizer</b>, plus Leave Color, Color Key, Find Edges, Emboss, Twirl, Spherize, Replicate, Linear Wipe, Gradient Ramp and Strobe Light")};
        text->setText(tr("Open Editor is ready to use.") + (later.isEmpty() ? QString() : "<br><br>" + tr("Still optional:") + "<ul><li>" + later.join("</li><li>") + "</li></ul>")
                      + "<br><br>" + tr("New in %1:").arg(QCoreApplication::applicationVersion()) + "<ul><li>" + news.join("</li><li>") + "</li></ul>"
                      + tr("You can run this wizard again from Help > Setup Wizard."));
    }
    QLabel *text;
};

class Wizard : public QWizard {
public:
    Wizard()
    {
        setWindowTitle(tr("Open Editor %1 Setup").arg(QCoreApplication::applicationVersion()));
        setWizardStyle(QWizard::ClassicStyle);
        setTitleFormat(Qt::RichText);
        setOption(QWizard::NoBackButtonOnStartPage);
        setMinimumSize(680, 500);
        addPage(deps = new DepsPage);
        addPage(model = new ModelPage);
        addPage(new DonePage);
    }
    DepsPage *deps;
    ModelPage *model;
};

} // namespace

bool Setup::run(bool atStartup)
{
    Wizard w;
    if (w.exec() == QDialog::Accepted) {
        markDone();
        return true;
    }
    CACHE->reloadTools();
    if (!atStartup || check().requiredOk()) return true;
    QMessageBox::critical(nullptr, QObject::tr("Open Editor Setup"), QObject::tr("Open Editor cannot run without FFmpeg and FFprobe. Start it again to finish setup."));
    return false;
}

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(4); }
}

// --setup-test <outdir>: run with FFmpeg off PATH and not beside the app. Downloads the real release.
int Setup::selfTest(const QStringList &args)
{
    if (args.isEmpty()) return 2;
    const QString out = args[0];
    QDir().mkpath(out);
    int failures = 0;
    auto expect = [&](bool ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); fflush(stdout); failures += !ok; };
    QSettings().clear();
    QDir(MediaCache::downloadedToolsDir()).removeRecursively();
    CACHE->reloadTools();
    expect(CACHE->ffmpeg().isEmpty(), "test setup: no FFmpeg is reachable");
    expect(needed(), "wizard is needed when nothing is configured");

    Wizard w;
    w.show();
    pump(400);
    auto *next = w.button(QWizard::NextButton);
    auto shot = [&](const QString &n) { pump(250); w.grab().save(out + "/" + n + ".png"); };
    shot("1_missing");
    expect(!next->isEnabled(), "missing FFmpeg blocks Next");
    expect(w.deps->banner->text().contains("missing"), "missing dependencies are reported");

    QString bad = out + "/tampered.zip";
    { QFile f(bad); if (f.open(QIODevice::WriteOnly)) f.write("not ffmpeg"); }
    FfmpegInstaller tampered(QUrl::fromLocalFile(bad).toString(), kFfmpegSha256);
    bool refused = false, accepted = false;
    QObject::connect(&tampered, &FfmpegInstaller::failed, [&](const QString &) { refused = true; });
    QObject::connect(&tampered, &FfmpegInstaller::finished, [&] { accepted = true; });
    tampered.start();
    for (int i = 0; i < 500 && !refused && !accepted; ++i) pump(10);
    expect(refused && !accepted, "a download with the wrong hash is refused");
    expect(!QFileInfo::exists(MediaCache::downloadedToolsDir() + "/download.zip"), "the refused download is deleted");
    QFile::remove(bad);

    QString wrong = out + "/empty";
    QDir().mkpath(wrong);
    expect(!useFfmpegDir(wrong), "a folder without FFmpeg is refused");

    w.deps->download->click();
    shot("2_downloading");
    QElapsedTimer t;
    t.start();
    while (w.deps->job && t.elapsed() < 540000) pump(200);
    printf("note: %s\n", qPrintable(w.deps->note->text()));
    expect(!w.deps->job && next->isEnabled(), "download fills the gap and Next unlocks");
    expect(CACHE->ffmpeg().startsWith(MediaCache::downloadedToolsDir()), "the downloaded FFmpeg is the one in use");
    expect(check().whisper, "the downloaded FFmpeg has the whisper filter");
    expect(check().encoders, "the downloaded FFmpeg has the H.264 and AAC encoders");
    shot("3_ready");
    w.next();
    shot("4_model");
    w.next();
    shot("5_done");
    w.button(QWizard::FinishButton)->click();
    pump(100);
    expect(w.result() == QDialog::Accepted, "wizard finishes");
    markDone();
    expect(!needed(), "wizard is not needed again");
    QSettings().setValue("setup/version", "1.0.0");
    expect(needed(), "wizard comes back once after an upgrade");
    markDone();
    expect(!needed(), "and not again for the same version");
    QFile::remove(MediaCache::downloadedToolsDir() + "/ffmpeg.exe");
    CACHE->reloadTools();
    expect(needed(), "wizard comes back when FFmpeg is removed later");
    printf(failures ? "\n%d SETUP FAILURES\n" : "\nALL SETUP TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
