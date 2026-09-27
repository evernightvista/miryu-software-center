#pragma once

#include <QWidget>
#include <QVariantMap>
#include "Package.h"

class QLabel;
class QTextBrowser;
class QTabWidget;

namespace Miryu {

class PackageInfoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PackageInfoWidget(QWidget *parent = nullptr);

    void setPackage(const Miryu::Package &pkg);
    void setPackageDetails(const QVariantMap &details);
    Miryu::Package currentPackage() const { return m_current; }

Q_SIGNALS:
    // Emitted when a package is selected and the widget needs extended
    // details (dependencies, provides, files, changelog) from the backend.
    // Both the package name and nevra are passed: the name is used for
    // querying (dnf5daemon patterns match package names, not full NEVRAs),
    // and the nevra is used to verify the correct package was selected.
    void packageDetailsRequested(const QString &pkgName, const QString &nevra);

private:
    Miryu::Package m_current;

    QLabel *m_nameLabel;
    QLabel *m_iconLabel;
    QLabel *m_summaryLabel;
    QLabel *m_nevraLabel;
    QLabel *m_repoLabel;
    QLabel *m_sizeLabel;
    QLabel *m_licenseLabel;
    QLabel *m_urlLabel;
    QLabel *m_stateLabel;

    QTabWidget *m_tabWidget;
    QTextBrowser *m_descBrowser;
    QTextBrowser *m_depsBrowser;
    QTextBrowser *m_providesBrowser;
    QTextBrowser *m_changelogBrowser;
    QTextBrowser *m_filesBrowser;

    QString formatSize(qint64 bytes) const;
};

}
