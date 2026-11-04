#include "UpdateNotifySettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <KConfigGroup>
#include <KGuiItem>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KStandardGuiItem>

namespace Miryu {

// Must stay in sync with checker/UpdateCheckNotifier.cpp, which reads the
// same key from the same file ("miryurc" — the KSharedConfig default of the
// main application, whose applicationName is "miryu").
static const QString kConfigGroup = QStringLiteral("General");
static const QString kFrequencyKey = QStringLiteral("updateNotifyFrequency");
static const QString kDefaultFrequency = QStringLiteral("daily");

UpdateNotifySettingsDialog::UpdateNotifySettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUI();
    loadSettings();
}

void UpdateNotifySettingsDialog::setupUI()
{
    setWindowTitle(i18n("Check Update Frequency Settings"));
    setMinimumWidth(420);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    auto *section = new QGroupBox(i18n("Check Update Frequency Settings"));
    auto *form = new QFormLayout(section);
    form->setSpacing(8);

    // Frequency selector — the values mirror the keys written to KConfig and
    // consumed by miryu-software-check.
    m_frequencyCombo = new QComboBox;
    m_frequencyCombo->addItem(QIcon::fromTheme(QStringLiteral("view-calendar-day")),
                              i18n("Daily"), QStringLiteral("daily"));
    m_frequencyCombo->addItem(QIcon::fromTheme(QStringLiteral("view-calendar-week")),
                              i18n("Weekly"), QStringLiteral("weekly"));
    m_frequencyCombo->addItem(QIcon::fromTheme(QStringLiteral("view-calendar-month")),
                              i18n("Monthly"), QStringLiteral("monthly"));
    m_frequencyCombo->addItem(QIcon::fromTheme(QStringLiteral("notification-disabled")),
                              i18n("Never"), QStringLiteral("never"));

    auto *label = new QLabel(i18n("Update Notification Frequency"));
    label->setBuddy(m_frequencyCombo);
    form->addRow(label, m_frequencyCombo);

    mainLayout->addWidget(section);

    auto *hint = new QLabel(i18n(
        "How often miryu-software-check may notify you about available updates.\n"
        "The change takes effect the next time the update checker runs (e.g. at login)."));
    hint->setWordWrap(true);
    QFont hintFont = hint->font();
    hintFont.setPointSize(qMax(hintFont.pointSize() - 1, 8));
    hint->setFont(hintFont);
    mainLayout->addWidget(hint);

    mainLayout->addStretch();

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    KGuiItem::assign(buttonBox->button(QDialogButtonBox::Ok), KStandardGuiItem::ok());
    KGuiItem::assign(buttonBox->button(QDialogButtonBox::Cancel), KStandardGuiItem::cancel());
    connect(buttonBox, &QDialogButtonBox::accepted, this, &UpdateNotifySettingsDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    btnLayout->addWidget(buttonBox);
    mainLayout->addLayout(btnLayout);
}

void UpdateNotifySettingsDialog::loadSettings()
{
    KConfigGroup group = KSharedConfig::openConfig()->group(kConfigGroup);
    const QString frequency = group.readEntry(kFrequencyKey, kDefaultFrequency);

    const int index = m_frequencyCombo->findData(frequency);
    m_frequencyCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void UpdateNotifySettingsDialog::onAccepted()
{
    KConfigGroup group = KSharedConfig::openConfig()->group(kConfigGroup);
    group.writeEntry(kFrequencyKey, m_frequencyCombo->currentData().toString());
    group.sync();
    KSharedConfig::openConfig()->sync();

    accept();
}

}
