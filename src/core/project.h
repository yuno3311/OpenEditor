// Project document: owns the state, the undo stack and the (non-undoable) UI selection.
#pragma once
#include "model.h"
#include "vault.h"
#include "vault.h"
#include <QObject>
#include <QSet>
#include <QUndoStack>
#include <functional>

class Project : public QObject {
    Q_OBJECT
public:
    static Project *instance();
    ProjectState s;
    QUndoStack undo;
    QString filePath;
    QSet<int> selClips;
    QPair<int, Frame> selTransition{0, 0}; // (trackId, cut), trackId 0 = none
    bool loaded = false;

    Sequence *active() { return s.seqById(s.activeSeq); }
    void edit(const QString &label, const std::function<void(ProjectState &)> &f);
    void liveBegin();
    void liveTouch();                         // emit change notification during a live drag
    void liveCommit(const QString &label);
    void liveCancel();
    bool inLive() const { return live; }

    void reset();
    bool save(const QString &path);
    bool saveCopy(const QString &path);          // writes the file without making it the project file
    bool load(const QString &path);
    // Replaces the project with a recovered or restored state. filePath stays where Save should go.
    void adopt(const ProjectState &st, const QString &path, bool dirty);
    bool isDirty() const { return !undo.isClean(); }
    Vault::LoadResult lastLoad = Vault::LoadResult::Ok;
    QString lastError;
    quint64 revision = 0;                        // counts model changes; auto-save skips unchanged states
    QString displayName() const;

    void setPlayhead(Frame f);
    void setSelection(const QSet<int> &sel);
    void selectTransition(int trackId, Frame cut);
    void setActiveSequence(int id);
    QSet<int> withLinked(const QSet<int> &sel) const;
    void applySnapshot(const ProjectState &st);
    int createSequence(const QString &name, int w, int h, int fpsNum, int fpsDen, int vt = 3, int at = 3);

    bool linkedSelection = true;
    bool snapping = true;
signals:
    void changed();          // any model change
    void playheadChanged(Frame f);
    void selectionChanged();
    void activeSeqChanged();
    void dirtyChanged();
private:
    explicit Project(QObject *p = nullptr);
    ProjectState liveBase;
    bool live = false;
};
#define PRJ (Project::instance())
