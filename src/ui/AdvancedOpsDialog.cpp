#include "AdvancedOpsDialog.h"
#include "Backend.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QToolButton>
#include <QFrame>
#include <QTextBrowser>
#include <QTextCursor>
#include <QProgressBar>
#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QFont>
#include <QIcon>
#include <QSizePolicy>
#include <QCloseEvent>
#include <QMessageBox>

#include <KLocalizedString>
#include <KGuiItem>
#include <KStandardGuiItem>
#include <KMessageBox>

namespace Miryu {

// os-release paths — /usr/lib/os-release is the canonical location, but
// /etc/os-release is a (usually identical) symlink-friendly fallback.
static const QString kOsReleasePrimary = QStringLiteral("/usr/lib/os-release");
static const QString kOsReleaseFallback = QStringLiteral("/etc/os-release");

// pkexec is the standard polkit-aware escalation helper used by KDE / GNOME
// applications to run a command as root after the user authenticates.
static const QString kPkexec = QStringLiteral("pkexec");
static const QString kDnf = QStringLiteral("dnf");

// A monospace stylesheet so the log area resembles a terminal.
static const char *kLogStyleSheet =
    "QTextBrowser {"
    "  background-color: #1e1e1e;"
    "  color: #d4d4d4;"
    "  font-family: monospace;"
    "  border: 1px solid palette(mid);"
    "  border-radius: 4px;"
    "}";

AdvancedOpsDialog::AdvancedOpsDialog(Backend *backend, QWidget *parent)
    : QDialog(parent)
    , m_backend(backend)
{
    setupUI();

    // Detect the running Fedora release so the upgrade field can be
    // pre-filled and the downgrade warning can compare against it.
    m_currentVersion = readCurrentVersionId();
    m_versionEdit->setText(m_currentVersion);
    onUpgradeVersionChanged(m_versionEdit->text());
}

AdvancedOpsDialog::~AdvancedOpsDialog()
{
    // Make sure we never leave a stray pkexec / dnf process behind when the
    // dialog is destroyed while a command is still running.
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning)
            m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void AdvancedOpsDialog::setupUI()
{
    setWindowTitle(i18n("Advanced Operations"));
    setMinimumSize(560, 560);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(12);

    // ---- Header ----
    auto *header = new QLabel(QStringLiteral("<h2>%1</h2>").arg(i18n("Advanced Operations")));
    mainLayout->addWidget(header);

    auto *subtitle = new QLabel(i18n("System-level package operations that require root privileges."));
    // palette(mid) resolves to near-black in dark themes; dim the theme text colour instead.
    QColor secondary = palette().color(QPalette::WindowText);
    secondary.setAlpha(160);
    subtitle->setStyleSheet(
        QStringLiteral("color: rgba(%1, %2, %3, 0.627);")
            .arg(secondary.red()).arg(secondary.green()).arg(secondary.blue()));
    mainLayout->addWidget(subtitle);

    // ---- DNF cache refresh ----
    auto *cacheBtnRow = new QHBoxLayout;
    cacheBtnRow->setSpacing(8);

    m_refreshCacheBtn = new QPushButton(
        QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Refresh DNF Cache"));
    m_refreshCacheBtn->setToolTip(i18n("Run: dnf makecache --refresh"));
    m_refreshCacheBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_refreshCacheBtn, &QPushButton::clicked, this, &AdvancedOpsDialog::onRefreshDnfCache);

    cacheBtnRow->addWidget(new QLabel(i18n("Download and rebuild metadata cache for all enabled repositories.")));
    cacheBtnRow->addStretch();
    cacheBtnRow->addWidget(m_refreshCacheBtn);
    mainLayout->addLayout(cacheBtnRow);

    // ---- Distro sync ----
    auto *syncBtnRow = new QHBoxLayout;
    syncBtnRow->setSpacing(8);

    m_distroSyncBtn = new QPushButton(
        QIcon::fromTheme(QStringLiteral("system-software-update")), i18n("Distro Sync"));
    m_distroSyncBtn->setToolTip(i18n("Run: dnf distro-sync"));
    m_distroSyncBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_distroSyncBtn, &QPushButton::clicked, this, &AdvancedOpsDialog::onDistroSync);

    syncBtnRow->addWidget(new QLabel(i18n("Synchronize all installed packages to the latest versions in repositories.")));
    syncBtnRow->addStretch();
    syncBtnRow->addWidget(m_distroSyncBtn);
    mainLayout->addLayout(syncBtnRow);

    // ---- Expandable system-upgrade section ----
    // A QToolButton with an arrow acts as the expand / collapse toggle.
    m_upgradeToggle = new QToolButton;
    m_upgradeToggle->setText(i18n("System Upgrade"));
    m_upgradeToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_upgradeToggle->setArrowType(Qt::ArrowType::RightArrow);
    m_upgradeToggle->setCheckable(true);
    m_upgradeToggle->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 4px;"
        "  padding: 6px 10px;"
        "  font-weight: bold;"
        "  text-align: left;"
        "}"
        "QToolButton:hover { background-color: rgba(0,0,0,0.05); }"));
    m_upgradeToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_upgradeToggle, &QToolButton::toggled, this, &AdvancedOpsDialog::onToggleUpgradeSection);
    mainLayout->addWidget(m_upgradeToggle);

    // The collapsible frame — hidden until the toggle is checked.
    m_upgradeFrame = new QFrame;
    m_upgradeFrame->setFrameShape(QFrame::StyledPanel);
    m_upgradeFrame->setVisible(false);
    auto *upgradeLayout = new QVBoxLayout(m_upgradeFrame);
    upgradeLayout->setContentsMargins(12, 12, 12, 12);
    upgradeLayout->setSpacing(8);

    auto *upgradeForm = new QFormLayout;
    upgradeForm->setSpacing(8);

    auto *versionLabel = new QLabel(i18n("Fedora Version"));
    versionLabel->setToolTip(i18n("Target Fedora release (VERSION_ID from /usr/lib/os-release)."));
    m_versionEdit = new QLineEdit;
    m_versionEdit->setPlaceholderText(QStringLiteral("40"));
    m_versionEdit->setMaximumWidth(120);
    m_versionEdit->setToolTip(versionLabel->toolTip());
    connect(m_versionEdit, &QLineEdit::textChanged,
            this, &AdvancedOpsDialog::onUpgradeVersionChanged);
    upgradeForm->addRow(versionLabel, m_versionEdit);

    // Downgrade warning — initially hidden, shown only when the entered
    // version is lower than the current one.
    m_versionWarningLabel = new QLabel;
    m_versionWarningLabel->setStyleSheet(QStringLiteral(
        "color: #cc6600; background: #fff3e0; padding: 6px 8px; border-radius: 4px;"));
    m_versionWarningLabel->setWordWrap(true);
    m_versionWarningLabel->setVisible(false);
    upgradeForm->addRow(QString(), m_versionWarningLabel);

    upgradeLayout->addLayout(upgradeForm);

    m_executeUpgradeBtn = new QPushButton(
        QIcon::fromTheme(QStringLiteral("miryu-package-manager")), i18n("Execute System Upgrade"));
    m_executeUpgradeBtn->setToolTip(i18n("Run: dnf system-upgrade download --releasever=<version>"));
    connect(m_executeUpgradeBtn, &QPushButton::clicked,
            this, &AdvancedOpsDialog::onExecuteSystemUpgrade);

    auto *execRow = new QHBoxLayout;
    execRow->addStretch();
    execRow->addWidget(m_executeUpgradeBtn);
    upgradeLayout->addLayout(execRow);

    mainLayout->addWidget(m_upgradeFrame);

    // ---- Status / progress ----
    m_statusLabel = new QLabel;
    {
        QColor secondary = palette().color(QPalette::WindowText);
        secondary.setAlpha(160);
        m_statusLabel->setStyleSheet(
            QStringLiteral("color: rgba(%1, %2, %3, 0.627);")
                .arg(secondary.red()).arg(secondary.green()).arg(secondary.blue()));
    }
    mainLayout->addWidget(m_statusLabel);

    m_busyBar = new QProgressBar;
    m_busyBar->setRange(0, 0); // busy / indeterminate
    m_busyBar->setVisible(false);
    m_busyBar->setTextVisible(false);
    mainLayout->addWidget(m_busyBar);

    // ---- Log view ----
    auto *logTitle = new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Command Output")));
    mainLayout->addWidget(logTitle);

    m_logView = new QTextBrowser;
    m_logView->setStyleSheet(QString::fromLatin1(kLogStyleSheet));
    m_logView->setMinimumHeight(120);
    m_logView->setReadOnly(true);
    mainLayout->addWidget(m_logView, 1);

    // ---- Button box ----
    auto *closeBtn = new QPushButton(
        QIcon::fromTheme(QStringLiteral("window-close")), i18n("Close"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    mainLayout->addLayout(btnRow);
}

// ---------------------------------------------------------------------------
// Action slots
// ---------------------------------------------------------------------------

void AdvancedOpsDialog::onRefreshDnfCache()
{
    if (m_busy) {
        KMessageBox::information(this, i18n("Another operation is still running."));
        return;
    }

    auto ret = KMessageBox::warningTwoActions(this,
        i18n("This will download and rebuild the metadata cache for all enabled "
             "repositories. Continue?"),
        i18n("Refresh DNF Cache"),
        KGuiItem(i18n("Refresh"), QIcon::fromTheme(QStringLiteral("view-refresh"))),
        KStandardGuiItem::cancel());
    if (ret != KMessageBox::PrimaryAction)
        return;

    runPrivilegedCommand({kDnf, QStringLiteral("makecache"), QStringLiteral("--refresh")},
                         i18n("Refresh DNF Cache"));
}

void AdvancedOpsDialog::onDistroSync()
{
    if (m_busy) {
        KMessageBox::information(this, i18n("Another operation is still running."));
        return;
    }

    auto ret = KMessageBox::warningTwoActions(this,
        i18n("This will synchronise all installed packages to the latest version "
             "available in the repositories. Continue?"),
        i18n("Distro Sync"),
        KGuiItem(i18n("Sync"), QIcon::fromTheme(QStringLiteral("system-software-update"))),
        KStandardGuiItem::cancel());
    if (ret != KMessageBox::PrimaryAction)
        return;

    runPrivilegedCommand({kDnf, QStringLiteral("distro-sync")},
                         i18n("Distro Sync"));
}

void AdvancedOpsDialog::onExecuteSystemUpgrade()
{
    if (m_busy) {
        KMessageBox::information(this, i18n("Another operation is still running."));
        return;
    }

    const QString target = m_versionEdit->text().trimmed();
    if (target.isEmpty()) {
        KMessageBox::error(this, i18n("Please enter a target Fedora version."));
        return;
    }

    // If the target is lower than the running version the user is about to
    // perform a downgrade-style upgrade — make them acknowledge the risk.
    if (!m_currentVersion.isEmpty() && isVersionLessThan(target, m_currentVersion)) {
        auto ret = KMessageBox::warningContinueCancel(this,
            i18n("The target version (%1) is lower than the currently running "
                 "version (%2). Downgrading the system is risky and may leave "
                 "the system in an inconsistent state.\n\n"
                 "Do you really want to continue?")
                .arg(target, m_currentVersion),
            i18n("Version Downgrade Warning"));
        if (ret != KMessageBox::Continue)
            return;
    }

    auto ret = KMessageBox::warningTwoActions(this,
        i18n("This will download all packages needed to upgrade the system to "
             "Fedora %1. After the download completes you will need to reboot "
             "to apply the upgrade.\n\nContinue?")
            .arg(target),
        i18n("Execute System Upgrade"),
        KGuiItem(i18n("Download"), QIcon::fromTheme(QStringLiteral("miryu-package-manager"))),
        KStandardGuiItem::cancel());
    if (ret != KMessageBox::PrimaryAction)
        return;

    runPrivilegedCommand({kDnf,
                          QStringLiteral("system-upgrade"),
                          QStringLiteral("download"),
                          QStringLiteral("--releasever=%1").arg(target)},
                         i18n("System Upgrade (releasever=%1)").arg(target));
}

void AdvancedOpsDialog::onUpgradeVersionChanged(const QString &text)
{
    const QString target = text.trimmed();

    // Show the downgrade warning only when we actually know the current
    // version and the entered target is provably lower.
    if (m_currentVersion.isEmpty() || target.isEmpty()) {
        m_versionWarningLabel->setVisible(false);
        return;
    }

    if (isVersionLessThan(target, m_currentVersion)) {
        m_versionWarningLabel->setText(i18n("Warning: Target version %1 is lower than current version %2. This is a version downgrade and carries risks.")
                                        .arg(target, m_currentVersion));
        m_versionWarningLabel->setVisible(true);
    } else {
        m_versionWarningLabel->setVisible(false);
    }
}

void AdvancedOpsDialog::onToggleUpgradeSection()
{
    const bool expanded = m_upgradeToggle->isChecked();
    m_upgradeToggle->setArrowType(expanded ? Qt::ArrowType::DownArrow
                                           : Qt::ArrowType::RightArrow);
    m_upgradeFrame->setVisible(expanded);
}

// ---------------------------------------------------------------------------
// QProcess plumbing
// ---------------------------------------------------------------------------

void AdvancedOpsDialog::runPrivilegedCommand(const QStringList &args,
                                             const QString &description)
{
    // The full argv is:  pkexec <args>
    // where args already starts with "dnf" so that e.g.
    //   { "dnf", "makecache", "--refresh" }
    // becomes:  pkexec dnf makecache --refresh
    appendLogLine(QStringLiteral("$ %1 %2")
                      .arg(kPkexec, args.join(QLatin1Char(' '))));

    if (!m_process)
        m_process = new QProcess(this);

    m_process->disconnect(this);

    connect(m_process, &QProcess::started,
            this, &AdvancedOpsDialog::onProcessStarted);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &AdvancedOpsDialog::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &AdvancedOpsDialog::onReadyReadStandardError);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &AdvancedOpsDialog::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred,
            this, &AdvancedOpsDialog::onProcessErrorOccurred);

    setBusy(true);
    m_statusLabel->setText(i18n("Executing: %1 ...").arg(description));

    m_process->start(kPkexec, args);
}

void AdvancedOpsDialog::onProcessStarted()
{
    appendLogLine(i18n("[started]"));
}

void AdvancedOpsDialog::onReadyReadStandardOutput()
{
    QByteArray data = m_process->readAllStandardOutput();
    appendLog(QString::fromUtf8(data));
}

void AdvancedOpsDialog::onReadyReadStandardError()
{
    QByteArray data = m_process->readAllStandardError();
    appendLog(QString::fromUtf8(data));
}

void AdvancedOpsDialog::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    setBusy(false);

    if (exitCode == 0) {
        m_statusLabel->setText(i18n("Operation completed successfully."));
        appendLogLine(i18n("[completed successfully]"));
        KMessageBox::information(this, i18n("Operation completed successfully."));
    } else if (exitCode == 126 || exitCode == 127) {
        // 126/127 from pkexec usually means the user dismissed the auth
        // dialog or the helper could not be found.
        m_statusLabel->setText(i18n("Operation cancelled or authentication failed."));
        appendLogLine(i18n("[cancelled or authentication failed (exit %1)]").arg(exitCode));
    } else {
        m_statusLabel->setText(i18n("Operation failed (exit code %1).").arg(exitCode));
        appendLogLine(i18n("[failed with exit code %1]").arg(exitCode));
        KMessageBox::error(this,
            i18n("Operation failed with exit code %1. Check the output below for details.").arg(exitCode),
            i18n("Operation Failed"));
    }
}

void AdvancedOpsDialog::onProcessErrorOccurred()
{
    setBusy(false);
    const QString errorString = m_process ? m_process->errorString() : QString();
    m_statusLabel->setText(i18n("Failed to start command: %1").arg(errorString));
    appendLogLine(i18n("[error: %1]").arg(errorString));
    KMessageBox::error(this,
        i18n("Failed to start command.\n%1\n\n"
             "Make sure pkexec (polkit) is installed and available.").arg(errorString),
        i18n("Command Start Failed"));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void AdvancedOpsDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_busyBar->setVisible(busy);
    m_refreshCacheBtn->setEnabled(!busy);
    m_distroSyncBtn->setEnabled(!busy);
    m_executeUpgradeBtn->setEnabled(!busy);
}

void AdvancedOpsDialog::appendLog(const QString &text)
{
    if (text.isEmpty())
        return;
    m_logView->moveCursor(QTextCursor::End);
    m_logView->insertPlainText(text);
    m_logView->moveCursor(QTextCursor::End);
}

void AdvancedOpsDialog::appendLogLine(const QString &text)
{
    appendLog(text + QStringLiteral("\n"));
}

void AdvancedOpsDialog::showWarning(const QString &text)
{
    m_versionWarningLabel->setText(text);
    m_versionWarningLabel->setVisible(true);
}

QString AdvancedOpsDialog::readCurrentVersionId()
{
    // Try /usr/lib/os-release first (canonical), fall back to /etc/os-release
    // which is typically a symlink to the former but may differ on some
    // setups.
    for (const QString &path : {kOsReleasePrimary, kOsReleaseFallback}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        QTextStream in(&file);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (!line.startsWith(QStringLiteral("VERSION_ID=")))
                continue;

            // Strip the key and surrounding quotes.
            QString value = line.mid(QStringLiteral("VERSION_ID=").length()).trimmed();
            if (value.size() >= 2 && value.startsWith(QLatin1Char('"'))
                && value.endsWith(QLatin1Char('"'))) {
                value = value.mid(1, value.size() - 2);
            }
            return value;
        }
    }
    return QString();
}

bool AdvancedOpsDialog::isVersionLessThan(const QString &target, const QString &current)
{
    // Try a plain numeric comparison first — Fedora VERSION_ID is typically
    // an integer (e.g. "40", "41", "42"). This is the common case.
    bool okT = false, okC = false;
    const int t = target.toInt(&okT);
    const int c = current.toInt(&okC);
    if (okT && okC)
        return t < c;

    // Fall back to a natural-ish string comparison for branched / rawhide
    // labels such as "40.1" or "rawhide". Note that string comparison is
    // only a heuristic for non-integer labels.
    return target.compare(current, Qt::CaseInsensitive) < 0;
}

}
