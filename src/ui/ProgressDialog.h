#pragma once

#include <QDialog>

class QProgressBar;
class QLabel;

namespace Miryu {

class ProgressDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProgressDialog(QWidget *parent = nullptr);

    // The main label describes the current phase, while the detail label
    // mirrors yumex-ng's subtitle with the current package/action and its
    // own progress. MainWindow::onOverallProgress() drives all three.
    void setMessage(const QString &message);
    void setDetail(const QString &detail);
    void setProgress(int percent);
    // Switch the bar between indeterminate ("busy") mode, used during the
    // "prepare" phase when no percent is reported yet, and the determinate
    // 0..100 mode used during the download / install / verify phases. The
    // dialog previously only exposed setValue(), so on the "prepare" phase
    // the bar stayed frozen at 0% with no animation — the user could not
    // tell whether the daemon was still working or the call had deadlocked.
    // Switching to QProgressBar's indeterminate range (0,0) makes the bar
    // animate ("busy spinner"), giving the same visual feedback the status-
    // bar progress bar already used in this phase.
    void setIndeterminate(bool on);
    // Reset the dialog back to the "Preparing..." 0% initial state so a new
    // transaction that reuses the same dialog instance does not briefly show
    // the previous transaction's percent.
    void reset();

private:
    QLabel *m_messageLabel;
    QLabel *m_detailLabel;
    QProgressBar *m_progressBar;
};

}
