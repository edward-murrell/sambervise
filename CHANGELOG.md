# Changelog

All notable changes to Sambervise are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- Sidebar connection rows: the dot indicating active vs. inactive
  connection now renders correctly on icon themes that don't inherit
  from Adwaita (e.g. Mint-X). `emblem-ok-symbolic` and
  `media-record-symbolic` are now bundled in the GResource icon set.

### Changed
- Application identity renamed from `org.ekm.sambervise` to
  `au.com.codefoundation.Sambervise`. This affects the GTK
  application id, GSettings schema id and path, GResource prefix,
  desktop file name, and About-dialog maintainer/copyright strings.
  **One-time migration:** GSettings preferences (last host, port,
  base DN, TLS toggles) reset to defaults on first run after upgrade,
  because their dconf path changed. Stored connection profiles in
  `~/.config/sambervise/connections.ini` are unaffected.

## [0.1.12] - 2026-04-30

### Added
- Raw LDAP browser: arbitrary filter search box above the tree. Type an
  LDAP filter (e.g. `(sAMAccountName=alice)`) and press Enter to run a
  subtree search rooted at the connection's base DN; matching entries
  appear as a flat list with their full DN, and selecting one opens it
  in the editor pane the same way as the tree view. Esc clears the
  filter and returns to the tree. Results are capped at 500 entries
  with a "truncated" hint so very broad filters don't lock up the UI.
- Raw LDAP browser: the attribute pane is now editable. Each existing
  attribute renders as a name + value entry with a trash button that
  marks the attribute for deletion (struck through; click again to
  undo). An "Add attribute" button appends a fresh row for adding new
  attributes, and "Save Changes" diffs the editor state against the
  loaded entry and emits a single LDAP modify request batching all
  MOD_ADD / MOD_REPLACE / MOD_DELETE operations. Multi-value attributes
  use `; ` as the value separator. After save the entry is re-read so
  the editor reflects DC-side normalisation.
- Connect / Edit Connection dialog: "Probe" button next to host/port
  performs an anonymous LDAP RootDSE read (using the dialog's TLS /
  LDAPS / skip-cert toggles) and surfaces the DC's hostname, default
  naming context, functionality levels, supported LDAP versions and
  supported SASL mechanisms inline. Useful for sanity-checking
  reachability and TLS settings before committing the profile.
- Edit Connection dialog: "About this connection" header button opens a
  popover summarising the saved profile (host:port, base DN, auth mode,
  bind DN for simple auth, transport flags, POSIX UID/GID ranges) so
  the static profile state is visible without scanning every form field.

### Fixed
- Users panel search: typing into the search box used to hide every row
  for any account where `displayName` was unset (a common case in AD).
  The filter array was NULL-terminated and iterated via `for (...;
  fields[i]; ...)`, so a NULL `displayName` at index 0 short-circuited
  the loop before any field was compared. The filter now iterates by
  count and skips NULL fields explicitly.

## [0.1.11] - 2026-04-28

### Added
- Computer detail panel: editable Service Principal Names section.
  Existing SPNs are listed with per-row remove buttons; a new SPN can
  be added via an entry + Add button (or Enter). Changes are staged
  locally and applied with "Save SPNs", which issues a single LDAP
  `MOD_REPLACE` against `servicePrincipalName`.
- Safety guard: when the selected computer has `pwdLastSet == 0`
  (created but never joined the domain), the SPN editor is disabled
  and a warning explains that the account must be initialized before
  SPNs can be registered — the directory accepts the modify, but the
  resulting SPN is non-functional without key material.

### Fixed
- Connect dialog: the "Discover" button now suggests a Base DN derived from
  the entered domain even when SRV discovery fails (no `_ldap._tcp.dc._msdcs`
  records, DNS error, etc.). Previously the suggestion was only filled on
  the success path, so users with a valid domain but no AD SRV records got
  an error and an empty Base DN field. The Base DN is filled when Discover
  is clicked rather than when it completes.

## [0.1.10] - 2026-04-26

### Added
- Bottom status bar showing the active connection at a glance: server
  (`host:port`), bound identity (the Kerberos principal read from the
  local credential cache via libkrb5, e.g.
  `Authentication: Kerberos (alice@EXAMPLE.COM)`; the bind DN for simple
  auth), and the resolved POSIX UID/GID range with source attribution.
  Only visible while connected.

### Build
- New build dependency on `libkrb5-dev` (and `libkrb5-3` at runtime) so
  the status bar can read the principal from the default credential
  cache.

### Changed
- The UID/GID range strip previously shown at the top of the Users and
  Groups panels has moved into the new window-level status bar, so the
  same information is visible regardless of which panel is open.

## [0.1.9] - 2026-04-26

### Added
- About dialog: a hamburger menu was added to the window header bar with
  an "About Sambervise" entry that opens an `AdwAboutWindow` showing the
  application name, version, license (GPL-3.0), website and issue
  tracker links.

### Changed
- Raw LDAP browser: leaf entries no longer show a tree expander arrow.
  The browser now reads the `hasSubordinates` operational attribute
  alongside each entry's user attributes and suppresses the expander on
  entries the DC reports as childless. Entries where the attribute is
  absent or unparseable still get an expander so they remain
  explorable.

## [0.1.8] - 2026-04-25

### Changed
- Raw LDAP browser: children of each container are now sorted
  alphabetically by label (case-insensitive). Previously they appeared
  in whatever order the DC returned them, which made navigating large
  containers like `CN=Users` slow.

## [0.1.7] - 2026-04-25

### Added
- POSIX UID/GID range fields in the Add/Edit Connection dialog. A new
  collapsible "POSIX UID/GID ranges" section lets you configure the
  per-profile uid-min/uid-max/gid-min/gid-max range without hand-editing
  `connections.ini`. Leaving a field at 0 keeps the built-in default
  (10000–29999). When editing an existing profile, the dialog also shows
  the effective range and where the values came from (profile config or
  defaults).
- Status row at the top of the Users and Groups panels showing the
  resolved UID/GID range for the active connection, plus the DC's
  `msSFU30MaxUidNumber`/`MaxGidNumber` "next hint" values when published,
  and a source attribution string. Populated automatically on connect
  (one DC probe, cached for the session).

## [0.1.6] - 2026-04-25

### Added
- Edit connection profile: a pencil icon on each sidebar profile row opens
  an "Edit Connection" dialog pre-populated from the saved profile. Save
  persists the change to `connections.ini` without attempting to connect.
  Renaming a profile cleanly replaces the old entry. If the edited
  profile is currently active, a toast confirms the changes apply on the
  next connect.

### Fixed
- "Raw LDAP" sidebar entry and the new edit-connection pencil now display
  correctly under icon themes that don't inherit from Adwaita (e.g.
  Mint-X). The two symbolic icons (`view-list-bullet-symbolic`,
  `document-edit-symbolic`) are bundled in the app's GResource and
  registered with the default `GtkIconTheme` at startup, so they render
  regardless of the active icon theme.

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
