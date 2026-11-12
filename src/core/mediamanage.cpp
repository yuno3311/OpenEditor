#include "mediamanage.h"
#include "effectdefs.h"
#include "vault.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>

namespace MediaManage {
static bool isFileMedia(const MediaItem &m)
{
    return !m.path.isEmpty() && (m.kind == MediaKind::Video || m.kind == MediaKind::Audio || m.kind == MediaKind::Image);
}

QVector<int> offlineMedia(ProjectState &st, bool recheck)
{
    QVector<int> ids;
    for (auto &m : st.media) {
        if (!isFileMedia(m)) continue;
        if (recheck) m.offline = !QFileInfo::exists(m.path);
        if (m.offline) ids << m.id;
    }
    return ids;
}

bool relink(ProjectState &st, int mediaId, const QString &newPath, QString *err)
{
    MediaItem *m = st.mediaById(mediaId);
    if (!m || !isFileMedia(*m)) { if (err) *err = QStringLiteral("No such media file."); return false; }
    QFileInfo fi(newPath);
    if (!fi.isFile()) { if (err) *err = QStringLiteral("%1 does not exist.").arg(newPath); return false; }
    m->path = QDir::cleanPath(fi.absoluteFilePath());
    m->size = fi.size();
    m->offline = false;
    return true;
}

int relinkFolder(ProjectState &st, const QString &folder, QStringList *relinked)
{
    QHash<QString, QVector<int>> wanted; // lower-case file name -> media indexes
    for (int i = 0; i < st.media.size(); ++i) {
        MediaItem &m = st.media[i];
        if (!isFileMedia(m)) continue;
        m.offline = !QFileInfo::exists(m.path);
        if (m.offline) wanted[QFileInfo(m.path).fileName().toLower()] << i;
    }
    if (wanted.isEmpty() || !QFileInfo(folder).isDir()) return 0;
    QHash<int, QString> pick;
    QHash<int, bool> exact;
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        auto w = wanted.constFind(it.fileName().toLower());
        if (w == wanted.constEnd()) continue;
        for (int i : *w) {
            const bool same = st.media[i].size > 0 && it.fileInfo().size() == st.media[i].size;
            if (!pick.contains(i) || (same && !exact.value(i))) { pick[i] = it.filePath(); exact[i] = same; }
        }
    }
    int n = 0;
    for (auto p = pick.constBegin(); p != pick.constEnd(); ++p)
        if (relink(st, st.media[p.key()].id, p.value())) { ++n; if (relinked) *relinked << p.value(); }
    return n;
}

static void useSeq(const ProjectState &st, const Sequence &q, QSet<int> &used, QSet<int> &seen)
{
    if (seen.contains(q.id)) return;
    seen << q.id;
    auto walk = [&](const QVector<Track> &tracks) {
        for (auto &t : tracks)
            for (auto &c : t.clips) {
                if (!c.mediaId) continue;
                used << c.mediaId;
                if (const MediaItem *m = st.mediaById(c.mediaId); m && m->kind == MediaKind::Nested)
                    if (const Sequence *n = st.seqById(m->seqId)) useSeq(st, *n, used, seen);
            }
    };
    walk(q.video);
    walk(q.audio);
}
QSet<int> usedMedia(const ProjectState &st)
{
    QSet<int> used, seen;
    for (auto &q : st.seqs) useSeq(st, q, used, seen);
    return used;
}
QVector<int> unusedMedia(const ProjectState &st)
{
    const QSet<int> used = usedMedia(st);
    QVector<int> out;
    for (auto &m : st.media)
        if (m.kind != MediaKind::Nested && !used.contains(m.id)) out << m.id;
    return out;
}
int removeUnused(ProjectState &st)
{
    const QVector<int> ids = unusedMedia(st);
    for (int id : ids)
        for (int i = 0; i < st.media.size(); ++i)
            if (st.media[i].id == id) { st.media.removeAt(i); break; }
    return ids.size();
}

static QVector<int> collectList(const ProjectState &st, bool includeUnused)
{
    const QSet<int> used = usedMedia(st);
    QVector<int> idx;
    for (int i = 0; i < st.media.size(); ++i)
        if (isFileMedia(st.media[i]) && (includeUnused || used.contains(st.media[i].id))) idx << i;
    return idx;
}
qint64 collectSize(const ProjectState &st, bool includeUnused)
{
    qint64 total = 0;
    QSet<QString> seen;
    for (int i : collectList(st, includeUnused)) {
        const QString p = QDir::cleanPath(st.media[i].path).toLower();
        if (seen.contains(p)) continue;
        seen << p;
        total += QFileInfo(st.media[i].path).size();
    }
    return total;
}

CollectResult collect(const ProjectState &src, const QString &dest, const CollectOptions &opt)
{
    CollectResult r;
    ProjectState st = src;
    const QString mediaDir = dest + "/Media";
    if (!QDir().mkpath(mediaDir)) { r.error = QStringLiteral("Cannot create %1.").arg(QDir::toNativeSeparators(mediaDir)); return r; }
    const qint64 need = collectSize(st, opt.includeUnused), free_ = Vault::freeBytes(dest);
    if (free_ >= 0 && free_ < need + (16 << 20)) {
        r.error = QStringLiteral("Not enough free space: %1 MB needed, %2 MB available.").arg(need >> 20).arg(free_ >> 20);
        return r;
    }
    if (!opt.includeUnused) removeUnused(st);
    const QVector<int> list = collectList(st, true);
    QHash<QString, QString> copiedAs; // source path -> collected path, so a file used twice copies once
    QSet<QString> names;
    int done = 0;
    for (int i : list) {
        MediaItem &m = st.media[i];
        const QString key = QDir::cleanPath(m.path).toLower();
        if (opt.progress && !opt.progress(done++, list.size(), QFileInfo(m.path).fileName())) { r.error = QStringLiteral("Cancelled."); return r; }
        if (copiedAs.contains(key)) { m.path = copiedAs[key]; m.offline = false; continue; }
        QFileInfo fi(m.path);
        if (!fi.isFile()) { ++r.skippedOffline; m.offline = true; continue; }
        // Different files that share a name get a numbered copy.
        QString name = fi.fileName();
        for (int k = 2; names.contains(name.toLower()); ++k) name = fi.completeBaseName() + QString(" (%1).").arg(k) + fi.suffix();
        names << name.toLower();
        const QString to = mediaDir + "/" + name;
        if (QDir::cleanPath(to).toLower() != key) {
            QFile::remove(to);
            if (!QFile::copy(m.path, to)) { r.error = QStringLiteral("Could not copy %1.").arg(fi.fileName()); return r; }
            ++r.copied;
            r.bytes += fi.size();
        }
        copiedAs[key] = to;
        m.path = to;
        m.offline = false;
    }
    if (opt.progress) opt.progress(list.size(), list.size(), QString());
    r.projectFile = dest + "/" + (opt.projectName.isEmpty() ? QStringLiteral("Collected Project") : opt.projectName) + ".oep";
    if (!Vault::saveProject(r.projectFile, st, false, &r.error)) return r;
    r.ok = true;
    return r;
}

QJsonObject report(const ProjectState &st)
{
    const QSet<int> used = usedMedia(st);
    int video = 0, audio = 0, image = 0, offline = 0, unused = 0;
    qint64 bytes = 0;
    QJsonArray offlineFiles;
    for (auto &m : st.media) {
        if (m.kind == MediaKind::Video) ++video;
        else if (m.kind == MediaKind::Audio) ++audio;
        else if (m.kind == MediaKind::Image) ++image;
        if (!isFileMedia(m)) continue;
        if (!QFileInfo::exists(m.path)) { ++offline; offlineFiles.append(m.path); }
        else bytes += QFileInfo(m.path).size();
        if (!used.contains(m.id)) ++unused;
    }
    QMap<QString, int> fx, fonts, trans;
    QJsonArray seqs;
    for (auto &q : st.seqs) {
        int clips = 0, titles = 0;
        auto walk = [&](const QVector<Track> &tracks) {
            for (auto &t : tracks) {
                for (auto &x : t.transitions) ++trans[x.type];
                for (auto &e : t.fx) ++fx[e.type];
                for (auto &c : t.clips) {
                    ++clips;
                    if (c.kind == MediaKind::Text) { ++titles; ++fonts[c.text.font]; }
                    for (auto &e : c.fx) {
                        const EffectDef *d = EffectRegistry::find(e.type);
                        if (!d || !d->intrinsic) ++fx[e.type];
                    }
                }
            }
        };
        walk(q.video);
        walk(q.audio);
        seqs.append(QJsonObject{{"id", q.id}, {"name", q.name}, {"width", q.w}, {"height", q.h}, {"fps", q.fps()}, {"seconds", q.duration() / q.fps()},
                                {"video_tracks", q.video.size()}, {"audio_tracks", q.audio.size()}, {"clips", clips}, {"titles", titles},
                                {"markers", q.markers.size()}, {"captions", q.captions.size()}});
    }
    auto toObj = [](const QMap<QString, int> &m) { QJsonObject o; for (auto i = m.begin(); i != m.end(); ++i) o[i.key()] = i.value(); return o; };
    return {{"media", QJsonObject{{"video", video}, {"audio", audio}, {"images", image}, {"offline", offline}, {"unused", unused}, {"megabytes", double(bytes >> 20)}}},
            {"offline_files", offlineFiles}, {"sequences", seqs}, {"effects", toObj(fx)}, {"transitions", toObj(trans)}, {"fonts", toObj(fonts)}};
}

QString reportText(const ProjectState &st)
{
    const QJsonObject r = report(st), m = r["media"].toObject();
    QStringList l;
    l << QStringLiteral("MEDIA")
      << QStringLiteral("  %1 video, %2 audio, %3 image file(s), %4 MB on disk").arg(m["video"].toInt()).arg(m["audio"].toInt()).arg(m["images"].toInt()).arg(qint64(m["megabytes"].toDouble()))
      << QStringLiteral("  %1 offline, %2 not used on any timeline").arg(m["offline"].toInt()).arg(m["unused"].toInt());
    for (auto v : r["offline_files"].toArray()) l << "    offline: " + QDir::toNativeSeparators(v.toString());
    l << QString() << QStringLiteral("SEQUENCES");
    for (auto v : r["sequences"].toArray()) {
        const QJsonObject q = v.toObject();
        const int sec = int(q["seconds"].toDouble());
        l << QStringLiteral("  %1: %2x%3 at %4 fps, %5:%6, %7 clip(s), %8 title(s), %9 marker(s), %10 caption(s)")
                 .arg(q["name"].toString()).arg(q["width"].toInt()).arg(q["height"].toInt()).arg(q["fps"].toDouble(), 0, 'f', 2)
                 .arg(sec / 60).arg(sec % 60, 2, 10, QChar('0')).arg(q["clips"].toInt()).arg(q["titles"].toInt()).arg(q["markers"].toInt()).arg(q["captions"].toInt());
    }
    auto section = [&](const char *title, const QJsonObject &o, bool effectNames) {
        if (o.isEmpty()) return;
        l << QString() << QString::fromLatin1(title);
        for (auto i = o.begin(); i != o.end(); ++i) {
            QString name = i.key();
            if (effectNames) {
                if (auto d = EffectRegistry::find(name)) name = d->name;
                else if (auto t = EffectRegistry::findTransition(name)) name = t->name;
            }
            l << QStringLiteral("  %1 x %2").arg(i.value().toInt()).arg(name);
        }
    };
    section("EFFECTS", r["effects"].toObject(), true);
    section("TRANSITIONS", r["transitions"].toObject(), true);
    section("FONTS", r["fonts"].toObject(), false);
    return l.join('\n');
}
}
