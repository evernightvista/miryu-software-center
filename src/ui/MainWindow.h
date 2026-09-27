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
class KLineEdit;
class KComboBox;
class QToolButton;
class QDialog;
class QTextBrowser;
class QProcess;
class QFrame;

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

private Q_SLOTS:
    void onSearch();
    void onFilterChanged();
    void onPackageSelected(const QModelIndex &index);
    void onQueuePackage(const Miryu::Package &pkg);
    void onPackagesQueued(const QList<Miryu::Package> &packages);
    void onUnqueuePackage(const QString &nevra);
    void onApplyQueue();
    void onClearQueue();
    void onRefresh();
    void onSystemUpgrade();
    void onLoadUpdates();
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
    void onError(const QString &error);
    void onUpdatesAvailable(int count);

private:
    void setupActions();
    void setupUI();
    void setupSidebar();
    void updateStatusBar();
    void switchToPage(int page);
    void doRefreshMetadataWithLog();
    void doDistroSyncWithLog();
    void runRpmInstallWithPolkit(const QStringList &files, bool offline);
    void createRestartBanner();
    void checkRestartNeededOnStartup();
    void checkRestartNeeded(const Miryu::TransactionResult &result);
    void runNeedsRestartingCheck();
    void setRestartNeeded();
    QString currentBootId() const;
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

    // Queue page
    QWidget *m_queuePage;
    QueueView *m_queueView;

    // Updates page
    QWidget *m_updatesPage;
    PackageView *m_updatesView;

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
