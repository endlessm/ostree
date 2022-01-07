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

setup_fake_remote_repo1 "archive"

echo '1..4'

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

for remoteurl in $(cat httpd-address)/ostree/gnomerepo \
		 file://$(pwd)/ostree-srv/gnomerepo; do
cd ${test_tmpdir}
rm repo -rf
mkdir repo
ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin ${remoteurl}

${CMD_PREFIX} ostree --repo=repo pull --subpath=/baz/deeper --subpath=/baz/another origin main

${CMD_PREFIX} ostree --repo=repo ls origin:main /baz/deeper
${CMD_PREFIX} ostree --repo=repo ls origin:main /baz/another
if ${CMD_PREFIX} ostree --repo=repo ls origin:main /firstfile 2>err.txt; then
    assert_not_reached
fi
assert_file_has_content err.txt "Couldn't find file object"
rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:main)
assert_has_file repo/state/${rev}.commitpartial

# Test pulling a file, not a dir
${CMD_PREFIX} ostree --repo=repo pull --subpath=/firstfile origin main
${CMD_PREFIX} ostree --repo=repo ls origin:main /firstfile

${CMD_PREFIX} ostree --repo=repo pull origin main
assert_not_has_file repo/state/${rev}.commitpartial
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok subpaths"

rm -rf repo

ostree_repo_init repo
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin ${remoteurl}

# Pull a directory which is not the first in the commit (/baz/another is before)
${CMD_PREFIX} ostree --repo=repo pull --subpath=/baz/deeper origin main

# Ensure it is there
${CMD_PREFIX} ostree --repo=repo ls origin:main /baz/deeper

# Now prune, this should not prune the /baz/deeper dirmeta even if the
# /baz/another dirmeta is not in the repo.
${CMD_PREFIX} ostree --repo=repo prune --refs-only

# Ensure it is still there
${CMD_PREFIX} ostree --repo=repo ls origin:main /baz/deeper

echo "ok prune with commitpartial"

done
