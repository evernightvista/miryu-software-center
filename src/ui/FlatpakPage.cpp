#include "FlatpakPage.h"
#include "../core/FlatpakApp.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListView>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QComboBox>
#include <QScrollArea>
#include <QFrame>
#include <QMenu>
#include <QAction>
#include <QStyledItemDelegate>
#include <QPainter>
#include <KLineEdit>
#include <KLocalizedString>
#include <KMessageBox>
#include <KGuiItem>

namespace Miryu {

// ---------------------------------------------------------------------------
// Custom list delegate
// ---------------------------------------------------------------------------

class FlatpakAppDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        if (opt.state & QStyle::State_Selected)
            painter->fillRect(opt.rect, opt.palette.highlight());
        else if (opt.state & QStyle::State_MouseOver)
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));

        bool installed = index.data(FlatpakAppModel::InstalledRole).toBool();
        bool upgradable = index.data(FlatpakAppModel::UpgradableRole).toBool();

        // Icon placeholder
        QRect iconRect(opt.rect.x() + 8, opt.rect.y() + 8, 40, 40);
        QColor iconColor = upgradable ? QColor(255, 200, 100)
                         : installed   ? QColor(100, 180, 255)
                                       : QColor(200, 200, 200);
        painter->setBrush(iconColor);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(iconRect, 6, 6);
        painter->setPen(Qt::white);
        QFont iconFont = opt.font;
        iconFont.setBold(true);
        iconFont.setPointSize(14);
        painter->setFont(iconFont);
        QString name = index.data(FlatpakAppModel::NameRole).toString();
        painter->drawText(iconRect, Qt::AlignCenter, name.left(1).toUpper());

        // Name
        QRect nameRect = opt.rect.adjusted(56, 6, -120, -opt.rect.height() / 2);
        QFont nameFont = opt.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(opt.palette.color(QPalette::WindowText));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(FlatpakAppModel::NameRole).toString());

        // App ID + version
        QRect idRect = opt.rect.adjusted(56, opt.rect.height() / 2, -120, -4);
        QFont idFont = opt.font;
        idFont.setPointSize(idFont.pointSize() - 1);
        painter->setFont(idFont);
        painter->setPen(opt.palette.color(QPalette::Mid));
        QString idText = index.data(FlatpakAppModel::AppIdRole).toString() +
                         QStringLiteral("  v") + index.data(FlatpakAppModel::VersionRole).toString();
        painter->drawText(idRect, Qt::AlignLeft | Qt::AlignTop, idText);

        // Status badge
        QRect statusRect = opt.rect.adjusted(opt.rect.width() - 110, 8, -8, -8);
        QFont badgeFont = opt.font;
        badgeFont.setPointSize(badgeFont.pointSize() - 1);
        badgeFont.setBold(true);
        painter->setFont(badgeFont);

        if (upgradable) {
            painter->setPen(QColor(255, 140, 0));
            painter->drawText(statusRect, Qt::AlignRight | Qt::AlignTop, i18n("Update"));
        } else if (installed) {
            painter->setPen(QColor(46, 160, 67));
            painter->drawText(statusRect, Qt::AlignRight | Qt::AlignTop, i18n("Installed"));
        } else {
            painter->setPen(QColor(0, 114, 178));
            painter->drawText(statusRect, Qt::AlignRight | Qt::AlignTop, i18n("Install"));
        }

        // Size
        QRect sizeRect = opt.rect.adjusted(opt.rect.width() - 110, opt.rect.height() / 2, -8, -4);
        painter->setFont(idFont);
        painter->setPen(opt.palette.color(QPalette::Mid));
        painter->drawText(sizeRect, Qt::AlignRight | Qt::AlignTop,
                          index.data(FlatpakAppModel::SizeTextRole).toString());

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        return QSize(400, 56);
    }
};

// ---------------------------------------------------------------------------
// FlatpakPage implementation
// ---------------------------------------------------------------------------

FlatpakPage::FlatpakPage(FlatpakBackend *backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
    , m_installedModel(new FlatpakAppModel(this))
    , m_storeModel(new FlatpakAppModel(this))
{
    setupUI();

    // Connect backend signals
    connect(m_backend, &FlatpakBackend::installedAppsLoaded,
            this, &FlatpakPage::onInstalledAppsLoaded);
    connect(m_backend, &FlatpakBackend::remotesLoaded,
            this, &FlatpakPage::onRemotesLoaded);
    connect(m_backend, &FlatpakBackend::searchCompleted,
            this, &FlatpakPage::onSearchCompleted);
    connect(m_backend, &FlatpakBackend::operationStarted,
            this, &FlatpakPage::onOperationStarted);
    connect(m_backend, &FlatpakBackend::operationProgress,
            this, &FlatpakPage::onOperationProgress);
    connect(m_backend, &FlatpakBackend::operationFinished,
            this, &FlatpakPage::onOperationFinished);
    connect(m_backend, &FlatpakBackend::errorOccurred,
            this, &FlatpakPage::onError);
}

void FlatpakPage::setupUI()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(1);

    // --- Sidebar ---
    m_sidebar = new QListWidget;
    m_sidebar->setObjectName(QStringLiteral("flatpakSidebar"));
    m_sidebar->setIconSize(QSize(22, 22));
    m_sidebar->setUniformItemSizes(true);
    m_sidebar->setSpacing(1);
    m_sidebar->setMinimumWidth(160);
    m_sidebar->setMaximumWidth(260);

    auto *appsItem = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("application-x-executable")),
                                         i18n("Flatpak Apps"));
    m_sidebar->addItem(appsItem);

    auto *storeItem = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("system-search")),
                                          i18n("Flatpak Store"));
    m_sidebar->addItem(storeItem);

    m_sidebar->setCurrentRow(0);
    connect(m_sidebar, &QListWidget::currentRowChanged,
            this, &FlatpakPage::onSidebarChanged);

    splitter->addWidget(m_sidebar);

    // --- Right content area ---
    auto *contentWidget = new QWidget;
    auto *contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_pageStack = new QStackedWidget;

    // Page 0: Installed apps
    m_installedPage = new QWidget;
    setupInstalledPage(m_installedPage);
    m_pageStack->addWidget(m_installedPage);

    // Page 1: Store / search
    m_storePage = new QWidget;
    setupStorePage(m_storePage);
    m_pageStack->addWidget(m_storePage);

    contentLayout->addWidget(m_pageStack, 1);

    // Shared progress bar
    m_progressBar = new QProgressBar;
    m_progressBar->setVisible(false);
    contentLayout->addWidget(m_progressBar);

    // Shared status label
    m_statusLabel = new QLabel(i18n("Loading..."));
    m_statusLabel->setContentsMargins(8, 4, 8, 4);
    contentLayout->addWidget(m_statusLabel);

    splitter->addWidget(contentWidget);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(splitter);
}

void FlatpakPage::setupInstalledPage(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    auto *toolbar = new QWidget;
    auto *tbLayout = new QHBoxLayout(toolbar);
    tbLayout->setContentsMargins(8, 8, 8, 4);

    m_filterEdit = new KLineEdit;
    m_filterEdit->setPlaceholderText(i18n("Filter installed Flatpak apps..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setTrapReturnKey(true);
    connect(m_filterEdit, &KLineEdit::textChanged, this, &FlatpakPage::onFilterInstalled);
    connect(m_filterEdit, &KLineEdit::returnPressed, this, &FlatpakPage::onFilterInstalled);

    m_updateAllBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("system-software-update")),
                                     i18n("Update All"));
    m_updateAllBtn->setEnabled(false);
    connect(m_updateAllBtn, &QPushButton::clicked, this, &FlatpakPage::onUpdateAll);

    m_uninstallSelectedBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                             i18n("Uninstall Selected"));
    m_uninstallSelectedBtn->setEnabled(false);
    connect(m_uninstallSelectedBtn, &QPushButton::clicked, this, &FlatpakPage::onUninstallSelected);

    m_updateSelectedBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("system-software-update")),
                                          i18n("Update Selected"));
    m_updateSelectedBtn->setEnabled(false);
    connect(m_updateSelectedBtn, &QPushButton::clicked, this, &FlatpakPage::onUpdateSelected);

    auto *refreshBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                       i18n("Refresh"));
    connect(refreshBtn, &QPushButton::clicked, this, &FlatpakPage::onRefresh);

    tbLayout->addWidget(m_filterEdit, 1);
    tbLayout->addWidget(m_updateSelectedBtn);
    tbLayout->addWidget(m_uninstallSelectedBtn);
    tbLayout->addWidget(m_updateAllBtn);
    tbLayout->addWidget(refreshBtn);

    layout->addWidget(toolbar);

    // App list
    m_installedList = new QListView;
    m_installedList->setModel(m_installedModel);
    m_installedList->setItemDelegate(new FlatpakAppDelegate(this));
    m_installedList->setUniformItemSizes(true);
    m_installedList->setSpacing(1);
    m_installedList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_installedList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_installedList, &QListView::activated, this, &FlatpakPage::onInstalledAppClicked);
    connect(m_installedList, &QListView::customContextMenuRequested,
             this, [this](const QPoint &pos) { showContextMenu(pos); });
    connect(m_installedList->selectionModel(), &QItemSelectionModel::selectionChanged,
             this, [this]() {
                 int count = selectedInstalledApps().size();
                 m_uninstallSelectedBtn->setEnabled(count > 0);
                 m_updateSelectedBtn->setEnabled(count > 0);
             });

    layout->addWidget(m_installedList, 1);
}

void FlatpakPage::setupStorePage(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    auto *toolbar = new QWidget;
    auto *tbLayout = new QHBoxLayout(toolbar);
    tbLayout->setContentsMargins(8, 8, 8, 4);

    m_searchEdit = new KLineEdit;
    m_searchEdit->setPlaceholderText(i18n("Search Flatpak store..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setTrapReturnKey(true);
    connect(m_searchEdit, &KLineEdit::returnPressed, this, &FlatpakPage::onSearch);

    m_remoteCombo = new QComboBox;
    m_remoteCombo->addItem(i18n("All Remotes"), QString());
    connect(m_remoteCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FlatpakPage::onRemoteChanged);

    m_installSelectedBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("download")),
                                           i18n("Install Selected"));
    m_installSelectedBtn->setEnabled(false);
    connect(m_installSelectedBtn, &QPushButton::clicked, this, &FlatpakPage::onInstallSelected);

    auto *refreshBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                       i18n("Refresh"));
    connect(refreshBtn, &QPushButton::clicked, this, &FlatpakPage::onRefresh);

    tbLayout->addWidget(m_searchEdit, 1);
    tbLayout->addWidget(m_remoteCombo);
    tbLayout->addWidget(m_installSelectedBtn);
    tbLayout->addWidget(refreshBtn);

    layout->addWidget(toolbar);

    // Stack: list view / detail view
    m_storeStack = new QStackedWidget;

    m_storeList = new QListView;
    m_storeList->setModel(m_storeModel);
    m_storeList->setItemDelegate(new FlatpakAppDelegate(this));
    m_storeList->setUniformItemSizes(true);
    m_storeList->setSpacing(1);
    m_storeList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_storeList, &QListView::activated, this, &FlatpakPage::onStoreAppClicked);
    connect(m_storeList->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() {
                m_installSelectedBtn->setEnabled(selectedStoreApps().size() > 0);
            });

    m_storeStack->addWidget(m_storeList);

    // Detail view
    setupDetailView();
    auto *detailScroll = new QScrollArea;
    detailScroll->setWidget(m_detailWidget);
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    m_storeStack->addWidget(detailScroll);

    layout->addWidget(m_storeStack, 1);
}

void FlatpakPage::setupDetailView()
{
    m_detailWidget = new QWidget;
    auto *layout = new QVBoxLayout(m_detailWidget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // Header: icon + title + buttons
    auto *headerLayout = new QHBoxLayout;

    m_detailIcon = new QLabel;
    m_detailIcon->setFixedSize(64, 64);
    m_detailIcon->setAlignment(Qt::AlignCenter);
    m_detailIcon->setStyleSheet(QStringLiteral(
        "background: #ddd; border-radius: 8px; font-size: 24pt; font-weight: bold;"));

    auto *titleLayout = new QVBoxLayout;
    m_detailName = new QLabel;
    QFont nameFont = m_detailName->font();
    nameFont.setBold(true);
    nameFont.setPointSize(nameFont.pointSize() + 4);
    m_detailName->setFont(nameFont);

    m_detailAppId = new QLabel;
    m_detailAppId->setStyleSheet(QStringLiteral("color: palette(mid);"));

    m_detailVersion = new QLabel;
    m_detailVersion->setStyleSheet(QStringLiteral("color: palette(mid);"));

    titleLayout->addWidget(m_detailName);
    titleLayout->addWidget(m_detailAppId);
    titleLayout->addWidget(m_detailVersion);
    titleLayout->addStretch();

    // Action buttons
    auto *btnLayout = new QVBoxLayout;
    btnLayout->setSpacing(4);

    m_installBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("download")),
                                    i18n("Install"));
    m_installBtn->setObjectName(QStringLiteral("flatpakInstallBtn"));
    connect(m_installBtn, &QPushButton::clicked, this, &FlatpakPage::onInstallApp);

    m_uninstallBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                     i18n("Uninstall"));
    connect(m_uninstallBtn, &QPushButton::clicked, this, &FlatpakPage::onUninstallApp);

    m_updateBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("system-software-update")),
                                  i18n("Update"));
    connect(m_updateBtn, &QPushButton::clicked, this, &FlatpakPage::onUpdateApp);

    m_runBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                               i18n("Run"));
    connect(m_runBtn, &QPushButton::clicked, this, &FlatpakPage::onRunApp);

    btnLayout->addWidget(m_installBtn);
    btnLayout->addWidget(m_uninstallBtn);
    btnLayout->addWidget(m_updateBtn);
    btnLayout->addWidget(m_runBtn);

    headerLayout->addWidget(m_detailIcon);
    headerLayout->addLayout(titleLayout, 1);
    headerLayout->addLayout(btnLayout);

    layout->addLayout(headerLayout);

    // Separator
    auto *sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    layout->addWidget(sep);

    // Info section
    m_detailBranch = new QLabel;
    m_detailRemote = new QLabel;
    m_detailInstallType = new QLabel;
    m_detailSize = new QLabel;
    layout->addWidget(m_detailBranch);
    layout->addWidget(m_detailRemote);
    layout->addWidget(m_detailInstallType);
    layout->addWidget(m_detailSize);

    // Description
    auto *descTitle = new QLabel(QStringLiteral("<b>%1</b>").arg(i18n("Description")));
    layout->addWidget(descTitle);

    m_detailDesc = new QLabel;
    m_detailDesc->setWordWrap(true);
    m_detailDesc->setTextFormat(Qt::RichText);
    layout->addWidget(m_detailDesc);

    layout->addStretch();

    // Back button
    m_backBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("go-previous")),
                                 i18n("Back to List"));
    connect(m_backBtn, &QPushButton::clicked, this, &FlatpakPage::showStoreListView);
    layout->addWidget(m_backBtn);
}

// ---------------------------------------------------------------------------
// Sidebar / page switching
// ---------------------------------------------------------------------------

void FlatpakPage::onSidebarChanged(int index)
{
    if (index < 0 || index >= m_pageStack->count())
        return;
    m_pageStack->setCurrentIndex(index);

    if (index == 0) {
        m_statusLabel->setText(i18n("Loading installed Flatpak apps..."));
    } else {
        m_statusLabel->setText(i18n("Search the Flatpak store"));
    }
}

// ---------------------------------------------------------------------------
// Installed page slots
// ---------------------------------------------------------------------------

void FlatpakPage::onFilterInstalled()
{
    QString keyword = m_filterEdit->text().trimmed().toLower();
    if (keyword.isEmpty()) {
        m_installedModel->setApps(m_allInstalledApps);
        return;
    }

    QList<FlatpakApp> filtered;
    for (const auto &app : m_allInstalledApps) {
        if (app.name.toLower().contains(keyword) ||
            app.appId.toLower().contains(keyword) ||
            app.description.toLower().contains(keyword))
            filtered.append(app);
    }
    m_installedModel->setApps(filtered);
    m_statusLabel->setText(i18np("%1 result", "%1 results", filtered.size()));
}

void FlatpakPage::onInstalledAppClicked(const QModelIndex &index)
{
    Q_UNUSED(index)
    // Installed list clicks are handled via context menu
}

void FlatpakPage::showContextMenu(const QPoint &pos)
{
    QModelIndex index = m_installedList->indexAt(pos);
    if (!index.isValid())
        return;

    FlatpakApp app = m_installedModel->appAt(index.row());

    QMenu menu(this);

    auto *runAction = menu.addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                                     i18n("Run"));
    connect(runAction, &QAction::triggered, this, [this, app]() {
        m_backend->runApp(app.appId);
    });

    if (app.upgradable) {
        menu.addSeparator();
        auto *updateAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("system-software-update")), i18n("Update"));
        connect(updateAction, &QAction::triggered, this, [this, app]() {
            m_backend->updateApp(app.appId);
        });
    }

    menu.addSeparator();

    auto *uninstallAction = menu.addAction(
        QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Uninstall"));
    connect(uninstallAction, &QAction::triggered, this, [this, app]() {
        auto ret = KMessageBox::warningTwoActions(this,
            i18n("Uninstall '%1' (%2)?", app.name, app.appId),
            i18n("Uninstall"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
        if (ret == KMessageBox::PrimaryAction)
            m_backend->uninstallApp(app.appId);
    });

    menu.exec(m_installedList->viewport()->mapToGlobal(pos));
}

void FlatpakPage::onUpdateAll()
{
    int upgCount = 0;
    for (const auto &app : m_allInstalledApps) {
        if (app.upgradable)
            ++upgCount;
    }

    if (upgCount == 0) {
        KMessageBox::information(this, i18n("No updates available."));
        return;
    }

    auto ret = KMessageBox::warningTwoActions(this,
        i18np("Update %1 Flatpak application?", "Update %1 Flatpak applications?", upgCount),
        i18n("Update All"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
    if (ret != KMessageBox::PrimaryAction)
        return;

    m_backend->updateAll();
}

// ---------------------------------------------------------------------------
// Store page slots
// ---------------------------------------------------------------------------

void FlatpakPage::onSearch()
{
    QString keyword = m_searchEdit->text().trimmed();
    if (keyword.isEmpty())
        return;

    showStoreListView();
    m_statusLabel->setText(i18n("Searching for '%1'...", keyword));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);
    m_backend->searchApps(keyword);
}

void FlatpakPage::onRemoteChanged()
{
    // Re-filter current store model by remote
    QString remote = m_remoteCombo->currentData().toString();
    if (remote.isEmpty())
        return;

    // If we have search results, re-filter them
    QList<FlatpakApp> all = m_storeModel->apps();
    if (all.isEmpty())
        return;

    QList<FlatpakApp> filtered;
    for (const auto &app : all) {
        if (app.remote == remote)
            filtered.append(app);
    }
    m_storeModel->setApps(filtered);
}

void FlatpakPage::onStoreAppClicked(const QModelIndex &index)
{
    FlatpakApp app = m_storeModel->appAt(index.row());
    if (app.appId.isEmpty())
        return;
    showStoreDetailView(app);
}

void FlatpakPage::showStoreDetailView(const FlatpakApp &app)
{
    m_currentDetailApp = app;

    // Icon placeholder with first letter
    m_detailIcon->setText(app.name.left(1).toUpper());

    m_detailName->setText(app.name.isEmpty() ? app.appId : app.name);
    m_detailAppId->setText(i18n("Application ID: %1", app.appId));
    m_detailVersion->setText(i18n("Version: %1", app.version));
    m_detailBranch->setText(i18n("Branch: %1", app.branch));
    m_detailRemote->setText(i18n("Remote: %1", app.remote));
    m_detailInstallType->setText(i18n("Installation: %1", app.installType));
    m_detailSize->setText(app.size > 0
                              ? i18n("Size: %1", FlatpakApp::formatSize(app.size))
                              : QString());

    QString desc = app.description;
    desc.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    m_detailDesc->setText(desc.isEmpty() ? i18n("<i>No description available.</i>") : desc);

    updateDetailButtons();

    m_storeStack->setCurrentIndex(1);
}

void FlatpakPage::showStoreListView()
{
    m_storeStack->setCurrentIndex(0);
}

void FlatpakPage::updateDetailButtons()
{
    const FlatpakApp &app = m_currentDetailApp;

    m_installBtn->setVisible(!app.installed);
    m_installBtn->setEnabled(!app.installed);

    m_uninstallBtn->setVisible(app.installed);
    m_uninstallBtn->setEnabled(app.installed);

    m_updateBtn->setVisible(app.installed);
    m_updateBtn->setEnabled(app.upgradable);

    m_runBtn->setVisible(app.installed);
    m_runBtn->setEnabled(app.installed);
}

void FlatpakPage::onInstallApp()
{
    if (m_currentDetailApp.appId.isEmpty())
        return;

    m_installBtn->setEnabled(false);
    m_installBtn->setText(i18n("Installing..."));
    m_progressBar->setRange(0, 100);
    m_progressBar->setVisible(true);

    m_backend->installApp(m_currentDetailApp.remote, m_currentDetailApp.appId);
}

void FlatpakPage::onUninstallApp()
{
    if (m_currentDetailApp.appId.isEmpty())
        return;

    auto ret = KMessageBox::warningTwoActions(this,
        i18n("Uninstall '%1' (%2)?", m_currentDetailApp.name, m_currentDetailApp.appId),
        i18n("Uninstall"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
    if (ret != KMessageBox::PrimaryAction)
        return;

    m_uninstallBtn->setEnabled(false);
    m_uninstallBtn->setText(i18n("Uninstalling..."));
    m_progressBar->setRange(0, 100);
    m_progressBar->setVisible(true);

    m_backend->uninstallApp(m_currentDetailApp.appId);
}

void FlatpakPage::onUpdateApp()
{
    if (m_currentDetailApp.appId.isEmpty())
        return;

    m_updateBtn->setEnabled(false);
    m_updateBtn->setText(i18n("Updating..."));
    m_progressBar->setRange(0, 100);
    m_progressBar->setVisible(true);

    m_backend->updateApp(m_currentDetailApp.appId);
}

void FlatpakPage::onRunApp()
{
    if (m_currentDetailApp.appId.isEmpty())
        return;
    m_backend->runApp(m_currentDetailApp.appId);
}

void FlatpakPage::onRefresh()
{
    reload();
}

// ---------------------------------------------------------------------------
// Data loading slots
// ---------------------------------------------------------------------------

void FlatpakPage::reload()
{
    m_statusLabel->setText(i18n("Loading installed Flatpak apps..."));
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);
    m_backend->loadInstalledApps();
    m_backend->loadRemotes();
}

void FlatpakPage::onInstalledAppsLoaded(const QList<FlatpakApp> &apps)
{
    m_progressBar->setVisible(false);
    m_allInstalledApps = apps;
    m_installedModel->setApps(apps);

    int upgCount = 0;
    for (const auto &app : apps) {
        if (app.upgradable)
            ++upgCount;
    }

    m_updateAllBtn->setEnabled(upgCount > 0);
    if (upgCount > 0)
        m_updateAllBtn->setText(i18n("Update All (%1)", upgCount));
    else
        m_updateAllBtn->setText(i18n("Update All"));

    if (upgCount > 0)
        m_statusLabel->setText(i18np("%1 app installed, %2 update available",
                                    "%1 apps installed, %2 updates available",
                                    apps.size(), upgCount));
    else
        m_statusLabel->setText(i18np("%1 app installed", "%1 apps installed", apps.size()));
}

void FlatpakPage::onRemotesLoaded(const QStringList &remotes)
{
    m_remotes = remotes;

    // Preserve selection
    QString currentRemote = m_remoteCombo->currentData().toString();

    m_remoteCombo->clear();
    m_remoteCombo->addItem(i18n("All Remotes"), QString());
    for (const auto &remote : remotes)
        m_remoteCombo->addItem(remote, remote);

    // Restore selection
    if (!currentRemote.isEmpty()) {
        int idx = m_remoteCombo->findData(currentRemote);
        if (idx >= 0)
            m_remoteCombo->setCurrentIndex(idx);
    }
}

void FlatpakPage::onSearchCompleted(const QList<FlatpakApp> &apps)
{
    m_progressBar->setVisible(false);

    // Filter by remote if one is selected
    QString remote = m_remoteCombo->currentData().toString();
    QList<FlatpakApp> filtered = apps;
    if (!remote.isEmpty()) {
        filtered.clear();
        for (const auto &app : apps) {
            if (app.remote == remote)
                filtered.append(app);
        }
    }

    m_storeModel->setApps(filtered);
    m_statusLabel->setText(i18np("%1 result", "%1 results", filtered.size()));
}

void FlatpakPage::onOperationStarted(const QString &appId, const QString &operation)
{
    Q_UNUSED(appId)
    m_statusLabel->setText(i18n("Operation: %1", operation));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);
}

void FlatpakPage::onOperationProgress(const QString &appId, const QString &message, int percent)
{
    Q_UNUSED(appId)
    if (!message.isEmpty())
        m_statusLabel->setText(message);
    if (percent >= 0)
        m_progressBar->setValue(percent);
    else if (m_progressBar->maximum() != 0)
        m_progressBar->setRange(0, 0); // indeterminate
}

void FlatpakPage::onOperationFinished(const QString &appId, const QString &operation,
                                      bool success, const QString &message)
{
    m_progressBar->setRange(0, 100);
    m_progressBar->setVisible(false);

    if (success) {
        m_statusLabel->setText(i18n("Operation completed successfully."));

        // Update the store model so the list reflects the new state
        if (operation == QStringLiteral("install"))
            m_storeModel->updateAppStatus(appId, true, false);
        else if (operation == QStringLiteral("uninstall"))
            m_storeModel->updateAppStatus(appId, false, false);
        else if (operation == QStringLiteral("update"))
            m_storeModel->updateAppStatus(appId, true, false);

        // Update detail buttons if this was the current app
        if (appId == m_currentDetailApp.appId) {
            if (operation == QStringLiteral("install")) {
                m_currentDetailApp.installed = true;
                m_installBtn->setText(i18n("Install"));
            } else if (operation == QStringLiteral("uninstall")) {
                m_currentDetailApp.installed = false;
                m_uninstallBtn->setText(i18n("Uninstall"));
            } else if (operation == QStringLiteral("update")) {
                m_currentDetailApp.upgradable = false;
                m_updateBtn->setText(i18n("Update"));
            }
            updateDetailButtons();
        }

        // Reload installed apps to reflect changes
        reload();
    } else {
        m_statusLabel->setText(i18n("Operation failed."));

        // Reset button texts
        m_installBtn->setText(i18n("Install"));
        m_installBtn->setEnabled(true);
        m_uninstallBtn->setText(i18n("Uninstall"));
        m_uninstallBtn->setEnabled(true);
        m_updateBtn->setText(i18n("Update"));
        m_updateBtn->setEnabled(true);

        KMessageBox::error(this, message, i18n("Flatpak Operation Error"));
    }
}

void FlatpakPage::onError(const QString &error)
{
    m_progressBar->setVisible(false);
    m_statusLabel->setText(i18n("Error"));
    KMessageBox::error(this, error, i18n("Flatpak Error"));
}

// ---------------------------------------------------------------------------
// Multi-select helpers and batch operations
// ---------------------------------------------------------------------------

QList<FlatpakApp> FlatpakPage::selectedStoreApps() const
{
    QList<FlatpakApp> apps;
    const QModelIndexList indexes = m_storeList->selectionModel()->selectedRows();
    for (const QModelIndex &index : indexes) {
        if (index.isValid())
            apps.append(m_storeModel->appAt(index.row()));
    }
    return apps;
}

QList<FlatpakApp> FlatpakPage::selectedInstalledApps() const
{
    QList<FlatpakApp> apps;
    const QModelIndexList indexes = m_installedList->selectionModel()->selectedRows();
    for (const QModelIndex &index : indexes) {
        if (index.isValid())
            apps.append(m_installedModel->appAt(index.row()));
    }
    return apps;
}

void FlatpakPage::onInstallSelected()
{
    QList<FlatpakApp> apps = selectedStoreApps();
    if (apps.isEmpty())
        return;

    auto ret = KMessageBox::warningTwoActions(this,
        i18np("Install %1 selected Flatpak application?", "Install %1 selected Flatpak applications?", apps.size()),
        i18n("Install Selected"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
    if (ret != KMessageBox::PrimaryAction)
        return;

    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);
    m_statusLabel->setText(i18np("Installing %1 application...", "Installing %1 applications...", apps.size()));

    for (const auto &app : apps) {
        if (!app.installed)
            m_backend->installApp(app.remote, app.appId);
    }
}

void FlatpakPage::onUninstallSelected()
{
    QList<FlatpakApp> apps = selectedInstalledApps();
    if (apps.isEmpty())
        return;

    auto ret = KMessageBox::warningTwoActions(this,
        i18np("Uninstall %1 selected Flatpak application?", "Uninstall %1 selected Flatpak applications?", apps.size()),
        i18n("Uninstall Selected"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
    if (ret != KMessageBox::PrimaryAction)
        return;

    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);
    m_statusLabel->setText(i18np("Uninstalling %1 application...", "Uninstalling %1 applications...", apps.size()));

    for (const auto &app : apps) {
        if (app.installed)
            m_backend->uninstallApp(app.appId);
    }
}

void FlatpakPage::onUpdateSelected()
{
    QList<FlatpakApp> apps = selectedInstalledApps();
    QList<FlatpakApp> toUpdate;
    for (const auto &app : apps) {
        if (app.upgradable)
            toUpdate.append(app);
    }

    if (toUpdate.isEmpty()) {
        KMessageBox::information(this, i18n("No upgradable applications selected."));
        return;
    }

    auto ret = KMessageBox::warningTwoActions(this,
        i18np("Update %1 selected Flatpak application?", "Update %1 selected Flatpak applications?", toUpdate.size()),
        i18n("Update Selected"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
    if (ret != KMessageBox::PrimaryAction)
        return;

    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(true);
    m_statusLabel->setText(i18np("Updating %1 application...", "Updating %1 applications...", toUpdate.size()));

    for (const auto &app : toUpdate) {
        m_backend->updateApp(app.appId);
    }
}

}
