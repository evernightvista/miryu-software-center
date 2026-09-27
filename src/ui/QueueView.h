#pragma once

#include <QTreeView>

namespace Miryu {

class QueueModel;

class QueueView : public QTreeView
{
    Q_OBJECT

public:
    explicit QueueView(QueueModel *model, QWidget *parent = nullptr);

    void refresh();

Q_SIGNALS:
    void packageRemoved(const QString &nevra);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QueueModel *m_model;
};

}
