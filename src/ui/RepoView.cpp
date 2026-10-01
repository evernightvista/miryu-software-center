#include "RepoView.h"
#include "../models/RepoModel.h"
#include "../core/Backend.h"
#include "../core/Repository.h"

#include <QHeaderView>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <KLocalizedString>
#include <KMessageBox>
#include <KGuiItem>

namespace Miryu {

class RepoItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        if (opt.state & QStyle::State_Selected)
            painter->fillRect(opt.rect, opt.palette.highlight());
        else if (opt.state & QStyle::State_MouseOver)
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));

        // Secondary text: same hue as the primary text, dimmed. QPalette::Mid is
        // a 3D/bevel shade that resolves to near-black in dark themes.
        QColor secondaryText = opt.palette.color(QPalette::WindowText);
        secondaryText.setAlpha(160);

        bool enabled = index.data(RepoModel::EnabledRole).toBool();
        QString id = index.data(RepoModel::IdRole).toString();
        QString name = index.data(RepoModel::NameRole).toString();
        int priority = index.data(RepoModel::PriorityRole).toInt();

        // Status indicator
        QRect indicatorRect(opt.rect.x() + 4, opt.rect.y() + 8, 4, opt.rect.height() - 16);
        QColor indicatorColor = enabled ? QColor(46, 160, 67) : QColor(120, 120, 120);
        painter->fillRect(indicatorRect, indicatorColor);

        // Name
        QRect nameRect = opt.rect.adjusted(16, 4, -200, -opt.rect.height() / 2);
        QFont nameFont = opt.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(opt.palette.color(QPalette::WindowText));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name);

        // ID
        QRect idRect = opt.rect.adjusted(16, opt.rect.height() / 2, -200, -4);
        QFont idFont = opt.font;
        idFont.setPointSize(idFont.pointSize() - 1);
        painter->setFont(idFont);
        painter->setPen(secondaryText);
        painter->drawText(idRect, Qt::AlignLeft | Qt::AlignTop, id);

        // Priority
        QRect prioRect = opt.rect.adjusted(opt.rect.width() - 180, 0, -100, 0);
        painter->setPen(secondaryText);
        painter->drawText(prioRect, Qt::AlignRight | Qt::AlignVCenter,
                          priority == 99 ? i18n("Default") : i18n("Priority: %1", priority));

        // Status
        QRect statusRect = opt.rect.adjusted(opt.rect.width() - 90, 0, -8, 0);
        painter->setPen(enabled ? QColor(46, 160, 67) : QColor(120, 120, 120));
        painter->drawText(statusRect, Qt::AlignRight | Qt::AlignVCenter,
                          enabled ? i18n("Enabled") : i18n("Disabled"));

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        return QSize(400, 48);
    }
};

RepoView::RepoView(RepoModel *model, Backend *backend, QWidget *parent)
    : QTreeView(parent)
    , m_model(model)
    , m_backend(backend)
{
    setModel(m_model);
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setHeaderHidden(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformRowHeights(true);
    setItemDelegate(new RepoItemDelegate(this));
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
}

// Run the polkit-gated repoEnable() call on a worker thread so the UI stays
// responsive while the user authenticates. On completion the UI thread
// applies the result and shows at most one dialog.
static void runRepoEnableAsync(RepoView *view, Backend *backend,
                               const QString &repoId, bool enable)
{
    auto *watcher = new QFutureWatcher<bool>(view);
    QObject::connect(watcher, &QFutureWatcher<bool>::finished, view, [view, backend, repoId, enable, watcher]() {
        const bool ok = watcher->result();
        watcher->deleteLater();
        if (ok) {
            // Do NOT refresh the software source here: per the desired
            // behaviour repository metadata is refreshed only at
            // application startup and when the user applies an upgrade
            // queue. repoEnable() already wrote the new enabled state
            // to the repo config; resetSession() drops the daemon's
            // stale in-memory sack so the subsequent repoList()
            // re-reads the updated configuration. The next package
            // query reloads metadata from disk on demand instead of
            // forcing an immediate (and possibly slow) re-download.
            backend->client()->resetSession();
            backend->loadRepositories();
            Q_EMIT view->repositoriesChanged();
        } else {
            const QString err = backend->client()->lastError();
            const QString errLower = err.toLower();
            // Polkit auth cancellation: show a friendly message instead
            // of the raw daemon error. errorOccurred() is already
            // suppressed for auth errors in Dnf5DaemonClient, so this
            // is the only dialog the user sees.
            if (errLower.contains(QStringLiteral("not authorized")) ||
                errLower.contains(QStringLiteral("cancel")) ||
                errLower.contains(QStringLiteral("auth"))) {
                KMessageBox::information(view,
                    i18n("Operation cancelled by user."),
                    i18n("Repository"));
            } else {
                KMessageBox::error(view,
                    err.isEmpty()
                        ? (enable
                            ? i18n("Failed to enable repository '%1'.", repoId)
                            : i18n("Failed to disable repository '%1'.", repoId))
                        : err,
                    i18n("Error"));
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([backend, repoId, enable]() {
        return backend->client()->repoEnable(repoId, enable);
    }));
}

void RepoView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndex index = indexAt(event->pos());
    if (!index.isValid())
        return;

    QString repoId = index.data(RepoModel::IdRole).toString();
    bool enabled = index.data(RepoModel::EnabledRole).toBool();

    QMenu menu(this);

    if (enabled) {
        auto *disableAction = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-cancel")),
                                             i18n("Disable Repository"));
        connect(disableAction, &QAction::triggered, this, [this, repoId]() {
            auto ret = KMessageBox::warningTwoActions(this,
                i18n("Disable repository '%1'?", repoId),
                i18n("Disable Repository"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
            if (ret == KMessageBox::PrimaryAction) {
                // Disable the repository through dnf5daemon. The D-Bus call
                // triggers the daemon's own polkit policy
                // (org.rpm.dnf.v0.rpm.Repo.conf_write) automatically.
                // Runs off the UI thread so the window stays responsive
                // during polkit authentication.
                runRepoEnableAsync(this, m_backend, repoId, false);
            }
        });
    } else {
        auto *enableAction = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")),
                                            i18n("Enable Repository"));
        connect(enableAction, &QAction::triggered, this, [this, repoId]() {
            auto ret = KMessageBox::warningTwoActions(this,
                i18n("Enable repository '%1'?", repoId),
                i18n("Enable Repository"), KGuiItem(i18n("Yes")), KGuiItem(i18n("No")));
            if (ret == KMessageBox::PrimaryAction) {
                // Enable the repository through dnf5daemon. The D-Bus call
                // triggers the daemon's own polkit policy
                // (org.rpm.dnf.v0.rpm.Repo.conf_write) automatically.
                // Runs off the UI thread so the window stays responsive
                // during polkit authentication.
                runRepoEnableAsync(this, m_backend, repoId, true);
            }
        });
    }

    menu.addSeparator();
    auto *refreshAction = menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                         i18n("Refresh Metadata"));
    connect(refreshAction, &QAction::triggered, this, [this]() {
        m_backend->client()->cleanCache(QStringLiteral("expire-cache"));
        m_backend->client()->readAllRepos();
        m_backend->loadRepositories();
    });

    menu.exec(event->globalPos());
}

}
