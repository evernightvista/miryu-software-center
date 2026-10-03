#pragma once

#include <KXmlGuiWindow>
#include <KSharedConfig>

#include <atomic>

class QStackedWidget;
class QTreeView;
class QSplitter;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QProgressBar;
class QPushButton;
class KLineEdit;
class KComboBox;
class QToolButton;
class QDialog;
class QTextBrowser;
class QProcess;
class QFrame;
class QEvent;

namespace Miryu {
class Package;
class Repository;
class Backend;
class PackageModel;
class QueueModel;
class RepoModel;
class PackageView;
class PackageInfoWidget;
class QueueView;
class RepoView;
class ProgressDialog;
class TransactionResultDialog;
class FlatpakBackend;
class FlatpakPage;
class AdvancedOpsDialog;
class UpdateChecker;
struct TransactionResult;

class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Backend *backend, QWidget *parent = nullptr);
    ~MainWindow();

    void loadInitialData();

    // Install RPM files passed on the command line.
    void installLocalRpmFiles(const QStringList &filePaths);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void onSearch();
    void onFilterChanged();
    void onPackageSelected(const QModelIndex &index);
    void onQueuePackage(const Miryu::Package &pkg);
    void onPackagesQueued(const QList<Miryu::Package> &packages);
    void onUnqueuePackage(const QString &nevra);
    void onApplyQueue();
    void onClearQueue();
    // Queue every package currently shown in the Updates list — the
    // "Select All" toolbar action. Only present on the Updates page (the
    // Packages page mixes installed / available / update / downgrade states,
    // so a blanket "queue everything" would be ambiguous).
    void onSelectAllUpdates();
    // Counterpart of onSelectAllUpdates(): unqueue every package shown in
    // the Updates list ("Deselect All").
    void onDeselectAllUpdates();
    void onRefresh();
    void onReloadData();
    void onSystemUpgrade();
    void onLoadUpdates(bool refreshMetadata = false);
    void onDistroSync();
    void onRefreshMetadata();
    void onAdvancedOps();
    void onInstallLocalRpm();
    void launchLinglongStore();
    void showAboutDialog();

    void onPackagesLoaded(int filter, const QList<Miryu::Package> &packages);
    void onSearchCompleted(const QList<Miryu::Package> &packages);
    void onRepositoriesLoaded(const QList<Miryu::Repository> &repos);
    void onUpdatesLoaded(const QList<Miryu::Package> &updates);
    void onTransactionProgress(const QString &message, int percent);
    void onDownloadProgress(const QString &downloadId, qint64 total, qint64 downloaded);
    // Drives the modal ProgressDialog with the *overall* (aggregated)
    // progress reported by the backend. During the download phase this is
    // the overall percent across every active download; during the install
    // phase it is the overall percent across all transaction actions. The
    // phase string identifies which message the dialog should show.
    void onOverallProgress(int percent, const QString &phase, const QString &message);
    void onError(const QString &error);
    void onUpdatesAvailable(int count);

private:
    void setupActions();
    void setupUI();
    void setupSidebar();
    void updateStatusBar();
    // Enable / disable the per-page "Apply" buttons (Packages & Updates)
    // according to whether the queue model currently holds any pending
    // transaction. Called from updateStatusBar() and every queue mutation.
    void updateApplyButtons();
    void switchToPage(int page);
    void doRefreshMetadataWithLog();
    void doDistroSyncWithLog();
    void runRpmInstallWithPolkit(const QStringList &files, bool offline);
    void createRestartBanner();
    void showInstalledPackages();
    void updateRestartBannerHeight();
    void checkRestartNeededOnStartup();
    void checkRestartNeeded(const Miryu::TransactionResult &result);
    void runNeedsRestartingCheck();
    void setRestartNeeded();
    QString currentBootId() const;
    // Returns the release string of the currently running kernel (e.g.
    // "6.8.10-200.fc39.x86_64"), read from /proc/sys/kernel/osrelease. Used
    // to detect whether a package queued for removal is the running kernel.
    QString runningKernelRelease() const;
    // Returns true if the given package is the currently running kernel
    // (name starts with "kernel" and its version-release.arch matches the
    // running kernel release string). Removing the running kernel would
    // leave the system unbootable, so such requests must be rejected.
    bool isRunningKernel(const Miryu::Package &pkg) const;
    // Load package names listed as "suggest reboot" from the dnf5
    // configuration drop-in directories
    // (/usr/share/dnf5/suggest-reboot.d/*.conf and
    // /etc/dnf/suggest-reboot.d/*.conf). Each .conf file is INI-style; the
    // [main] section's "suggest_reboot" key holds a list of package names
    // (separated by ';' or whitespace) whose update requires a reboot.
    QStringList loadSuggestRebootPackages() const;
    bool nevraMatchesAnyPackage(const QString &nevra, const QStringList &packages) const;

    Backend *m_backend;

    // Models
    PackageModel *m_packageModel;
    QueueModel *m_queueModel;
    RepoModel *m_repoModel;

    // UI
    QSplitter *m_mainSplitter;
    QStackedWidget *m_pageStack;
    QWidget *m_sidebar;
    QList<QToolButton*> m_sidebarButtons;

    // Package page
    QWidget *m_packagePage;
    KLineEdit *m_searchEdit;
    KComboBox *m_filterCombo;
    KComboBox *m_searchFieldCombo;
    PackageView *m_packageView;
    PackageInfoWidget *m_infoWidget;
    QPushButton *m_applyButton = nullptr;        // Packages page "Apply" — visible/enabled when queue non-empty

    // Queue page
    QWidget *m_queuePage;
    QueueView *m_queueView;

    // Updates page
    QWidget *m_updatesPage;
    PackageView *m_updatesView;
    QPushButton *m_updatesApplyButton = nullptr; // Updates page "Apply" — same behaviour as m_applyButton
    QPushButton *m_selectAllUpdatesButton = nullptr;   // Updates page "Select All"
    QPushButton *m_deselectAllUpdatesButton = nullptr; // Updates page "Deselect All"

    // Repo page
    QWidget *m_repoPage;
    RepoView *m_repoView;

    // Flatpak page
    QWidget *m_flatpakPage;
    FlatpakBackend *m_flatpakBackend;
    FlatpakPage *m_flatpakPageWidget;

    // Advanced Ops dialog
    AdvancedOpsDialog *m_advancedOpsDialog;

    // Restart-needed banner
    QFrame *m_restartBanner = nullptr;
    // Description label of the restart banner; its height is tracked via
    // eventFilter() so the banner hugs the wrapped text.
    QLabel *m_restartBannerDescLabel = nullptr;

    // Update checker
    UpdateChecker *m_updateChecker;

    // Progress
    QProgressBar *m_progressBar;
    QLabel *m_statusLabel;
    QLabel *m_queueCountLabel;
    QLabel *m_updateCountLabel;

    ProgressDialog *m_progressDialog;
    TransactionResultDialog *m_resultDialog;

    // Refresh-metadata log dialog
    QDialog *m_logDialog;
    QTextBrowser *m_logView;
    QProcess *m_logProcess;

    int m_currentPage = 0;

    // Flag set at the start of closeEvent() to guard all async callbacks
    // (QFutureWatcher, D-Bus signals, etc.) against accessing a partially
    // destroyed MainWindow during teardown.
    std::atomic<bool> m_closing{false};
};

}
