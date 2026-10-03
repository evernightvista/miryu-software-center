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
    // tells Qt to honor this exact button set instead of the WM Defaults.
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                     | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint
                     | Qt::CustomizeWindowHint)
{
    setWindowTitle(i18n("Processing"));
    setModal(true);
    // Fixed width so the window manager (notably KWin, which shows a
    // maximize button only for resizable windows) does not display a
    // maximize button for this small, transient modal dialog.
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
    // Show the percent value as text inside the bar (e.g. "0%", "42%") so
    // the dialog mirrors the simple "Preparing... 0%" layout requested.
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat(QStringLiteral("%p%"));
    layout->addWidget(m_progressBar);
}

void ProgressDialog::setMessage(const QString &message)
{
    m_messageLabel->setText(message);
}

void ProgressDialog::setProgress(int percent)
{
    // When the bar is in indeterminate ("busy") mode (range 0,0) Qt ignores
    // setValue() — the animation keeps spinning. When the bar is back to the
    // determinate 0..100 range, setValue() updates the fill.
    m_progressBar->setValue(percent);
}

void ProgressDialog::setIndeterminate(bool on)
{
    // QProgressBar's "busy" mode is range(0,0): Qt animates the fill back and
    // forth and ignores setValue() until the range is reset. We switch the
    // percent text off in this mode (it would otherwise read "0%" forever
    // even though the bar is visibly moving) and back on when the determinate
    // range is restored.
    if (on) {
        m_progressBar->setRange(0, 0);
        m_progressBar->setTextVisible(false);
    } else {
        m_progressBar->setRange(0, 100);
        m_progressBar->setTextVisible(true);
    }
}

void ProgressDialog::reset()
{
    m_messageLabel->setText(i18n("Preparing..."));
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(true);
    m_progressBar->setValue(0);
}

}
