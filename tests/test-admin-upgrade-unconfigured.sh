#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

# Exports OSTREE_SYSROOT so --sysroot not needed.
setup_os_repository "archive" "syslinux"

echo "1..2"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmain/x86_64-runtime
rev=$(${CMD_PREFIX} ostree --repo=sysroot/ostree/repo rev-parse testos/buildmain/x86_64-runtime)
export rev
echo "rev=${rev}"
# This initial deployment gets kicked off with some kernel arguments 
${CMD_PREFIX} ostree admin deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmain/x86_64-runtime
echo "unconfigured-state=Use \"subscription-manager\" to enable online updates for example.com OS" >> sysroot/ostree/deploy/testos/deploy/${rev}.0.origin

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmain/x86_64-runtime
if ${CMD_PREFIX} ostree admin upgrade --os=testos 2>err.txt; then
    assert_not_reached "upgrade unexpectedly succeeded"
fi
assert_file_has_content err.txt "Use.*subscription.*online"

echo "ok error"

${CMD_PREFIX} ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false otheros file://$(pwd)/testos-repo testos/buildmain/x86_64-runtime
${CMD_PREFIX} ostree admin switch --os=testos otheros:testos/buildmain/x86_64-runtime

echo "ok switch"
