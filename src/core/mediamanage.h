// Media management: relinking offline files, finding and removing unused media, collecting a
// project and its files into one folder, and a report of what a project contains.
#pragma once
#include "model.h"
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <functional>

namespace MediaManage {
QVector<int> offlineMedia(ProjectState &st, bool recheck = true); // ids; recheck updates the flags from disk
bool relink(ProjectState &st, int mediaId, const QString &newPath, QString *err = nullptr);
// Looks for every offline file by name under folder (sub-folders too). A file whose size matches
// the one recorded at import wins over one that only shares the name. Returns how many relinked.
int relinkFolder(ProjectState &st, const QString &folder, QStringList *relinked = nullptr);

QSet<int> usedMedia(const ProjectState &st);   // on any timeline, following nested sequences
QVector<int> unusedMedia(const ProjectState &st);
int removeUnused(ProjectState &st);

struct CollectOptions {
    bool includeUnused = false;
    QString projectName;                       // file name of the collected project, without .oep
    std::function<bool(int done, int total, const QString &file)> progress; // return false to cancel
};
struct CollectResult {
    bool ok = false;
    QString error, projectFile;
    int copied = 0, skippedOffline = 0;
    qint64 bytes = 0;
};
// Copies the media into <dest>/Media and writes a project there that points at the copies.
CollectResult collect(const ProjectState &st, const QString &dest, const CollectOptions &opt);
qint64 collectSize(const ProjectState &st, bool includeUnused);

QJsonObject report(const ProjectState &st);    // counts, durations, offline files, effects, fonts
QString reportText(const ProjectState &st);
}
