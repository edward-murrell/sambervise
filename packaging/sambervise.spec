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
* Fri Nov 06 2026 Edward Murrell <edward@murrell.co.nz> - 0.3.1-1
- Store the Unix home directory in unixHomeDirectory, not homeDirectory.

* Tue Nov 03 2026 Edward Murrell <edward@murrell.co.nz> - 0.3.0-1
- Native RPM packaging.
