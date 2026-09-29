#include "ProgressDialog.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <KLocalizedString>

namespace Miryu {

ProgressDialog::ProgressDialog(QWidget *parent)
    // Pass the window flags to the QDialog constructor so they are set at
    // window-creation time. Explicitly enumerate only Minimize + Close button
    // hints and omit Qt::WindowMaximizeButtonHint; Qt::CustomizeWindowHint
    // tells Qt to honor this exact button set instead of the WM defaults.
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                     | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint
                     | Qt::CustomizeWindowHint)
{
    setWindowTitle(i18n("Processing"));
    setModal(true);
    // Fixed width so the window manager (notably KWin, which shows a
    // maximize button only for resizable windows) does not display a
    // maximize button for this small, transient modal dialog. The height
    // stays automatic so the detail label can wrap when needed.
    setFixedWidth(480);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 20, 20, 20);

    m_messageLabel = new QLabel(i18n("Preparing..."));
    QFont msgFont = m_messageLabel->font();
    msgFont.setBold(true);
    msgFont.setPointSize(msgFont.pointSize() + 1);
    m_messageLabel->setFont(msgFont);
    layout->addWidget(m_messageLabel);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    layout->addWidget(m_progressBar);

    m_detailLabel = new QLabel;
    // palette(mid) resolves to near-black in dark themes; dim the theme text colour instead.
    QColor secondary = palette().color(QPalette::WindowText);
    secondary.setAlpha(160);
    m_detailLabel->setStyleSheet(
        QStringLiteral("color: rgba(%1, %2, %3, 0.627);")
            .arg(secondary.red()).arg(secondary.green()).arg(secondary.blue()));
    layout->addWidget(m_detailLabel);
}

void ProgressDialog::setMessage(const QString &message)
{
    m_messageLabel->setText(message);
}

void ProgressDialog::setProgress(int percent)
{
    m_progressBar->setValue(percent);
}

void ProgressDialog::setDownloadProgress(const QString &downloadId, qint64 total, qint64 downloaded)
{
    QString detail;
    if (total > 0) {
        double mbTotal = total / (1024.0 * 1024);
        double mbDownloaded = downloaded / (1024.0 * 1024);
        detail = i18n("Downloading %1: %2 MB / %3 MB",
                       downloadId,
                       QString::number(mbDownloaded, 'f', 1),
                       QString::number(mbTotal, 'f', 1));
    } else {
        detail = i18n("Downloading %1...", downloadId);
    }
    m_detailLabel->setText(detail);
}

}
