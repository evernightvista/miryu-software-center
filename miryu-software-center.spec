# RPM Spec for Miryu Software Center
# Build: rpmbuild -ba miryu.spec


Name:           miryu-software-center
Version:        1.0.1
Release:        1%{?dist}
Summary:        A modern RPM and Linglong software center powered by dnf5daemon

License:        GPL-3.0-or-later
URL:            https://github.com/evernightvista/miryu-software-center
Source0:        miryu-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules >= 6.0
BuildRequires:  qt6-qtbase-devel >= 6.5
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kxmlgui-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  kf6-kwidgetsaddons-devel
BuildRequires:  kf6-kitemviews-devel
BuildRequires:  kf6-kiconthemes-devel
BuildRequires:  kf6-kio-devel
BuildRequires:  kf6-kcrash-devel
BuildRequires:  kf6-kdbusaddons-devel
BuildRequires:  kf6-knotifications-devel
BuildRequires:  kf6-kwindowsystem-devel
BuildRequires:  gettext

Requires:       dnf5daemon-server >= 5.4.6.0
Requires:       linglong-bin
Requires:       /usr/bin/linglong-store
Requires:       qt6-qtbase-gui
Requires:       kf6-kcoreaddons
Requires:       kf6-kxmlgui
Requires:       kf6-kconfig
Requires:       kf6-kwidgetsaddons
Requires:       kf6-kiconthemes
Obsoletes:      miryu-package-manager
Provides:       miryu-package-manager

%description
Miryu is a modern graphical software center for RPM-based Linux distributions.
It uses the dnf5daemon-server D-Bus API for all RPM package operations and
provides an external launcher for the Linglong Store.

%prep
%autosetup -n miryu-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%find_lang miryu-software-center --with-qt --all-name || touch miryu-software-center.lang

install -Dm 644 data/evernight-vista.conf %{buildroot}/%{_datadir}/dnf5/suggest-reboot.d/evernight-vista.conf

%files -f miryu-software-center.lang
%license LICENSE
%doc README.md
%{_bindir}/miryu-software-center
%{_datadir}/applications/org.miryugaming.PackageManager.desktop
%{_datadir}/dnf5/suggest-reboot.d/evernight-vista.conf
%{_datadir}/metainfo/org.miryugaming.PackageManager.metainfo.xml
%{_datadir}/kxmlgui5/miryu/miryuui.rc
%{_datadir}/polkit-1/actions/org.miryugaming.PackageManager.polkit

%changelog
* Sun Sep 27 2026 KairikiFedora <13278297951@sina.cn> - 1.0.1-1
- Rename translation domain to miryu-software-center so the compiled
  catalog is miryu-software-center.mo (rename .pot/.po files, update
  KLocalizedString::setApplicationDomain and spec %%find_lang).
- Fix "Unsupported scope for package filtering \"obsoletes\"" error:
  dnf5daemon's rpm.Rpm.list does not support an obsoletes scope; revert
  to querying only the upgrades scope and rely on latest_limit=0 plus
  name+arch de-duplication and installed-version filtering to surface
  cross-repository upgrade candidates.
- Fix update detection missing cross-repository replacements (e.g. steam
  moving from rpmfusion-nonfree-updates-testing to terra): now query both
  "upgrades" and "obsoletes" scopes, use latest_limit=0 so candidates from
  every repo are returned, de-duplicate by name+arch (not just name), and
  discard candidates that are not newer than the installed version.
- Rewrite UI: remove Settings menu, slim Help menu to only "About Miryu Software Center"
- Custom About dialog without KDE default bug/support links
- Add restart-needed banner with reboot icon (kernel/systemd/kwin/etc. or dnf5 needs-restarting)
- Rename application to Miryu Software Center (update KAboutData, translations, desktop/metainfo/spec/polkit)
- Rename appstream/polkit/desktop identifiers from org.miryu to org.miryugaming
- Rename package and binary to miryu-software-center (CMake target, %{_bindir}, polkit exec.path, desktop Exec)

* Fri Aug 28 2026 jtgg114514 <jtgg114514@outlook.com> - 1.0.1-1
- Remove built-in Linglong store page (LinglongStorePage)
- Remove built-in Linglong installed apps management page (LinglongInstalledPage)
- Replace with external launcher for /usr/bin/linglong-store
- Remove LinglongBackend, LinglongStoreClient, and LinglongAppModel
- Add jtgg114514 as author (jtgg114514@outlook.com)
- Replace linglong-bin dependency with /usr/bin/linglong-store

* Sun Aug 23 2026 KairikiFedora <13278297951@sina.cn> - 1.0.0-2
- Change Icons

* Fri Aug 21 2026 KairikiFedora <13278297951@sina.cn> - 1.0.0-1
- Complete missing i18n translations (Provides, Changelog, Files tabs)
- Cache package details to avoid repeated dnf5 repoquery calls
- Show full version-release in package list and queue view
- Move repository name from list to package info panel
- Fix offline update option to only appear on Apply click
- Remove "Resolve Dependencies" button
- Add protected package removal error dialog
- Add Farna Herry as author
- Initial release of Miryu Software Center
- RPM package management via dnf5daemon-server D-Bus API
- Linglong package management via ll-cli
- Linglong app store integration
- Qt6/KF6 based UI with custom delegates
- Transaction queue with dependency resolution
- Repository management
- System upgrade support
- Offline transaction support
- Translations for zh_CN, zh_TW, ja, ko, de, fr
