#pragma once

#include <QDialog>
#include <QProcess>
#include <QString>

class QPushButton;
class QLineEdit;
class QLabel;
class QToolButton;
class QWidget;
class QTextBrowser;
class QFrame;
class QProgressBar;

namespace Miryu {

class Backend;

/**
 * "高级操作" (Advanced Operations) dialog.
 *
 * Exposes system-level package-management commands that go beyond the
 * everyday install / remove / queue workflow:
 *
 *   - "刷新 DNF 缓存"        → dnf makecache --refresh
 *   - "发行版同步系统"        → dnf distro-sync
 *   - "系统升级" (expandable) → dnf system-upgrade --releasever=<target>
 *
 * The current Fedora release is read from /usr/lib/os-release (falling
 * back to /etc/os-release). When the user enters a target version that
 * is lower than the running version a downgrade warning is shown before
 * the operation can be executed.
 *
 * All privileged commands are run through pkexec so that the polkit
 * authentication dialog is shown and the command runs as root. The
 * command's stdout / stderr is streamed into a log area at the bottom
 * of the dialog.
 */
class AdvancedOpsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AdvancedOpsDialog(Backend *backend, QWidget *parent = nullptr);
    ~AdvancedOpsDialog() override;

private Q_SLOTS:
    void onRefreshDnfCache();
    void onDistroSync();
    void onUpgradeVersionChanged(const QString &text);
    void onExecuteSystemUpgrade();
    void onToggleUpgradeSection();

    void onProcessStarted();
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessErrorOccurred();

private:
    void setupUI();
    void runPrivilegedCommand(const QStringList &args, const QString &description);
    void setBusy(bool busy);
    void appendLog(const QString &text);
    void appendLogLine(const QString &text);
    void showWarning(const QString &text);

    // Version helpers
    static QString readCurrentVersionId();
    static bool isVersionLessThan(const QString &target, const QString &current);

    Backend *m_backend;

    // Action buttons
    QPushButton *m_refreshCacheBtn = nullptr;
    QPushButton *m_distroSyncBtn = nullptr;
    QPushButton *m_executeUpgradeBtn = nullptr;

    // Expandable system-upgrade section
    QToolButton *m_upgradeToggle = nullptr;
    QFrame *m_upgradeFrame = nullptr;
    QLineEdit *m_versionEdit = nullptr;
    QLabel *m_versionWarningLabel = nullptr;
    QString m_currentVersion;

    // Log / progress
    QTextBrowser *m_logView = nullptr;
    QProgressBar *m_busyBar = nullptr;
    QLabel *m_statusLabel = nullptr;

    QProcess *m_process = nullptr;
    bool m_busy = false;
};

}
