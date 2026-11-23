#pragma once

#include <QWidget>

#include "../core/AppImageManager.h"

class QListView;
class QStandardItemModel;
class QLabel;
class QPushButton;
class KLineEdit;

namespace Miryu {

/*!
 * \class AppImagePage
 * \brief Manages the AppImages integrated by Miryu App Launcher.
 *
 * The page lists every integrated AppImage (icon + name) and offers a
 * search box, a Run button and an Uninstall button. The AppImage locations
 * are read from the launcher's desktop registrations / integration
 * directory, so no separate bookkeeping is required.
 */
class AppImagePage : public QWidget
{
    Q_OBJECT

public:
    explicit AppImagePage(QWidget *parent = nullptr);

    void reload();

private Q_SLOTS:
    void onFilterChanged(const QString &text);
    void onRun();
    void onUninstall();
    void onUninstallFinished(bool success, const QString &message);
    void updateButtons();

private:
    void setupUI();
    void populate();
    IntegratedAppImage selectedApp() const;

    AppImageManager *m_manager;
    QList<IntegratedAppImage> m_apps;

    KLineEdit *m_searchEdit;
    QPushButton *m_runButton;
    QPushButton *m_uninstallButton;
    QListView *m_listView;
    QStandardItemModel *m_model;
    QLabel *m_statusLabel;

    bool m_uninstalling = false;
};

}
