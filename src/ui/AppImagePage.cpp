#include "AppImagePage.h"

#include <KGuiItem>
#include <KLineEdit>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardGuiItem>

#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QVariant>

#include <utility>

namespace Miryu {

namespace {
constexpr int AppImagePathRole = Qt::UserRole + 1;

class AppImageDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        const bool selected = opt.state & QStyle::State_Selected;
        if (selected)
            painter->fillRect(opt.rect, opt.palette.highlight());
        else if (opt.state & QStyle::State_MouseOver)
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));

        QColor primary = opt.palette.color(selected ? QPalette::HighlightedText
                                                    : QPalette::WindowText);
        QColor secondary = primary;
        secondary.setAlpha(160);

        // Icon (40x40), with a first-letter placeholder when no icon file or
        // theme entry could be resolved.
        const QRect iconRect(opt.rect.x() + 8, opt.rect.y() + 8, 40, 40);
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QPixmap pixmap = icon.pixmap(iconRect.size(), QIcon::Normal, QIcon::Off);
        if (!pixmap.isNull()) {
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter->drawPixmap(iconRect, pixmap);
        } else {
            painter->setBrush(QColor(100, 180, 255));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(iconRect, 6, 6);
            painter->setPen(Qt::white);
            QFont iconFont = opt.font;
            iconFont.setBold(true);
            iconFont.setPointSize(14);
            painter->setFont(iconFont);
            painter->drawText(iconRect, Qt::AlignCenter,
                              index.data(Qt::DisplayRole).toString().left(1).toUpper());
        }

        // Application name
        const QRect nameRect = opt.rect.adjusted(56, 6, -12, -opt.rect.height() / 2);
        QFont nameFont = opt.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(primary);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(),
                                                            Qt::ElideRight, nameRect.width()));

        // AppImage location
        const QRect pathRect = opt.rect.adjusted(56, opt.rect.height() / 2, -12, -4);
        QFont pathFont = opt.font;
        pathFont.setPointSize(pathFont.pointSize() - 1);
        painter->setFont(pathFont);
        painter->setPen(secondary);
        painter->drawText(pathRect, Qt::AlignLeft | Qt::AlignTop,
                          painter->fontMetrics().elidedText(index.data(AppImagePathRole).toString(),
                                                            Qt::ElideMiddle, pathRect.width()));

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        return QSize(400, 56);
    }
};

} // namespace

AppImagePage::AppImagePage(QWidget *parent)
    : QWidget(parent)
    , m_manager(new AppImageManager(this))
{
    setupUI();
    connect(m_manager, &AppImageManager::uninstallFinished,
            this, &AppImagePage::onUninstallFinished);
}

void AppImagePage::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *toolbar = new QWidget;
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(8, 8, 8, 4);
    toolbarLayout->setSpacing(6);

    m_searchEdit = new KLineEdit;
    m_searchEdit->setPlaceholderText(i18n("Search integrated AppImages..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setTrapReturnKey(true);
    connect(m_searchEdit, &KLineEdit::textChanged,
            this, &AppImagePage::onFilterChanged);

    m_runButton = new QPushButton(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                                  i18n("Run"));
    m_runButton->setToolTip(i18n("Launch the selected AppImage"));
    m_runButton->setEnabled(false);
    connect(m_runButton, &QPushButton::clicked, this, &AppImagePage::onRun);

    m_uninstallButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                        i18n("Uninstall"));
    m_uninstallButton->setToolTip(i18n("Remove the selected AppImage integration"));
    m_uninstallButton->setEnabled(false);
    connect(m_uninstallButton, &QPushButton::clicked, this, &AppImagePage::onUninstall);

    toolbarLayout->addWidget(m_searchEdit, 1);
    toolbarLayout->addWidget(m_runButton);
    toolbarLayout->addWidget(m_uninstallButton);

    layout->addWidget(toolbar);

    m_model = new QStandardItemModel(this);
    m_listView = new QListView;
    m_listView->setModel(m_model);
    m_listView->setItemDelegate(new AppImageDelegate(this));
    m_listView->setUniformItemSizes(true);
    m_listView->setSpacing(1);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_listView, &QListView::activated, this, &AppImagePage::onRun);
    connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &AppImagePage::updateButtons);
    layout->addWidget(m_listView, 1);

    m_statusLabel = new QLabel;
    m_statusLabel->setContentsMargins(8, 4, 8, 4);
    layout->addWidget(m_statusLabel);
}

void AppImagePage::reload()
{
    m_apps = m_manager->scan();
    populate();
}

void AppImagePage::onFilterChanged(const QString &text)
{
    Q_UNUSED(text)
    populate();
}

void AppImagePage::populate()
{
    m_model->clear();

    const QString filter = m_searchEdit->text().trimmed();
    for (const IntegratedAppImage &app : std::as_const(m_apps)) {
        if (!filter.isEmpty()
            && !app.name.contains(filter, Qt::CaseInsensitive)
            && !app.appImagePath.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }

        auto *item = new QStandardItem(app.name);
        item->setEditable(false);
        item->setData(app.appImagePath, AppImagePathRole);
        item->setToolTip(app.appImagePath);

        QIcon icon;
        if (!app.iconPath.isEmpty())
            icon = QIcon(app.iconPath);
        if (icon.isNull() && !app.iconName.isEmpty())
            icon = QIcon::fromTheme(app.iconName);
        item->setIcon(icon);

        m_model->appendRow(item);
    }

    if (m_apps.isEmpty()) {
        m_statusLabel->setText(
            i18n("No integrated AppImages were found. Integrate AppImages with Miryu App Launcher first."));
    } else if (m_model->rowCount() == 0) {
        m_statusLabel->setText(i18n("No AppImages match the search."));
    } else {
        m_statusLabel->setText(
            i18np("%1 integrated AppImage", "%1 integrated AppImages", m_model->rowCount()));
    }

    updateButtons();
}

IntegratedAppImage AppImagePage::selectedApp() const
{
    const QModelIndexList selected = m_listView->selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return {};
    const QString path = selected.first().data(AppImagePathRole).toString();
    for (const IntegratedAppImage &app : m_apps) {
        if (app.appImagePath == path)
            return app;
    }
    return {};
}

void AppImagePage::updateButtons()
{
    const bool hasSelection = m_listView->selectionModel()->hasSelection();
    m_runButton->setEnabled(hasSelection && !m_uninstalling);
    m_uninstallButton->setEnabled(hasSelection && !m_uninstalling);
}

void AppImagePage::onRun()
{
    if (m_uninstalling)
        return;

    const IntegratedAppImage app = selectedApp();
    if (app.appImagePath.isEmpty())
        return;

    QString error;
    if (!m_manager->run(app, &error))
        KMessageBox::error(this, error, i18n("Run AppImage"));
    else
        m_statusLabel->setText(i18n("Started \"%1\".", app.name));
}

void AppImagePage::onUninstall()
{
    if (m_uninstalling)
        return;

    const IntegratedAppImage app = selectedApp();
    if (app.appImagePath.isEmpty())
        return;

    const auto answer = KMessageBox::warningTwoActions(
        this,
        i18n("Remove the desktop integration for \"%1\" and delete the AppImage file?", app.name),
        i18n("Uninstall AppImage"),
        KGuiItem(i18n("Uninstall"), QStringLiteral("edit-delete")),
        KStandardGuiItem::cancel());
    if (answer != KMessageBox::PrimaryAction)
        return;

    m_uninstalling = true;
    m_statusLabel->setText(i18n("Removing \"%1\"...", app.name));
    updateButtons();
    m_manager->uninstall(app);
}

void AppImagePage::onUninstallFinished(bool success, const QString &message)
{
    m_uninstalling = false;
    reload();

    if (!success)
        KMessageBox::error(this, message, i18n("Uninstall AppImage"));
    else
        m_statusLabel->setText(message);
}

} // namespace Miryu
