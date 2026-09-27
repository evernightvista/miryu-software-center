#pragma once

#include <QWidget>
#include "../core/FlatpakBackend.h"
#include "../models/FlatpakAppModel.h"
#include "../core/FlatpakApp.h"

class QListView;
class QListWidget;
class QComboBox;
class QLabel;
class QProgressBar;
class QStackedWidget;
class QPushButton;
class KLineEdit;

namespace Miryu {

/*!
 * \class FlatpakPage
 * \brief Combined Flatpak management page with a sidebar.
 *
 * The sidebar provides two tabs:
 *  - "Flatpak 应用" — installed applications list with context-menu
 *    actions (Update, Uninstall, Run) and an "Update All" button.
 *  - "Flatpak 商店" — search the flatpak store, with a remote filter,
 *    a custom list delegate, and a detail view that offers
 *    install / uninstall / update / run buttons.
 *
 * A progress bar and status label at the bottom are shared between both
 * tabs.
 */
class FlatpakPage : public QWidget
{
    Q_OBJECT

public:
    explicit FlatpakPage(FlatpakBackend *backend, QWidget *parent = nullptr);

    void reload();

private Q_SLOTS:
    void onSidebarChanged(int index);
    void onSearch();
    void onFilterInstalled();
    void onRemoteChanged();
    void onInstalledAppClicked(const QModelIndex &index);
    void onStoreAppClicked(const QModelIndex &index);
    void onInstallApp();
    void onUninstallApp();
    void onUpdateApp();
    void onRunApp();
    void onUpdateAll();
    void onRefresh();

    void onInstallSelected();
    void onUninstallSelected();
    void onUpdateSelected();

    void onInstalledAppsLoaded(const QList<FlatpakApp> &apps);
    void onRemotesLoaded(const QStringList &remotes);
    void onSearchCompleted(const QList<FlatpakApp> &apps);
    void onOperationStarted(const QString &appId, const QString &operation);
    void onOperationProgress(const QString &appId, const QString &message, int percent);
    void onOperationFinished(const QString &appId, const QString &operation, bool success, const QString &message);
    void onError(const QString &error);

private:
    void setupUI();
    void setupInstalledPage(QWidget *page);
    void setupStorePage(QWidget *page);
    void setupDetailView();
    void showContextMenu(const QPoint &pos);
    void showStoreDetailView(const FlatpakApp &app);
    void showStoreListView();
    void updateDetailButtons();

    QList<FlatpakApp> selectedStoreApps() const;
    QList<FlatpakApp> selectedInstalledApps() const;

    FlatpakBackend *m_backend;
    FlatpakAppModel *m_installedModel;
    FlatpakAppModel *m_storeModel;

    // Sidebar
    QListWidget *m_sidebar;
    QStackedWidget *m_pageStack;

    // Installed page
    QWidget *m_installedPage;
    KLineEdit *m_filterEdit;
    QListView *m_installedList;
    QPushButton *m_updateAllBtn;
    QPushButton *m_uninstallSelectedBtn;
    QPushButton *m_updateSelectedBtn;

    // Store page
    QWidget *m_storePage;
    KLineEdit *m_searchEdit;
    QComboBox *m_remoteCombo;
    QListView *m_storeList;
    QStackedWidget *m_storeStack;
    QPushButton *m_installSelectedBtn;

    // Detail view (within store page)
    QWidget *m_detailWidget;
    QLabel *m_detailIcon;
    QLabel *m_detailName;
    QLabel *m_detailAppId;
    QLabel *m_detailVersion;
    QLabel *m_detailBranch;
    QLabel *m_detailRemote;
    QLabel *m_detailInstallType;
    QLabel *m_detailSize;
    QLabel *m_detailDesc;
    QPushButton *m_installBtn;
    QPushButton *m_uninstallBtn;
    QPushButton *m_updateBtn;
    QPushButton *m_runBtn;
    QPushButton *m_backBtn;

    // Shared
    QProgressBar *m_progressBar;
    QLabel *m_statusLabel;

    // State
    QList<FlatpakApp> m_allInstalledApps;
    QStringList m_remotes;
    FlatpakApp m_currentDetailApp;
};

}
