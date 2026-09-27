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
                if (m_backend->client()->repoEnable(repoId, false)) {
                    m_backend->client()->readAllRepos();
                    m_backend->loadRepositories();
                    Q_EMIT repositoriesChanged();
                } else {
                    KMessageBox::error(this,
                        m_backend->client()->lastError().isEmpty()
                            ? i18n("Failed to disable repository '%1'.", repoId)
                            : m_backend->client()->lastError(),
                        i18n("Error"));
                }
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
                if (m_backend->client()->repoEnable(repoId, true)) {
                    m_backend->client()->readAllRepos();
                    m_backend->loadRepositories();
                    Q_EMIT repositoriesChanged();
                } else {
                    KMessageBox::error(this,
                        m_backend->client()->lastError().isEmpty()
                            ? i18n("Failed to enable repository '%1'.", repoId)
                            : m_backend->client()->lastError(),
                        i18n("Error"));
                }
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
