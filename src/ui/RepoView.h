#pragma once

#include <QTreeView>

namespace Miryu {

class RepoModel;
class Backend;

class RepoView : public QTreeView
{
    Q_OBJECT

public:
    explicit RepoView(RepoModel *model, Backend *backend, QWidget *parent = nullptr);

Q_SIGNALS:
    void repositoriesChanged();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    RepoModel *m_model;
    Backend *m_backend;
};

}
