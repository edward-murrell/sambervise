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
- Follow-ups deferred:
  - Don't show expander arrow for non-container entries.
  - Arbitrary LDAP filter search box.
  - Per-attribute add/modify/delete (editable mode).
  - Sort children alphabetically (currently in DC return order).

## Connection management

### 8. Edit connection profiles after creation
- Currently the connect dialog is "create-only": once a profile is saved
  there's no way to change host, port, base DN, auth type, TLS settings,
  or the new POSIX UID/GID range fields without hand-editing
  `~/.config/sambervise/connections.ini`.
- Wanted: a way to re-open the connect dialog (or an equivalent edit
  dialog) for an existing profile. Likely a context-menu or pencil-icon
  on each row in the sidebar profiles list. On save, `sbv_profiles_upsert`
  already does the right thing (matches by name).
- Decide what happens if the active connection's profile is edited —
  prompt to reconnect, or just take effect on next connect?

## UI visibility / inspection

### 7. Show the active connection's UID/GID ranges in the UI
- Today the per-profile range and the DC-published `msSFU30` hints are only
  observable by reading `connections.ini` or the source. Surface them so an
  admin can confirm "yes, the DC published these and yes, this is the range
  the auto-assign button will use".
- Candidate placements (pick one or more):
  - A small read-only pane in the connect dialog when editing a profile:
    show configured range + a "Probe DC" button that calls
    `sbv_idmap_hints_query_async` and displays what the DC returned.
  - A status row at the top (or bottom) of the users / groups panels showing
    `UID: 10000–29999 (next hint: 12431) — source: msSFU30 + profile config`
    once a connection is active.
  - An "About this connection" item under the profile in the sidebar.
- Make sure the source attribution string from `SbvIdmapHints.source` is
  displayed so the admin knows whether the values came from the DC or just
  from the local config.

## Open questions / deferred

- About page (already in `todo`)
- Krb default-ticket indication in profiles list (already in `todo`)
- Should delete operations also offer a "move to recycle bin" path on AD DCs
  that have the AD Recycle Bin feature enabled? Decide once basic delete
  lands.
