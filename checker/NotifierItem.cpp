#include "NotifierItem.h"

#include <QIcon>
#include <QMenu>
#include <QTimer>
#include <QUrl>

#include <KLocalizedString>
#include <KIO/CommandLauncherJob>
#include <KNotificationJobUiDelegate>
#include <KStatusNotifierItem>

namespace Miryu {

NotifierItem::NotifierItem(const QString &message, QObject *parent)
    : QObject(parent)
{
    m_item = new KStatusNotifierItem(QStringLiteral("miryu-software-check"), this);
    m_item->setTitle(i18n("Miryu Software Center"));
    m_item->setIconByName(QStringLiteral("update-low"));
    m_item->setToolTipTitle(i18n("Updates Available"));
    m_item->setToolTipSubTitle(message);
    m_item->setStatus(KStatusNotifierItem::Active);

    // Left click: open Miryu Software Center on the Updates page (same as
    // the notification's "View Updates" action) — no auto-queueing.
    connect(m_item, &KStatusNotifierItem::activateRequested, this, [this]() {
        launchSoftwareCenter(m_item->providedToken(), LaunchMode::ShowUpdates);
        QTimer::singleShot(500, this, [this]() {
            Q_EMIT quitRequested();
        });
    });

    auto *menu = new QMenu;
    connect(m_item, &QObject::destroyed, menu, &QObject::deleteLater);

    auto *openAction = menu->addAction(QIcon::fromTheme(QStringLiteral("miryu-package-manager")),
                                       i18n("Open Miryu Software Center"));
    connect(openAction, &QAction::triggered, this, [this]() {
        launchSoftwareCenter(m_item->providedToken(), LaunchMode::Open);
        QTimer::singleShot(500, this, [this]() {
            Q_EMIT quitRequested();
        });
    });

    auto *startAction = menu->addAction(QIcon::fromTheme(QStringLiteral("system-software-update")),
                                        i18n("Start Updates"));
    connect(startAction, &QAction::triggered, this, [this]() {
        launchSoftwareCenter(m_item->providedToken(), LaunchMode::StartUpdates);
        QTimer::singleShot(500, this, [this]() {
            Q_EMIT quitRequested();
        });
    });

    // Disable KStatusNotifierItem's built-in standard actions (Quit): their
    // labels come from the framework's own translation catalog, which may be
    // missing on the user's system (shows plain English). Ship our own
    // translated "Quit" action instead and quit through the notifier's
    // normal finish path.
    m_item->setStandardActionsEnabled(false);

    menu->addSeparator();
    auto *quitAction = menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                                       i18n("Quit"));
    connect(quitAction, &QAction::triggered, this, &NotifierItem::quitRequested);

    m_item->setContextMenu(menu);
}

NotifierItem::~NotifierItem()
{
    delete m_item;
}

void NotifierItem::launchSoftwareCenter(const QString &xdgActivationToken, LaunchMode mode)
{
    // Launch Miryu Software Center, forwarding the XDG activation token so
    // the window is raised/focused (same pattern as Discover's
    // DiscoverNotifier::showDiscover()). Depending on the mode the center is
    // passed --show-updates (open the Updates page only) or --update (open
    // the Updates page and start the update flow). CommandLauncherJob (not
    // ApplicationLauncherJob) because the center needs a command-line
    // OPTION, not file URLs; if the center is already running its
    // KDBusService::Unique activation handler receives the arguments and
    // reacts accordingly.
    QStringList arguments;
    if (mode == LaunchMode::StartUpdates)
        arguments.append(QStringLiteral("--update"));
    else if (mode == LaunchMode::ShowUpdates)
        arguments.append(QStringLiteral("--show-updates"));

    auto *job = new KIO::CommandLauncherJob(QStringLiteral("miryu-software-center"), arguments);
    job->setDesktopName(QStringLiteral("org.miryugaming.PackageManager"));
    job->setStartupId(xdgActivationToken.toUtf8());
    job->setUiDelegate(new KNotificationJobUiDelegate(KJobUiDelegate::AutoErrorHandlingEnabled));
    job->start();
}

}
