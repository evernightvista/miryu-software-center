#include "PackageView.h"
#include "../models/PackageModel.h"
#include "../core/Enums.h"
#include "../core/TodoColors.h"

#include <QHeaderView>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QStyleOptionButton>
#include <QMouseEvent>
#include <QClipboard>
#include <QFontMetrics>
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

        // Secondary text: same hue as the primary text, dimmed. QPalette::Mid is
        // a 3D/bevel shade that resolves to near-black in dark themes.
        QColor secondaryText = opt.palette.color(QPalette::WindowText);
        secondaryText.setAlpha(160);

        bool queued = index.data(PackageModel::QueuedRole).toBool();
        bool installed = index.data(PackageModel::IsInstalledRole).toBool();
        bool isDep = index.data(PackageModel::IsDepRole).toBool();
        int todo = index.data(PackageModel::TodoRole).toInt();

        // Draw the per-row checkbox. The PackageView shares the exact same
        // rectangle (PackageView::checkboxRect) for click handling, so
        // clicking anywhere inside this box toggles the queued state.
        {
            QStyleOptionButton cbOpt;
            cbOpt.state |= QStyle::State_Enabled;
            cbOpt.state |= queued ? QStyle::State_On : QStyle::State_Off;
            cbOpt.rect = PackageView::checkboxRectFor(opt.rect);
            QApplication::style()->drawControl(QStyle::CE_CheckBox, &cbOpt, painter, nullptr);
        }

        // The indicator (status / queued colour bar) lives just to the
        // right of the checkbox so the user can still tell installed /
        // available / queued apart at a glance. When the row is queued
        // the bar takes on the action colour (Remove = red, Install =
        // green, Update = orange, Reinstall = blue, Downgrade = yellow)
        // — matching the queue page — so a quick glance at the bar tells
        // the user *what* will happen to the package, not just that
        // something will. Previously the bar always turned the palette
        // Highlight colour (blue) when queued, which gave no clue about
        // the pending action and was actively misleading for removals
        // (a checked installed package looked "selected", not "to be
        // removed").
        QRect indicatorRect(opt.rect.x() + 28, opt.rect.y() + 4, 4, opt.rect.height() - 8);
        QColor indicatorColor;
        if (queued)
            indicatorColor = todoColor(static_cast<PackageTodo>(todo));
        else if (installed)
            indicatorColor = QColor(46, 160, 67);  // green
        else
            indicatorColor = QColor(120, 120, 120); // gray

        painter->fillRect(indicatorRect, indicatorColor);

        // Draw name
        QRect nameRect = opt.rect.adjusted(40, 4, -opt.rect.width() / 3, -opt.rect.height() / 2);
        QFont nameFont = opt.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(opt.palette.color(QPalette::WindowText));
        QString name = index.data(PackageModel::NameRole).toString();
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name);

        // Draw version and arch
        QRect verRect = opt.rect.adjusted(40, opt.rect.height() / 2, -opt.rect.width() / 3, -4);
        QFont verFont = opt.font;
        verFont.setPointSize(verFont.pointSize() - 1);
        painter->setFont(verFont);
        painter->setPen(secondaryText);
        QString verText = index.data(PackageModel::VersionRole).toString() +
                          QStringLiteral("-") + index.data(PackageModel::ReleaseRole).toString() +
                          QStringLiteral("  (") + index.data(PackageModel::ArchRole).toString() + QStringLiteral(")");
        painter->drawText(verRect, Qt::AlignLeft | Qt::AlignTop, verText);

        // Draw summary. Reserve the right edge for the queued todo marker
        // ("Install"/"Reinstall"/"Remove"/"Update"/"Downgrade") or the
        // "[dep]" tag: those markers are drawn afterwards (on top), so
        // without this reservation their text would visually overlap the
        // package description — the exact overlap reported in the bug.
        const int rightReserve = queued ? 100 : (isDep ? 80 : 8);
        QRect summaryRect = opt.rect.adjusted(opt.rect.width() * 2 / 3, 4, -rightReserve, -4);
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(QPalette::WindowText));
        QString summary = index.data(PackageModel::SummaryRole).toString();
        painter->drawText(summaryRect, Qt::AlignRight | Qt::AlignVCenter, summary);

        // Draw queued indicator. The marker text is colored by action type:
        // Reinstall = blue, Downgrade = yellow, Remove = red (Install =
        // green, Update = orange) — matching the queue page.
        if (queued) {
            QRect queueRect(opt.rect.right() - 100, opt.rect.y(), 100, opt.rect.height());
            QString todoText = index.data(PackageModel::TodoTextRole).toString();
            painter->setPen(todoColor(static_cast<PackageTodo>(todo)));
            QFont qFont = opt.font;
            qFont.setBold(true);
            qFont.setPointSize(qFont.pointSize() - 1);
            painter->setFont(qFont);
            painter->drawText(queueRect, Qt::AlignRight | Qt::AlignVCenter, todoText);
        }

        // Dependency marker. Localized via the existing "Dependencies" key
        // (already translated for QueueView / PackageInfoWidget) and drawn
        // in green so it stays readable in dark mode — the previous
        // dimmed-secondary-text approach was nearly invisible against dark
        // themes. Matches the colour used by QueueView and the
        // TransactionResultDialog for visual consistency.
        if (isDep && !queued) {
            QFont dFont = opt.font;
            dFont.setPointSize(dFont.pointSize() - 1);
            dFont.setItalic(true);
            QString depTag = QStringLiteral("[") + i18n("Dependencies") + QStringLiteral("]");
            QFontMetrics fm(dFont);
            int tagWidth = fm.horizontalAdvance(depTag) + 6;
            QRect depRect(opt.rect.right() - tagWidth - 4, opt.rect.y(), tagWidth + 4, opt.rect.height());
            painter->setFont(dFont);
            painter->setPen(QColor(46, 160, 67));  // green
            painter->drawText(depRect, Qt::AlignRight | Qt::AlignVCenter, depTag);
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

// A 16x16 box vertically centered inside the row rect, with a 6px left
// padding so it does not touch the viewport border. Both the delegate
// paint() and mousePressEvent() go through here, so the hit area always
// matches the painted checkbox exactly.
QRect PackageView::checkboxRectFor(const QRect &rowRect)
{
    return QRect(rowRect.x() + 6,
                 rowRect.y() + (rowRect.height() - 16) / 2,
                 16, 16);
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

void PackageView::mousePressEvent(QMouseEvent *event)
{
    // Intercept left-button presses that land on a row's checkbox area:
    // toggle that row's queue state instead of letting the base view treat
    // it as a normal selection / drag start. This keeps multi-selection
    // (Ctrl/Click, Shift+Click) working for the rest of the row while
    // making the checkbox a self-contained toggle.
    if (event->button() == Qt::LeftButton) {
        QModelIndex index = indexAt(event->pos());
        if (index.isValid()) {
            QRect rowRect = visualRect(index);
            if (checkboxRectFor(rowRect).contains(event->pos())) {
                toggleQueue(index);
                event->accept();
                return;
            }
        }
    }
    QTreeView::mousePressEvent(event);
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
        // Action set per package state. Installed packages get Reinstall and
        // Downgrade in addition to Removal, so the common maintenance
        // operations are reachable from the context menu (mirrors the
        // reference yumex-ng client). The menu pops up at the click position
        // inside the list column area, so the entries never overlap the
        // description column on the right.
        switch (pkg.state) {
        case PackageState::Installed: {
            auto *reinstallAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Queue for Reinstall"));
            connect(reinstallAction, &QAction::triggered, this, [this, index]() {
                queueWithTodo(index, PackageTodo::Reinstall);
            });

            auto *downgradeAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("arrow-down")), i18n("Queue for Downgrade"));
            connect(downgradeAction, &QAction::triggered, this, [this, index]() {
                queueWithTodo(index, PackageTodo::Downgrade);
            });

            auto *removeAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Queue for Removal"));
            connect(removeAction, &QAction::triggered, this, [this, index]() { toggleQueue(index); });
            break;
        }
        case PackageState::Available: {
            auto *queueAction = menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                               i18n("Queue for Installation"));
            connect(queueAction, &QAction::triggered, this, [this, index]() { toggleQueue(index); });
            break;
        }
        case PackageState::Update: {
            auto *queueAction = menu.addAction(QIcon::fromTheme(QStringLiteral("system-software-update")),
                                               i18n("Queue for Update"));
            connect(queueAction, &QAction::triggered, this, [this, index]() { toggleQueue(index); });
            break;
        }
        case PackageState::Downgrade: {
            auto *queueAction = menu.addAction(QIcon::fromTheme(QStringLiteral("arrow-down")),
                                               i18n("Queue for Downgrade"));
            connect(queueAction, &QAction::triggered, this, [this, index]() { toggleQueue(index); });
            break;
        }
        }
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

void PackageView::queueWithTodo(const QModelIndex &index, PackageTodo todo)
{
    Package pkg = m_model->packageAt(index.row());
    if (pkg.queued)
        return; // already queued — do not duplicate

    Package queuedPkg = pkg;
    queuedPkg.todo = todo;
    queuedPkg.queued = true;
    // Also update the model's todo so the in-row marker shows the real
    // action ("Reinstall"/"Downgrade"), not the state-derived default.
    m_model->setQueuedWithTodo(pkg.nevra(), true, todo);
    Q_EMIT queuePackage(queuedPkg);
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
