#include "PackageView.h"
#include "../models/PackageModel.h"
#include "../core/Enums.h"

#include <QHeaderView>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QClipboard>
#include <KLocalizedString>

namespace Miryu {

class PackageItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        // Draw background
        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, opt.palette.highlight());
        } else if (opt.state & QStyle::State_MouseOver) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
        }

        bool queued = index.data(PackageModel::QueuedRole).toBool();
        bool installed = index.data(PackageModel::IsInstalledRole).toBool();
        bool isDep = index.data(PackageModel::IsDepRole).toBool();

        // Draw indicator
        QRect indicatorRect(opt.rect.x() + 4, opt.rect.y() + 4, 4, opt.rect.height() - 8);
        QColor indicatorColor;
        if (queued)
            indicatorColor = opt.palette.color(QPalette::Highlight);
        else if (installed)
            indicatorColor = QColor(46, 160, 67);  // green
        else
            indicatorColor = QColor(120, 120, 120); // gray

        painter->fillRect(indicatorRect, indicatorColor);

        // Draw name
        QRect nameRect = opt.rect.adjusted(16, 4, -opt.rect.width() / 3, -opt.rect.height() / 2);
        QFont nameFont = opt.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(opt.palette.color(QPalette::WindowText));
        QString name = index.data(PackageModel::NameRole).toString();
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name);

        // Draw version and arch
        QRect verRect = opt.rect.adjusted(16, opt.rect.height() / 2, -opt.rect.width() / 3, -4);
        QFont verFont = opt.font;
        verFont.setPointSize(verFont.pointSize() - 1);
        painter->setFont(verFont);
        painter->setPen(opt.palette.color(QPalette::Mid));
        QString verText = index.data(PackageModel::VersionRole).toString() +
                          QStringLiteral("-") + index.data(PackageModel::ReleaseRole).toString() +
                          QStringLiteral("  (") + index.data(PackageModel::ArchRole).toString() + QStringLiteral(")");
        painter->drawText(verRect, Qt::AlignLeft | Qt::AlignTop, verText);

        // Draw summary
        QRect summaryRect = opt.rect.adjusted(opt.rect.width() * 2 / 3, 4, -8, -4);
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(QPalette::WindowText));
        QString summary = index.data(PackageModel::SummaryRole).toString();
        painter->drawText(summaryRect, Qt::AlignRight | Qt::AlignVCenter, summary);

        // Draw queued indicator
        if (queued) {
            QRect queueRect(opt.rect.right() - 80, opt.rect.y(), 80, opt.rect.height());
            QString todoText = index.data(PackageModel::TodoTextRole).toString();
            painter->setPen(QColor(46, 160, 67));
            QFont qFont = opt.font;
            qFont.setBold(true);
            qFont.setPointSize(qFont.pointSize() - 1);
            painter->setFont(qFont);
            painter->drawText(queueRect, Qt::AlignRight | Qt::AlignVCenter, todoText);
        }

        if (isDep) {
            QRect depRect(opt.rect.right() - 80, opt.rect.y(), 80, opt.rect.height());
            painter->setPen(opt.palette.color(QPalette::Mid));
            QFont dFont = opt.font;
            dFont.setPointSize(dFont.pointSize() - 1);
            dFont.setItalic(true);
            painter->setFont(dFont);
            painter->drawText(depRect, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("(dep)"));
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        return QSize(300, 48);
    }
};

PackageView::PackageView(PackageModel *model, QWidget *parent)
    : QTreeView(parent)
    , m_model(model)
{
    setupView();
}

void PackageView::setupView()
{
    setModel(m_model);
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setHeaderHidden(true);
    setAlternatingRowColors(false);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setUniformRowHeights(true);
    setItemDelegate(new PackageItemDelegate(this));
    setSortingEnabled(true);
    sortByColumn(0, Qt::AscendingOrder);
    setAnimated(true);

    header()->setSectionResizeMode(0, QHeaderView::Stretch);
}

PackageModel* PackageView::model() const
{
    return m_model;
}

QList<Package> PackageView::selectedPackages() const
{
    QList<Package> packages;
    const QModelIndexList indexes = selectionModel()->selectedRows();
    for (const QModelIndex &index : indexes) {
        if (index.isValid())
            packages.append(m_model->packageAt(index.row()));
    }
    return packages;
}

void PackageView::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous)
    Q_EMIT packageSelected(current);
}

void PackageView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndexList selected = selectionModel()->selectedRows();

    // Ensure the clicked item is selected. If the user right-clicks on an
    // item that is not part of the current selection, switch to single
    // selection of that item.
    QModelIndex clickedIndex = indexAt(event->pos());
    if (clickedIndex.isValid() && !selected.contains(clickedIndex)) {
        selectionModel()->clear();
        selectionModel()->select(clickedIndex,
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        selected = selectionModel()->selectedRows();
    }

    if (selected.isEmpty())
        return;

    QMenu menu(this);

    if (selected.size() > 1) {
        // Multi-selection: show batch actions
        QList<Package> packages = selectedPackages();

        int installCount = 0;
        int updateCount = 0;
        int removeCount = 0;
        for (const auto &pkg : packages) {
            switch (pkg.todo) {
            case PackageTodo::Install: ++installCount; break;
            case PackageTodo::Update:  ++updateCount;  break;
            case PackageTodo::Remove:  ++removeCount;  break;
            default: break;
            }
        }

        if (installCount > 0) {
            auto *installAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("list-add")),
                i18n("Install Selected (%1)", installCount));
            connect(installAction, &QAction::triggered, this, [this, packages]() {
                QList<Package> toQueue;
                for (auto pkg : packages) {
                    if (pkg.todo == PackageTodo::Install && !pkg.queued)
                        toQueue.append(pkg);
                }
                queuePackages(toQueue);
            });
        }

        if (updateCount > 0) {
            auto *updateAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("system-software-update")),
                i18n("Update Selected (%1)", updateCount));
            connect(updateAction, &QAction::triggered, this, [this, packages]() {
                QList<Package> toQueue;
                for (auto pkg : packages) {
                    if (pkg.todo == PackageTodo::Update && !pkg.queued)
                        toQueue.append(pkg);
                }
                queuePackages(toQueue);
            });
        }

        if (removeCount > 0) {
            auto *removeAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("edit-delete")),
                i18n("Remove Selected (%1)", removeCount));
            connect(removeAction, &QAction::triggered, this, [this, packages]() {
                QList<Package> toQueue;
                for (auto pkg : packages) {
                    if (pkg.todo == PackageTodo::Remove && !pkg.queued)
                        toQueue.append(pkg);
                }
                queuePackages(toQueue);
            });
        }

        menu.addSeparator();

        auto *copyNames = menu.addAction(i18n("Copy Selected Package Names"));
        connect(copyNames, &QAction::triggered, this, [packages]() {
            QStringList names;
            for (const auto &pkg : packages)
                names.append(pkg.name);
            QApplication::clipboard()->setText(names.join(QStringLiteral("\n")));
        });

        menu.exec(event->globalPos());
        return;
    }

    // Single selection (the original behaviour)
    QModelIndex index = selected.first();
    if (!index.isValid())
        return;

    Package pkg = m_model->packageAt(index.row());

    if (pkg.queued) {
        auto *unqueueAction = menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")),
                                             i18n("Remove from Queue"));
        connect(unqueueAction, &QAction::triggered, this, [this, index]() { toggleQueue(index); });
    } else {
        auto *queueAction = menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                           pkg.todo == PackageTodo::Remove ? i18n("Queue for Removal") :
                                           pkg.todo == PackageTodo::Update ? i18n("Queue for Update") :
                                           i18n("Queue for Installation"));
        connect(queueAction, &QAction::triggered, this, [this, index]() { toggleQueue(index); });
    }

    menu.addSeparator();

    auto *copyName = menu.addAction(i18n("Copy Package Name"));
    connect(copyName, &QAction::triggered, this, [pkg]() {
        QApplication::clipboard()->setText(pkg.name);
    });

    menu.exec(event->globalPos());
}

void PackageView::queuePackages(const QList<Package> &packages)
{
    QList<Package> queued;
    for (auto pkg : packages) {
        pkg.queued = true;
        m_model->setQueued(pkg.nevra(), true);
        queued.append(pkg);
    }
    if (!queued.isEmpty())
        Q_EMIT packagesQueued(queued);
}

void PackageView::onQueueSelected()
{
    QList<Package> packages = selectedPackages();
    QList<Package> toQueue;
    for (auto pkg : packages) {
        if (!pkg.queued) {
            pkg.queued = true;
            m_model->setQueued(pkg.nevra(), true);
            toQueue.append(pkg);
        }
    }
    if (!toQueue.isEmpty())
        Q_EMIT packagesQueued(toQueue);
}

void PackageView::onUnqueueSelected()
{
    QList<Package> packages = selectedPackages();
    for (const auto &pkg : packages) {
        if (pkg.queued) {
            m_model->setQueued(pkg.nevra(), false);
            Q_EMIT unqueuePackage(pkg.nevra());
        }
    }
}

void PackageView::toggleQueue(const QModelIndex &index)
{
    Package pkg = m_model->packageAt(index.row());
    if (pkg.queued) {
        m_model->setQueued(pkg.nevra(), false);
        Q_EMIT unqueuePackage(pkg.nevra());
    } else {
        Package queuedPkg = pkg;
        queuedPkg.queued = true;
        m_model->setQueued(pkg.nevra(), true);
        Q_EMIT queuePackage(queuedPkg);
    }
}

}
