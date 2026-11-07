// Tools, Effects, Info, History, Markers, Media Browser, Metadata, Audio Meters, Audio Mixer.
#pragma once
#include "editstate.h"
#include "panels.h"
#include "widgets.h"
#include "../engine/playback.h"
#include <QButtonGroup>
#include <QDial>
#include <QFileSystemModel>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <functional>

class ToolsPanel : public Panel {
    Q_OBJECT
public:
    explicit ToolsPanel(QWidget *p = nullptr);
private:
    QButtonGroup *group;
};

class EffectsTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit EffectsTree(QWidget *p);
protected:
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override;
    QStringList mimeTypes() const override;
};

class EffectsPanel : public Panel {
    Q_OBJECT
public:
    explicit EffectsPanel(QWidget *p = nullptr);
private:
    void build(const QString &filter);
    void applyItem(QTreeWidgetItem *i);
    EffectsTree *tree;
    QLineEdit *search;
};

class InfoPanel : public Panel {
    Q_OBJECT
public:
    explicit InfoPanel(QWidget *p = nullptr);
protected:
    void showEvent(QShowEvent *) override { refresh(); }
private:
    void refresh();
    QLabel *lab;
};

class HistoryPanel : public Panel {
    Q_OBJECT
public:
    explicit HistoryPanel(QWidget *p = nullptr);
};

class MarkersPanel : public Panel {
    Q_OBJECT
public:
    explicit MarkersPanel(QWidget *p = nullptr);
private:
    void refresh();
    QTreeWidget *list;
    bool busy = false;
};

class MediaBrowserPanel : public Panel {
    Q_OBJECT
public:
    explicit MediaBrowserPanel(QWidget *p = nullptr);
signals:
    void importRequested(const QStringList &paths);
private:
    QFileSystemModel *model;
    QTreeView *view;
};

class MetadataPanel : public Panel {
    Q_OBJECT
public:
    explicit MetadataPanel(QWidget *p = nullptr);
    void show(int mediaId);
private:
    QTreeWidget *list;
};

class MeterBar : public QWidget {
    Q_OBJECT
public:
    explicit MeterBar(QWidget *p, bool scale = false);
    void setLevels(float l, float r);
    QSize sizeHint() const override { return QSize(scale ? 46 : 20, 120); }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    float cur[2] = {0, 0}, hold[2] = {0, 0};
    int holdT[2] = {0, 0};
    bool clip[2] = {false, false};
    bool scale;
};

class PanKnob : public QDial {
    Q_OBJECT
public:
    explicit PanKnob(QWidget *p) : QDial(p) {}
protected:
    void paintEvent(QPaintEvent *) override;
};

class AudioMetersPanel : public Panel {
    Q_OBJECT
public:
    explicit AudioMetersPanel(QWidget *p = nullptr);
    std::function<PlaybackEngine *()> source;
private:
    MeterBar *bar;
    QTimer timer;
};

class AudioMixerPanel : public Panel {
    Q_OBJECT
public:
    explicit AudioMixerPanel(QWidget *p = nullptr);
    std::function<PlaybackEngine *()> source;
private:
    void rebuild();
    void trackFxMenu(int trackId, QWidget *anchor);
    void trackFxDialog(int trackId, int index);
    QWidget *strips = nullptr;
    class QScrollArea *scroll;
    QString sig;
    QVector<MeterBar *> meters;
    MeterBar *master = nullptr;
    QTimer timer;
};
