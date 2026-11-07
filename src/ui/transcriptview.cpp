#include "transcriptview.h"
#include "theme.h"
#include "../core/ops.h"
#include "../core/project.h"
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

TranscriptView::TranscriptView(QWidget *p) : QPlainTextEdit(p)
{
    setReadOnly(true);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setStyleSheet("QPlainTextEdit{background:#1f1f1f;border:0;padding:8px;font-size:15px;selection-background-color:#2d6fb5;}");
    setPlaceholderText(tr("Press Transcribe Sequence. Your words appear here, and deleting them cuts the video."));
    connect(PRJ, &Project::changed, this, &TranscriptView::rebuild);
    connect(PRJ, &Project::playheadChanged, this, &TranscriptView::highlight);
    rebuild();
}

void TranscriptView::rebuild()
{
    auto q = PRJ->active();
    QString sig;
    if (q) for (auto &c : q->captions) sig += QString("%1:%2:%3|").arg(c.start).arg(c.end).arg(c.text);
    if (sig == signature) return;
    signature = sig;
    const int scroll = verticalScrollBar()->value();
    map.clear();
    QString doc;
    if (q) {
        for (auto &c : q->captions) {
            if (!doc.isEmpty()) doc += '\n';
            if (c.words.isEmpty()) {
                map << Span{int(doc.size()), int(c.text.size()), c.start, c.end};
                doc += QString(c.text).replace('\n', ' ');
                continue;
            }
            for (int i = 0; i < c.words.size(); ++i) {
                const Word &w = c.words[i];
                if (i && !doc.endsWith('\n')) doc += ' ';
                map << Span{int(doc.size()), int(w.text.size()), w.start, w.end};
                doc += w.text;
            }
        }
    }
    setPlainText(doc);
    verticalScrollBar()->setValue(scroll);
    highlight();
}

int TranscriptView::wordAt(int pos) const
{
    for (int i = 0; i < map.size(); ++i)
        if (pos < map[i].pos + map[i].len) return i;
    return map.isEmpty() ? -1 : map.size() - 1;
}

void TranscriptView::highlight()
{
    auto q = PRJ->active();
    QList<QTextEdit::ExtraSelection> sel;
    if (q)
        for (auto &s : std::as_const(map))
            if (q->playhead >= s.start && q->playhead < s.end) {
                QTextEdit::ExtraSelection e;
                e.format.setBackground(QColor("#5a4a1f"));
                e.format.setForeground(Qt::white);
                e.cursor = textCursor();
                e.cursor.setPosition(s.pos);
                e.cursor.setPosition(s.pos + s.len, QTextCursor::KeepAnchor);
                sel << e;
                break;
            }
    setExtraSelections(sel);
}

bool TranscriptView::selectWords(int a, int b)
{
    if (a < 0 || b >= map.size() || a > b) return false;
    QTextCursor c = textCursor();
    c.setPosition(map[a].pos);
    c.setPosition(map[b].pos + map[b].len, QTextCursor::KeepAnchor);
    setTextCursor(c);
    return true;
}

bool TranscriptView::find(const QString &text, bool forward)
{
    if (text.isEmpty()) return false;
    if (QPlainTextEdit::find(text, forward ? QTextDocument::FindFlags() : QTextDocument::FindBackward)) return true;
    QTextCursor c = textCursor();
    c.movePosition(forward ? QTextCursor::Start : QTextCursor::End);
    setTextCursor(c);
    return QPlainTextEdit::find(text, forward ? QTextDocument::FindFlags() : QTextDocument::FindBackward);
}

// Contiguous words become one range running up to the next kept word, so no breath or tail of a
// deleted word is left behind.
QVector<QPair<Frame, Frame>> TranscriptView::selectedRanges() const
{
    QVector<QPair<Frame, Frame>> out;
    const QTextCursor c = textCursor();
    if (!c.hasSelection()) return out;
    const int from = c.selectionStart(), to = c.selectionEnd();
    auto q = const_cast<Project *>(PRJ)->active();
    const Frame pad = q ? Frame(q->fps() * .35) : 10;
    for (int i = 0; i < map.size(); ++i) {
        const Span &s = map[i];
        if (s.pos + s.len <= from || s.pos >= to) continue;
        Frame a = s.start, b = s.end;
        if (i + 1 < map.size() && map[i + 1].start - b <= pad) b = map[i + 1].start;
        if (!out.isEmpty() && a <= out.last().second) out.last().second = qMax(out.last().second, b);
        else out << qMakePair(a, b);
    }
    return out;
}

void TranscriptView::deleteSelection()
{
    auto ranges = selectedRanges();
    if (ranges.isEmpty() || !PRJ->active()) return;
    const Frame first = ranges.first().first;
    PRJ->edit(tr("Delete Transcript Text"), [&](ProjectState &st) { Ops::rippleRemoveRanges(st, *st.seqById(st.activeSeq), ranges); });
    PRJ->setSelection({});
    PRJ->setPlayhead(first);
}

bool TranscriptView::event(QEvent *e)
{
    // These keys belong to the transcript while it has focus, not to the window shortcuts.
    if (e->type() == QEvent::ShortcutOverride) {
        auto *k = static_cast<QKeyEvent *>(e);
        if ((k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) && textCursor().hasSelection()) { e->accept(); return true; }
    }
    return QPlainTextEdit::event(e);
}

void TranscriptView::keyPressEvent(QKeyEvent *e)
{
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && textCursor().hasSelection()) { deleteSelection(); return; }
    QPlainTextEdit::keyPressEvent(e);
}

void TranscriptView::mousePressEvent(QMouseEvent *e)
{
    pressAt = e->pos();
    wasSelecting = textCursor().hasSelection();
    QPlainTextEdit::mousePressEvent(e);
}

void TranscriptView::mouseReleaseEvent(QMouseEvent *e)
{
    QPlainTextEdit::mouseReleaseEvent(e);
    if (e->button() != Qt::LeftButton || (e->pos() - pressAt).manhattanLength() > 4 || wasSelecting) return;
    int w = wordAt(cursorForPosition(e->pos()).position());
    if (w >= 0) PRJ->setPlayhead(map[w].start);
}

void TranscriptView::contextMenuEvent(QContextMenuEvent *e)
{
    auto ranges = selectedRanges();
    QMenu m(this);
    auto *del = m.addAction(tr("Delete from Timeline"), [this] { deleteSelection(); });
    del->setEnabled(!ranges.isEmpty());
    auto *mark = m.addAction(tr("Mark In and Out"), [ranges] {
        PRJ->edit(tr("Mark Selection"), [&](ProjectState &st) { auto *q = st.seqById(st.activeSeq); q->inPt = ranges.first().first; q->outPt = ranges.last().second; });
    });
    mark->setEnabled(!ranges.isEmpty());
    auto *marker = m.addAction(tr("Add Marker"), [this, ranges] {
        const QString text = textCursor().selectedText().left(60);
        PRJ->edit(tr("Add Marker"), [&](ProjectState &st) {
            auto *q = st.seqById(st.activeSeq);
            Marker k; k.t = ranges.first().first; k.name = text; k.color = 13;
            q->markers << k;
            std::sort(q->markers.begin(), q->markers.end(), [](const Marker &a, const Marker &b) { return a.t < b.t; });
        });
    });
    marker->setEnabled(!ranges.isEmpty());
    m.addSeparator();
    m.addAction(tr("Copy"), this, &QPlainTextEdit::copy)->setEnabled(textCursor().hasSelection());
    m.exec(e->globalPos());
}
