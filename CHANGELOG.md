# Changelog

All notable changes to Sambervise are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.2] - 2026-04-22

### Added
- Computers panel: browse computer accounts with search, view OS details
  (operatingSystem, version, service pack), Service Principal Names, and the
  full LDAP attribute dump.
- Editing of computer description, dNSHostName, and enabled/disabled state.
- `SbvComputer` model type and `sbv-computers-backend` async LDAP operations.
- Model unit tests for `SbvComputer` (properties, UAC/enabled, SPN ownership).

## [0.1.1] - prior release

### Added
- Display of extended LDAP attributes and Unix (RFC2307) parameters for
  users and groups.
- Debian packaging via CPack with `postinst`/`postrm` for GSettings schema
  compilation.

## [0.1.0] - initial release

### Added
- User management: list, edit names/contact details, enable/disable,
  reset password, and configure password policy.
- Group management: list, view members, add/remove members.
- Multiple connection profiles stored in `~/.config/sambervise/connections.ini`.
- Kerberos (GSSAPI) and simple-bind authentication.
- Automatic DC discovery via DNS SRV records.
- LDAPS and STARTTLS support.
