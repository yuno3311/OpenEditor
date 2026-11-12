// The 3.0 tools: project safety, media management, the new timeline operations, beat detection,
// interchange files and the export queue. Same table, same undo stack as the rest.
#include "aitools.h"
#include "exportqueue.h"
#include "safety.h"
#include "../core/effectdefs.h"
#include "../core/mediacache.h"
#include "../core/mediamanage.h"
#include "../core/ops.h"
#include "../core/project.h"
#include "../engine/exporter.h"
#include "../engine/interchange.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <stdexcept>

namespace {
struct ToolError3 : std::runtime_error { using std::runtime_error::runtime_error; };
}
// AiTools::call catches std::runtime_error-derived ToolError from the main table; these tools
// report failure the same way, through an error result.
namespace {
QJsonObject P(const char *name, const char *type, const char *desc, bool required = false, const char *items = nullptr)
{
    QJsonObject o{{"name", name}, {"type", type}, {"description", desc}, {"required", required}};
    if (items) o["items"] = items;
    return o;
}
QJsonObject textResult(const QString &s) { return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", s}}}}}; }
QJsonObject errorResult(const QString &s) { QJsonObject r = textResult(s); r["isError"] = true; return r; }
QJsonObject jsonResult(const QJsonValue &v)
{
    QJsonDocument d = v.isArray() ? QJsonDocument(v.toArray()) : QJsonDocument(v.toObject());
    return textResult(QString::fromUtf8(d.toJson(QJsonDocument::Compact)));
}
QSet<int> idSet(const QJsonValue &v)
{
    QSet<int> s;
    for (auto x : v.toArray()) s << x.toInt();
    return s;
}
bool writeText(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(text.toUtf8()) >= 0;
}
ExportSettings exportArgs(const QJsonObject &a, const Sequence &q, QString *err)
{
    ExportSettings c;
    c.width = q.w; c.height = q.h;
    if (a.contains("preset")) {
        if (!ExportPresets::load(a["preset"].toString(), &c)) { *err = "No such export preset: " + a["preset"].toString(); return c; }
    }
    c.path = a["path"].toString();
    if (a.contains("format")) c.format = a["format"].toString();
    if (a.contains("width")) c.width = a["width"].toInt();
    if (a.contains("height")) c.height = a["height"].toInt();
    if (a.contains("crf")) c.crf = a["crf"].toInt();
    if (a.contains("kbps")) c.kbps = a["kbps"].toInt();
    if (a.contains("chapters")) c.chapters = a["chapters"].toBool();
    c.first = Frame(a["first_frame"].toDouble(0));
    c.last = Frame(a["last_frame"].toDouble(-1));
    if (a.contains("loudness_lufs")) {
        c.loudnessLufs = a["loudness_lufs"].toDouble();
        if (c.loudnessLufs >= 0 || c.loudnessLufs < -40) *err = "loudness_lufs must be between -40 and 0, for example -14.";
    }
    return c;
}
}

void AiTools::buildV3()
{
    auto needSeq = [](QJsonObject *err) -> Sequence * {
        Sequence *q = PRJ->active();
        if (!q) *err = errorResult("No active sequence. Use create_sequence first.");
        return q;
    };

    // ------------------------------------------------------ safety
    add("autosave_now", "Write an auto-save version of the project now, whether or not it changed. Versions are kept per project and restored with restore_version.", {},
        [](const QJsonObject &) {
            QString path;
            if (!PRJ->loaded) return errorResult("No project is open.");
            if (!SAFETY->autosaveNow(true, &path)) return errorResult("Auto-save failed: " + SAFETY->lastError);
            return jsonResult(QJsonObject{{"path", path}});
        });
    add("list_versions", "List earlier versions of the open project: auto-saves and the previous save, newest first.", {}, [](const QJsonObject &) {
        QJsonArray a;
        int i = 0;
        for (auto &v : Vault::versions(PRJ->filePath, PRJ->displayName()))
            a.append(QJsonObject{{"index", i++}, {"kind", v.kind}, {"time", v.time.toString(Qt::ISODate)}, {"path", v.path}, {"bytes", double(v.size)}});
        return jsonResult(a);
    });
    add("restore_version", "Replace the open project with an earlier version from list_versions. The current state is auto-saved first, and the project file itself only changes on save.",
        {P("index", "integer", "Index from list_versions", true)}, [](const QJsonObject &a) {
            const auto list = Vault::versions(PRJ->filePath, PRJ->displayName());
            const int i = a["index"].toInt(-1);
            if (i < 0 || i >= list.size()) return errorResult("No such version.");
            if (PRJ->loaded) SAFETY->autosaveNow(true);
            QString err;
            if (!SAFETY->restoreVersion(list[i], &err)) return errorResult(err);
            return textResult("Restored the version from " + list[i].time.toString(Qt::ISODate) + ". It is unsaved until save_project.");
        });
    add("project_report", "What the project contains: media counts and size, offline and unused files, each sequence, and the effects, transitions and fonts in use.", {},
        [](const QJsonObject &) { return jsonResult(MediaManage::report(PRJ->s)); });

    // ------------------------------------------------------ media management
    add("list_offline_media", "List media whose files are missing from disk.", {}, [](const QJsonObject &) {
        QJsonArray a;
        ProjectState st = PRJ->s;
        for (int id : MediaManage::offlineMedia(st))
            if (auto *m = st.mediaById(id)) a.append(QJsonObject{{"id", m->id}, {"name", m->name}, {"path", m->path}});
        return jsonResult(a);
    });
    add("relink_media", "Reconnect offline media. Give media_id and path for one file, or folder to search it (and its sub-folders) for every missing file by name.",
        {P("media_id", "integer", "Media id to relink"), P("path", "string", "Absolute path of the file for media_id"), P("folder", "string", "Absolute folder to search for all offline files")},
        [](const QJsonObject &a) {
            int n = 0;
            QString err;
            QStringList found;
            if (a.contains("folder")) {
                if (!QFileInfo(a["folder"].toString()).isDir()) return errorResult("That folder does not exist.");
                PRJ->edit("Link Media", [&](ProjectState &st) { n = MediaManage::relinkFolder(st, a["folder"].toString(), &found); });
                if (!n) PRJ->undo.undo();
            } else {
                if (!a.contains("media_id") || !a.contains("path")) return errorResult("Give folder, or media_id and path.");
                bool ok = false;
                PRJ->edit("Link Media", [&](ProjectState &st) { ok = MediaManage::relink(st, a["media_id"].toInt(), a["path"].toString(), &err); });
                if (!ok) { PRJ->undo.undo(); return errorResult(err); }
                n = 1;
                found << a["path"].toString();
            }
            for (auto &p : found) CACHE->ensureAudio(p);
            ProjectState st = PRJ->s;
            return jsonResult(QJsonObject{{"relinked", n}, {"still_offline", MediaManage::offlineMedia(st).size()}});
        });
    add("remove_unused_media", "Remove media that is not used on any timeline from the project. Files on disk are not touched.", {}, [](const QJsonObject &) {
        int n = MediaManage::unusedMedia(PRJ->s).size();
        if (n) PRJ->edit("Remove Unused", [&](ProjectState &st) { n = MediaManage::removeUnused(st); });
        return textResult(QString("Removed %1 unused item(s).").arg(n));
    });
    add("collect_project", "Project Manager: copy the project's media into <folder>/Media and write a project file there that uses the copies, ready to archive or move to another computer.",
        {P("folder", "string", "Absolute destination folder", true), P("include_unused", "boolean", "Also copy media that is on no timeline"), P("name", "string", "Project file name without .oep")},
        [](const QJsonObject &a) {
            const QString folder = a["folder"].toString();
            if (!QFileInfo(folder).isAbsolute()) return errorResult("folder must be absolute.");
            MediaManage::CollectOptions o;
            o.includeUnused = a["include_unused"].toBool();
            o.projectName = a["name"].toString(PRJ->displayName());
            const auto r = MediaManage::collect(PRJ->s, folder, o);
            if (!r.ok) return errorResult(r.error);
            return jsonResult(QJsonObject{{"project", r.projectFile}, {"files_copied", r.copied}, {"megabytes", double(r.bytes) / 1048576}, {"offline_skipped", r.skippedOffline}});
        });

    // ------------------------------------------------------ timeline
    add("close_gaps", "Remove every empty stretch of the active sequence, where no track has a clip, and close up the timeline.", {}, [needSeq](const QJsonObject &) {
        QJsonObject e;
        if (!needSeq(&e)) return e;
        int n = Ops::findGaps(*PRJ->active()).size();
        if (n) PRJ->edit("Close Gaps", [&](ProjectState &st) { n = Ops::closeGaps(st, *st.seqById(st.activeSeq)); });
        return textResult(QString("Closed %1 gap(s).").arg(n));
    });
    add("duplicate_clips", "Duplicate clips: the copies go in right after the originals and later clips move along.", {P("clip_ids", "array", "Clip ids", true, "integer")},
        [needSeq](const QJsonObject &a) {
            QJsonObject e;
            if (!needSeq(&e)) return e;
            const QSet<int> ids = PRJ->withLinked(idSet(a["clip_ids"]));
            QVector<int> made;
            PRJ->edit("Duplicate", [&](ProjectState &st) { made = Ops::duplicateClips(st, *st.seqById(st.activeSeq), ids); });
            if (made.isEmpty()) { PRJ->undo.undo(); return errorResult("None of those clips exist."); }
            QJsonArray out;
            for (int id : made) out.append(id);
            return jsonResult(QJsonObject{{"new_clip_ids", out}});
        });
    add("replace_clip", "Replace With Clip: swap a timeline clip's source for another media item, keeping its position, length and effects.",
        {P("clip_id", "integer", "Clip id", true), P("media_id", "integer", "New media id", true)}, [needSeq](const QJsonObject &a) {
            QJsonObject e;
            if (!needSeq(&e)) return e;
            QString err;
            bool ok = false;
            PRJ->edit("Replace Clip", [&](ProjectState &st) { ok = Ops::replaceClip(st, *st.seqById(st.activeSeq), a["clip_id"].toInt(), a["media_id"].toInt(), &err); });
            if (!ok) { PRJ->undo.undo(); return errorResult(err); }
            return textResult("Replaced.");
        });
    add("extend_to_frame", "Extend Edit: move the nearer edge of each clip to a frame, as far as its media and its neighbours allow.",
        {P("clip_ids", "array", "Clip ids", true, "integer"), P("frame", "integer", "Sequence frame", true)}, [needSeq](const QJsonObject &a) {
            QJsonObject e;
            if (!needSeq(&e)) return e;
            int n = 0;
            const QSet<int> ids = PRJ->withLinked(idSet(a["clip_ids"]));
            PRJ->edit("Extend Edit", [&](ProjectState &st) { n = Ops::extendToFrame(st, *st.seqById(st.activeSeq), ids, Frame(a["frame"].toDouble())); });
            if (!n) { PRJ->undo.undo(); return errorResult("Nothing could move: the clips are already there, or have no more media."); }
            return textResult(QString("Moved %1 edge(s).").arg(n));
        });
    add("fade_clip", "Fade a clip in and out: opacity keyframes on video, volume keyframes on audio. Zero for both removes the fades.",
        {P("clip_id", "integer", "Clip id", true), P("fade_in_frames", "integer", "Length of the fade in"), P("fade_out_frames", "integer", "Length of the fade out")},
        [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            if (!q->clip(a["clip_id"].toInt())) return errorResult("No such clip.");
            PRJ->edit("Fade", [&](ProjectState &st) { Ops::fadeClip(*st.seqById(st.activeSeq)->clip(a["clip_id"].toInt()), Frame(a["fade_in_frames"].toDouble()), Frame(a["fade_out_frames"].toDouble())); });
            return textResult("Done.");
        });
    add("apply_layout", QString("Place a video clip with a layout preset. Presets: %1.").arg(Ops::layoutPresets().join(", ")),
        {P("clip_id", "integer", "Video clip id", true), P("preset", "string", "Preset name", true)}, [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            const Clip *c = q->clip(a["clip_id"].toInt());
            if (!c || c->kind == MediaKind::Audio) return errorResult("Give the id of a video clip.");
            if (!Ops::layoutPresets().contains(a["preset"].toString())) return errorResult("Unknown preset. Use one of: " + Ops::layoutPresets().join(", "));
            PRJ->edit("Layout", [&](ProjectState &st) { Sequence &s = *st.seqById(st.activeSeq); Ops::applyLayout(s, *s.clip(a["clip_id"].toInt()), a["preset"].toString()); });
            return textResult("Done.");
        });
    add("auto_reframe", "Auto Reframe: copy the active sequence into a new one of another frame size (for example 1080x1920 for vertical video), scaling every picture to fill it. The original is unchanged.",
        {P("width", "integer", "New width", true), P("height", "integer", "New height", true), P("name", "string", "Name of the new sequence")}, [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            const int w = a["width"].toInt(), h = a["height"].toInt();
            if (w < 16 || h < 16 || w > 8192 || h > 8192) return errorResult("width and height must be between 16 and 8192.");
            int id = 0;
            const int from = q->id;
            PRJ->edit("Auto Reframe", [&](ProjectState &st) { id = Ops::autoReframe(st, from, w, h, a["name"].toString()); if (id) st.activeSeq = id; });
            emit PRJ->activeSeqChanged();
            return jsonResult(QJsonObject{{"sequence_id", id}, {"name", PRJ->s.seqById(id) ? PRJ->s.seqById(id)->name : QString()}});
        });
    add("set_keyframe_interpolation", "Set how values move between a clip's keyframes: linear, hold, ease, ease_in or ease_out. Leave effect or param empty for all.",
        {P("clip_id", "integer", "Clip id", true), P("type", "string", "linear, hold, ease, ease_in or ease_out", true), P("effect", "string", "Effect id"), P("param", "string", "Parameter id")},
        [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            static const QStringList names{"linear", "hold", "ease", "ease_in", "ease_out"};
            const int t = names.indexOf(a["type"].toString());
            if (t < 0) return errorResult("type must be one of: " + names.join(", "));
            if (!q->clip(a["clip_id"].toInt())) return errorResult("No such clip.");
            int n = 0;
            PRJ->edit("Keyframe Interpolation", [&](ProjectState &st) { n = Ops::setInterpolation(*st.seqById(st.activeSeq)->clip(a["clip_id"].toInt()), a["effect"].toString(), a["param"].toString(), Interp(t)); });
            if (!n) { PRJ->undo.undo(); return errorResult("No keyframes matched."); }
            return textResult(QString("Changed %1 keyframe(s).").arg(n));
        });

    // ------------------------------------------------------ analysis
    add("detect_beats", "Find the tempo and beats of the active sequence's audio, and optionally put a marker on each beat to cut to.",
        {P("add_markers", "boolean", "Add a marker named Beat on the beats (replaces earlier beat markers)"), P("every", "integer", "Mark every nth beat, default 1"),
         P("first_frame", "integer", "Range start"), P("last_frame", "integer", "Range end, exclusive")},
        [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            const Frame first = Frame(a["first_frame"].toDouble(0)), last = Frame(a["last_frame"].toDouble(-1));
            const Analysis::Beats b = Analysis::sequenceBeats(PRJ->s, q->id, first, last);
            if (b.bpm <= 0) return errorResult("No steady beat was found in the audio.");
            int marked = 0;
            if (a["add_markers"].toBool())
                PRJ->edit("Beat Markers", [&](ProjectState &st) { marked = Analysis::addBeatMarkers(*st.seqById(st.activeSeq), b, first, qMax(1, a["every"].toInt(1))); });
            QJsonArray frames;
            for (double t : b.times) frames.append(double(first + Frame(std::llround(t * q->fps()))));
            return jsonResult(QJsonObject{{"bpm", std::round(b.bpm * 10) / 10}, {"beats", int(b.times.size())}, {"markers_added", marked}, {"beat_frames", frames}});
        });

    // ------------------------------------------------------ interchange
    add("export_edl", "Write the active sequence as a CMX3600 edit decision list (.edl) for conforming in another editor or grading tool.",
        {P("path", "string", "Absolute .edl path", true)}, [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            if (!QFileInfo(a["path"].toString()).isAbsolute()) return errorResult("path must be absolute.");
            const QString text = Interchange::writeEdl(PRJ->s, *q);
            if (!writeText(a["path"].toString(), text)) return errorResult("Could not write the file.");
            return jsonResult(QJsonObject{{"path", a["path"].toString()}, {"events", Interchange::parseEdl(text).size()}});
        });
    add("export_markers", "Write the active sequence's markers to a file: csv (a spreadsheet list) or youtube (chapter lines to paste into a video description).",
        {P("path", "string", "Absolute output path", true), P("format", "string", "csv or youtube, default csv")}, [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            if (!QFileInfo(a["path"].toString()).isAbsolute()) return errorResult("path must be absolute.");
            if (q->markers.isEmpty()) return errorResult("The sequence has no markers.");
            const QString fmt = a["format"].toString("csv");
            if (fmt != "csv" && fmt != "youtube") return errorResult("format must be csv or youtube.");
            if (!writeText(a["path"].toString(), fmt == "csv" ? Interchange::markersCsv(*q) : Interchange::youtubeChapters(*q))) return errorResult("Could not write the file.");
            return jsonResult(QJsonObject{{"path", a["path"].toString()}, {"markers", q->markers.size()}});
        });

    // ------------------------------------------------------ export presets and queue
    add("list_export_presets", "List the export presets the user saved, with their settings.", {}, [](const QJsonObject &) {
        QJsonArray a;
        for (auto &n : ExportPresets::names()) {
            ExportSettings s;
            ExportPresets::load(n, &s);
            QJsonObject o = ExportPresets::toJson(s);
            o["name"] = n;
            a.append(o);
        }
        return jsonResult(a);
    });
    add("save_export_preset", "Save export settings under a name for reuse in export_sequence and queue_export.",
        {P("name", "string", "Preset name", true), P("format", "string", "Format name, default H.264"), P("width", "integer", "Frame width"), P("height", "integer", "Frame height"),
         P("crf", "integer", "Quality"), P("kbps", "integer", "Target bitrate, 0 for constant quality"), P("loudness_lufs", "number", "Loudness target"), P("chapters", "boolean", "Write markers as chapters")},
        [](const QJsonObject &a) {
            const QString name = a["name"].toString().trimmed();
            if (name.isEmpty()) return errorResult("Give the preset a name.");
            ExportSettings s;
            ExportPresets::load(name, &s);
            QJsonObject j = ExportPresets::toJson(s);
            for (const char *k : {"format", "width", "height", "crf", "kbps", "chapters"}) if (a.contains(k)) j[k] = a[k];
            if (a.contains("loudness_lufs")) j["lufs"] = a["loudness_lufs"];
            s = ExportPresets::fromJson(j);
            bool known = false;
            for (auto &f : exportFormats()) known |= f.name == s.format;
            if (!known) return errorResult("Unknown format: " + s.format);
            ExportPresets::save(name, s);
            return textResult("Saved preset " + name + ".");
        });
    add("queue_export", "Add an export of the active sequence to the render queue. It is rendered from the project as it is now, when start_export_queue runs.",
        {P("path", "string", "Absolute output path", true), P("preset", "string", "A saved export preset"), P("format", "string", "Format name, default H.264"), P("width", "integer", "Frame width"),
         P("height", "integer", "Frame height"), P("crf", "integer", "Quality"), P("kbps", "integer", "Target bitrate"), P("first_frame", "integer", "Range start"), P("last_frame", "integer", "Range end, exclusive"),
         P("loudness_lufs", "number", "Loudness target"), P("chapters", "boolean", "Write markers as chapters"), P("overwrite", "boolean", "Replace an existing file")},
        [needSeq](const QJsonObject &a) {
            QJsonObject e;
            Sequence *q = needSeq(&e);
            if (!q) return e;
            QString err;
            const ExportSettings c = exportArgs(a, *q, &err);
            if (!err.isEmpty()) return errorResult(err);
            if (QFileInfo::exists(c.path) && !a["overwrite"].toBool()) return errorResult("That file exists. Set overwrite to true to replace it.");
            const int id = EXPORTQ->add(PRJ->s, q->id, c, &err);
            if (!id) return errorResult(err);
            return jsonResult(QJsonObject{{"queue_id", id}, {"waiting", EXPORTQ->pending()}});
        });
    add("start_export_queue", "Start rendering the export queue, one export after another, in the background.", {}, [](const QJsonObject &) {
        if (!EXPORTQ->pending()) return errorResult("The queue is empty.");
        EXPORTQ->start();
        return textResult(QString("Rendering %1 export(s). Use export_queue_status to follow them.").arg(EXPORTQ->pending() + 1));
    });
    add("export_queue_status", "The render queue: every export with its state (queued, running, done, failed, stopped) and progress.", {},
        [](const QJsonObject &) { return jsonResult(QJsonObject{{"running", EXPORTQ->running()}, {"items", EXPORTQ->toJson()}}); });
    add("clear_export_queue", "Stop the running export, if any, and remove finished and waiting exports from the queue.", {}, [](const QJsonObject &) {
        EXPORTQ->stop();
        const auto items = EXPORTQ->items();
        for (auto &i : items) EXPORTQ->remove(i.id);
        return textResult("Cleared.");
    });
}
