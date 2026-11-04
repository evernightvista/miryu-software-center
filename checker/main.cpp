/*
 * miryu-software-check — standalone update checker for Miryu Software Center.
 *
 * Modelled after KDE Discover's notifier (notifier/main.cpp +
 * notifier/DiscoverNotifier.cpp): the "updates available" detection lives in
 * its own process so that notifications keep working while the software
 * center GUI is closed, and so the GUI never blocks on metadata refreshes.
 *
 * RPM updates are probed with `dnf5 check-update --refresh`, Flatpak updates
 * with `flatpak update` (run without a TTY, so flatpak aborts its own
 * "Proceed with these changes?" prompt — strictly read-only).
 *
 * When updates are found, a desktop notification is shown:
 *   "X RPM package(s) and/or Y Flatpak package(s) available for update"
 * with a "View Updates" action launching Miryu Software Center.
 *
 * The notification frequency (daily / weekly / monthly / never) is
 * configured in Miryu Software Center (File menu → "Check Update Frequency
 * Settings") and read from the shared miryurc configuration.
 */

#include "UpdateCheckNotifier.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QTimer>

#include <KAboutData>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedString>

int main(int argc, char *argv[])
{
    // QApplication (not QGuiApplication): the tray item's context menu is a
    // QMenu, which requires the widgets platform integration.
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("miryu-software-check"));
    app.setOrganizationName(QStringLiteral("miryu"));

    // Share the translation catalog with the software center so the
    // notification strings follow the user's language. Must be set before
    // any i18n() call (including the application display name below).
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("miryu-software-center"));

    app.setApplicationDisplayName(i18n("Miryu Update Checker"));
    app.setDesktopFileName(QStringLiteral("org.miryugaming.SoftwareCheck"));

    KCrash::initialize();

    KAboutData aboutData(
        QStringLiteral("miryu-software-check"),
        i18n("Miryu Update Checker"),
        QStringLiteral("45.0.0"),
        i18n("Checks for available RPM and Flatpak updates and notifies the user."),
        KAboutLicense::GPL_V3,
        QStringLiteral("© 2027 KairikiFedora © 2027 MiryuGaming"));
    KAboutData::setApplicationData(aboutData);

    QCommandLineParser parser;
    aboutData.setupCommandLine(&parser);

    QCommandLineOption checkDelayOption(
        QStringList{QStringLiteral("check-delay")},
        i18n("Delay checking for updates by N seconds"),
        QStringLiteral("seconds"),
        QStringLiteral("60"));
    parser.addOption(checkDelayOption);

    QCommandLineOption forceOption(
        QStringList{QStringLiteral("force-notify")},
        i18n("Check immediately and ignore the configured notification interval"));
    parser.addOption(forceOption);

    QCommandLineOption replaceOption(
        QStringList{QStringLiteral("replace")},
        i18n("Replace an existing instance"));
    parser.addOption(replaceOption);
    parser.process(app);
    aboutData.processCommandLine(&parser);

    const bool replace = parser.isSet(replaceOption);
    const int checkDelaySeconds = parser.value(checkDelayOption).toInt();
    const bool force = parser.isSet(forceOption);

    // Single instance: the autostart entry and a manual run must not probe
    // (and thus download metadata) twice in parallel.
    KDBusService::StartupOptions startup = KDBusService::Unique;
    if (replace)
        startup |= KDBusService::Replace;
    KDBusService service(startup);

    // Honour the notification frequency configured in the software center:
    // "never" disables the check entirely, otherwise the check only runs
    // once the configured interval (day / week / month) has elapsed since
    // the last notification. --force-notify bypasses the interval (testing).
    if (!force && !Miryu::UpdateCheckNotifier::shouldRunCheckNow()) {
        return 0;
    }

    Miryu::UpdateCheckNotifier notifier;
    QObject::connect(&notifier, &Miryu::UpdateCheckNotifier::finished,
                     &app, &QCoreApplication::quit);

    // Wait for the session to settle before starting the (metadata
    // downloading) probes — same idea as Discover's --check-delay.
    QTimer::singleShot(qMax(checkDelaySeconds, 0) * 1000, &notifier, &Miryu::UpdateCheckNotifier::start);

    return app.exec();
}
