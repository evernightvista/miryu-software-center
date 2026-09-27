#include "PackageInfoWidget.h"
#include "../core/Enums.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QTextBrowser>
#include <QFrame>
#include <QIcon>
#include <QTabWidget>
#include <KLocalizedString>

namespace Miryu {

PackageInfoWidget::PackageInfoWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // Header
    auto *headerLayout = new QHBoxLayout;
    m_iconLabel = new QLabel;
    m_iconLabel->setPixmap(QIcon::fromTheme(QStringLiteral("application-x-rpm")).pixmap(48, 48));

    auto *titleLayout = new QVBoxLayout;
    m_nameLabel = new QLabel;
    QFont nameFont = m_nameLabel->font();
    nameFont.setBold(true);
    nameFont.setPointSize(nameFont.pointSize() + 4);
    m_nameLabel->setFont(nameFont);

    m_summaryLabel = new QLabel;
    m_summaryLabel->setWordWrap(true);
    QFont sumFont = m_summaryLabel->font();
    sumFont.setPointSize(sumFont.pointSize() + 1);
    m_summaryLabel->setFont(sumFont);
    // palette(mid) is a 3D/bevel shade that resolves to near-black in dark themes.
    // Use the theme's window-text colour dimmed, so the summary stays readable.
    QColor sumColor = palette().color(QPalette::WindowText);
    sumColor.setAlpha(160);
    m_summaryLabel->setStyleSheet(
        QStringLiteral("color: rgba(%1, %2, %3, 0.627);")
            .arg(sumColor.red()).arg(sumColor.green()).arg(sumColor.blue()));

    titleLayout->addWidget(m_nameLabel);
    titleLayout->addWidget(m_summaryLabel);

    headerLayout->addWidget(m_iconLabel);
    headerLayout->addLayout(titleLayout, 1);
    layout->addLayout(headerLayout);

    // Separator
    auto *sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    // Details form
    auto *form = new QFormLayout;
    form->setSpacing(6);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_nevraLabel = new QLabel;
    m_repoLabel = new QLabel;
    m_sizeLabel = new QLabel;
    m_licenseLabel = new QLabel;
    m_urlLabel = new QLabel;
    m_urlLabel->setOpenExternalLinks(true);
    m_stateLabel = new QLabel;

    form->addRow(i18n("Version:"), m_nevraLabel);
    form->addRow(i18n("Repository:"), m_repoLabel);
    form->addRow(i18n("Size:"), m_sizeLabel);
    form->addRow(i18n("License:"), m_licenseLabel);
    form->addRow(i18n("URL:"), m_urlLabel);
    form->addRow(i18n("Status:"), m_stateLabel);

    layout->addLayout(form);

    // Separator
    auto *sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep2);

    // Tab widget for extended package info
    m_tabWidget = new QTabWidget;

    // Description tab
    m_descBrowser = new QTextBrowser;
    m_descBrowser->setOpenExternalLinks(true);
    m_descBrowser->setFrameStyle(QFrame::NoFrame);
    m_tabWidget->addTab(m_descBrowser, i18n("Description"));

    // Dependencies tab
    m_depsBrowser = new QTextBrowser;
    m_depsBrowser->setFrameStyle(QFrame::NoFrame);
    m_tabWidget->addTab(m_depsBrowser, i18n("Dependencies"));

    // Provides tab
    m_providesBrowser = new QTextBrowser;
    m_providesBrowser->setFrameStyle(QFrame::NoFrame);
    m_tabWidget->addTab(m_providesBrowser, i18n("Provides"));

    // Changelog tab
    m_changelogBrowser = new QTextBrowser;
    m_changelogBrowser->setFrameStyle(QFrame::NoFrame);
    m_tabWidget->addTab(m_changelogBrowser, i18n("Changelog"));

    // Files tab
    m_filesBrowser = new QTextBrowser;
    m_filesBrowser->setFrameStyle(QFrame::NoFrame);
    m_tabWidget->addTab(m_filesBrowser, i18n("Files"));

    layout->addWidget(m_tabWidget, 1);

    setMinimumWidth(300);
}

void PackageInfoWidget::setPackage(const Package &pkg)
{
    m_current = pkg;

    m_nameLabel->setText(pkg.name);
    m_summaryLabel->setText(pkg.summary);
    m_nevraLabel->setText(pkg.evr());
    m_repoLabel->setText(pkg.repo);
    m_sizeLabel->setText(formatSize(pkg.size));
    m_licenseLabel->setText(pkg.license);

    if (!pkg.url.isEmpty())
        m_urlLabel->setText(QStringLiteral("<a href=\"%1\">%1</a>").arg(pkg.url));
    else
        m_urlLabel->clear();

    switch (pkg.state) {
    case PackageState::Installed:
        m_stateLabel->setText(QStringLiteral("<span style='color: green;'>%1</span>").arg(i18n("Installed")));
        break;
    case PackageState::Available:
        m_stateLabel->setText(i18n("Available"));
        break;
    case PackageState::Update:
        m_stateLabel->setText(QStringLiteral("<span style='color: orange;'>%1</span>").arg(i18n("Update Available")));
        break;
    case PackageState::Downgrade:
        m_stateLabel->setText(i18n("Downgrade Available"));
        break;
    }

    // Description tab: show what we already have from the package list
    QString desc = pkg.description;
    desc.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    m_descBrowser->setHtml(desc.isEmpty() ? i18n("<i>No description available.</i>") : desc);

    // Reset extended info tabs until details are fetched
    m_depsBrowser->setHtml(i18n("<i>Loading...</i>"));
    m_providesBrowser->setHtml(i18n("<i>Loading...</i>"));
    m_changelogBrowser->setHtml(i18n("<i>Loading...</i>"));
    m_filesBrowser->setHtml(i18n("<i>Loading...</i>"));

    // Request extended details from the backend
    Q_EMIT packageDetailsRequested(pkg.name, pkg.nevra());
}

void PackageInfoWidget::setPackageDetails(const QVariantMap &details)
{
    // Description - only update if we got a non-empty one.
    // This preserves the description from the initial package list data
    // in case the details query fails or returns nothing.
    QString desc = details.value(QStringLiteral("description")).toString();
    if (!desc.isEmpty()) {
        desc.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
        m_descBrowser->setHtml(desc);
    }

    // Dependencies (requires)
    QStringList deps = details.value(QStringLiteral("requires")).toStringList();
    if (deps.isEmpty())
        m_depsBrowser->setHtml(i18n("<i>No dependencies.</i>"));
    else
        m_depsBrowser->setHtml(deps.join(QStringLiteral("<br>")));

    // Provides
    QStringList provides = details.value(QStringLiteral("provides")).toStringList();
    if (provides.isEmpty())
        m_providesBrowser->setHtml(i18n("<i>No provides.</i>"));
    else
        m_providesBrowser->setHtml(provides.join(QStringLiteral("<br>")));

    // Changelog
    QStringList changelog = details.value(QStringLiteral("changelogs")).toStringList();
    if (changelog.isEmpty())
        m_changelogBrowser->setHtml(i18n("<i>No changelog available.</i>"));
    else
        m_changelogBrowser->setHtml(changelog.join(QStringLiteral("<br><br>")));

    // Files
    QStringList files = details.value(QStringLiteral("files")).toStringList();
    if (files.isEmpty())
        m_filesBrowser->setHtml(i18n("<i>No file list available.</i>"));
    else
        m_filesBrowser->setHtml(files.join(QStringLiteral("<br>")));
}

QString PackageInfoWidget::formatSize(qint64 bytes) const
{
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    if (bytes < 1024 * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + QStringLiteral(" MB");
    return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + QStringLiteral(" GB");
}

}
