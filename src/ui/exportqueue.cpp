#include "exportqueue.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

ExportQueue *ExportQueue::instance() { static ExportQueue q; return &q; }
ExportQueue::ExportQueue(QObject *p) : QObject(p) {}

ExportQueue::Item *ExportQueue::find(int id)
{
    for (auto &i : list) if (i.id == id) return &i;
    return nullptr;
}

int ExportQueue::add(const ProjectState &st, int seqId, const ExportSettings &cfg, QString *err)
{
    const Sequence *q = st.seqById(seqId);
    auto no = [&](const QString &why) { if (err) *err = why; return 0; };
    if (!q) return no(tr("No sequence to export."));
    if (q->duration() <= 0) return no(tr("The sequence is empty."));
    bool known = false;
    for (auto &f : exportFormats()) known |= f.name == cfg.format;
    if (!known) return no(tr("Unknown format: %1").arg(cfg.format));
    if (cfg.path.isEmpty() || !QFileInfo(cfg.path).isAbsolute()) return no(tr("The output path must be absolute."));
    // Two queued exports to one file would overwrite each other.
    for (auto &i : list)
        if ((i.state == "queued" || i.state == "running") && QDir::cleanPath(i.cfg.path).toLower() == QDir::cleanPath(cfg.path).toLower())
            return no(tr("%1 is already in the queue.").arg(QFileInfo(cfg.path).fileName()));
    Item it;
    it.id = nextId++;
    it.sequence = q->name;
    it.cfg = cfg;
    it.snapshot = st;
    it.seqId = seqId;
    list << it;
    emit changed();
    return it.id;
}

int ExportQueue::pending() const
{
    int n = 0;
    for (auto &i : list) n += i.state == "queued";
    return n;
}

void ExportQueue::start()
{
    if (job) return;
    wantStop = false;
    doneCount = failCount = 0;
    next();
}

void ExportQueue::next()
{
    if (job) return;
    Item *it = nullptr;
    for (auto &i : list) if (i.state == "queued") { it = &i; break; }
    if (!it || wantStop) { emit changed(); emit finishedAll(doneCount, failCount); return; }
    it->state = "running";
    it->progress = 0;
    current = it->id;
    job = new Exporter(it->snapshot, it->seqId, it->cfg, this);
    it->snapshot = ProjectState(); // the exporter holds its own copy now
    const int id = current;
    connect(job, &Exporter::progress, this, [this, id](double f, const QString &s) { if (auto *i = find(id)) { i->progress = f; i->message = s; emit changed(); } });
    connect(job, &Exporter::finishedOk, this, [this, id](const QString &p) { if (auto *i = find(id)) { i->state = "done"; i->progress = 1; i->message = QDir::toNativeSeparators(p); ++doneCount; } });
    connect(job, &Exporter::failed, this, [this, id](const QString &e) { if (auto *i = find(id)) { i->state = wantStop ? "stopped" : "failed"; i->message = e; ++failCount; } });
    connect(job, &QThread::finished, this, [this] {
        job->deleteLater();
        job = nullptr;
        emit changed();
        next();
    });
    emit changed();
    job->start();
}

void ExportQueue::stop()
{
    wantStop = true;
    if (job) job->cancel();
}

bool ExportQueue::remove(int id)
{
    for (int i = 0; i < list.size(); ++i)
        if (list[i].id == id) {
            if (list[i].state == "running") return false;
            list.removeAt(i);
            emit changed();
            return true;
        }
    return false;
}

void ExportQueue::clearFinished()
{
    for (int i = list.size() - 1; i >= 0; --i)
        if (list[i].state != "queued" && list[i].state != "running") list.removeAt(i);
    emit changed();
}

QJsonArray ExportQueue::toJson() const
{
    QJsonArray a;
    for (auto &i : list)
        a.append(QJsonObject{{"id", i.id}, {"sequence", i.sequence}, {"path", i.cfg.path}, {"format", i.cfg.format}, {"state", i.state}, {"progress", i.progress}, {"message", i.message}});
    return a;
}
