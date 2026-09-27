#pragma once

#include <QDialog>
#include "../core/Transaction.h"

class QTreeWidget;
class QCheckBox;
class QLabel;

namespace Miryu {

class TransactionResultDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TransactionResultDialog(const TransactionResult &result, QWidget *parent = nullptr);

    bool isOffline() const;

private:
    void setupUI(const TransactionResult &result);

    QTreeWidget *m_treeWidget;
    QCheckBox *m_offlineCheck;
    QLabel *m_totalLabel;
};

}
