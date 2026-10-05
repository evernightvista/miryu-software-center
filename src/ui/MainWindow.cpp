#include "MainWindow.h"
#include "../core/Backend.h"
#include "../core/Package.h"
#include "../core/Repository.h"
#include "../core/Transaction.h"
#include "../core/Enums.h"
#include "../core/FlatpakBackend.h"
#include "../core/UpdateChecker.h"
#include "../models/PackageModel.h"
#include "../models/QueueModel.h"
#include "../models/RepoModel.h"
#include "PackageView.h"
#include "PackageInfoWidget.h"
#include "QueueView.h"
#include "RepoView.h"
#include "ProgressDialog.h"
#include "TransactionResultDialog.h"
#include "FlatpakPage.h"
#include "AdvancedOpsDialog.h"

#include <KActionCollection>
#include <KStandardAction>
#include <KAboutData>
#include <KLocalizedString>
#include <KMessageBox>
#include <KGuiItem>
#include <KStandardGuiItem>
#include <KLineEdit>
#include <KComboBox>
#include <KToolBar>
#include <KConfigGroup>
#include <KSharedConfig>

#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QSplitter>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QStatusBar>
#include <QTimer>
#include <QCloseEvent>
#include <QIcon>
#include <QButtonGroup>
#include <QToolButton>
#include <QScrollArea>
#include <QFrame>
#include <QSize>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QInputDialog>
#include <QFile>
#include <QTextStream>
#include <QVersionNumber>
#include <QLineEdit>
#include <QFileDialog>
#include <QCheckBox>
#include <QDialog>
#include <QTextBrowser>
#include <QTextCursor>
#include <QProcess>
#include <QDialogButtonBox>
#include <QFont>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <QRegularExpression>

namespace Miryu {

MainWindow::MainWindow(Backend *backend, QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_backend(backend)
    , m_packageModel(new PackageModel(this))
    , m_queueModel(new QueueModel(this))
    , m_repoModel(new RepoModel(this))
    , m_flatpakBackend(new FlatpakBackend(this))
    , m_advancedOpsDialog(nullptr)
    , m_updateChecker(nullptr)
    , m_progressDialog(nullptr)
    , m_resultDialog(nullptr)
    , m_logDialog(nullptr)
    , m_logView(nullptr)
    , m_logProcess(nullptr)
{
    // KMainWindow sets WA_DeleteOnClose by default.  But in main.cpp we
    // create MainWindow as a STACK object:
    //
    //   Miryu::MainWindow window(&backend);
    //
    // If WA_DeleteOnClose remains set, accepting the close event causes Qt
    // to call deleteLater(), which posts a DeferredDelete event.  When the
    // event loop delivers it, QObject::event() executes "delete this" on
    // a stack address → free() crashes with SIGSEGV.
    //
    // Clear the attribute so that closing the window merely hides it.
    // app.exec() then returns (lastWindowClosed signal), and the stack
    // objects are destroyed normally when main() returns.
    setAttribute(Qt::WA_DeleteOnClose, false);

    setupUI();
    setupActions();

    // Connect backend signals
    connect(m_backend, &Backend::packagesLoaded, this, [this](PackageFilter filter, const QList<Package> &packages) {
        onPackagesLoaded(static_cast<int>(filter), packages);
    });
    connect(m_backend, &Backend::searchCompleted, this, &MainWindow::onSearchCompleted);
    connect(m_backend, &Backend::repositoriesLoaded, this, &MainWindow::onRepositoriesLoaded);
    connect(m_backend, &Backend::updatesLoaded, this, &MainWindow::onUpdatesLoaded);
    connect(m_backend, &Backend::transactionProgress, this, &MainWindow::onTransactionProgress);
    connect(m_backend, &Backend::downloadProgress, this, &MainWindow::onDownloadProgress);
    // The modal ProgressDialog is driven entirely by the aggregated
    // overallProgress signal — onDownloadProgress / onTransactionProgress
    // only update the bottom status bar now, never the dialog itself, so the
    // dialog shows ONE overall percent per phase (download / install) as
    // requested instead of per-package ticks.
    connect(m_backend, &Backend::overallProgress, this, &MainWindow::onOverallProgress);
    connect(m_backend, &Backend::errorOccurred, this, &MainWindow::onError);

    // Disable the auto-generated KDE help menu (handbook, "What's This",
    // report bug, donate, about KDE). The Help menu is declared in
    // miryuui.rc with only the "About Miryu Software Center" entry.
    setHelpMenuEnabled(false);

    setupGUI(Default, QStringLiteral("miryuui.rc"));

    // Remove the standard "Find Actions..." entry (Ctrl+Alt+I) from the
    // Help menu — requested removal (circled entry in the screenshot).
    // Walk all top-level menus (the entry lives in the KHelpMenu-managed
    // Help menu) and drop the action whose shortcut is Ctrl+Alt+I.
    const QKeySequence findActionsShortcut(Qt::CTRL | Qt::ALT | Qt::Key_I);
    for (QAction *menuAction : menuBar()->actions()) {
        QMenu *m = menuAction->menu();
        if (!m)
            continue;
        const auto actions = m->actions();
        for (QAction *action : actions) {
            if (action->shortcut() == findActionsShortcut) {
                m->removeAction(action);
                action->deleteLater();
            }
        }
    }

    setWindowIcon(QIcon::fromTheme(QStringLiteral("miryu-package-manager")));

    resize(1200, 800);

    // Initialize update checker
    m_updateChecker = new UpdateChecker(m_backend, this);
    connect(m_updateChecker, &UpdateChecker::updatesAvailable, this, &MainWindow::onUpdatesAvailable);
    m_updateChecker->start();

    // Show the restart-needed banner if a previous transaction (in this boot
    // session) installed packages that require a reboot.
    checkRestartNeededOnStartup();
}

MainWindow::~MainWindow()
{
    // Set the closing flag in case the window is deleted without going
    // through closeEvent() (e.g. direct deleteLater call).
    m_closing.store(true, std::memory_order_release);

    // Child QObjects are destroyed automatically by Qt's parent-child mechanism.
    //
    // Targeted signal disconnection and async cleanup are performed in
    // closeEvent() — see comments there for details.
    //
    // We must NOT call blanket disconnect() here: KXmlGuiWindow needs its
    // internal signal connections (XML GUI factory, action collection,
    // toolbar management, etc.) for proper cleanup during its destructor.
    // Breaking those causes SIGSEGV on exit.
    //
    // Backend outlives MainWindow (stack-allocated in main.cpp), so we must
    // NOT close its session here — Backend's own destructor handles that.
}

void MainWindow::setupUI()
{
    m_mainSplitter = new QSplitter(Qt::Horizontal, this);

    setupSidebar();

    m_pageStack = new QStackedWidget;

    // Page 0: Packages
    {
        m_packagePage = new QWidget;
        auto *layout = new QVBoxLayout(m_packagePage);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto *searchBar = new QWidget;
        auto *searchLayout = new QHBoxLayout(searchBar);
        searchLayout->setContentsMargins(8, 8, 8, 4);

        m_searchEdit = new KLineEdit;
        m_searchEdit->setPlaceholderText(i18n("Search packages..."));
        m_searchEdit->setClearButtonEnabled(true);
        m_searchEdit->setTrapReturnKey(true);
        connect(m_searchEdit, &KLineEdit::returnPressed, this, &MainWindow::onSearch);

        // Clearing the search box immediately returns to the installed
        // packages view — no Enter needed: the instant the text becomes
        // empty (clear button, backspace, select-all+delete), switch the
        // filter combo back to Installed and load that list.
        connect(m_searchEdit, &KLineEdit::textChanged, this, [this](const QString &text) {
            if (text.trimmed().isEmpty())
                showInstalledPackages();
        });

        m_filterCombo = new KComboBox;
        m_filterCombo->addItem(i18n("All"), static_cast<int>(PackageFilter::All));
        m_filterCombo->addItem(i18n("Installed"), static_cast<int>(PackageFilter::Installed));
        m_filterCombo->addItem(i18n("Available"), static_cast<int>(PackageFilter::Available));
        m_filterCombo->addItem(i18n("Updates"), static_cast<int>(PackageFilter::Updates));
        m_filterCombo->addItem(i18n("Upgradable"), static_cast<int>(PackageFilter::Upgradable));
        connect(m_filterCombo, QOverload<int>::of(&KComboBox::currentIndexChanged), this, &MainWindow::onFilterChanged);

        m_searchFieldCombo = new KComboBox;
        m_searchFieldCombo->addItem(i18n("All Fields"), static_cast<int>(SearchField::All));
        m_searchFieldCombo->addItem(i18n("Name"), static_cast<int>(SearchField::Name));
        m_searchFieldCombo->addItem(i18n("Summary"), static_cast<int>(SearchField::Summary));
        m_searchFieldCombo->addItem(i18n("Description"), static_cast<int>(SearchField::Description));

        // Per-page "Apply" button. Visible whenever the queue holds pending
        // transactions so the user can confirm and execute them without
        // having to switch to the Queue page first. Disabled (still visible
        // but greyed out) when the queue is empty, mirroring the bottom
        // status-bar queue counter.
        m_applyButton = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18n("Apply"));
        m_applyButton->setToolTip(i18n("Apply pending operations in the queue"));
        m_applyButton->setEnabled(false);
        connect(m_applyButton, &QPushButton::clicked, this, &MainWindow::onApplyQueue);

        searchLayout->addWidget(m_searchEdit, 1);
        searchLayout->addWidget(m_filterCombo);
        searchLayout->addWidget(m_searchFieldCombo);
        searchLayout->addWidget(m_applyButton);

        layout->addWidget(searchBar);

        auto *contentSplitter = new QSplitter(Qt::Horizontal);
        m_packageView = new PackageView(m_packageModel);
        m_infoWidget = new PackageInfoWidget;
        // Keep the detail panel hidden until the user selects a package —
        // showing it beforehand renders an empty placeholder with no name,
        // version, or action state, which looks broken.
        m_infoWidget->hide();

        contentSplitter->addWidget(m_packageView);
        contentSplitter->addWidget(m_infoWidget);
        contentSplitter->setStretchFactor(0, 3);
        contentSplitter->setStretchFactor(1, 2);
        contentSplitter->setSizes({600, 400});

        layout->addWidget(contentSplitter, 1);

        connect(m_packageView, &PackageView::packageSelected, this, &MainWindow::onPackageSelected);
        connect(m_packageView, &PackageView::queuePackage, this, &MainWindow::onQueuePackage);
        connect(m_packageView, &PackageView::packagesQueued, this, &MainWindow::onPackagesQueued);
        connect(m_packageView, &PackageView::unqueuePackage, this, &MainWindow::onUnqueuePackage);

        // Fetch extended package details (deps, provides, files, changelog)
        // asynchronously when a package is selected in the info widget.
        // Results are cached so switching between packages is instant after
        // the first query.
        connect(m_infoWidget, &PackageInfoWidget::packageDetailsRequested,
                this, [this](const QString &pkgName, const QString &nevra) {
            if (!m_backend || !m_backend->isInitialized() || !m_backend->client())
                return;

            // If details are already cached, show them immediately.
            if (m_backend->cache()->hasDetails(pkgName)) {
                m_infoWidget->setPackageDetails(m_backend->cache()->getDetails(pkgName));
                return;
            }

            auto *watcher = new QFutureWatcher<QVariantMap>(this);
            QString currentNevra = nevra;
            connect(watcher, &QFutureWatcher<QVariantMap>::finished, this,
                    [this, watcher, currentNevra, pkgName]() {
                if (m_closing.load(std::memory_order_acquire)) {
                    watcher->deleteLater();
                    return;
                }
                QVariantMap details = watcher->result();
                // Cache the details for instant access next time.
                m_backend->cache()->setDetails(pkgName, details);
                // Only update if the user hasn't selected a different package
                if (m_infoWidget && m_infoWidget->currentPackage().nevra() == currentNevra)
                    m_infoWidget->setPackageDetails(details);
                watcher->deleteLater();
            });
            watcher->setFuture(QtConcurrent::run([this, pkgName, nevra]() {
                return m_backend->client()->getPackageDetails(pkgName, nevra);
            }));
        });

        // Action buttons in the info panel (Install / Reinstall / Remove /
        // Update / Downgrade) put the package into the transaction queue.
        connect(m_infoWidget, &PackageInfoWidget::markForAction,
                this, [this](const Package &pkg, PackageTodo todo) {
            if (pkg.queued)
                return; // already queued — do not duplicate
            Package queuedPkg = pkg;
            queuedPkg.todo = todo;
            m_queueModel->addPackage(queuedPkg);
            // Update the list model's todo as well, so the in-row marker
            // shows the chosen action (e.g. "Reinstall") instead of the
            // state-derived default ("Remove").
            m_packageModel->setQueuedWithTodo(pkg.nevra(), true, todo);
            updateStatusBar();
        });

        m_pageStack->addWidget(m_packagePage);
    }

    // Page 1: Updates
    {
        m_updatesPage = new QWidget;
        auto *layout = new QVBoxLayout(m_updatesPage);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *headerRow = new QWidget;
        auto *headerLayout = new QHBoxLayout(headerRow);
        headerLayout->setContentsMargins(8, 8, 8, 4);

        auto *header = new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Available Updates")));
        headerLayout->addWidget(header);
        headerLayout->addStretch();

        // "Select All" / "Deselect All" buttons — only on the Updates page.
        // The Packages page mixes installed / available / update / downgrade
        // states, so a blanket "queue everything" would be ambiguous there.
        // Updates are a single intent (upgrade), so the bulk-select buttons
        // sit next to the Apply button on the right of the header row.
        m_selectAllUpdatesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-select-all")), i18n("Select All"));
        m_selectAllUpdatesButton->setToolTip(i18n("Queue every update shown in the list"));
        connect(m_selectAllUpdatesButton, &QPushButton::clicked, this, &MainWindow::onSelectAllUpdates);
        headerLayout->addWidget(m_selectAllUpdatesButton);

        m_deselectAllUpdatesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-select-none")), i18n("Deselect All"));
        m_deselectAllUpdatesButton->setToolTip(i18n("Remove every update from the queue"));
        connect(m_deselectAllUpdatesButton, &QPushButton::clicked, this, &MainWindow::onDeselectAllUpdates);
        headerLayout->addWidget(m_deselectAllUpdatesButton);

        m_updatesApplyButton = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18n("Apply"));
        m_updatesApplyButton->setToolTip(i18n("Apply pending operations in the queue"));
        m_updatesApplyButton->setEnabled(false);
        connect(m_updatesApplyButton, &QPushButton::clicked, this, &MainWindow::onApplyQueue);
        headerLayout->addWidget(m_updatesApplyButton);

        layout->addWidget(headerRow);

        m_updatesView = new PackageView(new PackageModel(this));
        layout->addWidget(m_updatesView, 1);

        connect(m_updatesView, &PackageView::packageSelected, this, &MainWindow::onPackageSelected);
        connect(m_updatesView, &PackageView::queuePackage, this, &MainWindow::onQueuePackage);
        connect(m_updatesView, &PackageView::packagesQueued, this, &MainWindow::onPackagesQueued);
        connect(m_updatesView, &PackageView::unqueuePackage, this, &MainWindow::onUnqueuePackage);

        m_pageStack->addWidget(m_updatesPage);
    }

    // Page 2: Queue
    {
        m_queuePage = new QWidget;
        auto *layout = new QVBoxLayout(m_queuePage);
        layout->setContentsMargins(0, 0, 0, 0);

        m_queueView = new QueueView(m_queueModel);
        layout->addWidget(m_queueView, 1);

        // Keep the package-list markers in sync with the queue page:
        // removing a single queue item (context menu) or changing its action
        // (Reinstall/Downgrade) must update the package list immediately.
        connect(m_queueView, &QueueView::packageRemoved, this, &MainWindow::onUnqueuePackage);
        connect(m_queueView, &QueueView::todoChanged, this, [this](const QString &nevra, PackageTodo todo) {
            m_packageModel->setQueuedWithTodo(nevra, true, todo);
        });

        auto *buttonBar = new QWidget;
        auto *buttonLayout = new QHBoxLayout(buttonBar);
        buttonLayout->setContentsMargins(8, 4, 8, 8);

        auto *applyBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18n("Apply"));
        auto *clearBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear")), i18n("Clear"));
        auto *removeSelBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove"));

        applyBtn->setObjectName(QStringLiteral("applyBtn"));
        // The offline option is presented in the TransactionResultDialog
        // when the user clicks Apply, not as a persistent checkbox on the
        // queue page.
        connect(applyBtn, &QPushButton::clicked, this, &MainWindow::onApplyQueue);

        connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearQueue);

        // Visible "Remove" button for the selected queue item — the queue
        // page equivalent of the context-menu removal, always at hand.
        connect(removeSelBtn, &QPushButton::clicked, this, [this]() {
            const QString nevra = m_queueView->selectedNevra();
            if (!nevra.isEmpty())
                onUnqueuePackage(nevra);
        });

        buttonLayout->addStretch();
        buttonLayout->addWidget(removeSelBtn);
        buttonLayout->addWidget(clearBtn);
        buttonLayout->addWidget(applyBtn);

        layout->addWidget(buttonBar);

        m_pageStack->addWidget(m_queuePage);
    }

    // Page 3: Repositories
    {
        m_repoPage = new QWidget;
        auto *layout = new QVBoxLayout(m_repoPage);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *header = new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Repositories")));
        header->setContentsMargins(8, 8, 8, 4);
        layout->addWidget(header);

        m_repoView = new RepoView(m_repoModel, m_backend);
        layout->addWidget(m_repoView, 1);
        // Enabling/disabling a repository must NOT re-download repository
        // metadata: per the desired behaviour the software source is only
        // refreshed at startup and when the user applies an upgrade queue.
        // Reloading the lists is enough — the daemon already applied the
        // change and reads the affected repo's metadata on demand.
        connect(m_repoView, &RepoView::repositoriesChanged, this, &MainWindow::onReloadData);

        m_pageStack->addWidget(m_repoPage);
    }

    // Page 4: Flatpak
    {
        m_flatpakPageWidget = new FlatpakPage(m_flatpakBackend);
        m_flatpakPage = m_flatpakPageWidget;
        m_pageStack->addWidget(m_flatpakPage);
    }

    m_mainSplitter->addWidget(m_sidebar);
    m_mainSplitter->addWidget(m_pageStack);
    m_mainSplitter->setStretchFactor(0, 0);
    m_mainSplitter->setStretchFactor(1, 1);
    m_mainSplitter->setSizes({180, 1020});

    // Restart-needed banner (hidden until a transaction installs packages
    // that require a reboot). Placed above the splitter so it spans the full
    // content width.
    createRestartBanner();

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_restartBanner);
    centralLayout->addWidget(m_mainSplitter);
    setCentralWidget(central);

    // Status bar
    m_statusLabel = new QLabel(i18n("Ready"));
    m_queueCountLabel = new QLabel;
    m_updateCountLabel = new QLabel;
    m_progressBar = new QProgressBar;
    m_progressBar->setVisible(false);
    m_progressBar->setMaximumWidth(200);

    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_updateCountLabel);
    statusBar()->addPermanentWidget(m_queueCountLabel);
    statusBar()->addPermanentWidget(m_progressBar);
}

void MainWindow::setupSidebar()
{
    m_sidebar = new QWidget;
    m_sidebar->setObjectName(QStringLiteral("sidebar"));
    m_sidebar->setFixedWidth(180);
    m_sidebar->setStyleSheet(QStringLiteral(
        "QWidget#sidebar { background-color: palette(mid); }"
        "QToolButton { text-align: left; padding: 10px 14px; border: none; "
        "background: transparent; color: palette(text); font-size: 10pt; }"
        "QToolButton:hover { background-color: rgba(255,255,255,0.08); }"
        "QToolButton:checked { background-color: palette(highlight); color: palette(highlighted-text); }"
    ));

    auto *layout = new QVBoxLayout(m_sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *group = new QButtonGroup(this);
    group->setExclusive(true);

    struct PageInfo { QString icon; QString text; int page; };
    QList<PageInfo> pages = {
        {QStringLiteral("view-list"), i18n("Packages"), 0},
        {QStringLiteral("system-software-update"), i18n("Updates"), 1},
        {QStringLiteral("view-task"), i18n("Queue"), 2},
        {QStringLiteral("folder-download"), i18n("Repositories"), 3},
        {QStringLiteral("application-x-addon"), i18n("Flatpak"), 4},
    };

    for (const auto &p : pages) {
        auto *btn = new QToolButton;
        btn->setText(p.text);
        btn->setIcon(QIcon::fromTheme(p.icon));
        btn->setIconSize(QSize(20, 20));
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setCheckable(true);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        group->addButton(btn);
        m_sidebarButtons.append(btn);

        connect(btn, &QToolButton::clicked, this, [this, p]() {
            switchToPage(p.page);
        });

        layout->addWidget(btn);
    }

    // External Linglong Store launcher (replaces the built-in Linglong
    // store and installed-apps pages).  Launches /usr/bin/linglong-store
    // as a separate process.  This button is intentionally NOT part of
    // the exclusive QButtonGroup and is not checkable.
    auto *linglongLaunchBtn = new QToolButton;
    linglongLaunchBtn->setText(i18n("Linglong Store"));
    linglongLaunchBtn->setIcon(QIcon::fromTheme(QStringLiteral("linglong-store")));
    linglongLaunchBtn->setIconSize(QSize(20, 20));
    linglongLaunchBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    linglongLaunchBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    linglongLaunchBtn->setToolTip(i18n("Launch the external Linglong Store application"));
    connect(linglongLaunchBtn, &QToolButton::clicked, this, &MainWindow::launchLinglongStore);
    layout->addWidget(linglongLaunchBtn);

    m_sidebarButtons[0]->setChecked(true);
    layout->addStretch();
}

void MainWindow::setupActions()
{
    KActionCollection *ac = actionCollection();

    QAction *refreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Refresh"), this);
    refreshAction->setShortcut(QKeySequence::Refresh);
    // The manual "Refresh" action force-refreshes the software source, just
    // like the automatic refresh at application startup: it reloads the
    // update list with refreshMetadata=true, which calls
    // cleanCache("expire-cache") + readAllRepos() + resetSession() so the
    // daemon expires and re-downloads every repo's metadata. The CLI
    // cross-check inside fetchUpdates additionally runs
    // `dnf5 check-upgrade --refresh` so third-party-repo updates
    // (e.g. microsoft-edge-stable) are always detected.
    connect(refreshAction, &QAction::triggered, this, &MainWindow::onReloadData);
    ac->addAction(QStringLiteral("refresh"), refreshAction);

    QAction *upgradeAction = new QAction(QIcon::fromTheme(QStringLiteral("system-software-update")), i18n("System Upgrade"), this);
    connect(upgradeAction, &QAction::triggered, this, &MainWindow::onSystemUpgrade);
    ac->addAction(QStringLiteral("system_upgrade"), upgradeAction);

    QAction *distroSyncAction = new QAction(QIcon::fromTheme(QStringLiteral("miryu-package-manager")), i18n("Distro Sync"), this);
    connect(distroSyncAction, &QAction::triggered, this, &MainWindow::onDistroSync);
    ac->addAction(QStringLiteral("distro_sync"), distroSyncAction);

    QAction *refreshMetadataAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Refresh Metadata"), this);
    connect(refreshMetadataAction, &QAction::triggered, this, &MainWindow::onRefreshMetadata);
    ac->addAction(QStringLiteral("refresh_metadata"), refreshMetadataAction);

    QAction *advancedOpsAction = new QAction(QIcon::fromTheme(QStringLiteral("preferences-system")), i18n("Advanced Operations"), this);
    connect(advancedOpsAction, &QAction::triggered, this, &MainWindow::onAdvancedOps);
    ac->addAction(QStringLiteral("advanced_ops"), advancedOpsAction);

    QAction *installRpmAction = new QAction(QIcon::fromTheme(QStringLiteral("application-x-rpm")), i18n("Install RPM File..."), this);
    connect(installRpmAction, &QAction::triggered, this, &MainWindow::onInstallLocalRpm);
    ac->addAction(QStringLiteral("install_rpm"), installRpmAction);

    QAction *aboutAction = new QAction(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("About Miryu Software Center"), this);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAboutDialog);
    ac->addAction(QStringLiteral("about_miryu"), aboutAction);

    QAction *quitAction = KStandardAction::quit(qApp, &QApplication::quit, this);
    ac->addAction(QStringLiteral("quit"), quitAction);

    // Note: the "Find" action (toolbar button + Edit menu entry) was removed
    // on request — the in-page search box is the only search entry point.

    // Edit menu: Select All / Deselect All for the current package view.
    QAction *selectAllAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-select-all")), i18n("Select All"), this);
    selectAllAction->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAction, &QAction::triggered, this, [this]() {
        QAbstractItemView *view = (m_currentPage == 0) ? m_packageView
                                  : (m_currentPage == 1) ? m_updatesView
                                  : nullptr;
        if (view)
            view->selectAll();
    });
    ac->addAction(QStringLiteral("select_all"), selectAllAction);

    QAction *deselectAllAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-select-none")), i18n("Deselect All"), this);
    deselectAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    connect(deselectAllAction, &QAction::triggered, this, [this]() {
        QAbstractItemView *view = (m_currentPage == 0) ? m_packageView
                                  : (m_currentPage == 1) ? m_updatesView
                                  : nullptr;
        if (view)
            view->clearSelection();
    });
    ac->addAction(QStringLiteral("deselect_all"), deselectAllAction);
}

void MainWindow::switchToPage(int page)
{
    m_currentPage = page;
    m_pageStack->setCurrentIndex(page);

    switch (page) {
    case 0:
        m_statusLabel->setText(i18n("Package Browser"));
        if (m_packageModel->rowCount() == 0)
            m_backend->loadPackages(PackageFilter::Installed);
        break;
    case 1:
        m_statusLabel->setText(i18n("Updates"));
        // Opening the Updates page must not refresh the software source;
        // it reuses the cached metadata (see onLoadUpdates(false)).
        onLoadUpdates(false);
        break;
    case 2:
        m_statusLabel->setText(i18n("Transaction Queue"));
        break;
    case 3:
        m_statusLabel->setText(i18n("Repositories"));
        m_backend->loadRepositories();
        break;
    case 4:
        m_statusLabel->setText(i18n("Flatpak"));
        if (m_flatpakBackend->isAvailable())
            m_flatpakPageWidget->reload();
        else
            m_statusLabel->setText(i18n("Flatpak is not installed on this system."));
        break;
    }

    updateStatusBar();
}

void MainWindow::loadInitialData()
{
    if (!m_backend->isInitialized())
        return;

    // The package browser starts on the Installed view: select the
    // "Installed" filter by default (the combo otherwise stays on "All",
    // which would label the list as すべて while showing installed
    // packages). setCurrentIndex() emits currentIndexChanged, which
    // onFilterChanged() turns into loadPackages(Installed, false) — the
    // explicit load below then just hits the cache. If the combo is already
    // on Installed, setCurrentIndex() emits nothing and the explicit load
    // below is the one that populates the list.
    const int idx = m_filterCombo->findData(static_cast<int>(PackageFilter::Installed));
    if (idx >= 0 && m_filterCombo->currentIndex() != idx)
        m_filterCombo->setCurrentIndex(idx);

    m_backend->loadPackages(PackageFilter::Installed);
    m_backend->loadRepositories();
}

void MainWindow::installLocalRpmFiles(const QStringList &filePaths)
{
    if (filePaths.isEmpty())
        return;

    runRpmInstallWithPolkit(filePaths, false);
}

void MainWindow::runRpmInstallWithPolkit(const QStringList &files, bool offline)
{
    // Install local RPM files through the dnf5daemon D-Bus interface rather
    // than shelling out to `pkexec dnf install`. The dnf5daemon transaction
    // (do_transaction) triggers the daemon's own polkit policy
    // (org.rpm.dnf.v0.rpm.execute_transaction for local/unverified packages)
    // whose authentication dialog shows a proper localized message instead of
    // the bare "pkexec" title.
    if (!m_backend->isInitialized()) {
        KMessageBox::error(this,
            i18n("The package backend is not initialized. Cannot install RPM files."),
            i18n("RPM Installation"));
        return;
    }

    // For TransactionCommand::IsFile the backend installs the package specs
    // (file paths) directly via the dnf5daemon rpm.install D-Bus method.
    QList<Package> packages;
    for (const auto &file : files) {
        Package pkg;
        pkg.name = file;
        pkg.state = PackageState::Available;
        pkg.calcTodo();
        packages.append(pkg);
    }

    TransactionOptions buildOpts;
    buildOpts.command = TransactionCommand::IsFile;

    // Build the transaction silently — no busy progress bar animation that
    // could look like a freeze. Just show a descriptive status text.
    m_statusLabel->setText(i18n("Preparing RPM installation..."));

    auto *watcher = new QFutureWatcher<TransactionResult>(this);
    connect(watcher, &QFutureWatcher<TransactionResult>::finished, this, [this, offline, watcher]() {
        if (m_closing.load(std::memory_order_acquire)) {
            watcher->deleteLater();
            return;
        }
        TransactionResult result = watcher->result();
        watcher->deleteLater();

        if (!result.completed) {
            // The raw dnf5 problem text. Kept separate from the user-facing
            // header so the protected-package branch can show it without the
            // misleading "Dependency resolution failed" preamble — the
            // failure there is not a missing dependency, it is dnf5 refusing
            // to touch a protected package.
            const QString problemsText = result.problems.join(QStringLiteral("\n"));
            const bool isProtected =
                problemsText.contains(QStringLiteral("protected"), Qt::CaseInsensitive) ||
                problemsText.contains(QStringLiteral("essential"), Qt::CaseInsensitive);

            if (isProtected) {
                QString detail = problemsText;
                if (!result.error.isEmpty())
                    detail += QStringLiteral("\n\n") + result.error;
                KMessageBox::error(this,
                    i18n("Cannot remove protected packages or critical dependencies, removing them would break the system.\n\n%1").arg(detail),
                    i18n("Protected Package Error"));
            } else {
                QString error;
                if (!result.problems.isEmpty()) {
                    error = i18n("Dependency resolution failed. The reasons are as follows:\n\n")
                            + problemsText;
                    if (!result.error.isEmpty())
                        error += QStringLiteral("\n\n") + result.error;
                } else {
                    error = result.error;
                }
                KMessageBox::error(this, error, i18n("Transaction Error"));
            }
            m_statusLabel->setText(i18n("Ready"));
            return;
        }

        // Show the transaction result as a modal confirmation dialog on top
        // of the main window (exec() blocks until the user decides).
        m_resultDialog = new TransactionResultDialog(result, this);
        const int dlgResult = m_resultDialog->exec();

        // User cancelled — no error dialog, just reset status and return.
        if (dlgResult != QDialog::Accepted) {
            m_resultDialog->deleteLater();
            m_resultDialog = nullptr;
            m_statusLabel->setText(i18n("Ready"));
            return;
        }

        // Run the transaction. dnf5daemon will request polkit
        // authentication automatically (with a proper message).
        m_progressDialog = new ProgressDialog(this);
        m_progressDialog->show();

        auto *runWatcher = new QFutureWatcher<TransactionResult>(this);
        connect(runWatcher, &QFutureWatcher<TransactionResult>::finished, this, [this, runWatcher]() {
            if (m_closing.load(std::memory_order_acquire)) {
                runWatcher->deleteLater();
                return;
            }
            m_progressDialog->close();
            m_progressDialog->deleteLater();
            m_progressDialog = nullptr;

            TransactionResult runResult = runWatcher->result();
            runWatcher->deleteLater();

            if (runResult.completed) {
                KMessageBox::information(this, i18n("RPM installation completed successfully."));
                onRefresh();
                checkRestartNeeded(runResult);
            } else {
                // Detect polkit authentication cancellation and show a
                // friendly message instead of a scary error dialog.
                const QString errLower = runResult.error.toLower();
                bool cancelled = errLower.contains(QStringLiteral("not authorized")) ||
                                 errLower.contains(QStringLiteral("cancel")) ||
                                 errLower.contains(QStringLiteral("auth"));
                if (cancelled) {
                    KMessageBox::information(this,
                        i18n("Operation cancelled by user."),
                        i18n("RPM Installation"));
                } else {
                    KMessageBox::error(this,
                        runResult.error.isEmpty() ? i18n("RPM installation failed.") : runResult.error,
                        i18n("Transaction Failed"));
                }
            }
            m_statusLabel->setText(i18n("Ready"));
        });

        m_statusLabel->setText(i18n("Applying transaction..."));

        TransactionOptions runOpts;
        runOpts.command = TransactionCommand::IsFile;
        runOpts.offline = offline;
        runWatcher->setFuture(QtConcurrent::run([this, runOpts]() {
            return m_backend->runTransaction(runOpts);
        }));

        m_resultDialog->deleteLater();
        m_resultDialog = nullptr;
    });

    watcher->setFuture(QtConcurrent::run([this, packages, buildOpts]() {
        return m_backend->buildTransaction(packages, buildOpts);
    }));
}

void MainWindow::onSearch()
{
    QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty()) {
        onFilterChanged();
        return;
    }

    m_statusLabel->setText(i18n("Searching..."));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);

    SearchField field = static_cast<SearchField>(m_searchFieldCombo->currentData().toInt());
    PackageFilter scope = static_cast<PackageFilter>(m_filterCombo->currentData().toInt());
    m_backend->searchPackages(query, field, scope);
}

void MainWindow::onFilterChanged()
{
    PackageFilter filter = static_cast<PackageFilter>(m_filterCombo->currentData().toInt());

    if (!m_searchEdit->text().trimmed().isEmpty()) {
        onSearch();
        return;
    }

    m_statusLabel->setText(i18n("Loading..."));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);

    m_backend->loadPackages(filter, false);
}

void MainWindow::showInstalledPackages()
{
    // Switch the filter combo back to Installed. If it already shows
    // Installed, setCurrentIndex() emits no signal, so fall through to
    // onFilterChanged() to (re)load — a cache hit makes this instant.
    const int idx = m_filterCombo->findData(static_cast<int>(PackageFilter::Installed));
    if (idx >= 0 && m_filterCombo->currentIndex() != idx) {
        m_filterCombo->setCurrentIndex(idx); // triggers onFilterChanged()
        return;
    }
    onFilterChanged();
}

void MainWindow::onPackageSelected(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    auto *model = qobject_cast<PackageModel *>(const_cast<QAbstractItemModel*>(index.model()));
    if (!model)
        return;

    Package pkg = model->packageAt(index.row());
    // Reveal the detail panel now that the user has selected a package and
    // there is real content to display.
    m_infoWidget->show();
    m_infoWidget->setPackage(pkg);
}

void MainWindow::onQueuePackage(const Package &pkg)
{
    m_queueModel->addPackage(pkg);
    m_packageModel->setQueued(pkg.nevra(), true);
    updateStatusBar();
}

void MainWindow::onPackagesQueued(const QList<Miryu::Package> &packages)
{
    for (const auto &pkg : packages) {
        m_queueModel->addPackage(pkg);
        m_packageModel->setQueued(pkg.nevra(), true);
    }
    if (!packages.isEmpty())
        m_statusLabel->setText(i18np("%1 package queued", "%1 packages queued", packages.size()));
    updateStatusBar();
}

void MainWindow::onUnqueuePackage(const QString &nevra)
{
    m_queueModel->removePackage(nevra);
    m_packageModel->setQueued(nevra, false);
    updateStatusBar();
}

void MainWindow::onApplyQueue()
{
    auto packages = m_queueModel->userPackages();
    if (packages.isEmpty()) {
        KMessageBox::information(this, i18n("Queue is empty. Add packages to the queue first."));
        return;
    }

    // Reject any attempt to remove the currently running kernel: doing so
    // would leave the system without a bootable kernel. We scan the user-
    // requested packages (not the auto-pulled dependencies) for a removal
    // whose target is the running kernel. The check is intentionally
    // conservative — if osrelease cannot be read we let the transaction
    // through and let dnf5 enforce its own protection (protected_packages).
    for (const auto &pkg : packages) {
        if (pkg.todo == PackageTodo::Remove && isRunningKernel(pkg)) {
            KMessageBox::error(this,
                i18n("Cannot uninstall the running kernel. "
                     "Doing so would make the system unbootable.\n\n"
                     "Please boot into a different kernel before removing %1.")
                    .arg(pkg.nevra()),
                i18n("Cannot Remove Running Kernel"));
            return;
        }
    }

    // Reject any attempt to remove the miryu-software-center package itself:
    // it is the running application, so removing it from inside the UI would
    // pull the rug out from under the user (the process would keep running
    // against a deleted binary / resources, and the post-transaction refresh
    // would crash). The user should close the app and use a different tool
    // (dnf5 / dnfdragora) to uninstall it.
    for (const auto &pkg : packages) {
        if (pkg.todo == PackageTodo::Remove &&
            pkg.name == QStringLiteral("miryu-software-center")) {
            KMessageBox::error(this,
                i18n("Miryu Software Center is running, cannot uninstall."),
                i18n("Cannot Remove Miryu Software Center"));
            return;
        }
    }

    // Build transaction
    m_statusLabel->setText(i18n("Resolving transaction..."));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);

    auto *watcher = new QFutureWatcher<TransactionResult>(this);
    connect(watcher, &QFutureWatcher<TransactionResult>::finished, this, [this, watcher]() {
        if (m_closing.load(std::memory_order_acquire)) {
            m_progressBar->setVisible(false);
            watcher->deleteLater();
            return;
        }
        m_progressBar->setVisible(false);
        TransactionResult result = watcher->result();

        if (!result.completed) {
            // The raw dnf5 problem text (e.g. "Problem: The operation would
            // result in removing the following protected packages: ...").
            // Kept separate from the user-facing header so the protected-
            // package branch can show it without the misleading "Dependency
            // resolution failed" preamble — the failure there is not a
            // missing dependency, it is dnf5 refusing to touch a protected
            // package.
            const QString problemsText = result.problems.join(QStringLiteral("\n"));
            const bool isProtected =
                problemsText.contains(QStringLiteral("protected"), Qt::CaseInsensitive) ||
                problemsText.contains(QStringLiteral("essential"), Qt::CaseInsensitive);

            if (isProtected) {
                // Protected packages: do not prefix with "Dependency
                // resolution failed" — the transaction was not blocked by a
                // missing dependency but by dnf5's protected_packages policy.
                // Just explain that protected packages cannot be removed and
                // show which ones dnf5 refused to touch.
                QString detail = problemsText;
                if (!result.error.isEmpty())
                    detail += QStringLiteral("\n\n") + result.error;
                KMessageBox::error(this,
                    i18n("Cannot remove protected packages or critical dependencies, removing them would break the system.\n\n%1").arg(detail),
                    i18n("Protected Package Error"));
            } else {
                // Genuine dependency / resolution failure. Prepend the clear
                // header so the user understands the transaction was rejected
                // because dnf5 could not satisfy the requested operation.
                QString error;
                if (!result.problems.isEmpty()) {
                    error = i18n("Dependency resolution failed. The reasons are as follows:\n\n")
                            + problemsText;
                    if (!result.error.isEmpty())
                        error += QStringLiteral("\n\n") + result.error;
                } else {
                    error = result.error;
                }
                KMessageBox::error(this, error, i18n("Transaction Error"));
            }
            m_statusLabel->setText(i18n("Ready"));
            watcher->deleteLater();
            return;
        }

        // Show transaction result dialog
        m_resultDialog = new TransactionResultDialog(result, this);
        if (m_resultDialog->exec() == QDialog::Accepted) {
            // Run transaction
            m_progressDialog = new ProgressDialog(this);
            m_progressDialog->show();

            auto *runWatcher = new QFutureWatcher<TransactionResult>(this);
            connect(runWatcher, &QFutureWatcher<TransactionResult>::finished, this, [this, runWatcher]() {
                if (m_closing.load(std::memory_order_acquire)) {
                    runWatcher->deleteLater();
                    return;
                }
                m_progressDialog->close();
                m_progressDialog->deleteLater();
                m_progressDialog = nullptr;

                TransactionResult runResult = runWatcher->result();
                if (runResult.completed) {
                    KMessageBox::information(this, i18n("Transaction completed successfully."));
                    m_queueModel->clear();
                    m_packageModel->clearQueued();
                    updateStatusBar();
                    onRefresh();
                    checkRestartNeeded(runResult);
                } else {
                    const QString errLower = runResult.error.toLower();
                    if (errLower.contains(QStringLiteral("not authorized")) ||
                        errLower.contains(QStringLiteral("cancel")) ||
                        errLower.contains(QStringLiteral("auth"))) {
                        KMessageBox::information(this,
                            i18n("Operation cancelled by user."),
                            i18n("Transaction"));
                    } else {
                        KMessageBox::error(this, runResult.error, i18n("Transaction Failed"));
                    }
                }
                m_statusLabel->setText(i18n("Ready"));
                runWatcher->deleteLater();
            });

            bool offline = m_resultDialog->isOffline();
            m_statusLabel->setText(i18n("Applying transaction..."));

            TransactionOptions opts;
            opts.offline = offline;
            runWatcher->setFuture(QtConcurrent::run([this, opts]() {
                return m_backend->runTransaction(opts);
            }));
        }

        m_resultDialog->deleteLater();
        m_resultDialog = nullptr;
        watcher->deleteLater();
    });

    m_statusLabel->setText(i18n("Building transaction..."));
    watcher->setFuture(QtConcurrent::run([this, packages]() {
        return m_backend->buildTransaction(packages);
    }));
}

void MainWindow::onClearQueue()
{
    m_queueModel->clear();
    m_packageModel->clearQueued();
    updateStatusBar();
}

void MainWindow::onSelectAllUpdates()
{
    // Queue every package currently shown in the Updates list view (every
    // row of m_updatesView's model that is not yet queued). Goes through
    // the existing onQueuePackage / onPackagesQueued slots so the queue
    // model, the package model, the Apply button and the status bar all
    // stay in sync — the same code path used when the user ticks the
    // per-row checkbox manually. Skip packages already in the queue to
    // avoid duplicate entries when the user clicks "Select All" twice.
    auto *model = qobject_cast<PackageModel *>(m_updatesView ? m_updatesView->model() : nullptr);
    if (!model)
        return;

    QList<Package> toQueue;
    const QList<Package> packages = model->packages();
    for (auto pkg : packages) {
        if (!pkg.queued && !m_queueModel->contains(pkg.nevra())) {
            pkg.queued = true;
            pkg.todo = calcTodo(pkg.state);
            model->setQueued(pkg.nevra(), true);
            toQueue.append(pkg);
        }
    }
    if (!toQueue.isEmpty()) {
        // Reuse the existing batched-queue path so the queue model,
        // package model, status bar and Apply button are all updated in
        // one shot. This also emits the same "N packages queued" status
        // message the per-row checkbox path produces.
        onPackagesQueued(toQueue);
    }
}

void MainWindow::onDeselectAllUpdates()
{
    // Counterpart of onSelectAllUpdates(): remove every package shown in
    // the Updates list view from the queue. Goes through onUnqueuePackage
    // for the same single-source-of-truth reason. Packages that are not
    // actually queued (or not present in the queue model) are skipped to
    // avoid spurious model churn.
    auto *model = qobject_cast<PackageModel *>(m_updatesView ? m_updatesView->model() : nullptr);
    if (!model)
        return;

    // Walk a *copy* of the package list: onUnqueuePackage() mutates the
    // underlying model's queued state in place, so iterating the live
    // QList<Package> reference directly would risk skipping rows after a
    // removal. The snapshot is unaffected.
    const QList<Package> packages = model->packages();
    for (const auto &pkg : packages) {
        if (pkg.queued || m_queueModel->contains(pkg.nevra()))
            onUnqueuePackage(pkg.nevra());
    }
}

void MainWindow::onRefresh()
{
    if (!m_backend->isInitialized())
        return;

    // Post-transaction reload: drop the daemon's in-memory sack so the next
    // query re-reads the rpmdb and repo metadata from disk (reflecting the
    // packages just installed/upgraded/removed).
    //
    // This is NOT a software-source refresh: no repository metadata is
    // downloaded here. The only automatic refresh moment is application
    // startup (Backend::fetchUpdates with refreshMetadata=true, which calls
    // readAllRepos so only expired metadata is re-downloaded). Applying an
    // update queue resolves directly from the daemon cache that produced
    // the update list, so re-running readAllRepos() here would be a
    // redundant, slow re-download.
    m_backend->client()->resetSession();

    // Clear cached package details so refreshed data is fetched.
    m_backend->cache()->clearDetails();

    PackageFilter filter = static_cast<PackageFilter>(m_filterCombo->currentData().toInt());
    m_backend->loadPackages(filter, true);

    if (m_currentPage == 1)
        onLoadUpdates(false); // reuse the cache refreshed during the transaction
    if (m_currentPage == 3)
        m_backend->loadRepositories();
}

void MainWindow::onReloadData()
{
    if (!m_backend->isInitialized())
        return;

    // Force-refresh: reload the displayed lists from the daemon while
    // force-refreshing the software source (repository metadata), exactly
    // as application startup does. Passing refreshMetadata=true to
    // onLoadUpdates makes fetchUpdates call
    // cleanCache("expire-cache") + readAllRepos() + resetSession()
    // (on a worker thread, so the UI stays responsive), which expires and
    // re-downloads every repo's metadata. The CLI cross-check runs
    // `dnf5 check-upgrade --refresh`, guaranteeing that newly published
    // updates (e.g. microsoft-edge-stable) are detected.
    m_backend->cache()->clearDetails();

    PackageFilter filter = static_cast<PackageFilter>(m_filterCombo->currentData().toInt());
    m_backend->loadPackages(filter, true);

    if (m_currentPage == 1)
        onLoadUpdates(true);
    if (m_currentPage == 3)
        m_backend->loadRepositories();
}

void MainWindow::onDistroSync()
{
    doDistroSyncWithLog();
}

void MainWindow::doDistroSyncWithLog()
{
    // Ask confirmation
    auto ret = KMessageBox::warningTwoActions(this,
        i18n("Distribution sync will synchronize all installed packages to the versions available in the repositories. "
             "This may downgrade or upgrade packages. Continue?"),
        i18n("Distro Sync"),
        KStandardGuiItem::cont(), KStandardGuiItem::cancel());
    if (ret != KMessageBox::PrimaryAction)
        return;

    m_statusLabel->setText(i18n("Performing distribution sync..."));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);

    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher]() {
        if (m_closing.load(std::memory_order_acquire)) {
            m_progressBar->setVisible(false);
            watcher->deleteLater();
            return;
        }
        m_progressBar->setVisible(false);
        bool ok = watcher->result();
        watcher->deleteLater();
        if (ok) {
            KMessageBox::information(this, i18n("Distribution sync completed successfully."), i18n("Distro Sync"));
            onRefresh();
        } else {
            QString err = m_backend->client()->lastError();
            KMessageBox::error(this,
                err.isEmpty() ? i18n("Distribution sync failed.") : err,
                i18n("Distro Sync"));
        }
        m_statusLabel->setText(i18n("Ready"));
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        return m_backend->client()->distroSync(QStringList());
    }));
}

void MainWindow::onRefreshMetadata()
{
    doRefreshMetadataWithLog();
}

void MainWindow::doRefreshMetadataWithLog()
{
    if (!m_backend->isInitialized())
        return;

    // Create or reuse log dialog
    if (!m_logDialog) {
        m_logDialog = new QDialog(this);
        m_logDialog->setWindowTitle(i18n("Refresh Metadata Log"));
        m_logDialog->setMinimumSize(700, 500);
        auto *layout = new QVBoxLayout(m_logDialog);
        m_logView = new QTextBrowser;
        m_logView->setFont(QFont(QStringLiteral("Monospace")));
        layout->addWidget(m_logView);
        auto *btns = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(btns, &QDialogButtonBox::rejected, m_logDialog, &QDialog::reject);
        layout->addWidget(btns);
    }

    m_logView->clear();
    m_logView->append(QStringLiteral("$ pkexec dnf5 makecache --refresh"));
    m_logDialog->show();
    m_logDialog->raise();
    m_logDialog->activateWindow();

    if (m_logProcess) {
        m_logProcess->kill();
        m_logProcess->deleteLater();
    }

    m_logProcess = new QProcess(this);
    m_logProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_logProcess, &QProcess::readyRead, this, [this]() {
        if (m_logProcess && m_logView) {
            QByteArray data = m_logProcess->readAll();
            m_logView->append(QString::fromUtf8(data).trimmed());
            auto cursor = m_logView->textCursor();
            cursor.movePosition(QTextCursor::End);
            m_logView->setTextCursor(cursor);
        }
    });

    connect(m_logProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) {
        if (exitCode == 0) {
            m_logView->append(QStringLiteral("\n[SUCCESS] ") + i18n("Metadata refreshed."));
            // `dnf5 makecache --refresh` refreshed the *system* cache
            // (/var/cache/dnf). The daemon keeps its *own* cache at
            // /var/cache/dnf5daemon-server/, which is still stale. Expire it
            // and readAllRepos() so the daemon re-downloads into its own
            // cache and the subsequent list queries match `dnf check-update`.
            m_backend->client()->cleanCache(QStringLiteral("expire-cache"));
            m_backend->client()->readAllRepos();
            // onRefresh() drops the daemon's in-memory sack (resetSession) so
            // the next list query reloads from the freshly synced daemon
            // cache.
            onRefresh();
        } else {
            m_logView->append(QStringLiteral("\n[ERROR] ") + i18n("Failed to refresh metadata (exit code %1).").arg(exitCode));
        }
        m_statusLabel->setText(i18n("Ready"));
    });

    m_statusLabel->setText(i18n("Refreshing repository metadata..."));
    m_logProcess->start(QStringLiteral("pkexec"), {QStringLiteral("dnf5"), QStringLiteral("makecache"), QStringLiteral("--refresh")});
}

void MainWindow::onSystemUpgrade()
{
    // 1. Read the current VERSION_ID from /usr/lib/os-release
    QString currentVersion;
    QFile osRelease(QStringLiteral("/usr/lib/os-release"));
    if (osRelease.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&osRelease);
        const QString prefix = QStringLiteral("VERSION_ID=");
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.startsWith(prefix)) {
                currentVersion = line.mid(prefix.length()).trimmed();
                if (currentVersion.size() >= 2 &&
                    currentVersion.startsWith(QLatin1Char('"')) &&
                    currentVersion.endsWith(QLatin1Char('"'))) {
                    currentVersion = currentVersion.mid(1, currentVersion.size() - 2);
                }
                break;
            }
        }
        osRelease.close();
    }

    // 2. Ask the user which version they want to upgrade to
    bool inputOk = false;
    QString targetVersion = QInputDialog::getText(this,
        i18n("System Upgrade"),
        i18n("Enter the version you want to upgrade to:"),
        QLineEdit::Normal,
        currentVersion,
        &inputOk);
    if (!inputOk || targetVersion.trimmed().isEmpty())
        return;

    targetVersion = targetVersion.trimmed();

    // 3. Warn if the target version is less than the current version
    if (!currentVersion.isEmpty()) {
        QVersionNumber currentVN = QVersionNumber::fromString(currentVersion);
        QVersionNumber targetVN = QVersionNumber::fromString(targetVersion);
        if (!targetVN.isNull() && targetVN < currentVN) {
            auto verRet = KMessageBox::warningTwoActions(this,
                i18n("The target version (%1) is lower than the current version (%2). "
                     "Downgrading the system may cause instability. Continue anyway?",
                     targetVersion, currentVersion),
                i18n("Version Warning"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
            if (verRet != KMessageBox::PrimaryAction)
                return;
        }
    }

    auto ret = KMessageBox::warningTwoActions(this,
        i18n("This will prepare a system upgrade transaction. Continue?"),
        i18n("System Upgrade"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
    if (ret != KMessageBox::PrimaryAction)
        return;

    m_statusLabel->setText(i18n("Preparing system upgrade..."));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);

    auto *watcher = new QFutureWatcher<TransactionResult>(this);
    connect(watcher, &QFutureWatcher<TransactionResult>::finished, this, [this, targetVersion, watcher]() {
        if (m_closing.load(std::memory_order_acquire)) {
            m_progressBar->setVisible(false);
            watcher->deleteLater();
            return;
        }
        m_progressBar->setVisible(false);
        TransactionResult result = watcher->result();
        if (result.completed) {
            m_resultDialog = new TransactionResultDialog(result, this);
            if (m_resultDialog->exec() == QDialog::Accepted) {
                m_progressDialog = new ProgressDialog(this);
                m_progressDialog->show();

                auto *runWatcher = new QFutureWatcher<TransactionResult>(this);
                connect(runWatcher, &QFutureWatcher<TransactionResult>::finished, this, [this, runWatcher]() {
                    if (m_closing.load(std::memory_order_acquire)) {
                        runWatcher->deleteLater();
                        return;
                    }
                    m_progressDialog->close();
                    m_progressDialog->deleteLater();
                    m_progressDialog = nullptr;
                    TransactionResult r = runWatcher->result();
                    if (r.completed) {
                        KMessageBox::information(this, i18n("System upgrade transaction completed."));
                        checkRestartNeeded(r);
                    } else {
                        const QString errLower = r.error.toLower();
                        if (errLower.contains(QStringLiteral("not authorized")) ||
                            errLower.contains(QStringLiteral("cancel")) ||
                            errLower.contains(QStringLiteral("auth"))) {
                            KMessageBox::information(this,
                                i18n("Operation cancelled by user."),
                                i18n("System Upgrade"));
                        } else {
                            KMessageBox::error(this, r.error, i18n("System Upgrade Failed"));
                        }
                    }
                    m_statusLabel->setText(i18n("Ready"));
                    runWatcher->deleteLater();
                });

                TransactionOptions opts;
                opts.offline = m_resultDialog->isOffline();
                opts.parameter = targetVersion;
                opts.command = TransactionCommand::SystemUpgrade;
                runWatcher->setFuture(QtConcurrent::run([this, opts]() {
                    return m_backend->runTransaction(opts);
                }));
            }
            m_resultDialog->deleteLater();
            m_resultDialog = nullptr;
        } else {
            KMessageBox::error(this, result.error, i18n("System Upgrade Error"));
            m_statusLabel->setText(i18n("Ready"));
        }
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this, targetVersion]() {
        TransactionOptions opts;
        opts.command = TransactionCommand::SystemUpgrade;
        opts.parameter = targetVersion;
        return m_backend->buildTransaction({}, opts);
    }));
}

void MainWindow::onLoadUpdates(bool refreshMetadata)
{
    m_statusLabel->setText(i18n("Checking for updates..."));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);
    // Page switches and post-transaction reloads pass false so the cached
    // metadata is reused. The manual Refresh action and application startup
    // pass true, which calls readAllRepos() so expired metadata is
    // re-downloaded and resetSession() drops the stale in-memory sack.
    // Applying an update queue does NOT refresh metadata — it resolves
    // directly from the daemon cache that produced the update list, so the
    // transaction always matches what was displayed.
    m_backend->loadUpdates(refreshMetadata);
}

void MainWindow::onAdvancedOps()
{
    if (!m_advancedOpsDialog)
        m_advancedOpsDialog = new AdvancedOpsDialog(m_backend, this);
    m_advancedOpsDialog->exec();
}

void MainWindow::showAboutDialog()
{
    // Custom About dialog driven by KAboutData (the single source of truth
    // configured in main.cpp). Unlike the standard KAboutApplicationDialog,
    // this dialog intentionally does NOT render the default KDE bug/support
    // box, so only Miryu's own information is shown.
    const KAboutData about = KAboutData::applicationData();

    QDialog dlg(this);
    dlg.setWindowTitle(i18n("About Miryu Software Center"));
    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(10);

    // Header: icon + name + version
    auto *header = new QHBoxLayout;
    header->setSpacing(12);
    auto *iconLabel = new QLabel;
    iconLabel->setPixmap(QIcon::fromTheme(QStringLiteral("miryu-package-manager")).pixmap(QSize(64, 64)));
    header->addWidget(iconLabel);
    auto *titleBox = new QVBoxLayout;
    titleBox->setSpacing(2);
    auto *nameLabel = new QLabel(QStringLiteral("<b><font size=\"5\">%1</font></b>").arg(about.displayName()));
    auto *versionLabel = new QLabel(i18n("Version %1", about.version()));
    titleBox->addWidget(nameLabel);
    titleBox->addWidget(versionLabel);
    header->addLayout(titleBox);
    header->addStretch();
    layout->addLayout(header);

    if (!about.shortDescription().isEmpty()) {
        auto *descLabel = new QLabel(about.shortDescription());
        descLabel->setWordWrap(true);
        layout->addWidget(descLabel);
    }

    if (!about.copyrightStatement().isEmpty()) {
        auto *copyrightLabel = new QLabel(about.copyrightStatement());
        copyrightLabel->setWordWrap(true);
        layout->addWidget(copyrightLabel);
    }

    const QList<KAboutPerson> authors = about.authors();
    if (!authors.isEmpty()) {
        layout->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Authors"))));
        for (const KAboutPerson &person : authors) {
            QString text = person.name();
            if (!person.emailAddress().isEmpty())
                text += QStringLiteral(" &lt;%1&gt;").arg(person.emailAddress());
            layout->addWidget(new QLabel(text));
        }
    }

    layout->addStretch();

    auto *closeBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-close")), i18n("Close"));
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    dlg.setMinimumWidth(360);
    dlg.exec();
}

void MainWindow::createRestartBanner()
{
    // The banner lives at the top of the central widget (above the main
    // splitter), so it is visible across every page (Packages / Updates /
    // Queue / Repositories / Flatpak) — switching pages only changes the
    // QStackedWidget below it, the banner stays put. It is hidden until
    // setRestartNeeded() makes it visible. The layout is a single row
    // (icon + title + description); the description wraps to additional
    // lines when the window is too narrow, growing the banner vertically
    // instead of clipping or eliding the text.
    m_restartBanner = new QFrame(this);
    m_restartBanner->setObjectName(QStringLiteral("restartBanner"));
    m_restartBanner->setVisible(false);
    m_restartBanner->setStyleSheet(QStringLiteral(
        "QFrame#restartBanner { background-color: #fdebc6; border-bottom: 1px solid #e6a817; }"
        "QLabel { background: transparent; }"
    ));
    // The banner height is locked and re-adjusted in eventFilter() from the
    // description label's heightForWidth(). A QLabel with wordWrap(true)
    // reports an inflated sizeHint (computed for a very narrow width), which
    // would otherwise blow the banner up to a huge height; locking the banner
    // height keeps it compact (single row when the window is wide) and lets
    // it grow exactly to the wrapped text when the window narrows. The
    // vertical margins (4px each side) are symmetric, so the icon/title/
    // description stay exactly centered.
    m_restartBanner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_restartBanner->setFixedHeight(36);
    m_restartBanner->installEventFilter(this);

    auto *layout = new QHBoxLayout(m_restartBanner);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->setSpacing(8);

    auto *iconLabel = new QLabel;
    iconLabel->setPixmap(QIcon::fromTheme(QStringLiteral("system-reboot")).pixmap(QSize(18, 18)));
    iconLabel->setFixedSize(20, 20);
    layout->addWidget(iconLabel);

    auto *titleLabel = new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Need to Restart")));
    titleLabel->setStyleSheet(QStringLiteral("color: #7a4f00;"));
    layout->addWidget(titleLabel);
    auto *descLabel = new QLabel(i18n("You have installed software packages that require a computer restart. "
                                      "Please restart the operating system as soon as possible to ensure "
                                      "the updates take full effect."));
    descLabel->setStyleSheet(QStringLiteral("color: #6b4d00;"));
    // Word-wrapping: the description starts a new line when the window is
    // too narrow. Ignored horizontal policy lets the label fill the banner
    // width without forcing the window to widen to fit the full text; the
    // label height is locked to the wrapped text height (eventFilter()).
    descLabel->setWordWrap(true);
    descLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    descLabel->setMinimumWidth(0);
    descLabel->setFixedHeight(36);
    m_restartBannerDescLabel = descLabel;
    descLabel->installEventFilter(this);
    layout->addWidget(descLabel, 1);

    // Vertically center every element within the banner row so the icon,
    // title and (possibly multi-line) description share the exact same
    // center line.
    layout->setAlignment(iconLabel, Qt::AlignVCenter);
    layout->setAlignment(titleLabel, Qt::AlignVCenter);
    layout->setAlignment(descLabel, Qt::AlignVCenter);
}

QString MainWindow::currentBootId() const
{
    QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(f.readAll()).trimmed();
    return QString();
}

QString MainWindow::runningKernelRelease() const
{
    // /proc/sys/kernel/osrelease holds the uname -r string, e.g.
    // "6.8.10-200.fc39.x86_64". This is exactly the version-release.arch
    // tuple that identifies the running kernel's RPM subpackages (kernel,
    // kernel-core, kernel-modules, …).
    QFile f(QStringLiteral("/proc/sys/kernel/osrelease"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(f.readAll()).trimmed();
    return QString();
}

bool MainWindow::isRunningKernel(const Package &pkg) const
{
    const QString release = runningKernelRelease();
    if (release.isEmpty())
        return false;
    // Kernel subpackages all share the "kernel" name prefix (kernel,
    // kernel-core, kernel-modules, kernel-devel, kernel-headers, …). The
    // running kernel's subpackages all carry the same version-release.arch
    // as the osrelease string.
    if (!pkg.name.startsWith(QStringLiteral("kernel")))
        return false;
    return (pkg.evr() + QStringLiteral(".") + pkg.arch) == release;
}

void MainWindow::setRestartNeeded()
{
    KConfigGroup cg(KSharedConfig::openConfig(), QStringLiteral("Restart"));
    cg.writeEntry(QStringLiteral("needed"), true);
    cg.writeEntry(QStringLiteral("bootId"), currentBootId());
    cg.sync();

    if (m_restartBanner)
        m_restartBanner->setVisible(true);
}

void MainWindow::checkRestartNeededOnStartup()
{
    KConfigGroup cg(KSharedConfig::openConfig(), QStringLiteral("Restart"));
    const bool needed = cg.readEntry(QStringLiteral("needed"), false);
    const QString storedBoot = cg.readEntry(QStringLiteral("bootId"), QString());
    const QString current = currentBootId();

    if (needed && !storedBoot.isEmpty() && storedBoot == current) {
        // Same boot session: a previous transaction still requires a reboot.
        if (m_restartBanner)
            m_restartBanner->setVisible(true);
        // Already known to need a reboot — no need to probe again.
        return;
    } else if (needed) {
        // The boot id changed since the flag was set, so the system has been
        // rebooted. Clear the stale flag.
        cg.writeEntry(QStringLiteral("needed"), false);
        cg.writeEntry(QStringLiteral("bootId"), QString());
        cg.sync();
    }

    // The persistent flag did not indicate a pending reboot, but the user
    // may have updated packages outside of Miryu (e.g. `dnf5 upgrade` from
    // a terminal) since the last boot. Run `dnf5 needs-restarting`
    // asynchronously a moment from now so the banner still appears for
    // such out-of-band updates. The delay lets the window finish painting
    // first and keeps the probe off the GUI thread.
    QTimer::singleShot(2000, this, [this]() {
        if (m_closing.load(std::memory_order_acquire))
            return;
        runNeedsRestartingCheck();
    });
}

void MainWindow::checkRestartNeeded(const TransactionResult &result)
{
    // Packages whose update/install requires a reboot to take effect.
    // This hard-coded list is the always-available baseline; entries from
    // the dnf5 suggest-reboot drop-in directories are merged on top of it
    // so distributions / admins can extend the set without rebuilding the
    // application.
    static const QStringList restartPackages = {
        QStringLiteral("kernel-miryu"),
        QStringLiteral("kernel-miryu-core"),
        QStringLiteral("kernel-evernight"),
        QStringLiteral("kernel-evernight-core"),
        QStringLiteral("plasma-desktop"),
        QStringLiteral("kwin"),
        QStringLiteral("evernight-vista-system-config"),
        QStringLiteral("kmscon"),
        QStringLiteral("systemd"),
    };

    // Merge in any package names declared by the dnf5 suggest-reboot
    // configuration drop-ins (both /usr/share/dnf5/suggest-reboot.d and
    // /etc/dnf/suggest-reboot.d, the latter overriding/extends the former).
    QStringList allRestart = restartPackages;
    allRestart.append(loadSuggestRebootPackages());
    // De-duplicate (case-insensitively) so the matching loop below does
    // not run the same name twice.
    {
        QSet<QString> seen;
        QStringList unique;
        for (const QString &p : std::as_const(allRestart)) {
            QString key = p.toLower();
            if (!seen.contains(key)) {
                seen.insert(key);
                unique.append(p);
            }
        }
        allRestart = std::move(unique);
    }

    // Only installing/upgrading packages can trigger a reboot requirement.
    static const QStringList installActions = {
        QStringLiteral("Install"),
        QStringLiteral("Upgrade"),
        QStringLiteral("Reinstall"),
        QStringLiteral("Downgrade"),
    };

    for (const QString &action : installActions) {
        for (const auto &item : result.itemsByAction(action)) {
            if (nevraMatchesAnyPackage(item.nevra, allRestart)) {
                setRestartNeeded();
                return;
            }
        }
    }

    // None of the well-known packages matched. Ask dnf5 which packages
    // require a restart so other updated packages (e.g. glibc) are caught too.
    runNeedsRestartingCheck();
}

void MainWindow::runNeedsRestartingCheck()
{
    // dnf5 needs-restarting prints one package name per line when a reboot is
    // required, and exits with code 1 in that case (code 0 = nothing needs
    // restarting). The command is read-only and --disablerepo="*" avoids
    // loading repository metadata, keeping it fast. We only need to know
    // WHETHER a reboot is needed, so we parse stdout for package-name lines
    // (ignoring the banner lines dnf5 also prints) and trigger the banner
    // as soon as we see one.
    auto *p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p](int exitCode, QProcess::ExitStatus) {
        if (m_closing.load(std::memory_order_acquire)) {
            p->deleteLater();
            return;
        }
        // needs-restarting exit codes:
        //   0 = nothing needs restarting
        //   1 = a reboot/restart is required (stdout lists the packages)
        // We treat any non-zero exit that produced package-name lines as
        // "reboot required". We deliberately do NOT trigger on exitCode != 0
        // alone, because a failure to run dnf5 (e.g. not installed) would
        // also be non-zero and we do not want to cry wolf then.
        if (exitCode == 0 || exitCode == 1) {
            const QStringList lines = QString::fromUtf8(p->readAllStandardOutput())
                                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                const QString trimmed = line.trimmed();
                // dnf5 prints banner lines like
                //   "Core libraries or services have been updated since boot-up:"
                //   "Reboot is required to fully utilize these updates."
                //   "More information: ..."
                // and the actual package names are prefixed with "* " (e.g.
                // "* kernel-core"). Only a "* " line is a real package.
                if (trimmed.startsWith(QLatin1String("* "))) {
                    setRestartNeeded();
                    break;
                }
            }
        }
        p->deleteLater();
    });

    p->start(QStringLiteral("dnf5"),
             {QStringLiteral("needs-restarting"), QStringLiteral("--disablerepo=*")});
}

QStringList MainWindow::loadSuggestRebootPackages() const
{
    // Scan the dnf5 suggest-reboot drop-in directories and collect every
    // package name listed under [main] / suggest_reboot (the standard
    // dnf5 format). /usr/share/dnf5/suggest-reboot.d ships the
    // distribution defaults; /etc/dnf/suggest-reboot.d is the admin
    // override/extension point. We merge both (etc takes precedence on
    // conflicts, but since this is just a name set precedence does not
    // matter — duplicates are removed by the caller).
    static const QStringList dirs = {
        QStringLiteral("/usr/share/dnf5/suggest-reboot.d"),
        QStringLiteral("/etc/dnf/suggest-reboot.d"),
    };

    QStringList packages;
    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists())
            continue;
        // Process files in a stable order so the result is reproducible.
        const QStringList confFiles = dir.entryList({QStringLiteral("*.conf")},
                                                    QDir::Files | QDir::Readable,
                                                    QDir::Name);
        for (const QString &fileName : confFiles) {
            const QString path = dir.absoluteFilePath(fileName);
            // QSettings parses INI; dnf5's suggest-reboot conf files use the
            // [main] section with a "suggest_reboot" key whose value is a
            // list of package names separated by ';' (and/or whitespace).
            QSettings conf(path, QSettings::IniFormat);
            conf.beginGroup(QStringLiteral("main"));
            const QVariant v = conf.value(QStringLiteral("suggest_reboot"));
            conf.endGroup();

            QStringList names;
            if (v.canConvert<QStringList>()) {
                names = v.toStringList();
            } else if (v.canConvert<QString>()) {
                // QSettings often returns the value as a single string
                // when the list is written on one line; split it ourselves.
                const QString raw = v.toString();
                for (const QString &part : raw.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                    // Each part may itself contain whitespace/newline
                    // separated names, so split on whitespace too.
                    for (const QString &n : part.split(QRegularExpression(QStringLiteral("\\s+")),
                                                       Qt::SkipEmptyParts)) {
                        names << n;
                    }
                }
            }
            for (const QString &n : std::as_const(names)) {
                const QString trimmed = n.trimmed();
                if (!trimmed.isEmpty())
                    packages << trimmed;
            }
        }
    }
    return packages;
}

bool MainWindow::nevraMatchesAnyPackage(const QString &nevra, const QStringList &packages) const
{
    // A transaction item's nevra is the full "name-version-release.arch"
    // string. A suggest-reboot entry is a bare package name (e.g. "kernel",
    // "systemd"). We match when the entry equals the nevra, or the nevra
    // starts with "<entry>-". The dash separator is what RPM uses between
    // the package name and the version, so "systemd-256.7-1.fc44.x86_64"
    // matches "systemd" but "systemd-libs-..." would NOT match "systemd".
    // Entries ending with '*' are treated as a prefix glob so
    // "kernel-miryu*" matches "kernel-miryu-core-...".
    const QString lowerNevra = nevra.toLower();
    for (const QString &pkg : packages) {
        const QString lowerPkg = pkg.toLower();
        if (lowerPkg.endsWith(QLatin1Char('*'))) {
            const QString prefix = lowerPkg.left(lowerPkg.size() - 1);
            if (lowerNevra.startsWith(prefix, Qt::CaseInsensitive))
                return true;
        } else if (lowerNevra == lowerPkg ||
                   lowerNevra.startsWith(lowerPkg + QLatin1Char('-'), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

void MainWindow::onInstallLocalRpm()
{
    QStringList files = QFileDialog::getOpenFileNames(this,
        i18n("Select RPM Package Files"),
        QString(),
        i18n("RPM Packages (*.rpm)"));
    if (files.isEmpty())
        return;

    // Ask whether to use offline mode
    auto ret = KMessageBox::questionTwoActions(this,
        i18n("Download now and install on next reboot (offline update)?"),
        i18n("Installation Mode"),
        KGuiItem(i18n("Offline Update")), KGuiItem(i18n("Install Now")));
    bool offline = (ret == KMessageBox::PrimaryAction);

    runRpmInstallWithPolkit(files, offline);
}

void MainWindow::launchLinglongStore()
{
    // Launch the external Linglong Store application as a detached process.
    const QString storePath = QStringLiteral("/usr/bin/linglong-store");
    if (!QProcess::startDetached(storePath, QStringList())) {
        KMessageBox::error(this,
            i18n("Failed to launch the Linglong Store.\n\n"
                 "Please ensure that the linglong-store application is installed\n"
                 "and available at %1.").arg(storePath),
            i18n("Linglong Store"));
    }
}

void MainWindow::onPackagesLoaded(int filter, const QList<Package> &packages)
{
    Q_UNUSED(filter)
    m_progressBar->setVisible(false);
    m_packageModel->setPackages(packages);
    m_statusLabel->setText(i18np("%1 package", "%1 packages", packages.size()));
    // The detail panel stays hidden until the user actually selects a package
    // — showing it now would render an empty placeholder (no name, version,
    // etc.) because no row is selected yet.
}

void MainWindow::onSearchCompleted(const QList<Package> &packages)
{
    m_progressBar->setVisible(false);
    m_packageModel->setPackages(packages);
    m_statusLabel->setText(i18np("%1 result", "%1 results", packages.size()));
}

void MainWindow::onRepositoriesLoaded(const QList<Repository> &repos)
{
    m_repoModel->setRepositories(repos);
    m_progressBar->setVisible(false);
    m_statusLabel->setText(i18n("Ready"));
}

void MainWindow::onUpdatesLoaded(const QList<Package> &updates)
{
    m_progressBar->setVisible(false);
    auto *model = qobject_cast<PackageModel*>(m_updatesView->model());
    if (model)
        model->setPackages(updates);
    m_statusLabel->setText(i18np("%1 update available", "%1 updates available", updates.size()));
}

void MainWindow::onTransactionProgress(const QString &message, int percent)
{
    // The status bar still shows the per-action message + percent so the
    // user can see what the daemon is currently doing. The modal
    // ProgressDialog is no longer driven from here — it is driven by
    // onOverallProgress() with the aggregated overall percent instead.
    m_statusLabel->setText(message);
    if (percent >= 0) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(percent);
        m_progressBar->setVisible(true);
    }
}

void MainWindow::onDownloadProgress(const QString &downloadId, qint64 total, qint64 downloaded)
{
    // Status-bar-only: the per-package message lets the user see which
    // package is currently being downloaded. The modal ProgressDialog is
    // driven by the aggregated overallProgress signal (onOverallProgress)
    // so it shows a single overall download percent instead of bouncing
    // per-package ticks.
    m_statusLabel->setText(i18n("Downloading %1", downloadId));
    if (total > 0) {
        const int percent = static_cast<int>(downloaded * 100 / total);
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(percent);
        m_progressBar->setVisible(true);
    }
}

void MainWindow::onOverallProgress(int percent, const QString &phase, const QString &message)
{
    // Single source of truth for the modal ProgressDialog: the backend
    // already picked the right phase message ("Downloading packages...",
    // "Installing packages...", "Verifying packages...", "Preparing...")
    // and computed the overall percent. The dialog only reflects it.
    if (!m_progressDialog)
        return;

    m_progressDialog->setMessage(message);
    // During the "prepare" phase the daemon has not started downloading or
    // installing yet, so keep BOTH bars (the status-bar one AND the dialog
    // one) in indeterminate busy mode until real progress arrives. As soon
    // as the phase switches to download / install / verify, switch back to
    // a 0..100 range and set the actual percent. The dialog's bar previously
    // stayed frozen at 0% with no animation during "prepare", which gave the
    // strong impression the call had deadlocked — the bar now visibly spins
    // so the user knows the daemon is still resolving the goal / loading
    // metadata.
    const bool indeterminate = (phase == QStringLiteral("prepare"));
    m_progressBar->setRange(0, indeterminate ? 0 : 100);
    if (!indeterminate) {
        m_progressBar->setValue(percent);
        m_progressBar->setVisible(true);
    }
    m_progressDialog->setIndeterminate(indeterminate);
    m_progressDialog->setProgress(percent);
}

void MainWindow::onError(const QString &error)
{
    m_progressBar->setVisible(false);
    m_statusLabel->setText(i18n("Error"));
    KMessageBox::error(this, error, i18n("Error"));
}

void MainWindow::onUpdatesAvailable(int count)
{
    if (count > 0) {
        m_updateCountLabel->setText(i18n("%1 updates", count));
        m_updateCountLabel->setStyleSheet(
            QStringLiteral("color: white; background-color: #e87431; padding: 2px 8px; border-radius: 3px;"));
    } else {
        m_updateCountLabel->clear();
        m_updateCountLabel->setStyleSheet(QString());
    }
}

void MainWindow::updateStatusBar()
{
    int count = m_queueModel->count();
    if (count > 0) {
        m_queueCountLabel->setText(i18n("Queue: %1 items", count));
        m_queueCountLabel->setStyleSheet(QStringLiteral("color: palette(highlighted-text); background-color: palette(highlight); padding: 2px 8px; border-radius: 3px;"));
    } else {
        m_queueCountLabel->clear();
        m_queueCountLabel->setStyleSheet(QString());
    }
    updateApplyButtons();
}

void MainWindow::updateApplyButtons()
{
    // The per-page "Apply" buttons are visible on the Packages and Updates
    // pages so the user can execute a pending queue without first switching
    // to the Queue page. They are enabled only when the queue actually holds
    // pending transactions; otherwise they are greyed out so the affordance
    // is still discoverable but cannot be triggered by accident.
    const bool hasPending = m_queueModel->count() > 0;
    if (m_applyButton)
        m_applyButton->setEnabled(hasPending);
    if (m_updatesApplyButton)
        m_updatesApplyButton->setEnabled(hasPending);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Keep the restart banner hugging its content: on any resize/show of the
    // banner or its description label, recompute the wrapped text height via
    // heightForWidth at the CURRENT width, lock the description label to it
    // and the banner to it plus the vertical margins. Wide window -> compact
    // single row; narrow window -> the text wraps to a new line and the
    // banner grows exactly to fit it, always centered.
    if ((watched == m_restartBanner || watched == m_restartBannerDescLabel)
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        updateRestartBannerHeight();
    }
    return KXmlGuiWindow::eventFilter(watched, event);
}

void MainWindow::updateRestartBannerHeight()
{
    if (!m_restartBanner || !m_restartBannerDescLabel)
        return;
    const int labelWidth = m_restartBannerDescLabel->width();
    if (labelWidth <= 0)
        return;
    const int textHeight = m_restartBannerDescLabel->heightForWidth(labelWidth);
    if (textHeight <= 0)
        return;
    // Lock the description label to the wrapped text height and the banner
    // to text height + the symmetric 4px vertical margins. With every
    // element AlignVCenter'ed, icon/title/description stay exactly centered.
    m_restartBannerDescLabel->setFixedHeight(textHeight);
    m_restartBanner->setFixedHeight(qMax(36, textHeight + 8));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Set the closing flag FIRST so that all async callbacks
    // (QFutureWatcher finished handlers, D-Bus signal relays, etc.)
    // can bail out early instead of accessing a partially destroyed
    // MainWindow.  This must happen before any teardown begins.
    m_closing.store(true, std::memory_order_release);

    if (m_queueModel->count() > 0) {
        auto ret = KMessageBox::warningTwoActions(this,
            i18n("There are pending operations in the queue. Close anyway?"),
            i18n("Pending Operations"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
        if (ret != KMessageBox::PrimaryAction) {
            m_closing.store(false, std::memory_order_release);
            event->ignore();
            return;
        }
    }

    // Stop update checker timer so it cannot fire during teardown.
    if (m_updateChecker)
        m_updateChecker->stop();

    // Kill any running log process and wait briefly for it to exit.
    if (m_logProcess && m_logProcess->state() != QProcess::NotRunning) {
        m_logProcess->kill();
        m_logProcess->waitForFinished(1000);
    }

    // --- Targeted signal disconnection ---
    // We must NOT use blanket QObject::disconnect(this, nullptr, nullptr, nullptr)
    // or QObject::disconnect(nullptr, nullptr, this, nullptr).
    //
    // KXmlGuiWindow has many internal signal connections (toolbar management,
    // action collection, XML GUI factory, etc.) that are required for proper
    // cleanup during its destructor.  Breaking those connections causes a
    // use-after-free or null pointer dereference, resulting in SIGSEGV on exit.
    //
    // Instead, we disconnect only the external backend signals that could
    // fire during teardown and reach this window.

    // Backend outlives MainWindow (stack-allocated in main.cpp), so we must
    // disconnect all signals it emits that are received by this window.
    if (m_backend)
        m_backend->disconnect(this);

    // Disconnect child backends as a precaution.  They are parented to this
    // window and will be destroyed by Qt's parent-child mechanism, but
    // disconnecting early prevents any in-flight signal deliveries from
    // QProcess or QNetworkAccessManager from reaching us during teardown.
    if (m_flatpakBackend)
        m_flatpakBackend->disconnect(this);

    // Also disconnect UpdateChecker from both Backend and MainWindow so that
    // a pending loadUpdates() result cannot trigger updatesAvailable() after
    // we start tearing down.
    if (m_updateChecker) {
        if (m_backend)
            m_backend->disconnect(m_updateChecker);
        m_updateChecker->disconnect(this);
    }

    // Disconnect PackageInfoWidget signals that trigger async operations.
    if (m_infoWidget)
        m_infoWidget->disconnect(this);

    // Disconnect package view signals that could trigger queued operations.
    if (m_packageView)
        m_packageView->disconnect(this);
    if (m_updatesView)
        m_updatesView->disconnect(this);

    // Disconnect repo view signals that could trigger backend operations.
    if (m_repoView)
        m_repoView->disconnect(this);

    // NOTE: We intentionally do NOT call processEvents() here.
    //
    // Calling processEvents() during teardown is dangerous: it causes
    // deleteLater() callbacks to fire (destroying child objects that may
    // still be referenced by posted events) and delivers queued D-Bus
    // signals to slot methods on partially-destroyed objects.  This was
    // the root cause of the SEGV crash in free() during QObject::event()
    // processing — a posted event was delivered to MainWindow after
    // processEvents() had already destroyed some of its children.
    //
    // The m_closing flag is the correct mechanism: all async callbacks
    // (QFutureWatcher finished handlers, D-Bus signal relays, etc.) check
    // it and bail out early.  Qt's own parent-child destruction sequence
    // handles the rest safely.

    // NOTE: We intentionally do NOT call m_backend->client()->closeSession() here.
    // Backend outlives MainWindow and its own destructor handles session cleanup
    // safely.  Closing the session from MainWindow could race with Backend's
    // internal signal connections (Dnf5DaemonClient signals -> Backend lambdas)
    // during Backend's own cleanup path.

    event->accept();

    // Explicitly quit the application.  We cleared WA_DeleteOnClose so that
    // Qt would not delete this stack-allocated window via deleteLater() (which
    // caused the SEGV).  But without WA_DeleteOnClose, closing the window only
    // hides it — Qt's lastWindowClosed mechanism may not fire reliably (hidden
    // top-level windows, KDBusService, etc. can prevent it).  Calling quit()
    // explicitly ensures app.exec() returns and stack objects are destroyed
    // normally when main() returns.
    QCoreApplication::quit();
}

}
