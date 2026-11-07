#pragma once
#include "widgets.h"
#include "../engine/exporter.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileSystemModel>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

QIcon appIcon();

class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(QWidget *p = nullptr);
    void refreshRecent();
signals:
    void newProject();
    void openProject();
    void openPath(const QString &path);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QListWidget *recent;
};

class ImportPage : public QWidget {
    Q_OBJECT
public:
    explicit ImportPage(QWidget *p = nullptr);
signals:
    void importFiles(const QStringList &paths, bool makeSequence);
private:
    void go(const QString &path);
    QFileSystemModel *model;
    QListView *view;
    QLabel *pathLabel, *selLabel;
    QCheckBox *makeSeq;
    QPushButton *importBtn;
};

class ExportPage : public QWidget {
    Q_OBJECT
public:
    explicit ExportPage(QWidget *p = nullptr);
    void prepare(const QImage &preview);
private:
    void fromPreset();
    void updateSummary();
    void start();
    QLineEdit *fileName, *folder;
    QComboBox *format, *preset, *speed, *range, *loud;
    QSpinBox *wEd, *hEd, *crf, *kbps, *aKbps;
    QCheckBox *withV, *withA, *sidecar;
    QLabel *preview, *summary, *status;
    QProgressBar *bar;
    QPushButton *exportBtn, *cancelBtn;
    Exporter *job = nullptr;
};
