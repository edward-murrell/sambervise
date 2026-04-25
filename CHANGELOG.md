# Changelog

All notable changes to Sambervise are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.5] - 2026-04-25

### Added
- Raw LDAP browser panel: a "Raw LDAP" entry in the sidebar (under
  Computers) opens a tree view of the directory rooted at the connection's
  base DN. Each container is expanded lazily with a one-level LDAP search;
  selecting any entry shows its full attribute dump in the right pane,
  using the same renderer as the users/groups/computers detail views.
  Read-only for this release.

## [0.1.4] - 2026-04-24

### Added
- Live UID/GID collision detection: as you type a uidNumber or gidNumber on
  the user/group detail pane, sambervise probes the DC (debounced ~350 ms)
  and shows an inline warning naming the conflicting object(s) when the
  value is already in use elsewhere. The current object is excluded from
  the search so editing without changing the value is silent.
- "Suggest" button next to each UID/GID entry: scans the configured POSIX
  range on the DC and fills the field with the lowest unused integer.
  Range comes from per-profile config in `connections.ini`, optionally
  enriched by the DC's published `msSFU30MaxUidNumber/MaxGidNumber` hints.

## [0.1.3] - 2026-04-24

### Added
- Create computer: "+" button on the computers panel opens a dialog for
  computer name (auto-derives `sAMAccountName=<name>$`), DNS hostname,
  description, and container DN (defaults to `CN=Computers,<base>`). New
  accounts are created with `userAccountControl=WORKSTATION_TRUST_ACCOUNT |
  ACCOUNTDISABLE`.
- Delete computer: "Delete Computer…" button in a Danger Zone section on the
  computer detail page; type-to-confirm modal requires the account name
  (including the trailing `$`).

## [0.1.2] - 2026-04-22

### Added
- Create user: "+" button on the users panel opens a dialog for sAMAccountName,
  first/last/full name and container DN (defaults to `CN=Users,<base>`). New
  accounts are created disabled with no password; password is set from the
  detail page afterwards.
- Delete user: "Delete User…" button in a Danger Zone section on the user
  detail page; opens a type-to-confirm modal that requires the user's
  sAMAccountName before the destructive button activates.
- Create group: "+" button on the groups panel opens a dialog for group name,
  description, scope (Global / Domain Local / Universal), type (Security /
  Distribution) and container DN (defaults to `CN=Users,<base>`).
- Delete group: "Delete Group…" button in a Danger Zone section on the group
  detail page; type-to-confirm modal requires the group's sAMAccountName.
- Computers panel: browse computer accounts with search, view OS details
  (operatingSystem, version, service pack), Service Principal Names, and the
  full LDAP attribute dump.
- Editing of computer description, dNSHostName, and enabled/disabled state.
- `SbvComputer` model type and `sbv-computers-backend` async LDAP operations.
- Per-profile POSIX UID/GID range configuration in `connections.ini`
  (`uid-min`, `uid-max`, `gid-min`, `gid-max`) for upcoming Unix-attribute
  collision detection and auto-assignment.
- Backend helper that reads the DC's `msSFU30MaxUidNumber` /
  `msSFU30MaxGidNumber` hints when present.
- Model unit tests for `SbvComputer` and the new idmap-hints helper.

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
