#include "fonts.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QSet>
#include <QStandardPaths>

namespace Fonts {

static QHash<QString, QStringList> &loaded() // canonical file path -> families
{
    static QHash<QString, QStringList> h;
    return h;
}

QString folder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Fonts";
}

bool isFontFile(const QString &path)
{
    static const QSet<QString> ext = {"ttf", "otf", "ttc", "otc", "woff", "woff2"};
    return ext.contains(QFileInfo(path).suffix().toLower());
}

static QStringList addFile(const QString &file)
{
    const QString key = QFileInfo(file).canonicalFilePath();
    if (loaded().contains(key)) return loaded()[key];
    const int id = QFontDatabase::addApplicationFont(file);
    const QStringList fam = id < 0 ? QStringList() : QFontDatabase::applicationFontFamilies(id);
    if (!fam.isEmpty()) loaded().insert(key, fam);
    return fam;
}

QStringList loadImported()
{
    QStringList all;
    for (const QFileInfo &fi : QDir(folder()).entryInfoList(QDir::Files))
        if (isFontFile(fi.filePath())) all << addFile(fi.filePath());
    all.removeDuplicates();
    return all;
}

static QByteArray digest(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result();
}

QStringList import(const QString &path, QString *error)
{
    auto fail = [&](const QString &e) { if (error) *error = e; return QStringList(); };
    QFileInfo src(path);
    if (!src.isFile()) return fail(QObject::tr("%1 does not exist.").arg(QDir::toNativeSeparators(path)));
    if (!isFontFile(path)) return fail(QObject::tr("%1 is not a font file (.ttf, .otf, .ttc, .otc, .woff, .woff2).").arg(src.fileName()));
    // Check the font loads before copying it anywhere.
    const int probe = QFontDatabase::addApplicationFont(path);
    if (probe < 0) return fail(QObject::tr("%1 could not be read as a font.").arg(src.fileName()));
    QFontDatabase::removeApplicationFont(probe);

    QDir().mkpath(folder());
    QString dest = folder() + "/" + src.fileName();
    const QByteArray want = digest(path);
    // A different font with the same file name gets a numbered name; the same font is reused.
    for (int n = 2; QFileInfo::exists(dest) && digest(dest) != want; ++n)
        dest = folder() + "/" + src.completeBaseName() + QString("-%1.").arg(n) + src.suffix();
    if (!QFileInfo::exists(dest) && !QFile::copy(path, dest)) return fail(QObject::tr("Could not copy the font into %1.").arg(QDir::toNativeSeparators(folder())));
    const QStringList fam = addFile(dest);
    if (fam.isEmpty()) return fail(QObject::tr("%1 could not be loaded.").arg(src.fileName()));
    return fam;
}

QStringList importedFamilies()
{
    QStringList all;
    for (auto &f : std::as_const(loaded())) all << f;
    all.removeDuplicates();
    all.sort(Qt::CaseInsensitive);
    return all;
}

bool available(const QString &family)
{
    static QSet<QString> cache;
    static int seen = -1;
    // Rebuilt whenever a font is added, since the database grows during a session.
    if (seen != loaded().size()) {
        cache.clear();
        for (auto &f : QFontDatabase::families()) cache << f.toLower();
        seen = loaded().size();
    }
    return cache.contains(family.toLower());
}

QStringList missingFamilies(const ProjectState &st)
{
    QSet<QString> out;
    for (auto &q : st.seqs)
        for (auto &t : q.video)
            for (auto &c : t.clips)
                if (c.kind == MediaKind::Text && !c.text.font.isEmpty() && !available(c.text.font)) out << c.text.font;
    QStringList l(out.begin(), out.end());
    l.sort(Qt::CaseInsensitive);
    return l;
}

} // namespace Fonts
