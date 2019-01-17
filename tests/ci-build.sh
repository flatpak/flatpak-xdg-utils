#!/bin/bash

# Copyright © 2015-2019 Collabora Ltd.
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

set -euo pipefail
set -x

NULL=

# ci_distro:
# OS distribution in which we are testing
# Typical values: ubuntu, debian; maybe fedora in future
: "${ci_distro:=ubuntu}"

# ci_docker:
# If non-empty, this is the name of a Docker image. ci-install.sh will
# fetch it with "docker pull" and use it as a base for a new Docker image
# named "ci-image" in which we will do our testing.
#
# If empty, we test on "bare metal".
# Typical values: ubuntu:xenial, debian:jessie-slim
: "${ci_docker:=}"

# ci_parallel:
# A number of parallel jobs, passed to make -j
: "${ci_parallel:=1}"

# ci_sudo:
# If yes, assume we can get root using sudo; if no, only use current user
: "${ci_sudo:=no}"

# ci_suite:
# OS suite (release, branch) in which we are testing.
# Typical values for ci_distro=debian: sid, jessie
# Typical values for ci_distro=fedora might be 25, rawhide
: "${ci_suite:=xenial}"

# ci_test:
# If yes, run tests; if no, just build
: "${ci_test:=yes}"

# ci_test_fatal:
# If yes, test failures break the build; if no, they are reported but ignored
: "${ci_test_fatal:=yes}"

# ci_variant:
# debug or production
: "${ci_variant:=debug}"

if [ -n "$ci_docker" ]; then
    exec docker run \
        --env=ci_docker="" \
        --env=ci_parallel="${ci_parallel}" \
        --env=ci_sudo=yes \
        --env=ci_test="${ci_test}" \
        --env=ci_test_fatal="${ci_test_fatal}" \
        --env=ci_variant="${ci_variant}" \
        --privileged \
        ci-image \
        tests/ci-build.sh
fi

maybe_fail_tests () {
    if [ "$ci_test_fatal" = yes ]; then
        exit 1
    fi
}

# For pip
export PATH="$HOME/.local/bin:$PATH"

srcdir="$(pwd)"

case "$ci_variant" in
    (debug)
        CFLAGS="-fsanitize=address -fsanitize=undefined -fPIE -pie"
        export CFLAGS
        # TODO: Fix the leaks and enable leak checking
        ASAN_OPTIONS=detect_leaks=0
        export ASAN_OPTIONS
        ;;

    (*)
        ;;
esac

meson -Dinstalled_tests=true ci-build

ninja -C ci-build
[ "$ci_test" = no ] || meson test --verbose -C ci-build || maybe_fail_tests

DESTDIR=$(pwd)/DESTDIR ninja -C ci-build install
( cd DESTDIR && find . -ls )

if [ "$ci_sudo" = yes ] && [ "$ci_test" = yes ]; then
    sudo ninja -C ci-build install
    gnome-desktop-testing-runner -d /usr/local/share flatpak-xdg-utils || \
        maybe_fail_tests
fi

# vim:set sw=4 sts=4 et:
