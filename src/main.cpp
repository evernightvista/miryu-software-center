#include "core/Backend.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QTimer>
#include <QFileInfo>
#include <QStringList>
#include <QIcon>

#include <KAboutData>
#include <KLocalizedString>
#include <KCrash>
#include <KDBusService>
#include <KMessageBox>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("miryu"));
    app.setOrganizationName(QStringLiteral("miryu"));
    app.setApplicationDisplayName(QStringLiteral("Miryu Software Center"));
    app.setApplicationVersion(QStringLiteral("1.0.1"));
    app.setDesktopFileName(QStringLiteral("org.miryugaming.PackageManager"));

    // Set the translation domain so i18n() calls find the compiled .mo files.
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("miryu-software-center"));

    KCrash::initialize();

    KAboutData aboutData(
        QStringLiteral("miryu"),
        i18n("Miryu Software Center"),
        QStringLiteral("1.0.1"),
        i18n("A modern RPM package manager powered by dnf5daemon, built with Qt6 and KDE Frameworks 6."),
        KAboutLicense::GPL_V3,
        QStringLiteral("© 2027 KairikiFedora and © 2027 MiryuGaming"),
        QString(),
        QStringLiteral("https://github.com/evernightvista/miryu-package-manager"),
        QStringLiteral("https://github.com/evernightvista/miryu-package-manager/issues"));

    aboutData.addAuthor(QStringLiteral("KairikiFedora"), QString(),
                        QStringLiteral("13278297951@sina.cn"));
    aboutData.addAuthor(QStringLiteral("MiryuGaming"), QString(),
                        QStringLiteral("3479026736@qq.com"));
    aboutData.addAuthor(QStringLiteral("Farna Herry"), QString(),
                        QStringLiteral("farna_herry@163.com"));
    aboutData.addAuthor(QStringLiteral("jtgg114514"), QString(),
                        QStringLiteral("jtgg114514@outlook.com"));

    KAboutData::setApplicationData(aboutData);

    // Set the application window icon so the window title and the about dialog
    // (which uses the application icon) display a proper software-install icon.
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("miryu-package-manager")));

    QCommandLineParser parser;
    aboutData.setupCommandLine(&parser);

    // Add option for installing local RPM files
    parser.addOption(QCommandLineOption(
        QStringList{QStringLiteral("install"), QStringLiteral("i")},
        i18n("Install the specified RPM file(s)")));

    // Positional arguments: RPM files to install
    parser.addPositionalArgument(QStringLiteral("files"),
        i18n("RPM package files to install"), QStringLiteral("[files...]"));

    parser.process(app);
    aboutData.processCommandLine(&parser);

    // Single instance
    KDBusService service(KDBusService::Unique);

    // Collect RPM files from positional arguments
    QStringList rpmFiles;
    const QStringList args = parser.positionalArguments();
    for (const QString &arg : args) {
        QFileInfo fi(arg);
        if (fi.exists() && fi.suffix().compare(QStringLiteral("rpm"), Qt::CaseInsensitive) == 0) {
            rpmFiles.append(fi.absoluteFilePath());
        }
    }

    // Initialize backend
    Miryu::Backend backend;

    if (!backend.initialize()) {
        QString detail = backend.lastError();
        QString msg = i18n("Failed to connect to dnf5daemon-server.\n\n"
                           "Please ensure the dnf5daemon-server service is running:\n\n"
                           "  systemctl start dnf5daemon-server\n\n"
                           "If you don't have it installed, install it with:\n"
                           "  dnf install dnf5daemon-server");
        if (!detail.isEmpty()) {
            msg += QStringLiteral("\n\n") + i18n("D-Bus error details:") +
                   QStringLiteral("\n") + detail;
        }
        KMessageBox::error(nullptr, msg, i18n("Connection Error"));
        return 1;
    }

    Miryu::MainWindow window(&backend);
    window.show();

    // Load initial data after window is shown
    QTimer::singleShot(100, &window, &Miryu::MainWindow::loadInitialData);

    // If RPM files were passed, install them after the window is shown
    if (!rpmFiles.isEmpty()) {
        QTimer::singleShot(200, [&window, rpmFiles]() {
            window.installLocalRpmFiles(rpmFiles);
        });
    }

    return app.exec();
}
