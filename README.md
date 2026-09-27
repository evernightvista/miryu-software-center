# Miryu Software Center

A modern **RPM** software center powered by **dnf5daemon-server**, built with **Qt6** and **KDE Frameworks 6** in **C++**. It also provides an external launcher for the **Linglong Store** (`/usr/bin/linglong-store`).

Miryu is inspired by [yumex-ng](https://github.com/EvernightFedora/yumex-ng) for RPM management, reimplemented as a native KDE application.

## Features

### RPM Package Management (via dnf5daemon-server)
- **Package Browsing** — View installed, available, and upgradable packages
- **Search** — Find packages by name, summary, or description with scope filtering
- **Package Operations** — Install, update, remove, downgrade, and reinstall RPM packages
- **Transaction Queue** — Queue multiple operations and resolve dependencies before applying
- **Transaction Summary** — Review all actions before confirming, with size calculations
- **Repository Management** — View, enable/disable repositories, and refresh metadata
- **System Upgrade** — Prepare and execute system upgrade transactions
- **Offline Transactions** — Schedule transactions for the next reboot
- **Update Checking** — Automatic update detection with repository priority filtering
- **Progress Feedback** — Real-time download and transaction progress reporting

### External Integrations
- **Linglong Store Launcher** — Click "Linglong Store" in the sidebar to launch `/usr/bin/linglong-store` as an external application
- **Flatpak Support** — Browse and manage Flatpak applications (if Flatpak is installed)

## Architecture

```
miryu/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                     # Application entry point
│   ├── miryuui.rc                   # KDE UI configuration
│   ├── core/
│   │   ├── Enums.h                  # Package states, actions, filters
│   │   ├── Package.h                # Package data model (Q_GADGET)
│   │   ├── Repository.h             # Repository data model
│   │   ├── Transaction.h            # Transaction result/options structs
│   │   ├── Dnf5DaemonClient.h/cpp   # dnf5daemon-server DBus client
│   │   ├── TransactionManager.h/cpp # Transaction building/execution
│   │   ├── PackageCache.h/cpp       # Package caching layer
│   │   ├── Backend.h/cpp            # Unified backend facade
│   │   ├── FlatpakApp.h             # Flatpak app data model
│   │   ├── FlatpakBackend.h/cpp     # Flatpak CLI wrapper backend
│   │   └── UpdateChecker.h/cpp      # Automatic update checking
│   ├── models/
│   │   ├── PackageModel.h/cpp       # QAbstractListModel for packages
│   │   ├── QueueModel.h/cpp         # QAbstractListModel for queue
│   │   ├── RepoModel.h/cpp          # QAbstractListModel for repos
│   │   ├── FlatpakAppModel.h/cpp    # QAbstractListModel for Flatpak apps
│   │   └── ...
│   └── ui/
│       ├── MainWindow.h/cpp         # Main KXmlGuiWindow
│       ├── PackageView.h/cpp        # Package list with custom delegate
│       ├── PackageInfoWidget.h/cpp  # Package details panel
│       ├── QueueView.h/cpp          # Transaction queue view
│       ├── RepoView.h/cpp           # Repository management view
│       ├── ProgressDialog.h/cpp     # Download/transaction progress
│       ├── TransactionResultDialog.h/cpp  # Transaction confirmation
│       ├── FlatpakPage.h/cpp        # Flatpak management page
│       └── ...
├── data/
│   ├── org.miryugaming.PackageManager.desktop
│   └── org.miryugaming.PackageManager.metainfo.xml
└── po/
    └── CMakeLists.txt
```

### dnf5daemon-server Integration

Miryu communicates with `dnf5daemon-server` via the system D-Bus. The service name is `org.rpm.dnf.v0` with the root object path `/org/rpm/dnf.v0`.

**Session lifecycle:**
1. Connect to `org.rpm.dnf.v0.SessionManager` on the system bus
2. Call `open_session()` to create a session and receive a session object path
3. Create interface proxies for `Base`, `rpm.Repo`, `rpm.Rpm`, `Goal`, `Offline`, `comps.Group`, `Advisory`
4. Connect D-Bus signals for download and transaction progress
5. Perform package queries and transaction operations
6. Call `close_session()` when done

**Key D-Bus methods used:**

| Operation | Interface | Method |
|---|---|---|
| Open session | SessionManager | `open_session(options)` |
| Close session | SessionManager | `close_session(path)` |
| List repos | rpm.Repo | `list(options)` |
| List packages | rpm.Rpm | `list(options)` |
| Install | rpm.Rpm | `install(specs, options)` |
| Upgrade | rpm.Rpm | `upgrade(specs, options)` |
| Remove | rpm.Rpm | `remove(specs, options)` |
| Downgrade | rpm.Rpm | `downgrade(specs, options)` |
| Reinstall | rpm.Rpm | `reinstall(specs, options)` |
| Distro sync | rpm.Rpm | `distro_sync(specs, options)` |
| System upgrade | rpm.Rpm | `system_upgrade(options)` |
| Resolve | Goal | `resolve(options)` |
| Execute | Goal | `do_transaction(options)` |
| Cancel | Goal | `cancel()` |
| Reset | Goal | `reset()` |
| Offline status | Offline | `get_status()` |
| Schedule offline | Offline | `schedule_for_next_boot(options)` |

**Signals handled:**

- Download: `download_add_new`, `download_progress`, `download_end`, `download_mirror_failure`
- Key import: `repo_key_import_request`
- Transaction: `transaction_action_start/progress/stop`, `transaction_before_begin`, `transaction_after_complete`, `transaction_script_start/stop/error`, `transaction_verify_start/progress/stop`, `transaction_elem_progress`

## Requirements

### Build dependencies

- C++20 compiler (GCC 13+, Clang 16+)
- CMake 3.20+
- Extra CMake Modules (ECM) 6.0+
- Qt6 (6.5+): Core, Gui, Widgets, DBus, Concurrent
- KDE Frameworks 6: CoreAddons, I18n, XmlGui, Config, WidgetsAddons, ItemViews, IconThemes, KIO, Crash, DBusAddons, Notifications, WindowSystem

### Runtime dependencies

- dnf5daemon-server (the D-Bus daemon for dnf5)
- linglong-store (optional, for the external Linglong Store launcher)
- flatpak (optional, for Flatpak package management)
- Polkit (for privileged operations)
- KDE Plasma 6 runtime (or at least the KF6 runtime libraries)

### Installing on Fedora

```bash
# Install build dependencies
sudo dnf install cmake extra-cmake-modules gcc-c++ \
    qt6-qtbase-devel qt6-qtdeclarative-devel \
    kf6-coreaddons-devel kf6-i18n-devel kf6-xmlgui-devel \
    kf6-config-devel kf6-widgetsaddons-devel kf6-itemviews-devel \
    kf6-iconthemes-devel kf6-kio-devel kf6-crash-devel \
    kf6-dbusaddons-devel kf6-notifications-devel kf6-windowing-devel \
    dnf5daemon-server

# Build and install
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)
sudo make install
```

## Building

### From source

```bash
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

### RPM package

```bash
# Create tarball
tar czf miryu-1.0.0.tar.gz miryu-1.0.0/

# Build RPM
rpmbuild -ta miryu-1.0.0.tar.gz
# or
rpmbuild -ba miryu.spec
```

## Running

```bash
# Ensure dnf5daemon-server is running
sudo systemctl start dnf5daemon-server

# (Optional) Ensure Linglong Store is available for the external launcher
which linglong-store

# Run Miryu
./build/src/miryu-software-center
# or after installation
miryu-software-center
```

## Usage

### RPM Package Management
1. **Browse packages** — Select "Packages" in the sidebar, choose a filter (All/Installed/Available/Updates)
2. **Search** — Type in the search bar and press Enter to search packages
3. **Queue operations** — Right-click a package and select "Queue for Installation/Update/Removal"
4. **View queue** — Click "Queue" in the sidebar to see pending operations
5. **Resolve dependencies** — Click "Resolve Dependencies" to discover required dependencies
6. **Apply** — Click "Apply" to build and execute the transaction
7. **Confirm** — Review the transaction summary and click "Apply" to proceed
8. **Updates** — Click "Updates" to check for available system updates
9. **Repositories** — Click "Repositories" to manage software repositories
10. **System Upgrade** — Use "System Upgrade" in the File menu to prepare a distribution upgrade

### Linglong Store
1. Click "Linglong Store" in the sidebar to launch the external `/usr/bin/linglong-store` application
2. The Linglong Store opens as a separate, standalone application

## License

MIT License
© 2027 KairikiFedora and © 2027 MiryuGaming

## Acknowledgments

- [yumex-ng](https://github.com/EvernightFedora/yumex-ng) — Original Yum Extender NG by Tim Lauridsen, which Miryu is reverse-engineered from
- [dnf5](https://github.com/rpm-software-management/dnf5) — The dnf5daemon-server backend
- [Linyaps](https://linyaps.org.cn/) — The Linglong (玲珑/Linyaps) package management system
- KDE Frameworks 6 — The Qt/KDE libraries powering the UI
