#pragma once

#include <QTreeView>
#include "Enums.h"

namespace Miryu {

class QueueModel;

class QueueView : public QTreeView
{
    Q_OBJECT

public:
    explicit QueueView(QueueModel *model, QWidget *parent = nullptr);

    void refresh();

    // NEVRA of the currently selected queue item (empty if none).
    QString selectedNevra() const;

Q_SIGNALS:
    void packageRemoved(const QString &nevra);
    // Emitted when the in-queue action (todo) is changed via the context
    // menu (Reinstall / Downgrade) — MainWindow keeps the package-list
    // marker in sync.
    void todoChanged(const QString &nevra, PackageTodo todo);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QueueModel *m_model;
};

}
