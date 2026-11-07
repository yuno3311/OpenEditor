#pragma once
#include "editstate.h"
#include "panels.h"
#include "widgets.h"
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTreeWidget>

class ProjectTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit ProjectTree(QWidget *p);
protected:
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override;
    QStringList mimeTypes() const override;
    void dropEvent(QDropEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
signals:
    void filesDropped(const QStringList &paths);
    void moved(const QList<int> &ids, int toBin);
};

class ProjectGrid : public QListWidget {
    Q_OBJECT
public:
    explicit ProjectGrid(QWidget *p);
protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
    QStringList mimeTypes() const override;
};

class ProjectPanel : public Panel {
    Q_OBJECT
public:
    explicit ProjectPanel(QWidget *p = nullptr);
    void fillMenu(QMenu *m) override;
    void importFiles(const QStringList &paths);
    void importDialog();
    void newBin();
    void newSequenceDialog();
    void newColorMatte();
    void newBars();
    void newBlack();
    void newAdjustment();
    void deleteSelected();
    QList<int> selectedMedia() const;
signals:
    void openInSource(int mediaId);
    void openSequence(int seqId);
    void multicamRequested();
    void proxiesRequested();
private:
    void rebuild();
    void activated(int id);
    void contextMenu(const QPoint &g);
    void addGenerated(MediaKind k, const QString &name, const QColor &c, int w = 0, int h = 0);
    QIcon iconFor(const MediaItem &m) const;
    void addTreeChildren(QTreeWidgetItem *parent, int bin, const QString &filter);
    ProjectTree *tree;
    ProjectGrid *grid;
    QLineEdit *search;
    QLabel *count;
    QToolButton *listBtn, *gridBtn;
    bool gridMode = false;
    bool rebuilding = false;
};

class NewSequenceDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewSequenceDialog(QWidget *p = nullptr);
    QString name() const;
    int widthPx() const;
    int heightPx() const;
    void rate(int &num, int &den) const;
    int videoTracks() const;
    int audioTracks() const;
private:
    QLineEdit *nameEd;
    QComboBox *preset, *rateBox;
    class QSpinBox *wEd, *hEd, *vt, *at;
};
