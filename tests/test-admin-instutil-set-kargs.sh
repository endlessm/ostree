#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
# Copyright (C) 2014 Owen Taylor <otaylor@redhat.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive-z2" "syslinux"

echo "1..5"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime
${CMD_PREFIX} ostree admin deploy --os=testos testos:testos/buildmaster/x86_64-runtime

${CMD_PREFIX} ostree admin instutil set-kargs FOO=BAR
${CMD_PREFIX} ostree admin instutil set-kargs FOO=BAZ FOO=BIF TESTARG=TESTVALUE
assert_not_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=BAZ .*FOO=BIF'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*TESTARG=TESTVALUE'
echo "ok instutil set-kargs (basic)"

${CMD_PREFIX} ostree admin instutil set-kargs --merge FOO=BAR
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=BAZ .*FOO=BIF .*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*TESTARG=TESTVALUE'
echo "ok instutil set-kargs --merge"

${CMD_PREFIX} ostree admin instutil set-kargs --merge --replace=FOO=XXX
assert_not_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=XXX'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*TESTARG=TESTVALUE'
echo "ok instutil set-kargs --replace"

${CMD_PREFIX} ostree admin instutil set-kargs --merge --append=FOO=BAR --append=APPENDARG=VALAPPEND --append=APPENDARG=2NDAPPEND testos:testos/buildmaster/x86_64-runtime
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*FOO=XXX.*FOO=BAR'
assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf 'options.*APPENDARG=VALAPPEND .*APPENDARG=2NDAPPEND'
echo "ok instutil set-kargs --append"

${CMD_PREFIX} ostree admin instutil set-kargs --import-proc-cmdline
for arg in $(cat /proc/cmdline); do
    case "$arg" in
	ostree=*) # Skip ostree arg that gets stripped out
	   ;;
	initrd=*|BOOT_IMAGE=*) # Skip options set by bootloader that gets filtered out
	   ;;
	*) assert_file_has_content sysroot/boot/loader/entries/ostree-testos-0.conf "options.*$arg"
	   ;;
    esac
done
echo "ok instutil set-kargs --import-proc-cmdline"
