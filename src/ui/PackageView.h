#pragma once

#include <QTreeView>
#include <QList>
#include "Package.h"

namespace Miryu {

class PackageModel;

class PackageView : public QTreeView
{
    Q_OBJECT

public:
    explicit PackageView(PackageModel *model, QWidget *parent = nullptr);

    PackageModel* model() const;

    QList<Package> selectedPackages() const;

Q_SIGNALS:
    void packageSelected(const QModelIndex &index);
    void queuePackage(const Miryu::Package &pkg);
    void unqueuePackage(const QString &nevra);
    void packagesQueued(const QList<Miryu::Package> &packages);

private Q_SLOTS:
    void onQueueSelected();
    void onUnqueueSelected();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;

private:
    void setupView();
    void toggleQueue(const QModelIndex &index);
    void queueWithTodo(const QModelIndex &index, PackageTodo todo);
    void queuePackages(const QList<Package> &packages);

    PackageModel *m_model;
};

}
