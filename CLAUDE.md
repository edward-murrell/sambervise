# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Sambervise** is a Linux GUI application for remotely administering Samba 4 Active Directory Domain Controllers. It connects to one or more remote DCs over the network — it has no effect on the local machine and requires no local elevated privileges.

## Technology Stack

- **Language**: C (C11)
- **GUI**: GTK 4 + libadwaita
- **Build system**: Meson + ninja
- **DC interface**: LDAP/LDAPS via `libldap` (OpenLDAP client) with GSSAPI/Kerberos authentication
- **DNS management**: AD-integrated DNS zones via LDAP (stored in the domain partition)
- **Testing**: GLib test framework (`g_test_*`)

## Architecture

The application follows a layered architecture. The most important constraint is that backend source files must never include GTK or libadwaita headers — only GLib/GIO and protocol library headers.

```
src/
  ui/         # GTK windows, dialogs, widgets (View layer)
  backend/    # Remote DC interaction — GLib/GIO + libldap only, no GTK
  model/      # Plain C structs + GObject types for data (no GTK, no I/O)
  main.c      # Entry point; creates the AdwApplication and runs it
  app.c/.h    # AdwApplication subclass; top-level wiring
data/
  ui/         # .blp (GTK Blueprint) or .ui XML files
  icons/
  org.ekm.sambervise.gschema.xml
  org.ekm.sambervise.desktop
tests/
  test_users.c
  test_groups.c
  ...
```

### Backend modules (planned)

- `backend/connection.c` — LDAP connection lifecycle: connect, GSSAPI/Kerberos bind, rebind, disconnect; holds the `LDAP *` handle and the domain's base DN
- `backend/users.c` — user account LDAP operations (search, create, modify, delete, enable/disable, password reset)
- `backend/groups.c` — group LDAP operations; member enumeration and modification
- `backend/ous.c` — Organizational Unit tree: list, create, move, delete
- `backend/dns.c` — read/write DNS records stored in `DC=DomainDnsZones` and `DC=ForestDnsZones` partitions via LDAP
- `backend/domain.c` — read domain metadata: functional level, PDC emulator, site topology

### Connection and authentication

The app maintains a `SbvConnection` GObject per server profile. Authentication uses GSSAPI so the user's existing Kerberos ticket (from `kinit` or a desktop SSO session) is used automatically if available. The UI offers a credential dialog for username/password as a fallback, which performs a simple LDAP bind.

Multiple server profiles (host, port, base DN) are stored in GSettings under the `org.ekm.sambervise` schema and selectable from a sidebar.

### Async pattern

All backend operations use `GTask` for async dispatch so the UI never blocks. Each backend function follows the GIO async convention:

```c
void sbv_users_list_async  (SbvConnection *conn,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data);
GSList *sbv_users_list_finish (GAsyncResult *result, GError **error);
```

UI signal handlers call the `_async` variant; tests call a synchronous wrapper built on `g_main_context_iteration`.

### Error handling

Backend functions set a `GError` with the domain `SBV_ERROR` (quark: `sbv-error-quark`). LDAP error codes are translated to human-readable messages at the backend boundary; the UI catches `GError` and surfaces it via `adw_message_dialog_new`.

### GObject usage

Use the GObject type system for anything that needs property change notifications or signal emission (`SbvConnection`, `SbvApplication`, model types). Plain data containers that don't need signals can be simple C structs with `g_new`/`g_free`.

## Build & Run

```bash
# Install build dependencies (Debian/Ubuntu)
sudo apt install build-essential meson ninja-build pkg-config \
    libgtk-4-dev libadwaita-1-dev libldap-dev libsasl2-dev

# Configure
meson setup builddir

# Build
ninja -C builddir

# Run
./builddir/sambervise

# Install
sudo ninja -C builddir install
```

### Blueprint compilation

If using GTK Blueprint (`.blp`) files, `blueprint-compiler` must be installed and is invoked automatically by Meson. Otherwise use hand-written `.ui` XML. UI files are compiled into a GResource bundle at build time — never loaded from disk at runtime.

## Testing

```bash
# Run all tests
meson test -C builddir

# Run a single test suite
meson test -C builddir test_users

# Run with verbose output
meson test -C builddir --verbose

# Run under valgrind
meson test -C builddir --setup=valgrind
```

Tests use `g_test_add_func` / `g_test_run`. LDAP calls are intercepted via a `connection_ops` vtable on `SbvConnection` that tests replace with a stub returning canned LDAP result data, so no live DC is required.

## Meson structure

```
meson.build              # project(), dependency(), subdir() calls
src/meson.build          # main executable; gresource_bundle()
data/meson.build         # install desktop, icons; compile gschemas
tests/meson.build        # one executable per test_*.c, registered with meson test
```
