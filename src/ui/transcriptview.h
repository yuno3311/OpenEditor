// Text-based editing: the transcript reads like a document. Click a word to go there, select words
// and press Delete to cut that footage from every track.
#pragma once
#include "../core/model.h"
#include <QPlainTextEdit>

class TranscriptView : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit TranscriptView(QWidget *p = nullptr);
    void rebuild();
    bool selectWords(int firstWord, int lastWord);      // used by tests and search
    int wordCount() const { return map.size(); }
    bool find(const QString &text, bool forward);
    QVector<QPair<Frame, Frame>> selectedRanges() const;
    void deleteSelection();
protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
private:
    struct Span { int pos, len; Frame start, end; };
    void highlight();
    int wordAt(int pos) const;
    QVector<Span> map;
    QString signature;
    QPoint pressAt;
    bool wasSelecting = false;
};
