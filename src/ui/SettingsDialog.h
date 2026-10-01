#pragma once

#include <QDialog>

class QTabWidget;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QWidget;
class QGroupBox;
class QFormLayout;

namespace Miryu {

/**
 * Application configuration dialog.
 *
 * The dialog uses a QTabWidget with two pages:
 *  - "设置" (General Settings)   — Flatpak, metadata and updater settings
 *  - "仓库" (Repositories)        — repository-related information
 *
 * All values are persisted to KSharedConfig ("General" group) so that
 * both the SettingsDialog and the UpdateChecker share the same keys:
 *
 *   flatpakDefaultLocation        (string,  default "user")
 *   flatpakDefaultRemote          (string,  default "flathub")
 *   metadataMinRefreshInterval    (int,     default 60)
 *   customSystemUpdaterPath       (string,  default "")
 *   updateCheckInterval           (int,     default 60)
 *   showTrayIcon                  (bool,    default true)
 *   darkTrayIcon                  (bool,    default false)
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

public Q_SLOTS:
    /**
     * Reads all settings from KSharedConfig into the widgets.
     */
    void loadSettings();

    /**
     * Writes all widget values back to KSharedConfig and calls
     * KSharedConfig::sync() so that the changes are flushed to disk.
     */
    void saveSettings();

private Q_SLOTS:
    void onAccepted();
    void onRejected();
    void onTrayIconToggled(bool checked);

private:
    void setupUI();
    QWidget *createSettingsTab();
    QWidget *createRepositoriesTab();
    QGroupBox *createSection(const QString &title);
    void applyToggleStyle(QCheckBox *checkBox);
    void addRow(QFormLayout *form, const QString &label, QWidget *field, const QString &toolTip = QString());

    QTabWidget *m_tabWidget = nullptr;

    // --- Flatpak 设置 ---
    QComboBox *m_flatpakLocationCombo = nullptr;
    QLineEdit *m_flatpakRemoteEdit = nullptr;

    // --- 元数据设置 ---
    QSpinBox *m_metadataRefreshSpin = nullptr;

    // --- 更新器设置 ---
    QLineEdit *m_updaterPathEdit = nullptr;
    QSpinBox *m_updateCheckSpin = nullptr;
    QCheckBox *m_showTrayIconCheck = nullptr;
    QCheckBox *m_darkTrayIconCheck = nullptr;
};

}
