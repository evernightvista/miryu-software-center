#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QGroupBox>
#include <QScrollArea>
#include <QFrame>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QIcon>
#include <QStyle>

#include <KSharedConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KGuiItem>
#include <KStandardGuiItem>

namespace Miryu {

// ---------------------------------------------------------------------------
// Defaults — kept in sync with UpdateChecker and documented in the header.
// ---------------------------------------------------------------------------
static const QString kConfigGroup = QStringLiteral("General");
static const QString kDefaultFlatpakLocation = QStringLiteral("user");
static const QString kDefaultFlatpakRemote = QStringLiteral("flathub");
static constexpr int kDefaultMetadataRefresh = 60;
static constexpr int kDefaultUpdateCheckInterval = 60;

// A toggle-switch style sheet applied to QCheckBox indicators so that the
// checkbox visually resembles an iOS / Plasma style toggle switch rather
// than a plain square. The indicator is drawn as a wide rounded rectangle
// that changes colour when checked.
static constexpr const char *kToggleStyleSheet = R"(
QCheckBox {
    spacing: 10px;
    background: transparent;
}
QCheckBox::indicator {
    width: 42px;
    height: 22px;
    border-radius: 11px;
    background: palette(mid);
    border: 1px solid palette(dark);
}
QCheckBox::indicator:checked {
    background: palette(highlight);
    border: 1px solid palette(highlight);
}
QCheckBox::indicator:hover {
    border: 1px solid palette(midlight);
}
QCheckBox::indicator:disabled {
    background: palette(window);
    border: 1px solid palette(dark);
}
QCheckBox:disabled {
    color: palette(disabled-text);
}
)";

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUI();
    loadSettings();
}

SettingsDialog::~SettingsDialog() = default;

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void SettingsDialog::setupUI()
{
    setWindowTitle(i18n("Settings"));
    setMinimumSize(540, 460);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_tabWidget = new QTabWidget;
    m_tabWidget->addTab(createSettingsTab(), i18n("Settings"));
    m_tabWidget->addTab(createRepositoriesTab(), i18n("Repositories"));
    mainLayout->addWidget(m_tabWidget, 1);

    // Standard OK / Cancel button row following KDE conventions.
    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        this);

    // Use KGuiItem to localise the button labels consistently with the rest
    // of KDE.
    KGuiItem::assign(buttonBox->button(QDialogButtonBox::Ok),
                     KStandardGuiItem::ok());
    KGuiItem::assign(buttonBox->button(QDialogButtonBox::Cancel),
                     KStandardGuiItem::cancel());

    QPushButton *defaultsBtn = buttonBox->button(QDialogButtonBox::RestoreDefaults);
    if (defaultsBtn) {
        defaultsBtn->setText(i18n("Defaults"));
        defaultsBtn->setIcon(QIcon::fromTheme(QStringLiteral("edit-reset")));
        connect(defaultsBtn, &QPushButton::clicked, this, [this]() {
            // Reset widgets to their compiled-in defaults without writing
            // anything to disk; the user still has to press OK to persist.
            m_flatpakLocationCombo->setCurrentText(kDefaultFlatpakLocation);
            m_flatpakRemoteEdit->setText(kDefaultFlatpakRemote);
            m_metadataRefreshSpin->setValue(kDefaultMetadataRefresh);
            m_updaterPathEdit->clear();
            m_updateCheckSpin->setValue(kDefaultUpdateCheckInterval);
            m_showTrayIconCheck->setChecked(true);
            m_darkTrayIconCheck->setChecked(false);
            m_updateNotificationsCheck->setChecked(true);
        });
    }

    connect(buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::onRejected);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(12, 8, 12, 8);
    btnLayout->addStretch();
    btnLayout->addWidget(buttonBox);
    mainLayout->addLayout(btnLayout);
}

QWidget *SettingsDialog::createSettingsTab()
{
    // The settings tab is placed inside a scroll area so that on small
    // screens or with a large font all sections remain reachable.
    auto *content = new QWidget;
    auto *outerLayout = new QVBoxLayout(content);
    outerLayout->setContentsMargins(12, 12, 12, 12);
    outerLayout->setSpacing(12);

    // ---- Flatpak Settings ----
    auto *flatpakSection = createSection(i18n("Flatpak Settings"));
    auto *flatpakForm = new QFormLayout(flatpakSection);
    flatpakForm->setSpacing(8);

    m_flatpakLocationCombo = new QComboBox;
    m_flatpakLocationCombo->addItem(QStringLiteral("user"), QStringLiteral("user"));
    m_flatpakLocationCombo->addItem(QStringLiteral("system"), QStringLiteral("system"));
    addRow(flatpakForm, i18n("Default Location"), m_flatpakLocationCombo,
           i18n("Whether new Flatpak installs default to the user or system installation."));

    m_flatpakRemoteEdit = new QLineEdit;
    m_flatpakRemoteEdit->setPlaceholderText(QStringLiteral("flathub"));
    addRow(flatpakForm, i18n("Default Remote Repository"), m_flatpakRemoteEdit,
           i18n("Remote repository name used for Flatpak operations by default."));

    outerLayout->addWidget(flatpakSection);

    // ---- Metadata Settings ----
    auto *metadataSection = createSection(i18n("Metadata Settings"));
    auto *metadataForm = new QFormLayout(metadataSection);
    metadataForm->setSpacing(8);

    m_metadataRefreshSpin = new QSpinBox;
    m_metadataRefreshSpin->setRange(1, 10080); // 1 minute .. 7 days
    m_metadataRefreshSpin->setSuffix(i18n(" minutes"));
    m_metadataRefreshSpin->setSingleStep(5);
    addRow(metadataForm, i18n("Min Refresh Interval"), m_metadataRefreshSpin,
           i18n("Minimum interval between automatic repository metadata refreshes."));

    outerLayout->addWidget(metadataSection);

    // ---- Updater Settings ----
    auto *updaterSection = createSection(i18n("Updater Settings"));
    auto *updaterForm = new QFormLayout(updaterSection);
    updaterForm->setSpacing(8);

    m_updaterPathEdit = new QLineEdit;
    m_updaterPathEdit->setPlaceholderText(i18n("e.g. /usr/bin/dnfdragora"));
    addRow(updaterForm, i18n("Custom System Updater Path"), m_updaterPathEdit,
           i18n("Full path to an external system updater to launch instead of the built-in one."));

    m_updateCheckSpin = new QSpinBox;
    m_updateCheckSpin->setRange(1, 10080);
    m_updateCheckSpin->setSuffix(i18n(" minutes"));
    m_updateCheckSpin->setSingleStep(5);
    addRow(updaterForm, i18n("Check Update Interval"), m_updateCheckSpin,
           i18n("How often Miryu checks for available RPM updates in the background."));

    // Toggle switches — styled QCheckBox instances.
    m_showTrayIconCheck = new QCheckBox(i18n("Show Tray Icon"));
    applyToggleStyle(m_showTrayIconCheck);
    connect(m_showTrayIconCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onTrayIconToggled);
    addRow(updaterForm, QString(), m_showTrayIconCheck,
           i18n("Show a system tray icon while Miryu is running."));

    m_darkTrayIconCheck = new QCheckBox(i18n("Dark Tray Icon"));
    applyToggleStyle(m_darkTrayIconCheck);
    addRow(updaterForm, QString(), m_darkTrayIconCheck,
           i18n("Use a dark tray icon for better visibility on light panels."));

    m_updateNotificationsCheck = new QCheckBox(i18n("Update Notifications"));
    applyToggleStyle(m_updateNotificationsCheck);
    addRow(updaterForm, QString(), m_updateNotificationsCheck,
           i18n("Show a desktop notification when updates become available."));

    outerLayout->addWidget(updaterSection);
    outerLayout->addStretch();

    // Wrap in scroll area.
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);

    return scroll;
}

QWidget *SettingsDialog::createRepositoriesTab()
{
    // The Repositories tab is intentionally lightweight — actual repository
    // enable/disable management happens in the main Repositories page. Here
    // we show guidance and a shortcut description.
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto *iconLabel = new QLabel;
    iconLabel->setPixmap(QIcon::fromTheme(QStringLiteral("folder-download"))
                             .pixmap(48, 48));
    iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(iconLabel);

    auto *title = new QLabel(QStringLiteral("<h3>%1</h3>").arg(i18n("Repository Management")));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto *desc = new QLabel(i18n(
        "Repository enable / disable management is performed from the main "
        "Repositories page in the sidebar.\n\n"
        "Use the sidebar \"Repositories\" entry to view, enable or disable "
        "individual software repositories and to refresh their metadata."));
    desc->setWordWrap(true);
    desc->setAlignment(Qt::AlignCenter);
    desc->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(desc);

    layout->addStretch();

    return page;
}

QGroupBox *SettingsDialog::createSection(const QString &title)
{
    auto *box = new QGroupBox(title);
    // A subtle flat-style group box with a bold, slightly larger title.
    box->setStyleSheet(QStringLiteral(
        "QGroupBox {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 6px;"
        "  margin-top: 12px;"
        "  padding: 8px 4px 4px 4px;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  left: 10px;"
        "  padding: 0 4px;"
        "  font-weight: bold;"
        "}"));
    return box;
}

void SettingsDialog::applyToggleStyle(QCheckBox *checkBox)
{
    if (!checkBox)
        return;
    checkBox->setStyleSheet(QString::fromLatin1(kToggleStyleSheet));
}

void SettingsDialog::addRow(QFormLayout *form, const QString &label,
                            QWidget *field, const QString &toolTip)
{
    if (!form || !field)
        return;

    if (!toolTip.isEmpty())
        field->setToolTip(toolTip);

    // When a label is provided, create a managed QLabel so that a tooltip
    // can be attached and a buddy relationship can be established. When the
    // label is empty (e.g. for full-width toggle switches) add the field
    // without a label row.
    if (label.isEmpty()) {
        form->addRow(field);
    } else {
        auto *labelWidget = new QLabel(label);
        labelWidget->setBuddy(field);
        if (!toolTip.isEmpty())
            labelWidget->setToolTip(toolTip);
        form->addRow(labelWidget, field);
    }
}

// ---------------------------------------------------------------------------
// KConfig load / save
// ---------------------------------------------------------------------------

void SettingsDialog::loadSettings()
{
    KConfigGroup group = KSharedConfig::openConfig()->group(kConfigGroup);

    // Flatpak
    const QString location = group.readEntry(
        QStringLiteral("flatpakDefaultLocation"), kDefaultFlatpakLocation);
    int idx = m_flatpakLocationCombo->findData(location);
    if (idx >= 0)
        m_flatpakLocationCombo->setCurrentIndex(idx);
    else
        m_flatpakLocationCombo->setCurrentIndex(0); // "user"

    m_flatpakRemoteEdit->setText(group.readEntry(
        QStringLiteral("flatpakDefaultRemote"), kDefaultFlatpakRemote));

    // Metadata
    m_metadataRefreshSpin->setValue(group.readEntry(
        QStringLiteral("metadataMinRefreshInterval"), kDefaultMetadataRefresh));

    // Updater
    m_updaterPathEdit->setText(group.readEntry(
        QStringLiteral("customSystemUpdaterPath"), QString()));

    m_updateCheckSpin->setValue(group.readEntry(
        QStringLiteral("updateCheckInterval"), kDefaultUpdateCheckInterval));

    m_showTrayIconCheck->setChecked(group.readEntry(
        QStringLiteral("showTrayIcon"), true));

    m_darkTrayIconCheck->setChecked(group.readEntry(
        QStringLiteral("darkTrayIcon"), false));

    m_updateNotificationsCheck->setChecked(group.readEntry(
        QStringLiteral("updateNotificationsEnabled"), true));

    // The dark-tray-icon toggle is only meaningful when the tray icon
    // itself is enabled.
    onTrayIconToggled(m_showTrayIconCheck->isChecked());
}

void SettingsDialog::saveSettings()
{
    KConfigGroup group = KSharedConfig::openConfig()->group(kConfigGroup);

    // Flatpak
    group.writeEntry(QStringLiteral("flatpakDefaultLocation"),
                     m_flatpakLocationCombo->currentData().toString());

    group.writeEntry(QStringLiteral("flatpakDefaultRemote"),
                      m_flatpakRemoteEdit->text().trimmed());

    // Metadata
    group.writeEntry(QStringLiteral("metadataMinRefreshInterval"),
                     m_metadataRefreshSpin->value());

    // Updater
    group.writeEntry(QStringLiteral("customSystemUpdaterPath"),
                     m_updaterPathEdit->text().trimmed());

    group.writeEntry(QStringLiteral("updateCheckInterval"),
                     m_updateCheckSpin->value());

    group.writeEntry(QStringLiteral("showTrayIcon"),
                     m_showTrayIconCheck->isChecked());

    group.writeEntry(QStringLiteral("darkTrayIcon"),
                     m_darkTrayIconCheck->isChecked());

    group.writeEntry(QStringLiteral("updateNotificationsEnabled"),
                     m_updateNotificationsCheck->isChecked());

    group.sync();
    KSharedConfig::openConfig()->sync();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void SettingsDialog::onAccepted()
{
    saveSettings();
    accept();
}

void SettingsDialog::onRejected()
{
    reject();
}

void SettingsDialog::onTrayIconToggled(bool checked)
{
    // The dark-tray-icon option depends on the tray icon being visible.
    m_darkTrayIconCheck->setEnabled(checked);
}

}
