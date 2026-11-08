# Sambervise TODO

Working list of upcoming features. Each item captures the agreed scope so we
can pick it up later without re-litigating decisions.

Planned work follows the order in `sambervise-roadmap.md`. Roadmap items are
numbered R1 to R18. Older items keep their original numbers (#0 to #13) and
are listed under "Completed" once done.

Versioning: each new feature ships as a minor version bump (0.3.x → 0.4.0).
Bug fixes ship as a patch bump.

# Planned

## Phase 1: Finish the ADUC basics

### R1. OU management and moving objects  *(DONE in 0.4.0)*
- Backend: `backend/sbv-ou-backend.{h,c}` lists, creates, updates
  (rename + description), deletes and moves. `sbv_ou_move_object_async`
  works for any object and uses `ldap_rename_s` with a new superior.
- DN helpers: `backend/sbv-dn.{h,c}` for RFC 4514 escaping, splitting at
  unescaped commas, ancestry checks and display paths. Tested in
  `tests/test-dn.c`.
- Model: `model/sbv-ou.{h,c}` (`SbvOu`, kind OU / container / domain,
  critical flag, tree-order comparator). Tested in `tests/test-ous.c`.
- UI: "OUs" sidebar panel, `ui/sbv-create-ou-dialog`, and a shared
  `ui/sbv-move-dialog` used by the OU panel and the "Move…" button on
  users, groups, computers and contacts.
- The list shows OUs and containers without `showInAdvancedViewOnly`,
  matching ADUC's default view. Only non-critical OUs can be renamed,
  moved or deleted. Delete is refused for non-empty OUs.
- The OU panel emits "directory-changed" after a rename, move or delete,
  and the window reloads the other panels.
- Follow-ups (open):
  - Container picker in the create dialogs instead of a typed DN.
  - The existing user and contact rename code splits DNs on the first
    comma, so a CN containing an escaped comma renames wrongly. Switch it
    to `sbv_dn_split`.
  - GPO links (R15), delegation (R17) and move-to-OU in bulk (R8) build on
    this.

### R2. Search box and basic filters  *(open, replaces #12)*
- Every panel already has a search box that filters the loaded list.
- Extend matching to name and sAMAccountName where it isn't already.
- Add toggles for disabled, locked out and password expired.
- Deferred from #12: password age, stale computers (no logon in N days)
  and missing DNS registration.

### R3. Lockout status and unlock  *(open)*
- Show lockout state on the user pane.
- Unlock button clears `lockoutTime`.
- The R2 "locked out" toggle depends on the same lockout read.

### R4. "Member Of" on users and computers  *(open)*
- `memberOf` is a read-only back-link. Display it on the user and
  computer panes.
- Edit membership by modifying each group's `member` attribute.

### R5. Profile fields  *(DONE in 0.5.0)*
- Done: Windows Profile section on users for `profilePath`,
  `scriptPath`, `homeDirectory` and `homeDrive`.
- Open: R9 (copy from template) should copy these, with the username
  substituted in paths.

## Phase 2: The escape hatch and bulk work

### R6. Editable raw LDAP attributes  *(partly done in 0.1.12)*
- Done: per-attribute add, modify and delete in the Raw LDAP browser,
  saved as one batched modify.
- Open: type-aware editors for multi-valued strings and integers.
  Multi-valued attributes are still edited as one "; "-joined string.
- Open: show `objectSid`, `objectGUID` and timestamps in readable form,
  read-only.

### R7. Arbitrary LDAP filter search  *(partly done in 0.1.12)*
- Done: filter search box in the Raw LDAP browser (see #6 follow-ups).
- Open: saved queries.

### R8. Bulk operations  *(open, replaces #11)*
- Multi-select on the users and computers lists. No panel supports
  multi-select yet.
- Actions: enable, disable, reset password, and move to OU (needs R1).
- Confirmation dialog showing the count and the first N items.
- Backend: one `ldap_modify_ext_s` per object on a single connection.

### R9. Copy user from template  *(open)*
- Copy group membership, OU, profile path and account flags to a new
  user.
- Needs R4 (membership) and R5 (profile fields).

## Phase 3: DNS

### R10. DNS management  *(open)*
- Zones live in LDAP as `dnsZone` and `dnsNode` objects under the
  `DomainDnsZones` and `ForestDnsZones` partitions. No RPC needed.
- Main work: parse and build the binary `dnsRecord` blobs (MS-DNSP).
- First cut: A, AAAA, CNAME and PTR records in existing zones.
- Later: zone creation, SRV, MX and TXT.

## Phase 4: Domain-level policy and structure

### R11. Domain password and lockout policy  *(open)*
- Form over attributes on the domain root: `minPwdLength`,
  `lockoutThreshold`, `maxPwdAge` and related.

### R12. Fine-grained password policies (PSOs)  *(open)*
- `msDS-PasswordSettings` objects in the Password Settings Container,
  applied to users or groups. Reuses the R11 form.

### R13. Sites and subnets  *(open)*
- Start with a read-only view of which DC sits in which site.
- Then create sites and subnets and assign subnets to sites, all in the
  Configuration partition.

### R14. FSMO role owner display  *(open)*
- Read `fSMORoleOwner` for health checks.
- Transfers stay with `samba-tool fsmo`.

## Phase 5: Harder but high-value

### R15. GPO links  *(open, needs R1)*
- Link, unlink, order, enforce and block inheritance via `gPLink` and
  `gPOptions` on OUs and the domain.
- Ship before any GPO editing.

### R16. GPO creation and deletion  *(open)*
- Needs a `groupPolicyContainer` object plus the SYSVOL folder over SMB,
  which adds a libsmbclient dependency.
- Editing policy contents stays with `samba-tool gpo` and Windows GPMC.

### R17. Object security and delegation  *(open)*
- View and edit `nTSecurityDescriptor`. Needs the SD_FLAGS control,
  SDDL or binary parsing, and an ACL editor.
- First cut: read-only effective-permissions view.

### R18. Recycle Bin support  *(open)*
- Restore objects from the Deleted Objects container with the
  show-deleted control.
- Check support in target Samba versions first.
- Settles the "move to recycle bin" open question below.

## Skip or defer

- Replication status and forcing replication. These need DRSUAPI RPC, and
  `samba-tool drs showrepl` already covers them.
- Trusts. Rare in Samba deployments.
- Mail-enabled groups (#10). Scope kept below. Comes after Phases 1 to 3.

# Completed and older items

## Pre-requisite

### 0. Expose well-known LDAP locations  *(DONE in 0.1.2)*
- Backend module: `backend/sbv-idmap-hints.{h,c}` with `SbvIdmapHints` struct
  (`uid_min/uid_max/gid_min/gid_max/next_uid_hint/next_gid_hint/source`).
- Reads `msSFU30MaxUidNumber` / `msSFU30MaxGidNumber` from
  `CN=ypservers,CN=ypServ30,CN=RpcServices,CN=System,<base>` when present.
- `SbvProfile` extended with `uid-min/uid-max/gid-min/gid-max` keys persisted
  in `connections.ini`; profile range overrides defaults, defaults are
  10000–29999.
- `sbv_idmap_hints_from_profile()` for sync use; `sbv_idmap_hints_query_async()`
  for live DC query that merges DC hints with profile config.
- Tests: `tests/test-idmap.c` (defaults, profile override, profile copy).
- Follow-ups:
  - *(DONE in 0.1.7)* Surface the configured range in the connect dialog
    and profile editor (see #7).
  - *(DONE in 0.1.10)* Display the resolved hints in the UI (window
    status bar).

## Object lifecycle

### 1. Add/delete users  *(DONE in 0.1.2)*
- Backend: `sbv_users_create_async` (ldap_add_ext_s) +
  `sbv_users_delete_async` (ldap_delete_ext_s).
- Create dialog `ui/sbv-create-user-dialog.{h,c}`: sAMAccountName, given/sn,
  cn (auto-filled from given+sn), container DN (defaults to
  `CN=Users,<base>`, editable). Created disabled, no password.
- Delete: "Delete User…" button in a Danger Zone section. Type-to-confirm
  modal requires the user's sAMAccountName before the destructive button
  activates.

### 2. Add/delete groups  *(DONE in 0.1.2)*
- Backend: `sbv_groups_create_async` (ldap_add_ext_s with groupType bitmask)
  + `sbv_groups_delete_async` (ldap_delete_ext_s).
- Create dialog `ui/sbv-create-group-dialog.{h,c}`: sAMAccountName,
  description, scope dropdown (Global/Domain Local/Universal), type dropdown
  (Security/Distribution), container DN (defaults to `CN=Users,<base>`).
- Delete: "Delete Group…" button in a Danger Zone section on the group
  detail pane. Type-to-confirm modal requires sAMAccountName.
- `groupType` AD bits exported from `sbv-groups-backend.h` so the dialog
  doesn't redefine them.

### 3. Add/delete computers  *(DONE in 0.1.3)*
- Backend: `sbv_computers_create_async` (ldap_add_ext_s with
  `userAccountControl=WORKSTATION_TRUST_ACCOUNT|ACCOUNTDISABLE`) +
  `sbv_computers_delete_async` (ldap_delete_ext_s).
- Create dialog `ui/sbv-create-computer-dialog.{h,c}`: computer name
  (auto-derives sAMAccountName=`<name>$`), optional DNS hostname,
  description, container DN (defaults to `CN=Computers,<base>`).
- Delete: "Delete Computer…" button in a Danger Zone section. Type-to-confirm
  requires the full sAMAccountName (including trailing `$`).
- Auto-select the just-created computer in the list (DN-based reselection,
  same pattern as users).

## RFC2307 / POSIX

### 4. UID/GID collision detection  *(DONE in 0.1.3)*
- Backend: `backend/sbv-collisions.{h,c}` —
  `sbv_collisions_uid_check_async` (filter
  `(&(objectClass=user)(uidNumber=<n>))`) and
  `sbv_collisions_gid_check_async` (filter
  `(&(objectClass=group)(gidNumber=<n>))`). Both exclude a caller-supplied
  DN and return a `GPtrArray<SbvCollisionHit*>`.
- Users panel: rfc_uid_entry / rfc_gid_entry get debounced (~350 ms)
  collision checks; inline warning labels under each entry name the first
  two conflicts (with "+N more" if there are more). Save still allowed —
  warn, don't block.
- Groups panel: same wiring on rfc_gid_entry.
- Per-keystroke caching deferred — the debounce + race-cancel (drop result
  if the entry text has changed since the request) was sufficient for v1.
  Revisit if DC traffic becomes noticeable.

### 5. UID/GID auto-assignment  *(DONE in 0.1.4)*
- Backend: `sbv_collisions_next_free_uid_async/finish` and
  `_gid_async/finish` — bounded LDAP search over `[range_min, range_max]`
  for the chosen attribute, sort taken values, return the first gap (-1
  if exhausted).
- "Suggest" button next to each UID/GID entry on the user panel and the
  GID entry on the group panel. First press triggers a one-shot
  `sbv_idmap_hints_query_async` to load the range, cached per-connection
  thereafter; subsequent presses just run the next-free scan.
- If the DC publishes `msSFU30MaxUidNumber/MaxGidNumber`, the scan starts
  from there to shorten the work the DC has to do.
- Side benefit: required `SbvConnection` to retain the bound `SbvProfile`
  (new `sbv_connection_get_profile` accessor) so panels don't need a
  separate profile parameter.

## LDAP browser

### 6. Raw LDAP browser panel (read-only first cut)  *(DONE in 0.1.5)*
- New "Raw LDAP" entry in the sidebar nav, under "Computers".
- Model: `model/sbv-ldap-node.{h,c}` (`SbvLdapNode` GObject — DN, label,
  lazy children store, loaded flag, raw attrs hashtable).
- Backend: `backend/sbv-ldap-browse.{h,c}` —
  `sbv_ldap_browse_children_async` (LDAP one-level search, returns a
  `GListStore<SbvLdapNode>` with each child's full attribute dump) and
  `sbv_ldap_browse_attrs_async` (base scope, used for the root which has
  no parent listing).
- UI: `ui/sbv-ldap-panel.{h,c}` — `GtkBox` subclass with a left
  `GtkListView` over a `GtkTreeListModel` (lazy `create_child_model`
  callback issues the LDAP search on first expand) and a right detail
  pane that renders the selected entry's attributes using the same
  raw-attrs row format as the other panels.
- Follow-ups:
  - *(DONE in 0.1.8)* Sort children alphabetically — `children_thread`
    now sorts the `GListStore<SbvLdapNode>` by label (case-insensitive
    `g_utf8_collate`) before returning.
  - *(DONE in 0.1.9)* Don't show expander arrow for non-container
    entries — `SbvLdapNode` gained a tristate `has_children` field
    populated from the DC's `hasSubordinates` operational attribute
    (requested explicitly alongside `*` since it's operational).
    `create_child_model` returns NULL for known leaves so GTK marks
    them non-expandable. Unknown nodes still get an expander.
  - *(DONE in 0.1.12)* Arbitrary LDAP filter search box. Saved queries
    are tracked under R7.
  - *(DONE in 0.1.12)* Per-attribute add/modify/delete (editable mode).
    Type-aware editors are tracked under R6.

## Mail-enabled objects

### 9. Contacts  *(DONE in 0.3.0)*
- Backend: `backend/sbv-contacts-backend.{h,c}` —
  `sbv_contacts_list_async/finish`, `sbv_contacts_create_async/finish`,
  `sbv_contacts_delete_async/finish`, `sbv_contacts_update_async/finish`.
- Model: extend or add `model/sbv-contact.{h,c}` GObject with mail, phone,
  address fields (mail, proxyAddresses, displayName, cn, sn, givenName,
  telephoneNumber, mobile, facsimileTelephoneNumber, physicalDeliveryOfficeName,
  streetAddress, l, st, postalCode, co).
- UI: new "Contacts" sidebar nav entry + detail pane.
- Create dialog `ui/sbv-create-contact-dialog.{h,c}`: given/sn, mail, phone,
  container DN (defaults to `CN=Users,<base>`).

### 10. Mail-enable groups (distribution lists)  *(deferred, see roadmap)*
- Backend: extend `sbv_groups_update_async` to handle mail attributes
  (mail, proxyAddresses, displayName).
- UI: "Mail" section in group detail pane with editable mail and
  proxyAddresses fields.
- Note: requires Exchange for advanced mail routing; Samba 4 supports schema
  but not full mailbox/moderation features.

## Bulk operations

### 11. Bulk enable/disable and password reset  *(moved to R8)*
- Extend users/computers list views with checkbox selection mode.
- Bulk action buttons: "Enable Selected", "Disable Selected",
  "Reset Password for Selected".
- Confirmation dialog showing count and optionally first N items.
- Backend: batch LDAP modify operations (single connection, multiple
  ldap_modify_ext_s calls or single modify with multiple values if
  supported).

## Kerberos principals

### 13. Computer SPN management  *(DONE in 0.1.11)*
- Backend: `sbv_computers_set_spn_async/finish` issues a single
  `LDAP_MOD_REPLACE` against `servicePrincipalName`. Empty list collapses
  to an attribute delete.
- UI: SPN section in the computer detail panel — list of current SPNs
  with per-row trash buttons, an entry + Add button (Enter activates) for
  new SPNs, and a "Save SPNs" button gated on the working list differing
  from the loaded values. Edits are staged locally; nothing hits the DC
  until Save.
- Safety guard: when `pwdLastSet == 0` the entry, Add, Save, and per-row
  remove buttons are all disabled and a warning banner explains that the
  account must be initialised before SPNs can be registered.
- Lenient validation only — `service/host` shape and de-duplication; the
  DC remains the source of truth for SPN syntax.

## Search and filtering

### 12. Advanced search / filtering  *(moved to R2)*
- Add search box to users/groups/computers panels.
- Filters: enabled/disabled, password age, stale computers (no logon
  in N days), missing DNS registration.
- Filter bar with toggles or dropdown menu for common predicates.
- Backend: LDAP filter construction (e.g. `(&(objectClass=user)(disabled=...))`,
  timestamp comparisons on `pwdLastSet`, etc.).

## Connection management

### 8. Edit connection profiles after creation  *(DONE in 0.1.6)*
- New `sbv_edit_dialog_new` reuses the connect-dialog UI in "save without
  connecting" mode (title "Edit Connection", action button "Save").
- Pencil icon (`document-edit-symbolic`, flat) added at the trailing edge
  of each sidebar profile row; clicking it opens the edit dialog
  pre-populated from the saved profile. Clicking the pencil consumes its
  own gesture and does not trigger row-activated / connect.
- Rename support: when the profile name changes during edit,
  `sbv_profiles_remove(old_name)` runs before `sbv_profiles_upsert(new)`
  so the rename actually replaces the entry instead of duplicating it.
- Active-connection edit: changes are persisted but the running session
  is left alone. A toast tells the admin the changes apply on next
  connect.
- POSIX UID/GID range fields are still hand-edited in `connections.ini`;
  surfacing them in the connect/edit dialog is tracked under #7.

## UI visibility / inspection

### 7. Show the active connection's UID/GID ranges in the UI  *(DONE in 0.1.7)*
- Add/Edit Connection dialog gained a collapsible "POSIX UID/GID ranges"
  section with editable spinners for uid-min/uid-max/gid-min/gid-max.
  0 = "use default", which translates to `-1` in the profile so the
  persistence layer omits the key. Edit-mode also shows a one-line
  "Effective range … (source: …)" caption built from
  `sbv_idmap_hints_from_profile`, surfacing `SbvIdmapHints.source`.
- Users and Groups panels gained a top status row showing the resolved
  range, DC-published `next_*_hint` values, and the source attribution.
  Populated by `sbv_idmap_hints_query_async` on panel load (one probe per
  connection, cached for the session — Suggest reuses the cache).
  *(0.1.10: the per-panel row was removed and the same information moved
  into a new window-level status bar that also shows server and
  principal — see "Status bar" item below.)*
- Deferred follow-ups:
  - *(DONE in 0.2.1)* "Probe DC" button now performs a bound idmap
    read in addition to the anonymous RootDSE summary. `sbv-probe`
    grew an optional bind step (Kerberos or simple, mirroring
    `sbv-connection`) that opens a fresh handle, binds with the
    dialog's profile + password, and queries the SFU30 idmap location
    directly — no shared connection borrowing needed. Results render
    inline in the existing probe frame.
  - *(DONE in 0.1.12)* "About this connection" popover.

## Window chrome

### Status bar  *(DONE in 0.1.10)*
- Window-level status bar at the bottom shows the active connection's
  server (`host:port`), authentication mode (Kerberos vs simple-bind
  DN), and the resolved POSIX UID/GID range with source attribution.
  Only visible while connected. Populated from
  `sbv_idmap_hints_query_async` once per connect.

## Bugs

### ~~Unix home directory written to the Windows home attribute~~ *(DONE in 0.3.1)*
- The Unix attributes section read and wrote `homeDirectory`, the Windows
  home folder. The RFC2307 attribute in AD is `unixHomeDirectory`, which
  winbind and sssd read.
- Fix: the users backend now uses `unixHomeDirectory`. The model accessors
  are renamed to `sbv_user_{get,set}_unix_home_dir` so R5 can add the
  Windows home folder without a clash.

### ~~Bogus `ekm` / `org.ekm` identity~~ *(DONE)*
- Renamed to `au.com.codefoundation.Sambervise` across gschema
  (id + path + filename), desktop file, gresource prefix, GtkBuilder
  resource paths, icon resource path, and About-dialog
  maintainer/copyright. No migration shim — GSettings prefs reset on
  first run after upgrade (documented in CHANGELOG). Connection
  profiles in `~/.config/sambervise/connections.ini` are unaffected.

### ~~Connection icon broken~~ *(DONE)*
- Root cause: `emblem-ok-symbolic` (active) and `media-record-symbolic`
  (inactive) are referenced by `make_profile_row()` in `sbv-window.c`
  but weren't bundled in the GResource. On icon themes that don't
  inherit from Adwaita (e.g. Mint-X) they resolved to the broken-image
  fallback. Fix: bundle both SVGs (copied from Adwaita) under
  `data/icons/scalable/actions/` and add them to
  `sambervise.gresource.xml`. The existing
  `gtk_icon_theme_add_resource_path` call in `sbv-app.c` picks them up.

## Safety warnings

### ~~Warn when assigning a gidNumber to Domain Admins~~ *(DONE)*
- Backend now requests `objectSid` for groups and parses the trailing
  RID into `SbvGroup.rid` (locale-invariant, unlike `cn`).
- Groups panel: when "Save Unix Attributes" is clicked and a
  `gidNumber` is being set (not cleared) on a group whose RID is one
  of {512 Domain Admins, 518 Schema Admins, 519 Enterprise Admins},
  an `AdwMessageDialog` explains the `ID_TYPE_BOTH` breakage and
  offers Cancel / Save Anyway (destructive). Confirming proceeds
  with the modify; cancelling aborts. Doesn't hard-block.
- Could extend later to BUILTIN groups (`S-1-5-32-*` like
  Administrators = 544) by also tracking the domain-vs-builtin SID
  prefix, but the well-known domain-RID set covers the practical
  Sysvol-breakage cases.

## Open questions / deferred

- ~~About page (already in `todo`)~~ *(DONE in 0.1.9: hamburger menu in
  header bar opens an `AdwAboutWindow`.)*
- Krb default-ticket indication in profiles list. *(open, not on the roadmap)*
- Should delete operations also offer a "move to recycle bin" path on AD DCs
  that have the AD Recycle Bin feature enabled? Decide as part of R18.
