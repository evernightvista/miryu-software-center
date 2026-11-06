#include "ProgressDialog.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QFontMetrics>
#include <QSizePolicy>
#include <KLocalizedString>

namespace Miryu {

namespace {

// QLabel's normal word wrapping can leave an unbroken NEVRA or a run of
// Japanese text wider than the fixed-width dialog. Insert real line breaks at
// the measured display width so every character remains visible.
QString wrapTextAnywhere(const QString &text, const QFontMetrics &metrics, int availableWidth)
{
    if (text.isEmpty() || availableWidth <= 0)
        return text;

    QString wrapped;
    QString line;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\n')) {
            wrapped += line;
            wrapped += ch;
            line.clear();
            continue;
        }

        if (!line.isEmpty() && metrics.horizontalAdvance(line + ch) > availableWidth) {
            wrapped += line;
            wrapped += QLatin1Char('\n');
            line.clear();
        }
        line += ch;
    }
    wrapped += line;
    return wrapped;
}

} // namespace

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

    m_detailLabel = new QLabel;
    // Wrapping is performed explicitly in setDetail(). Ignored horizontal
    // sizing prevents a long package name from forcing the label beyond the
    // dialog's content rectangle.
    m_detailLabel->setWordWrap(false);
    m_detailLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_detailLabel->setMinimumWidth(0);
    m_detailLabel->setTextFormat(Qt::PlainText);
    m_detailLabel->hide();
    layout->addWidget(m_detailLabel);

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

void ProgressDialog::setDetail(const QString &detail)
{
    const QMargins margins = layout()->contentsMargins();
    const int availableWidth = qMax(1, width() - margins.left() - margins.right());
    m_detailLabel->setText(wrapTextAnywhere(detail, m_detailLabel->fontMetrics(), availableWidth));
    m_detailLabel->setVisible(!detail.isEmpty());
    // The width stays fixed; grow vertically to fit every wrapped line.
    adjustSize();
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
    m_detailLabel->clear();
    m_detailLabel->hide();
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(true);
    m_progressBar->setValue(0);
}

}
