#pragma once

#include <QObject>
#include <QString>
#include <QList>

class QProcess;

namespace Miryu {

/*!
 * A single AppImage integrated by Miryu App Launcher.
 */
struct IntegratedAppImage
{
    QString name;          // Desktop Entry Name=
    QString desktopFile;   // appimagekit_*.desktop path
    QString appImagePath;  // TryExec= / Exec= target
    QString iconName;      // Icon= value (theme icon name or absolute path)
    QString iconPath;      // Resolved icon file, empty when none was found
};

/*!
 * \class AppImageManager
 * \brief Reads (and removes) the AppImages integrated by Miryu App Launcher.
 *
 * The launcher moves integrated AppImages into its destination directory
 * (configured in ~/.config/miryurc, "~/Applications" by default) and has
 * libappimage register an appimagekit_*.desktop file. Scanning those desktop
 * files keeps the software center in sync with the launcher's own
 * bookkeeping instead of maintaining a second database.
 */
class AppImageManager : public QObject
{
    Q_OBJECT

public:
    explicit AppImageManager(QObject *parent = nullptr);

    // Integration directory configured by the launcher.
    static QString integrationDirectory();

    // All integrated AppImages, sorted by display name.
    QList<IntegratedAppImage> scan() const;

    // Launch the AppImage detached. Returns false and fills \a error when the
    // executable could not be started.
    bool run(const IntegratedAppImage &app, QString *error = nullptr) const;

    // Remove the desktop integration (and the integrated file) through
    // miryu-app-launcher-cli. Asynchronous: emits uninstallFinished().
    void uninstall(const IntegratedAppImage &app);

Q_SIGNALS:
    void uninstallFinished(bool success, const QString &message);

private:
    QProcess *m_uninstallProcess = nullptr;
};

}
