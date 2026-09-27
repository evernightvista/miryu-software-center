#include "TransactionResultDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <KLocalizedString>
#include <KMessageBox>

namespace Miryu {

static QString formatSize(qint64 bytes)
{
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    if (bytes < 1024 * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + QStringLiteral(" MB");
    return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + QStringLiteral(" GB");
}

TransactionResultDialog::TransactionResultDialog(const TransactionResult &result, QWidget *parent)
    : QDialog(parent)
{
    setupUI(result);
}

void TransactionResultDialog::setupUI(const TransactionResult &result)
{
    setWindowTitle(i18n("Transaction Summary"));
    setMinimumSize(600, 400);

    auto *layout = new QVBoxLayout(this);

    // Description
    auto *descLabel = new QLabel(i18n("The following operations will be performed:"));
    layout->addWidget(descLabel);

    // Warning for problems
    if (!result.problems.isEmpty()) {
        auto *warnLabel = new QLabel(QStringLiteral("<b>%1:</b><br>%2").arg(
            i18n("Warnings"), result.problems.join(QStringLiteral("<br>"))));
        warnLabel->setStyleSheet(QStringLiteral("color: #cc6600; background: #fff3e0; padding: 8px; border-radius: 4px;"));
        warnLabel->setWordWrap(true);
        layout->addWidget(warnLabel);
    }

    // Tree widget
    m_treeWidget = new QTreeWidget;
    m_treeWidget->setColumnCount(3);
    m_treeWidget->setHeaderLabels({i18n("Package"), i18n("Repository"), i18n("Size")});
    m_treeWidget->setRootIsDecorated(true);
    m_treeWidget->setAlternatingRowColors(true);
    m_treeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_treeWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_treeWidget->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    static const QHash<QString, QString> actionLabels = {
        {QStringLiteral("install"),   i18n("Install")},
        {QStringLiteral("upgrade"),   i18n("Upgrade")},
        {QStringLiteral("downgrade"), i18n("Downgrade")},
        {QStringLiteral("reinstall"), i18n("Reinstall")},
        {QStringLiteral("remove"),    i18n("Remove")},
        {QStringLiteral("replaced"),  i18n("Replace")},
        {QStringLiteral("obsoleted"), i18n("Obsolete")},
    };

    qint64 totalSize = 0;

    for (const auto &action : result.actionKeys()) {
        auto items = result.itemsByAction(action);
        if (items.isEmpty())
            continue;

        QString actionLabel = actionLabels.value(action, action);
        QColor actionColor;
        if (action == QStringLiteral("install") || action == QStringLiteral("upgrade"))
            actionColor = QColor(46, 160, 67);
        else if (action == QStringLiteral("remove") || action == QStringLiteral("obsoleted"))
            actionColor = QColor(192, 28, 40);
        else
            actionColor = QColor(0, 114, 178);

        auto *groupItem = new QTreeWidgetItem(m_treeWidget);
        groupItem->setText(0, QStringLiteral("%1 (%2)").arg(actionLabel).arg(items.size()));
        QFont groupFont = groupItem->font(0);
        groupFont.setBold(true);
        groupItem->setFont(0, groupFont);
        groupItem->setForeground(0, actionColor);

        qint64 groupSize = 0;
        for (const auto &item : items) {
            auto *child = new QTreeWidgetItem(groupItem);
            child->setText(0, item.nevra);
            child->setText(1, item.repo);
            child->setText(2, formatSize(item.size));
            child->setTextAlignment(2, Qt::AlignRight);
            groupSize += item.size;
        }

        groupItem->setText(2, formatSize(groupSize));
        groupItem->setTextAlignment(2, Qt::AlignRight);
        groupItem->setExpanded(true);

        totalSize += groupSize;
    }

    layout->addWidget(m_treeWidget, 1);

    // Total
    m_totalLabel = new QLabel(QStringLiteral("<b>%1: %2</b>").arg(i18n("Total Download Size"), formatSize(totalSize)));
    layout->addWidget(m_totalLabel);

    // Offline checkbox
    m_offlineCheck = new QCheckBox(i18n("Apply offline (on next reboot)"));
    layout->addWidget(m_offlineCheck);

    // Buttons
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->button(QDialogButtonBox::Ok)->setText(i18n("Apply"));
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);
}

bool TransactionResultDialog::isOffline() const
{
    return m_offlineCheck->isChecked();
}

}
