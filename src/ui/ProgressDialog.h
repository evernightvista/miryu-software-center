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

    void setMessage(const QString &message);
    void setProgress(int percent);
    void setDownloadProgress(const QString &downloadId, qint64 total, qint64 downloaded);

private:
    QLabel *m_messageLabel;
    QProgressBar *m_progressBar;
    QLabel *m_detailLabel;
};

}
