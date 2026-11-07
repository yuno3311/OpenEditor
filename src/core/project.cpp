#include "project.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUndoCommand>

namespace {
class SnapCmd : public QUndoCommand {
public:
    SnapCmd(Project *p, const ProjectState &a, const ProjectState &b, const QString &t) : QUndoCommand(t), p(p), before(a), after(b) {}
    void undo() override { p->applySnapshot(before); }
    void redo() override { if (first) { first = false; return; } p->applySnapshot(after); }
    Project *p; ProjectState before, after; bool first = true;
};
}

Project *Project::instance() { static Project p; return &p; }
Project::Project(QObject *par) : QObject(par)
{
    undo.setUndoLimit(200);
    connect(&undo, &QUndoStack::cleanChanged, this, &Project::dirtyChanged);
}

void Project::applySnapshot(const ProjectState &st)
{
    QHash<int, Frame> heads;
    for (auto &q : s.seqs) heads[q.id] = q.playhead;
    int nid = s.nextId;
    s = st;
    s.nextId = qMax(nid, s.nextId);
    for (auto &q : s.seqs) if (heads.contains(q.id)) q.playhead = heads[q.id];
    QSet<int> keep;
    if (auto q = active())
        for (int c : std::as_const(selClips)) if (q->clip(c)) keep << c;
    selClips = keep;
    selTransition = {0, 0};
    emit activeSeqChanged();
    emit changed();
    emit selectionChanged();
}

void Project::edit(const QString &label, const std::function<void(ProjectState &)> &f)
{
    ProjectState before = s;
    f(s);
    undo.push(new SnapCmd(this, before, s, label));
    emit changed();
}
void Project::liveBegin() { if (!live) { liveBase = s; live = true; } }
void Project::liveTouch() { emit changed(); }
void Project::liveCommit(const QString &label)
{
    if (!live) return;
    live = false;
    undo.push(new SnapCmd(this, liveBase, s, label));
    emit changed();
}
void Project::liveCancel()
{
    if (!live) return;
    live = false;
    s = liveBase;
    emit changed();
}

void Project::reset()
{
    s = ProjectState();
    undo.clear();
    selClips.clear();
    selTransition = {0, 0};
    filePath.clear();
    loaded = false;
    emit activeSeqChanged();
    emit changed();
    emit selectionChanged();
    emit dirtyChanged();
}

bool Project::save(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(stateToJson(s)).toJson(QJsonDocument::Compact));
    filePath = path;
    undo.setClean();
    emit dirtyChanged();
    return true;
}
bool Project::load(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject() || doc.object()["format"].toString() != "OpenEditorProject") return false;
    reset();
    s = stateFromJson(doc.object());
    for (auto &m : s.media)
        if (!m.path.isEmpty() && !QFileInfo::exists(m.path)) m.offline = true;
    filePath = path;
    loaded = true;
    emit activeSeqChanged();
    emit changed();
    emit dirtyChanged();
    return true;
}
QString Project::displayName() const
{
    return filePath.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(filePath).completeBaseName();
}

void Project::setPlayhead(Frame f)
{
    auto q = active();
    if (!q) return;
    f = qMax<Frame>(0, f);
    if (q->playhead == f) return;
    q->playhead = f;
    emit playheadChanged(f);
}
void Project::setSelection(const QSet<int> &sel)
{
    selClips = sel;
    selTransition = {0, 0};
    emit selectionChanged();
}
void Project::selectTransition(int trackId, Frame cut)
{
    selClips.clear();
    selTransition = {trackId, cut};
    emit selectionChanged();
}
void Project::setActiveSequence(int id)
{
    if (s.activeSeq == id) return;
    s.activeSeq = id;
    selClips.clear();
    selTransition = {0, 0};
    emit activeSeqChanged();
    emit selectionChanged();
    emit changed();
}
QSet<int> Project::withLinked(const QSet<int> &sel) const
{
    QSet<int> r = sel;
    if (!linkedSelection) return r;
    auto q = const_cast<Project *>(this)->active();
    if (!q) return r;
    QSet<int> links;
    for (int id : sel) if (auto c = q->clip(id)) if (c->link) links << c->link;
    if (links.isEmpty()) return r;
    for (auto *t : q->allTracks())
        for (auto &c : t->clips)
            if (c.link && links.contains(c.link)) r << c.id;
    return r;
}

int Project::createSequence(const QString &name, int w, int h, int num, int den, int vt, int at)
{
    int newSeq = 0;
    edit(tr("New Sequence"), [&](ProjectState &st) {
        Sequence q;
        q.id = st.newId(); q.name = name; q.w = w; q.h = h; q.fpsNum = num; q.fpsDen = den;
        for (int i = 0; i < vt; ++i) {
            Track t; t.id = st.newId(); t.type = TrackType::Video; t.name = QString("Video %1").arg(i + 1);
            q.video << t;
        }
        for (int i = 0; i < at; ++i) {
            Track t; t.id = st.newId(); t.type = TrackType::Audio; t.name = QString("Audio %1").arg(i + 1);
            q.audio << t;
        }
        st.seqs << q;
        MediaItem m;
        m.id = st.newId(); m.kind = MediaKind::Nested; m.name = name; m.seqId = q.id; m.w = w; m.h = h;
        m.fps = q.fps(); m.hasVideo = m.hasAudio = true; m.label = defaultLabel(MediaKind::Nested);
        st.media << m;
        st.activeSeq = q.id;
        newSeq = q.id;
    });
    selClips.clear();
    emit activeSeqChanged();
    return newSeq;
}
