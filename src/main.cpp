#include "core/Backend.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QTimer>
#include <QFileInfo>
#include <QStringList>
#include <QIcon>
#include <QUrl>

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
    app.setApplicationVersion(QStringLiteral("45.0.0"));
    app.setDesktopFileName(QStringLiteral("org.miryugaming.PackageManager"));

    // Set the translation domain so i18n() calls find the compiled .mo files.
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("miryu-software-center"));

    KCrash::initialize();

    KAboutData aboutData(
        QStringLiteral("miryu"),
        i18n("Miryu Software Center"),
        QStringLiteral("45.0.0"),
        i18n("A modern RPM package manager powered by dnf5daemon, built with Qt6 and KDE Frameworks 6."),
        KAboutLicense::GPL_V3,
        QStringLiteral("© 2027 KairikiFedora © 2027 MiryuGaming"),
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

    // Extract existing, locally-accessible .rpm file paths from a list of
    // command-line arguments / file names (shared by the first-launch path
    // and the single-instance activation path below).
    auto collectRpmFiles = [](const QStringList &arguments) {
        QStringList rpmFiles;
        for (const QString &arg : arguments) {
            QFileInfo fi(arg);
            if (fi.exists() && fi.suffix().compare(QStringLiteral("rpm"), Qt::CaseInsensitive) == 0) {
                rpmFiles.append(fi.absoluteFilePath());
            }
        }
        return rpmFiles;
    };

    // Collect RPM files from positional arguments
    QStringList rpmFiles = collectRpmFiles(parser.positionalArguments());

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

    // Single-instance activation: when the main window is ALREADY open and the
    // user double-clicks an RPM file (or picks "Open with" in the file
    // manager), KDBusService::Unique delivers the new command line to this
    // running instance instead of launching a second one. Without this handler
    // the request was silently dropped — raise the window and react with the
    // dnf5 transaction summary for the received RPM file(s).
    auto handleActivation = [&window, collectRpmFiles](const QStringList &arguments) {
        // Bring the existing window to the foreground so the reaction is
        // visible even if it was minimized or hidden behind other windows.
        window.show();
        window.raise();
        window.activateWindow();

        const QStringList files = collectRpmFiles(arguments);
        if (!files.isEmpty())
            window.installLocalRpmFiles(files);
    };
    QObject::connect(&service, &KDBusService::activateRequested, &app, handleActivation);

    // KIO / file managers may deliver "open" requests as a URL list instead
    // of command-line arguments. KF6's KDBusService::openRequested takes
    // const QList<QUrl>& (there is no separate openUrlRequested signal).
    QObject::connect(&service, &KDBusService::openRequested, &app,
                     [&window](const QList<QUrl> &urls) {
                         window.show();
                         window.raise();
                         window.activateWindow();

                         QStringList files;
                         for (const QUrl &url : urls) {
                             if (url.isLocalFile())
                                 files.append(url.toLocalFile());
                         }
                         if (!files.isEmpty())
                             window.installLocalRpmFiles(files);
                     });

    return app.exec();
}
