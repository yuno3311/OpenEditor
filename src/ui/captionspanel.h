// Captions and transcript panel, and the multicam viewer.
#pragma once
#include "panels.h"
#include "transcriptview.h"
#include "widgets.h"
#include "../engine/analysis.h"
#include "../engine/compositor.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <condition_variable>
#include <mutex>
#include <thread>

class CaptionsPanel : public Panel {
    Q_OBJECT
public:
    explicit CaptionsPanel(QWidget *p = nullptr);
    void transcribe();
    TranscriptView *transcriptView() const { return transcript; }
    void beginTranscribeForTest(const QString &model) { beginTranscribe(model); }
    bool transcribing() const { return job != nullptr; }
    void importFile();
    void exportFile();
private:
    void refresh();
    void removeSelectedFootage();
    void addCaption();
    void deleteSelected();
    void beginTranscribe(const QString &modelPath);
    QTableWidget *table;
    TranscriptView *transcript;
    QTabWidget *tabs;
    QLineEdit *searchBox;
    QComboBox *modelBox, *langBox;
    QPushButton *goBtn, *cancelBtn;
    QProgressBar *bar;
    QLabel *status;
    QCheckBox *burn;
    QSpinBox *size;
    bool busy = false;
    Transcriber *job = nullptr;
};

class MulticamPanel : public Panel {
    Q_OBJECT
public:
    explicit MulticamPanel(QWidget *p = nullptr);
    ~MulticamPanel() override;
    void cut(int angle);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
private:
    QRect cell(int i, int n) const;
    const Clip *currentClip() const;
    void request();
    void worker();
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool quit = false, want = false;
    std::shared_ptr<const ProjectState> snap;
    int seqId = 0;
    Frame frame = 0;
    int angles = 0;
    QVector<QImage> images;
    QTimer timer;
};
