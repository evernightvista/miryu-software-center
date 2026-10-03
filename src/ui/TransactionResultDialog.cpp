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
#include <QFont>
#include <QPalette>
#include <QColor>
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

// Column indices of the transaction tree widget. Keeping these as a
// named enum instead of bare integers makes the ( Package | Repository |
// Download Size | Install Size ) layout self-documenting and resistant to
// future column-order regressions. For removal-only transactions the two
// size columns collapse into a single "Freed Space" column (ColFreedSpace)
// at index 2 — install/upgrade transactions keep both size columns at
// indices 2 and 3.
enum TransactionColumn {
    ColPackage = 0,
    ColRepository = 1,
    ColDownloadSize = 2,
    ColInstallSize = 3,
    ColFreedSpace = 2,   // same position as ColDownloadSize in removal-only mode
    ColumnCountDefault = 4,
    ColumnCountRemoval = 3
};

TransactionResultDialog::TransactionResultDialog(const TransactionResult &result, QWidget *parent)
    : QDialog(parent)
{
    setupUI(result);
}

void TransactionResultDialog::setupUI(const TransactionResult &result)
{
    setWindowTitle(i18n("Transaction Summary"));
    // The dialog needs enough room to show long package NEVRAs alongside
    // the new repository / download-size / install-size columns without
    // truncating them at the default window size. 720x460 is the same
    // ballpark as dnf5dragora / yumex-ng transaction summaries.
    setMinimumSize(720, 460);
    resize(820, 520);

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

    // Determine whether this is a removal-only transaction (only "remove"
    // / "obsoleted" actions, excluding the always-hidden "replaced" group).
    // In that case there is nothing to download and nothing new to install,
    // so the Download Size / Install Size columns are replaced by a single
    // "Freed Space" column that shows how much disk each removed package
    // will return to the system.
    QStringList visibleActions;
    for (const QString &a : result.actionKeys()) {
        if (a != QStringLiteral("replaced"))
            visibleActions.append(a);
    }
    bool isRemovalOnly = !visibleActions.isEmpty();
    for (const QString &a : visibleActions) {
        if (a != QStringLiteral("remove") && a != QStringLiteral("obsoleted")) {
            isRemovalOnly = false;
            break;
        }
    }

    const int columnCount = isRemovalOnly ? ColumnCountRemoval : ColumnCountDefault;

    // Tree widget. For install/upgrade transactions: four columns so the
    // user can see both how much will be downloaded and how much disk the
    // installed packages will occupy. For removal-only transactions: three
    // columns (Package | Repository | Freed Space).
    m_treeWidget = new QTreeWidget;
    m_treeWidget->setColumnCount(columnCount);
    if (isRemovalOnly) {
        m_treeWidget->setHeaderLabels({i18n("Package"), i18n("Repository"),
                                       i18n("Freed Space")});
    } else {
        m_treeWidget->setHeaderLabels({i18n("Package"), i18n("Repository"),
                                       i18n("Download Size"), i18n("Install Size")});
    }
    m_treeWidget->setRootIsDecorated(true);
    m_treeWidget->setAlternatingRowColors(true);
    m_treeWidget->setUniformRowHeights(true);

    // User-resizable columns: every column is Interactive so the user can
    // drag the section handles to whatever width suits their packages
    // (some NEVRAs are very long, some repos have short IDs). The Package
    // column is given a generous default and is also the one stretched
    // when the dialog is resized wider than its default — but only *after*
    // the user has had a chance to set their own preferred widths, hence
    // the manual defaults + stretch indicator rather than Stretch mode on
    // column 0 (which would prevent manual width adjustment).
    QHeaderView *header = m_treeWidget->header();
    header->setMinimumSectionSize(60);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setStretchLastSection(false);
    header->setSectionsMovable(false);

    // Default widths chosen to fit a typical Fedora-style transaction
    // (long "kernel-modules-6.x-yyy.fcNN.x86_64" NEVRAs, repo IDs like
    // "updates-testing", 8-char size cells). These are starting points;
    // the user can drag them wider/narrower at will.
    m_treeWidget->setColumnWidth(ColPackage,      360);
    m_treeWidget->setColumnWidth(ColRepository,   140);
    if (isRemovalOnly) {
        m_treeWidget->setColumnWidth(ColFreedSpace, 140);
    } else {
        m_treeWidget->setColumnWidth(ColDownloadSize, 110);
        m_treeWidget->setColumnWidth(ColInstallSize, 110);
    }

    // Right-align numeric size columns for easy comparison down the rows.
    if (isRemovalOnly) {
        m_treeWidget->headerItem()->setTextAlignment(ColFreedSpace, Qt::AlignRight | Qt::AlignVCenter);
    } else {
        m_treeWidget->headerItem()->setTextAlignment(ColDownloadSize, Qt::AlignRight | Qt::AlignVCenter);
        m_treeWidget->headerItem()->setTextAlignment(ColInstallSize, Qt::AlignRight | Qt::AlignVCenter);
    }

    static const QHash<QString, QString> actionLabels = {
        {QStringLiteral("install"),   i18n("Install")},
        {QStringLiteral("upgrade"),   i18n("Upgrade")},
        {QStringLiteral("downgrade"), i18n("Downgrade")},
        {QStringLiteral("reinstall"), i18n("Reinstall")},
        {QStringLiteral("remove"),    i18n("Remove")},
        {QStringLiteral("replaced"),  i18n("Replace")},
        {QStringLiteral("obsoleted"), i18n("Obsolete")},
    };

    qint64 totalDownloadSize = 0;
    qint64 totalInstallSize = 0;
    qint64 totalFreedSize = 0;
    int dependencyCount = 0;

    for (const auto &action : result.actionKeys()) {
        // yumex-ng hides "replaced" entries: they are the already-installed
        // old versions being removed by an upgrade and need no download, so
        // they must not be shown nor counted in the transaction total.
        if (action == QStringLiteral("replaced"))
            continue;

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

        qint64 groupDownload = 0;
        qint64 groupInstall = 0;
        for (const auto &item : items) {
            auto *child = new QTreeWidgetItem(groupItem);

            // Pre-tag dependency items with "[Dependencies]" so the user
            // can tell which packages were pulled in automatically to
            // satisfy their explicit selections, vs the ones they
            // actually asked for. The tag colour follows the *action's*
            // colour, not a fixed green: for install / upgrade the deps
            // are being added (green = install action), for remove /
            // obsoleted the deps are being torn out with the requested
            // package (red = remove action). Italic + action-colour also
            // matches the convention used by QueueView and PackageView.
            if (item.isDependency) {
                ++dependencyCount;
                child->setText(0, QStringLiteral("[%1] %2").arg(i18n("Dependencies"), item.nevra));
                QFont depFont = child->font(0);
                depFont.setItalic(true);
                child->setFont(0, depFont);
                child->setForeground(0, actionColor);
            } else {
                child->setText(0, item.nevra);
            }

            child->setText(ColRepository, item.repo);
            if (isRemovalOnly) {
                // For removals the on-disk size of the package being deleted
                // is exactly the space the user will get back, so show it in
                // the single "Freed Space" column. item.installSize is the
                // RPM's installed footprint.
                child->setText(ColFreedSpace, formatSize(item.installSize));
                child->setTextAlignment(ColFreedSpace, Qt::AlignRight | Qt::AlignVCenter);
            } else {
                child->setText(ColDownloadSize, formatSize(item.size));
                child->setTextAlignment(ColDownloadSize, Qt::AlignRight | Qt::AlignVCenter);
                child->setText(ColInstallSize, formatSize(item.installSize));
                child->setTextAlignment(ColInstallSize, Qt::AlignRight | Qt::AlignVCenter);
            }

            groupDownload += item.size;
            groupInstall += item.installSize;
        }

        // Group totals on the group row, bolded for emphasis.
        if (isRemovalOnly) {
            groupItem->setText(ColFreedSpace, formatSize(groupInstall));
            groupItem->setTextAlignment(ColFreedSpace, Qt::AlignRight | Qt::AlignVCenter);
            QFont totalFont = groupItem->font(ColFreedSpace);
            totalFont.setBold(true);
            groupItem->setFont(ColFreedSpace, totalFont);
        } else {
            groupItem->setText(ColDownloadSize, formatSize(groupDownload));
            groupItem->setTextAlignment(ColDownloadSize, Qt::AlignRight | Qt::AlignVCenter);
            groupItem->setText(ColInstallSize, formatSize(groupInstall));
            groupItem->setTextAlignment(ColInstallSize, Qt::AlignRight | Qt::AlignVCenter);
            QFont totalFont = groupItem->font(ColDownloadSize);
            totalFont.setBold(true);
            groupItem->setFont(ColDownloadSize, totalFont);
            groupItem->setFont(ColInstallSize, totalFont);
        }
        groupItem->setExpanded(true);

        // Only actions that require downloading packages contribute to the
        // "Total Download Size". remove / obsoleted operate on already
        // installed packages and fetch nothing.
        if (action != QStringLiteral("remove") && action != QStringLiteral("obsoleted")) {
            totalDownloadSize += groupDownload;
            totalInstallSize += groupInstall;
        } else {
            // For removals, accumulate the freed disk space (the on-disk
            // footprint of every package being torn out).
            totalFreedSize += groupInstall;
        }
    }

    layout->addWidget(m_treeWidget, 1);

    // Totals. For removal-only transactions there is nothing to download
    // or install, so the only meaningful figure is how much disk space the
    // removed packages will free up. For install/upgrade transactions show
    // both the total download size and the total install size. In both
    // cases also report how many dependencies were pulled in automatically.
    QString totalText;
    if (isRemovalOnly) {
        totalText = QStringLiteral("<b>%1:</b> %2").arg(
            i18n("Total Freed Space"), formatSize(totalFreedSize));
    } else {
        totalText = QStringLiteral("<b>%1:</b> %2").arg(
            i18n("Total Download Size"), formatSize(totalDownloadSize));
        if (totalInstallSize > 0) {
            totalText += QStringLiteral(" &nbsp;&nbsp; <b>%1:</b> %2").arg(
                i18n("Total Install Size"), formatSize(totalInstallSize));
        }
    }
    if (dependencyCount > 0) {
        totalText += QStringLiteral(" &nbsp;&nbsp; <b>%1:</b> %2").arg(
            i18n("Dependencies"), QString::number(dependencyCount));
    }
    m_totalLabel = new QLabel(totalText);
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
