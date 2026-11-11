Name:           sambervise
Version:        %{pkg_version}
Release:        1%{?dist}
Summary:        GTK 4 GUI for administering Samba 4 Active Directory Domain Controllers

License:        GPL-3.0-or-later
URL:            https://github.com/edward-murrell/sambervise
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.18
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libadwaita-1) >= 1.2
BuildRequires:  pkgconfig(krb5)
BuildRequires:  openldap-devel
BuildRequires:  krb5-devel
BuildRequires:  glib2-devel
BuildRequires:  desktop-file-utils

# The SASL GSSAPI mechanism is dlopen()ed by libldap for Kerberos binds, so
# rpm's automatic soname dependencies can't see it.
Requires:       cyrus-sasl-gssapi

%description
Sambervise is a graphical tool for remotely managing users, groups,
computer accounts and contacts on Samba 4 Active Directory Domain
Controllers. It talks to one or more DCs over LDAP/LDAPS, authenticating
with Kerberos (GSSAPI) or a simple bind, and can discover DCs through
DNS SRV records.

It makes no changes to the local machine and needs no local elevated
privileges.

%prep
%autosetup

%build
%cmake -G Ninja
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/au.com.codefoundation.Sambervise.desktop
%ctest

%files
%license COPYING
%{_bindir}/sambervise
%{_datadir}/applications/au.com.codefoundation.Sambervise.desktop
%{_datadir}/glib-2.0/schemas/au.com.codefoundation.Sambervise.gschema.xml
%dir %{_docdir}/%{name}
%{_docdir}/%{name}/CHANGELOG.md

%changelog
* Wed Nov 11 2026 Edward Murrell <edward@murrell.co.nz> - 0.7.0-1
- Add multi-select and bulk Enable, Disable, Reset Password and Move
  for users, and bulk Enable, Disable and Move for computers.
- Add Copy for users: create a new user from an existing one, with its
  container, groups, profile paths and account options.
- Refresh the Groups panel after moving users or computers.

* Tue Nov 10 2026 Edward Murrell <edward@murrell.co.nz> - 0.6.0-1
- Add Disabled, Locked and Expired filters to the Users list, and a
  Disabled filter to the Computers list.
- Tag users whose password has expired.
- Add a Member Of section to users and computers, with Add to Group
  and Remove.
- Fix search missing groups, computers and users with empty fields.
- Fix a warning and a possible crash when closing the Move and Add
  Member dialogs.

* Mon Nov 09 2026 Edward Murrell <edward@murrell.co.nz> - 0.5.0-1
- Add the Windows Profile section to users: profile path, logon script,
  home folder and home drive.
- Show user lockout status and add an Unlock button.
- Add a Refresh button (F5) that reloads from the DC and discards
  unsaved edits.
- Fix operations hanging after the DC connection dropped.
- Fix a warning when closing the Move and Add Member dialogs.

* Sun Nov 08 2026 Edward Murrell <edward@murrell.co.nz> - 0.4.0-1
- Add the OUs panel: create, rename, move and delete organisational units.
- Move users, groups, computers and contacts between containers.
- Fix a crash when closing the window.

* Fri Nov 06 2026 Edward Murrell <edward@murrell.co.nz> - 0.3.1-1
- Store the Unix home directory in unixHomeDirectory, not homeDirectory.

* Tue Nov 03 2026 Edward Murrell <edward@murrell.co.nz> - 0.3.0-1
- Native RPM packaging.
