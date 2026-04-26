# Sambervise TODO

Working list of upcoming features. Each item captures the agreed scope so we
can pick it up later without re-litigating decisions.

## Pre-requisite

### 0. Expose well-known LDAP locations  *(DONE — 0.1.2)*
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
- Follow-ups (open):
  - Surface the configured range in the connect dialog / profile editor UI
    (currently only editable by hand in `connections.ini`).
  - Display the resolved hints somewhere in the UI so an admin can confirm
    "yes, the DC published these".

## Object lifecycle

### 1. Add/delete users  *(DONE — 0.1.2)*
- Backend: `sbv_users_create_async` (ldap_add_ext_s) +
  `sbv_users_delete_async` (ldap_delete_ext_s).
- Create dialog `ui/sbv-create-user-dialog.{h,c}`: sAMAccountName, given/sn,
  cn (auto-filled from given+sn), container DN (defaults to
  `CN=Users,<base>`, editable). Created disabled, no password.
- Delete: "Delete User…" button in a Danger Zone section. Type-to-confirm
  modal requires the user's sAMAccountName before the destructive button
  activates.

### 2. Add/delete groups  *(DONE — 0.1.2)*
- Backend: `sbv_groups_create_async` (ldap_add_ext_s with groupType bitmask)
  + `sbv_groups_delete_async` (ldap_delete_ext_s).
- Create dialog `ui/sbv-create-group-dialog.{h,c}`: sAMAccountName,
  description, scope dropdown (Global/Domain Local/Universal), type dropdown
  (Security/Distribution), container DN (defaults to `CN=Users,<base>`).
- Delete: "Delete Group…" button in a Danger Zone section on the group
  detail pane. Type-to-confirm modal requires sAMAccountName.
- `groupType` AD bits exported from `sbv-groups-backend.h` so the dialog
  doesn't redefine them.

### 3. Add/delete computers  *(DONE — 0.1.3)*
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

### 4. UID/GID collision detection  *(DONE — 0.1.3)*
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

### 5. UID/GID auto-assignment  *(DONE — 0.1.4)*
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

### 6. Raw LDAP browser panel (read-only first cut)  *(DONE — 0.1.5)*
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
  - *(DONE — 0.1.8)* Sort children alphabetically — `children_thread`
    now sorts the `GListStore<SbvLdapNode>` by label (case-insensitive
    `g_utf8_collate`) before returning.
  - *(DONE — 0.1.9)* Don't show expander arrow for non-container
    entries — `SbvLdapNode` gained a tristate `has_children` field
    populated from the DC's `hasSubordinates` operational attribute
    (requested explicitly alongside `*` since it's operational).
    `create_child_model` returns NULL for known leaves so GTK marks
    them non-expandable. Unknown nodes still get an expander.
  - Arbitrary LDAP filter search box. *(open)*
  - Per-attribute add/modify/delete (editable mode). *(open)*

## Connection management

### 8. Edit connection profiles after creation  *(DONE — 0.1.6)*
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

### 7. Show the active connection's UID/GID ranges in the UI  *(DONE — 0.1.7)*
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
- Deferred follow-ups (open):
  - "Probe DC" button in the connect/edit dialog that calls
    `sbv_idmap_hints_query_async` directly (would need a way to
    acquire/borrow an LDAP connection from edit-mode where there is no
    live conn of its own).
  - An "About this connection" sidebar entry / popover.

## Open questions / deferred

- About page (already in `todo`)
- Krb default-ticket indication in profiles list (already in `todo`)
- Should delete operations also offer a "move to recycle bin" path on AD DCs
  that have the AD Recycle Bin feature enabled? Decide once basic delete
  lands.
