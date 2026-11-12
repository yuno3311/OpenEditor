// Render queue: exports wait in line and run one after another, each from a snapshot of the
// project taken when it was queued, so editing can continue while they render.
#pragma once
#include "../core/model.h"
#include "../engine/exporter.h"
#include <QJsonArray>
#include <QObject>

class ExportQueue : public QObject {
    Q_OBJECT
public:
    struct Item {
        int id = 0;
        QString sequence, state = "queued", message; // queued, running, done, failed, stopped
        double progress = 0;
        ExportSettings cfg;
        ProjectState snapshot;
        int seqId = 0;
    };
    static ExportQueue *instance();
    int add(const ProjectState &st, int seqId, const ExportSettings &cfg, QString *err = nullptr);
    void start();
    void stop();                 // stops the running export; queued ones stay queued
    bool remove(int id);         // not while it is running
    void clearFinished();
    bool running() const { return job != nullptr; }
    int pending() const;
    const QVector<Item> &items() const { return list; }
    QJsonArray toJson() const;
signals:
    void changed();
    void finishedAll(int done, int failed);
private:
    explicit ExportQueue(QObject *p = nullptr);
    void next();
    Item *find(int id);
    QVector<Item> list;
    Exporter *job = nullptr;
    int current = 0, nextId = 1, doneCount = 0, failCount = 0;
    bool wantStop = false;
};
#define EXPORTQ (ExportQueue::instance())
