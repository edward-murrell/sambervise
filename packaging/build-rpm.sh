#!/usr/bin/env bash
# Builds this project's .rpm natively for the distro it runs on.
# Run as root inside a clean almalinux:9 / almalinux:10 / fedora container,
# from the repo root:
#   packaging/build-rpm.sh 1.2.0
# Output: out/<name>-<version>-1.<dist>.<arch>.rpm  (dist = el9, el10, fc44, ...)
#
# The spec must use:   Version: %{pkg_version}   and   Release: 1%{?dist}
# The dist tag is the contract with the central package repository: it decides
# which rpm/<releasever>/ folder the package is published in.
set -euo pipefail

VERSION="${1:?usage: $0 <upstream-version>}"
SPEC=$(ls packaging/*.spec | head -n1)
NAME=$(sed -n 's/^Name: *//p' "$SPEC" | head -n1)

# Read only ID: sourcing os-release would clobber VERSION with the distro version.
OS_ID=$(. /etc/os-release && echo "$ID")
dnf install -y -q dnf-plugins-core rpm-build
if [ "$OS_ID" != fedora ]; then
  # EL: CodeReady Builder (-devel packages) and EPEL (extra libraries)
  dnf config-manager --set-enabled crb
  dnf install -y -q epel-release
fi

mkdir -p ~/rpmbuild/SOURCES
tar --exclude=./.git --exclude=./out --exclude=./build \
    --transform "s,^\.,$NAME-$VERSION," \
    -czf ~/rpmbuild/SOURCES/"$NAME-$VERSION".tar.gz .

dnf builddep -y -q --define "pkg_version $VERSION" "$SPEC"
rpmbuild -bb --define "pkg_version $VERSION" "$SPEC"

mkdir -p out
find ~/rpmbuild/RPMS -name '*.rpm' ! -name '*-debuginfo-*' ! -name '*-debugsource-*' -exec cp {} out/ \;
ls -l out/
