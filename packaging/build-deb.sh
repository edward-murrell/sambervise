#!/usr/bin/env bash
# Builds this project's .deb natively for the distro it runs on.
# Run as root inside a clean container of the target release, from the repo root:
#   packaging/build-deb.sh 1.2.0
# Output: out/<name>_<version>+<codename>_<arch>.deb
#
# The "+<codename>" version suffix is the contract with the central package
# repository: it decides which APT suite the package is published in.
set -euo pipefail

UPSTREAM="${1:?usage: $0 <upstream-version>}"
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq --no-install-recommends dpkg-dev >/dev/null
apt-get build-dep -y -qq ./ >/dev/null          # installs Build-Depends from debian/control

. /etc/os-release
CODENAME="$VERSION_CODENAME"
SOURCE=$(sed -n 's/^Source: *//p' debian/control | head -n1)
MAINT=$(sed -n 's/^Maintainer: *//p' debian/control | head -n1)
VERSION="${UPSTREAM}+${CODENAME}"

# Prepend a changelog entry for this build; dpkg takes the version from it.
{
  printf '%s (%s) %s; urgency=medium\n\n' "$SOURCE" "$VERSION" "$CODENAME"
  printf '  * Automated build of %s for %s.\n\n' "$UPSTREAM" "$CODENAME"
  printf ' -- %s  %s\n\n' "$MAINT" "$(date -R)"
  cat debian/changelog
} > debian/changelog.new
mv debian/changelog.new debian/changelog

dpkg-buildpackage -b -us -uc

mkdir -p out
find .. -maxdepth 1 -name "*_${VERSION}_*.deb" ! -name '*-dbgsym_*' -exec cp {} out/ \;
ls -l out/
