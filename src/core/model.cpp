#include "model.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QtMath>
#include <algorithm>

double Param::at(Frame t) const
{
    if (!animated || keys.isEmpty()) return value;
    if (t <= keys.first().t) return keys.first().v;
    if (t >= keys.last().t) return keys.last().v;
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](Frame a, const Keyframe &k) { return a < k.t; });
    const Keyframe &b = *it, &a = *(it - 1);
    if (a.i == Interp::Hold) return a.v;
    double u = double(t - a.t) / double(b.t - a.t);
    if (a.i == Interp::Ease) u = u * u * (3 - 2 * u);
    return a.v + (b.v - a.v) * u;
}
int Param::keyIndexAt(Frame t) const
{
    for (int i = 0; i < keys.size(); ++i)
        if (keys[i].t == t) return i;
    return -1;
}
void Param::setKey(Frame t, double v)
{
    int i = keyIndexAt(t);
    if (i >= 0) { keys[i].v = v; return; }
    Keyframe k; k.t = t; k.v = v;
    auto it = std::lower_bound(keys.begin(), keys.end(), t, [](const Keyframe &a, Frame b) { return a.t < b; });
    keys.insert(it, k);
}
void Param::removeKeyAt(Frame t)
{
    int i = keyIndexAt(t);
    if (i >= 0) keys.removeAt(i);
}

Param *Effect::find(const QString &id)
{
    for (auto &p : params) if (p.id == id) return &p;
    return nullptr;
}
const Param *Effect::find(const QString &id) const
{
    for (auto &p : params) if (p.id == id) return &p;
    return nullptr;
}
double Effect::val(const QString &id, Frame t, double fallback) const
{
    auto p = find(id);
    return p ? p->at(t) : fallback;
}

double Clip::sourceTime(Frame f, double fps) const
{
    // Time remapping integrates a keyframed speed curve, so ramps stay continuous in source time.
    if (const Effect *r = effect("timeremap"); r && r->enabled) {
        double t = srcIn;
        const Param *p = r->find("speed");
        Frame n = qBound<Frame>(0, f - start, dur);
        for (Frame i = 0; i < n; ++i) t += qMax(0.0, p ? p->at(i + kfOffset) : 100.0) / 100.0 / fps;
        return t;
    }
    double local = double(f - start);
    if (reverse) local = double(dur) - 1 - local;
    return srcIn + local / fps * speed;
}
Effect *Clip::effect(const QString &type)
{
    for (auto &e : fx) if (e.type == type) return &e;
    return nullptr;
}
const Effect *Clip::effect(const QString &type) const
{
    for (auto &e : fx) if (e.type == type) return &e;
    return nullptr;
}

const Clip *Track::clipAt(Frame f) const
{
    auto it = std::upper_bound(clips.begin(), clips.end(), f, [](Frame a, const Clip &c) { return a < c.start; });
    if (it == clips.begin()) return nullptr;
    --it;
    return f < it->end() ? &*it : nullptr;
}
Clip *Track::clipById(int cid)
{
    for (auto &c : clips) if (c.id == cid) return &c;
    return nullptr;
}

Frame Sequence::duration() const
{
    Frame d = 0;
    for (auto &t : video) if (!t.clips.isEmpty()) d = qMax(d, t.clips.last().end());
    for (auto &t : audio) if (!t.clips.isEmpty()) d = qMax(d, t.clips.last().end());
    return d;
}
Track *Sequence::track(int tid)
{
    for (auto &t : video) if (t.id == tid) return &t;
    for (auto &t : audio) if (t.id == tid) return &t;
    return nullptr;
}
const Track *Sequence::track(int tid) const
{
    for (auto &t : video) if (t.id == tid) return &t;
    for (auto &t : audio) if (t.id == tid) return &t;
    return nullptr;
}
Clip *Sequence::clip(int cid, Track **out)
{
    for (auto *t : allTracks())
        if (auto c = t->clipById(cid)) { if (out) *out = t; return c; }
    return nullptr;
}
const Clip *Sequence::clip(int cid) const
{
    for (auto &t : video) for (auto &c : t.clips) if (c.id == cid) return &c;
    for (auto &t : audio) for (auto &c : t.clips) if (c.id == cid) return &c;
    return nullptr;
}
QVector<Track *> Sequence::allTracks()
{
    QVector<Track *> r;
    for (auto &t : video) r << &t;
    for (auto &t : audio) r << &t;
    return r;
}

MediaItem *ProjectState::mediaById(int id) { for (auto &m : media) if (m.id == id) return &m; return nullptr; }
const MediaItem *ProjectState::mediaById(int id) const { for (auto &m : media) if (m.id == id) return &m; return nullptr; }
Sequence *ProjectState::seqById(int id) { for (auto &s : seqs) if (s.id == id) return &s; return nullptr; }
const Sequence *ProjectState::seqById(int id) const { for (auto &s : seqs) if (s.id == id) return &s; return nullptr; }

QString framesToTimecode(Frame f, double fps, bool df)
{
    bool neg = f < 0;
    if (neg) f = -f;
    int nominal = qRound(fps);
    if (nominal <= 0) nominal = 30;
    if (df && (nominal == 30 || nominal == 60)) {
        int drop = nominal == 30 ? 2 : 4;
        qint64 per10 = 10 * 60 * nominal - 9 * drop, perMin = 60 * nominal - drop;
        qint64 d = f / per10, m = f % per10;
        f += 9 * drop * d;
        if (m >= drop) f += drop * ((m - drop) / perMin + 1);
    }
    qint64 ff = f % nominal, tot = f / nominal;
    return QString("%1%2:%3:%4%5%6")
        .arg(neg ? "-" : "")
        .arg(tot / 3600, 2, 10, QChar('0'))
        .arg((tot / 60) % 60, 2, 10, QChar('0'))
        .arg(tot % 60, 2, 10, QChar('0'))
        .arg(df ? ";" : ":")
        .arg(ff, 2, 10, QChar('0'));
}

Frame timecodeToFrames(const QString &tc, double fps, bool df, bool *ok)
{
    QString s = tc.trimmed();
    if (ok) *ok = false;
    if (s.isEmpty()) return 0;
    bool neg = s.startsWith('-');
    if (neg) s = s.mid(1);
    int nominal = qMax(1, qRound(fps));
    qint64 h = 0, m = 0, sec = 0, ff = 0;
    QStringList parts;
    if (s.contains(':') || s.contains(';') || s.contains('.')) {
        parts = s.split(QRegularExpression("[:;.]"));
    } else { // typed digits: HHMMSSFF, right aligned
        QString d = s;
        while (d.size() > 0) { parts.prepend(d.right(2)); d.chop(2); }
    }
    for (auto &p : parts) { bool k; p.toLongLong(&k); if (!k && !p.isEmpty()) return 0; }
    int n = parts.size();
    if (n >= 1) ff = parts[n - 1].toLongLong();
    if (n >= 2) sec = parts[n - 2].toLongLong();
    if (n >= 3) m = parts[n - 3].toLongLong();
    if (n >= 4) h = parts[n - 4].toLongLong();
    qint64 f = ((h * 3600 + m * 60 + sec) * nominal) + ff;
    if (df && (nominal == 30 || nominal == 60)) {
        int drop = nominal == 30 ? 2 : 4;
        qint64 mins = h * 60 + m;
        f -= drop * (mins - mins / 10);
    }
    if (ok) *ok = true;
    return neg ? -f : f;
}

static const struct { const char *name; QRgb c; } kLabels[kLabelCount] = {
    {"Violet", 0xA58BD8}, {"Iris", 0x7E8CDB}, {"Caribbean", 0x1FAE99}, {"Lavender", 0xD79BE6},
    {"Cerulean", 0x3BA7DE}, {"Forest", 0x4C9A50}, {"Rose", 0xDD6A92}, {"Mango", 0xE98F32},
    {"Purple", 0x8A55C9}, {"Blue", 0x4470D8}, {"Teal", 0x2C8E96}, {"Magenta", 0xC944A8},
    {"Tan", 0xC6A682}, {"Green", 0x6DBA4A}, {"Brown", 0x90623C}, {"Yellow", 0xDCC63C}};
QColor labelColor(int i) { return QColor(kLabels[qBound(0, i, kLabelCount - 1)].c); }
QString labelName(int i) { return kLabels[qBound(0, i, kLabelCount - 1)].name; }
int defaultLabel(MediaKind k)
{
    switch (k) {
    case MediaKind::Video: return 1;
    case MediaKind::Audio: return 2;
    case MediaKind::Image: return 3;
    case MediaKind::Nested: return 5;
    case MediaKind::Text: return 6;
    case MediaKind::Adjustment: return 7;
    default: return 0;
    }
}

// ---------------------------------------------------------------- JSON
static QString colorStr(const QColor &c) { return c.name(QColor::HexArgb); }
static QJsonObject paramJ(const Param &p)
{
    QJsonObject o{{"id", p.id}, {"v", p.value}};
    if (p.animated) {
        o["anim"] = true;
        QJsonArray a;
        for (auto &k : p.keys) a.append(QJsonArray{double(k.t), k.v, int(k.i)});
        o["keys"] = a;
    }
    return o;
}
static Param paramF(const QJsonObject &o)
{
    Param p;
    p.id = o["id"].toString();
    p.value = o["v"].toDouble();
    p.animated = o["anim"].toBool();
    for (auto v : o["keys"].toArray()) {
        auto a = v.toArray();
        Keyframe k; k.t = a[0].toDouble(); k.v = a[1].toDouble(); k.i = Interp(a[2].toInt());
        p.keys << k;
    }
    return p;
}
static QJsonObject effectJ(const Effect &e)
{
    QJsonArray ps;
    for (auto &p : e.params) ps.append(paramJ(p));
    QJsonObject o{{"type", e.type}, {"on", e.enabled}, {"params", ps}};
    if (!e.extra.isEmpty()) o["extra"] = QJsonObject::fromVariantMap(e.extra);
    return o;
}
static Effect effectF(const QJsonObject &o)
{
    Effect e;
    e.type = o["type"].toString();
    e.enabled = o["on"].toBool(true);
    for (auto v : o["params"].toArray()) e.params << paramF(v.toObject());
    e.extra = o["extra"].toObject().toVariantMap();
    return e;
}
static QJsonObject textJ(const TextStyle &t)
{
    return {{"text", t.text}, {"font", t.font}, {"size", t.size}, {"b", t.bold}, {"i", t.italic}, {"u", t.underline},
            {"align", t.align}, {"fill", colorStr(t.fill)}, {"stroke", t.stroke}, {"strokeC", colorStr(t.strokeColor)},
            {"strokeW", t.strokeWidth}, {"shadow", t.shadow}, {"shadowC", colorStr(t.shadowColor)},
            {"shadowD", t.shadowDist}, {"shadowB", t.shadowBlur}, {"bg", t.background}, {"bgC", colorStr(t.bgColor)},
            {"track", t.tracking}, {"lead", t.leading}};
}
static TextStyle textF(const QJsonObject &o)
{
    TextStyle t;
    t.text = o["text"].toString(t.text); t.font = o["font"].toString(t.font); t.size = o["size"].toDouble(t.size);
    t.bold = o["b"].toBool(); t.italic = o["i"].toBool(); t.underline = o["u"].toBool();
    t.align = o["align"].toInt(1); t.fill = QColor(o["fill"].toString("#ffffffff"));
    t.stroke = o["stroke"].toBool(); t.strokeColor = QColor(o["strokeC"].toString("#ff000000"));
    t.strokeWidth = o["strokeW"].toDouble(4); t.shadow = o["shadow"].toBool();
    t.shadowColor = QColor(o["shadowC"].toString("#b4000000")); t.shadowDist = o["shadowD"].toDouble(6);
    t.shadowBlur = o["shadowB"].toDouble(8); t.background = o["bg"].toBool();
    t.bgColor = QColor(o["bgC"].toString("#a0000000")); t.tracking = o["track"].toDouble(); t.leading = o["lead"].toDouble();
    return t;
}
QJsonObject clipToJson(const Clip &c)
{
    QJsonArray fx;
    for (auto &e : c.fx) fx.append(effectJ(e));
    QJsonObject o{{"id", c.id}, {"media", c.mediaId}, {"kind", int(c.kind)}, {"name", c.name}, {"start", double(c.start)},
                  {"dur", double(c.dur)}, {"in", c.srcIn}, {"speed", c.speed}, {"rev", c.reverse}, {"on", c.enabled},
                  {"link", c.link}, {"label", c.label}, {"kfo", double(c.kfOffset)}, {"cam", c.cam}, {"fx", fx}};
    if (c.kind == MediaKind::Text) o["text"] = textJ(c.text);
    return o;
}
Clip clipFromJson(const QJsonObject &o)
{
    Clip c;
    c.id = o["id"].toInt(); c.mediaId = o["media"].toInt(); c.kind = MediaKind(o["kind"].toInt());
    c.name = o["name"].toString(); c.start = o["start"].toDouble(); c.dur = o["dur"].toDouble();
    c.srcIn = o["in"].toDouble(); c.speed = o["speed"].toDouble(1); c.reverse = o["rev"].toBool();
    c.enabled = o["on"].toBool(true); c.link = o["link"].toInt(); c.label = o["label"].toInt(-1);
    c.kfOffset = o["kfo"].toDouble();
    c.cam = o["cam"].toInt(-1);
    for (auto v : o["fx"].toArray()) c.fx << effectF(v.toObject());
    if (o.contains("text")) c.text = textF(o["text"].toObject());
    return c;
}
static QJsonObject trackJ(const Track &t)
{
    QJsonArray cl, tr;
    for (auto &c : t.clips) cl.append(clipToJson(c));
    for (auto &x : t.transitions)
        tr.append(QJsonObject{{"type", x.type}, {"cut", double(x.cut)}, {"dur", double(x.dur)}, {"align", x.align}});
    QJsonArray tfx;
    for (auto &e : t.fx) tfx.append(effectJ(e));
    return {{"id", t.id}, {"type", int(t.type)}, {"name", t.name}, {"mute", t.muted}, {"solo", t.solo}, {"lock", t.locked},
            {"sync", t.sync}, {"target", t.target}, {"h", t.height}, {"vol", t.volumeDb}, {"pan", t.pan}, {"clips", cl},
            {"trans", tr}, {"fx", tfx}, {"volAuto", paramJ(t.volAuto)}, {"panAuto", paramJ(t.panAuto)}, {"wa", t.writeAuto}, {"sa", t.showAuto}};
}
static Track trackF(const QJsonObject &o)
{
    Track t;
    t.id = o["id"].toInt(); t.type = TrackType(o["type"].toInt()); t.name = o["name"].toString();
    t.muted = o["mute"].toBool(); t.solo = o["solo"].toBool(); t.locked = o["lock"].toBool();
    t.sync = o["sync"].toBool(true); t.target = o["target"].toBool(true); t.height = o["h"].toInt(44);
    t.volumeDb = o["vol"].toDouble(); t.pan = o["pan"].toDouble();
    for (auto v : o["fx"].toArray()) t.fx << effectF(v.toObject());
    t.volAuto = paramF(o["volAuto"].toObject()); t.panAuto = paramF(o["panAuto"].toObject());
    t.writeAuto = o["wa"].toBool(); t.showAuto = o["sa"].toBool();
    for (auto v : o["clips"].toArray()) t.clips << clipFromJson(v.toObject());
    for (auto v : o["trans"].toArray()) {
        auto x = v.toObject();
        Transition tr; tr.type = x["type"].toString(); tr.cut = x["cut"].toDouble(); tr.dur = x["dur"].toDouble();
        tr.align = x["align"].toDouble(.5);
        t.transitions << tr;
    }
    return t;
}
QJsonObject stateToJson(const ProjectState &s)
{
    QJsonArray media, bins, seqs;
    for (auto &m : s.media)
        media.append(QJsonObject{{"id", m.id}, {"bin", m.bin}, {"kind", int(m.kind)}, {"path", m.path}, {"name", m.name},
                                 {"w", m.w}, {"h", m.h}, {"fps", m.fps}, {"dur", m.duration}, {"hv", m.hasVideo},
                                 {"ha", m.hasAudio}, {"alpha", m.hasAlpha}, {"ch", m.channels}, {"sr", m.sampleRate},
                                 {"vc", m.vcodec}, {"ac", m.acodec}, {"br", double(m.bitrate)}, {"size", double(m.size)},
                                 {"color", colorStr(m.color)}, {"label", m.label}, {"seq", m.seqId}, {"in", m.inSec},
                                 {"out", m.outSec}, {"comment", m.comment}, {"hdr", m.hdr}, {"trc", m.colorTransfer}});
    for (auto &b : s.bins) bins.append(QJsonObject{{"id", b.id}, {"parent", b.parent}, {"name", b.name}});
    for (auto &q : s.seqs) {
        QJsonArray v, a, mk, caps;
        for (auto &c : q.captions) {
            QJsonArray ws;
            for (auto &w : c.words) ws.append(QJsonArray{double(w.start), double(w.end), w.text});
            caps.append(QJsonArray{double(c.start), double(c.end), c.text, ws});
        }
        for (auto &t : q.video) v.append(trackJ(t));
        for (auto &t : q.audio) a.append(trackJ(t));
        for (auto &m : q.markers)
            mk.append(QJsonObject{{"t", double(m.t)}, {"dur", double(m.dur)}, {"name", m.name}, {"comment", m.comment}, {"color", m.color}});
        seqs.append(QJsonObject{{"id", q.id}, {"name", q.name}, {"w", q.w}, {"h", q.h}, {"num", q.fpsNum}, {"den", q.fpsDen},
                                {"df", q.dropFrame}, {"video", v}, {"audio", a}, {"ph", double(q.playhead)},
                                {"in", double(q.inPt)}, {"out", double(q.outPt)}, {"markers", mk}, {"master", q.masterDb},
                                {"caps", caps}, {"burn", q.burnCaptions}, {"capSize", q.captionSize}, {"multicam", q.multicam}});
    }
    return {{"format", "OpenEditorProject"}, {"version", 1}, {"media", media}, {"bins", bins}, {"seqs", seqs},
            {"active", s.activeSeq}, {"nextId", s.nextId}};
}
ProjectState stateFromJson(const QJsonObject &o)
{
    ProjectState s;
    for (auto v : o["media"].toArray()) {
        auto j = v.toObject();
        MediaItem m;
        m.id = j["id"].toInt(); m.bin = j["bin"].toInt(); m.kind = MediaKind(j["kind"].toInt());
        m.path = j["path"].toString(); m.name = j["name"].toString(); m.w = j["w"].toInt(); m.h = j["h"].toInt();
        m.fps = j["fps"].toDouble(); m.duration = j["dur"].toDouble(); m.hasVideo = j["hv"].toBool();
        m.hasAudio = j["ha"].toBool(); m.hasAlpha = j["alpha"].toBool(); m.channels = j["ch"].toInt();
        m.sampleRate = j["sr"].toInt(); m.vcodec = j["vc"].toString(); m.acodec = j["ac"].toString();
        m.bitrate = j["br"].toDouble(); m.size = j["size"].toDouble(); m.color = QColor(j["color"].toString("#ff000000"));
        m.label = j["label"].toInt(-1); m.seqId = j["seq"].toInt(); m.inSec = j["in"].toDouble(-1);
        m.outSec = j["out"].toDouble(-1); m.comment = j["comment"].toString();
        m.hdr = j["hdr"].toBool(); m.colorTransfer = j["trc"].toString();
        s.media << m;
    }
    for (auto v : o["bins"].toArray()) {
        auto j = v.toObject();
        Bin b; b.id = j["id"].toInt(); b.parent = j["parent"].toInt(); b.name = j["name"].toString();
        s.bins << b;
    }
    for (auto v : o["seqs"].toArray()) {
        auto j = v.toObject();
        Sequence q;
        q.id = j["id"].toInt(); q.name = j["name"].toString(); q.w = j["w"].toInt(); q.h = j["h"].toInt();
        q.fpsNum = j["num"].toInt(30); q.fpsDen = j["den"].toInt(1); q.dropFrame = j["df"].toBool();
        for (auto t : j["video"].toArray()) q.video << trackF(t.toObject());
        for (auto t : j["audio"].toArray()) q.audio << trackF(t.toObject());
        q.playhead = j["ph"].toDouble(); q.inPt = j["in"].toDouble(-1); q.outPt = j["out"].toDouble(-1);
        q.masterDb = j["master"].toDouble();
        q.burnCaptions = j["burn"].toBool(true); q.captionSize = j["capSize"].toInt(46); q.multicam = j["multicam"].toBool();
        for (auto c : j["caps"].toArray()) { auto x = c.toArray(); Caption k; k.start = x[0].toDouble(); k.end = x[1].toDouble(); k.text = x[2].toString(); for (auto w : x[3].toArray()) { auto a = w.toArray(); k.words << Word{Frame(a[0].toDouble()), Frame(a[1].toDouble()), a[2].toString()}; } q.captions << k; }
        for (auto m : j["markers"].toArray()) {
            auto x = m.toObject();
            Marker k; k.t = x["t"].toDouble(); k.dur = x["dur"].toDouble(); k.name = x["name"].toString();
            k.comment = x["comment"].toString(); k.color = x["color"].toInt();
            q.markers << k;
        }
        s.seqs << q;
    }
    s.activeSeq = o["active"].toInt();
    s.nextId = o["nextId"].toInt(1);
    return s;
}
