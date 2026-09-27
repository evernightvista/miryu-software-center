#pragma once

#include <QObject>
#include <QString>

#include <KLocalizedString>

namespace Miryu {

Q_NAMESPACE

enum class PackageState {
    Installed,
    Available,
    Update,
    Downgrade
};
Q_ENUM_NS(PackageState)

enum class PackageAction {
    None,
    Install,
    Update,
    Remove,
    Downgrade,
    Reinstall,
    DistroSync
};
Q_ENUM_NS(PackageAction)

enum class PackageTodo {
    None,
    Install,
    Update,
    Remove,
    Downgrade,
    Reinstall,
    DistroSync
};
Q_ENUM_NS(PackageTodo)

enum class PackageFilter {
    All,
    Installed,
    Available,
    Updates,
    Upgradable
};
Q_ENUM_NS(PackageFilter)

enum class SearchField {
    Name,
    Summary,
    Description,
    All
};
Q_ENUM_NS(SearchField)

enum class TransactionCommand {
    None,
    SystemUpgrade,
    IsFile,
    SystemDistroSync
};
Q_ENUM_NS(TransactionCommand)

enum class DownloadType {
    Unknown,
    Package,
    Repo
};
Q_ENUM_NS(DownloadType)

enum class TransactionActionType {
    Install,
    Upgrade,
    Downgrade,
    Reinstall,
    Remove,
    Replaced
};
Q_ENUM_NS(TransactionActionType)

inline QString actionToString(TransactionActionType action)
{
    switch (action) {
    case TransactionActionType::Install:    return i18n("Installing");
    case TransactionActionType::Upgrade:    return i18n("Upgrading");
    case TransactionActionType::Downgrade:  return i18n("Downgrading");
    case TransactionActionType::Reinstall:  return i18n("Reinstalling");
    case TransactionActionType::Remove:     return i18n("Removing");
    case TransactionActionType::Replaced:   return i18n("Replacing");
    }
    return i18n("Processing");
}

inline QString packageStateToString(PackageState state)
{
    switch (state) {
    case PackageState::Installed:  return QStringLiteral("installed");
    case PackageState::Available:  return QStringLiteral("available");
    case PackageState::Update:     return QStringLiteral("update");
    case PackageState::Downgrade:  return QStringLiteral("downgrade");
    }
    return QStringLiteral("all");
}

inline PackageTodo calcTodo(PackageState state)
{
    switch (state) {
    case PackageState::Installed:  return PackageTodo::Remove;
    case PackageState::Available:  return PackageTodo::Install;
    case PackageState::Update:     return PackageTodo::Update;
    case PackageState::Downgrade:  return PackageTodo::Downgrade;
    }
    return PackageTodo::None;
}

}
