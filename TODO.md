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

### 2. Add/delete groups
- Create-group dialog (sAMAccountName, scope = Global / Domain Local /
  Universal, type = Security / Distribution, container DN).
- Default container: `CN=Users,<base>`, with override.
- Delete action with **type-to-confirm** (sAMAccountName).
- Backend: `ldap_add_ext_s` with the right `groupType` bitmask;
  `ldap_delete_ext_s` for delete.

### 3. Add/delete computers
- Create-computer dialog (cn → derives sAMAccountName=`cn$`, optional
  dNSHostName, container DN).
- Default container: `CN=Computers,<base>`, with override.
- Set `userAccountControl=WORKSTATION_TRUST_ACCOUNT (0x1000)` on create;
  allow created-disabled (add `ACCOUNTDISABLE` bit).
- Delete action with **type-to-confirm** (sAMAccountName).
- Backend: `ldap_add_ext_s`, `ldap_delete_ext_s`.

## RFC2307 / POSIX

### 4. UID/GID collision detection
- Live-as-you-type validation on the uidNumber/gidNumber entries in the user
  and group panels. Debounce ~250 ms.
- Query the current connection's tree only:
  - `(&(objectClass=user)(uidNumber=<n>))` excluding the current DN
  - `(&(objectClass=group)(gidNumber=<n>))` excluding the current DN
- Show inline warning under the field when a collision is found, including the
  conflicting object's DN. Save still allowed (warn, don't block) — admin may
  intentionally reuse.
- Cache results per session to avoid hammering the DC on every keystroke.

### 5. UID/GID auto-assignment
- "Suggest next" button next to the UID and GID entries.
- Range source: read from the well-known LDAP locations exposed by item #0;
  fall back to the per-profile range stored in `connections.ini`.
- Algorithm: scan existing `uidNumber`/`gidNumber` values within the
  configured range and return the lowest unused integer. Prefer
  `next_uid_hint`/`next_gid_hint` if the DC publishes one.

## LDAP browser

### 6. Raw LDAP browser panel (read-only first cut)
- New "Raw LDAP" entry in the sidebar nav, **under "Computers"**.
- `GtkTreeListModel`-backed tree of containers/OUs starting at the base DN,
  lazy-loaded per node (one-level scope on expand).
- Selecting a leaf node shows all attributes using the existing raw-attrs
  renderer (the same one the users/groups/computers detail panes use under
  "All LDAP Attributes").
- **Tree navigation only** to start — no arbitrary LDAP filter search box yet.
- **Read-only** initially. Per-attribute add/modify/delete will land in a
  follow-up iteration once the browser shape settles.

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
