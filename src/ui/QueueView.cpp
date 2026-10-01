#include "QueueView.h"
#include "../models/QueueModel.h"
#include "../core/Package.h"
#include "../core/TodoColors.h"

#include <QHeaderView>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QFontMetrics>
#include <KLocalizedString>

namespace Miryu {

class QueueItemDelegate : public QStyledItemDelegate
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

        bool isDep = index.data(QueueModel::IsDepRole).toBool();
        QString todoText = index.data(QueueModel::TodoTextRole).toString();

        // Color indicator and badge by action type (Reinstall = blue,
        // Downgrade = yellow, Remove = red, Install = green, Update =
        // orange). Uses the typed TodoRole, not the localized text, so the
        // colors keep working in every language.
        int todo = index.data(QueueModel::TodoRole).toInt();
        QColor actionColor = todoColor(static_cast<PackageTodo>(todo));

        QRect indicatorRect(opt.rect.x() + 4, opt.rect.y() + 4, 4, opt.rect.height() - 8);
        painter->fillRect(indicatorRect, actionColor);

        // Name
        QRect nameRect = opt.rect.adjusted(16, 4, -120, -opt.rect.height() / 2);
        QFont nameFont = opt.font;
        nameFont.setBold(true);
        if (isDep)
            nameFont.setItalic(true);
        painter->setFont(nameFont);

        QString name = index.data(QueueModel::NameRole).toString();
        if (isDep) {
            // Show a "[依赖]" tag before the package name for dependency packages.
            QString depTag = QStringLiteral("[") + i18n("Dependencies") + QStringLiteral("] ");
            QFontMetrics fm(nameFont);
            int tagWidth = fm.horizontalAdvance(depTag);

            // Draw the dependency tag in a muted colour.
            painter->setPen(secondaryText);
            painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, depTag);

            // Draw the package name in the normal window text colour.
            QRect nameOnlyRect = nameRect.adjusted(tagWidth, 0, 0, 0);
            painter->setPen(opt.palette.color(QPalette::WindowText));
            painter->drawText(nameOnlyRect, Qt::AlignLeft | Qt::AlignVCenter, name);
        } else {
            painter->setPen(opt.palette.color(QPalette::WindowText));
            painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name);
        }

        // Version + release + arch
        QRect verRect = opt.rect.adjusted(16, opt.rect.height() / 2, -120, -4);
        QFont verFont = opt.font;
        verFont.setPointSize(verFont.pointSize() - 1);
        painter->setFont(verFont);
        painter->setPen(secondaryText);
        QString verText = index.data(QueueModel::VersionRole).toString() +
                          QStringLiteral("-") + index.data(QueueModel::ReleaseRole).toString() +
                          QStringLiteral("  (") + index.data(QueueModel::ArchRole).toString() + QStringLiteral(")");
        painter->drawText(verRect, Qt::AlignLeft | Qt::AlignTop, verText);

        // Action badge
        QRect actionRect = opt.rect.adjusted(opt.rect.width() - 110, 4, -8, -4);
        painter->setFont(verFont);
        painter->setPen(actionColor);
        painter->drawText(actionRect, Qt::AlignRight | Qt::AlignVCenter, todoText);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        return QSize(300, 44);
    }
};

QueueView::QueueView(QueueModel *model, QWidget *parent)
    : QTreeView(parent)
    , m_model(model)
{
    setModel(m_model);
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setHeaderHidden(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformRowHeights(true);
    setItemDelegate(new QueueItemDelegate(this));
    setAnimated(true);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
}

void QueueView::refresh()
{
    // Force viewport update
    viewport()->update();
}

QString QueueView::selectedNevra() const
{
    const QModelIndex idx = currentIndex();
    if (!idx.isValid())
        return {};
    return idx.data(QueueModel::NevraRole).toString();
}

void QueueView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndex index = indexAt(event->pos());
    if (!index.isValid())
        return;

    QString nevra = index.data(QueueModel::NevraRole).toString();

    QMenu menu(this);

    auto *reinstallAction = menu.addAction(QIcon::fromTheme(QStringLiteral("tools-wrench")),
                                           i18n("Reinstall"));
    connect(reinstallAction, &QAction::triggered, this, [this, nevra]() {
        m_model->updateTodo(nevra, PackageTodo::Reinstall);
        Q_EMIT todoChanged(nevra, PackageTodo::Reinstall);
        refresh();
    });

    auto *downgradeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-down")),
                                           i18n("Downgrade"));
    connect(downgradeAction, &QAction::triggered, this, [this, nevra]() {
        m_model->updateTodo(nevra, PackageTodo::Downgrade);
        Q_EMIT todoChanged(nevra, PackageTodo::Downgrade);
        refresh();
    });

    // "Remove" (uninstall) — change this queue entry's action to Remove, so
    // the package gets uninstalled when the transaction is applied. Distinct
    // from "Remove from Queue" below, which only drops the entry from the
    // queue without applying anything.
    auto *removePkgAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                           i18n("Remove"));
    connect(removePkgAction, &QAction::triggered, this, [this, nevra]() {
        m_model->updateTodo(nevra, PackageTodo::Remove);
        Q_EMIT todoChanged(nevra, PackageTodo::Remove);
        refresh();
    });

    menu.addSeparator();

    auto *removeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")),
                                        i18n("Remove from Queue"));
    connect(removeAction, &QAction::triggered, this, [this, nevra]() {
        m_model->removePackage(nevra);
        Q_EMIT packageRemoved(nevra);
    });

    menu.exec(event->globalPos());
}

}
