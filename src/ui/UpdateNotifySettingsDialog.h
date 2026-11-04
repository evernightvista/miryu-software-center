#pragma once

#include <QDialog>

class QComboBox;

namespace Miryu {

/**
 * "检查更新频率设置" dialog — how often the standalone miryu-software-check
 * binary may notify the user about available updates (RPM and/or Flatpak):
 *
 *   每天 / 每周 / 每月 / 不通知  (daily / weekly / monthly / never)
 *
 * The choice is persisted to KSharedConfig ("General" group, key
 * "updateNotifyFrequency") — the same file and key miryu-software-check
 * reads at every launch (e.g. from the autostart entry), so the change
 * takes effect the next time the checker runs.
 */
class UpdateNotifySettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UpdateNotifySettingsDialog(QWidget *parent = nullptr);

private Q_SLOTS:
    void onAccepted();

private:
    void setupUI();
    void loadSettings();

    QComboBox *m_frequencyCombo = nullptr;
};

}
