# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Release notes

**Always update `CHANGELOG.md` when making user-visible changes.** The file is bundled into the Debian package at `/usr/share/doc/sambervise/CHANGELOG.md` and is referenced by `CPACK_PACKAGE_DESCRIPTION_FILE` in `CMakeLists.txt`, so it ships with every release.

- Add entries under the current in-progress version, or start a new `## [x.y.z] - YYYY-MM-DD` section for a release bump (and update `project(sambervise VERSION ...)` in `CMakeLists.txt` to match).
- Use Keep-a-Changelog sections: `### Added`, `### Changed`, `### Fixed`, `### Removed`.
- Describe changes in user-facing terms (what a DC admin sees), not implementation detail.

## Project Overview

**Sambervise** is a Linux GUI application for remotely administering Samba 4 Active Directory Domain Controllers. It connects to one or more remote DCs over the network — it has no effect on the local machine and requires no local elevated privileges.

## Technology Stack

- **Language**: C (C11)
- **GUI**: GTK 4 + libadwaita 1.x
- **Build system**: CMake + ninja (or make)
- **DC interface**: LDAP/LDAPS via `libldap` (OpenLDAP client) with GSSAPI/Kerberos or simple-bind authentication
- **DNS discovery**: GIO `GResolver` querying `_ldap._tcp.dc._msdcs.<domain>` SRV records
- **Connection profiles**: GKeyFile stored at `~/.config/sambervise/connections.ini`
- **Testing**: GLib test framework (`g_test_*`)

## Architecture

The application follows a layered architecture. The most important constraint is that backend source files must never include GTK or libadwaita headers — only GLib/GIO and protocol library headers.

```
src/
  ui/         # GTK windows, dialogs, widgets (View layer)
  backend/    # Remote DC interaction — GLib/GIO + libldap only, no GTK
  model/      # Plain C structs + GObject types for data (no GTK, no I/O)
  main.c
  sbv-app.c/.h    # AdwApplication subclass; loads profiles, creates window
  sbv-error.h/.c  # SBV_ERROR GQuark + SbvError enum
data/
  ui/         # GtkBuilder XML files (loaded into GResource bundle)
  org.ekm.sambervise.gschema.xml
  org.ekm.sambervise.desktop
  sambervise.gresource.xml
tests/
```

### Backend modules

- `backend/sbv-connection.c` — LDAP connection lifecycle; holds `LDAP*` under a `GMutex`; exposes `acquire_ldap`/`release_ldap` for thread-safe use by other backends
- `backend/sbv-dns.c` — async DNS SRV discovery via `GResolver`; returns a `GListStore<SbvDcTarget>` (private GObject wrapper around `GSrvTarget`)
- `backend/sbv-profiles.c` — load/save/upsert/remove `SbvProfile` objects using `GKeyFile`
- `backend/sbv-users-backend.c` — user LDAP operations (list, enable/disable, password reset)
- `backend/sbv-groups-backend.c` — group LDAP operations (list, add/remove members)
- `backend/sbv-computers-backend.c` — computer LDAP operations (list `(objectClass=computer)`, enable/disable, update description and dNSHostName)

### Model types

- `model/sbv-profile.c` — `SbvProfile` GObject; includes `SbvAuthType` enum (`SBV_AUTH_KERBEROS` / `SBV_AUTH_SIMPLE`)
- `model/sbv-user.c` — `SbvUser` GObject
- `model/sbv-group.c` — `SbvGroup` GObject
- `model/sbv-computer.c` — `SbvComputer` GObject (computer accounts: dn, sam, cn, description, dNSHostName, OS attributes, UAC, pwdLastSet, servicePrincipalName, raw LDAP attrs)

### Authentication

`SbvConnection` supports two auth modes set in `SbvProfile.auth_type`:

- **Kerberos (GSSAPI)** — calls `ldap_sasl_interactive_bind_s(ld, NULL, "GSSAPI", ...)`. Uses the current Kerberos credential cache (`KRB5CCNAME`). The user must have run `kinit` beforehand. No compile-time dependency on `libsasl2-dev`; `libsasl2-modules-gssapi-mit` must be installed at runtime.
- **Simple bind** — calls `ldap_sasl_bind_s` with `LDAP_SASL_SIMPLE` and the password. Suitable for plain LDAP; passwords over the wire require STARTTLS or LDAPS.

The `SbvSaslInteract` struct is defined inline in `sbv-connection.c` to match the SASL2 ABI without requiring the `sasl/sasl.h` header.

### DNS discovery

`sbv_dns_discover_async()` queries `_ldap._tcp.dc._msdcs.<domain>` first (MS-DNS convention for AD DCs), falling back to `_ldap._tcp.<domain>`. Results are sorted by RFC 2782 priority/weight by `GResolver`. The connect dialog's "Discover" button populates a list of found DCs; clicking one fills in the Host and Port fields, and suggests a Base DN derived from the domain (e.g. `example.com` → `DC=example,DC=com`).

### Connection profiles

Profiles are persisted by `sbv-profiles.c` to `~/.config/sambervise/connections.ini` (a `GKeyFile`). Each section is a named profile. Passwords are never stored. The window sidebar always shows the profiles list; clicking a profile connects (Kerberos profiles connect immediately; simple-auth profiles prompt for a password via `SbvPasswordDialog`).

### Async pattern

All backend operations use `GTask` for async dispatch so the UI never blocks. Each backend function follows the GIO async convention:

```c
void sbv_users_list_async  (SbvConnection *conn,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data);
GListStore *sbv_users_list_finish (SbvConnection *conn,
                                   GAsyncResult *result,
                                   GError **error);
```

### Error handling

Backend functions set a `GError` with domain `SBV_ERROR`. LDAP error codes are translated to human-readable messages at the backend boundary; the UI surfaces them via `AdwToast` (non-fatal) or `GtkLabel` inside the relevant dialog.

### GObject usage

Use the GObject type system for anything with property notifications or signals (`SbvConnection`, `SbvApp`, model types). Plain data containers that don't need signals use simple C structs with `g_new`/`g_free`.

## Build & Run

```bash
# Install build dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake ninja-build pkg-config \
    libgtk-4-dev libadwaita-1-dev libldap-dev

# Runtime dependency for Kerberos auth (usually already installed)
sudo apt install libsasl2-modules-gssapi-mit

# Configure and build
cmake -B build -G Ninja
cmake --build build

# Run from the source tree (development)
GSETTINGS_SCHEMA_DIR=build/data ./build/src/sambervise

# Install (handles schema compilation automatically)
sudo cmake --install build
```

## Testing

```bash
# Run all tests
ctest --test-dir build

# Run a single test
ctest --test-dir build -R users

# Verbose output
ctest --test-dir build --output-on-failure

# Under Valgrind
ctest --test-dir build -T memcheck
```

Tests live in `tests/`. They use `g_test_add_func` / `g_test_run` and exercise the model layer only — no live DC or GTK display required. Backend tests requiring LDAP use a `connection_ops` vtable stub (to be wired when backend tests are added).

## CMake structure

```
CMakeLists.txt           # project(), dependencies, subdirectories
src/CMakeLists.txt       # main executable + GResource bundle compilation
data/CMakeLists.txt      # install desktop, schema; compile GSettings schemas
tests/CMakeLists.txt     # one executable per test_*.c
```

UI files in `data/ui/` are compiled into a GResource bundle at build time and accessed at `/org/ekm/sambervise/ui/<name>.ui` — never loaded from disk at runtime.
