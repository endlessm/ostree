#!/bin/bash
#
# Copyright (C) 2015 Endless Mobile, Inc.
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
#
# Author: John Hiesey <john@endlessm.com>

set -e

. $(dirname $0)/libtest.sh

setup_fake_remote_repo1 "archive-z2"

echo '1..3'

cd ${test_tmpdir}
rm repo -rf
mkdir repo
${CMD_PREFIX} ostree --repo=repo init
${CMD_PREFIX} ostree --repo=repo remote add --set=gpg-verify=false origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo size-summary origin main | tee sizes.txt
echo "ok size summary run"
assert_file_has_content sizes.txt "files: 0/5 entries"
assert_file_has_content sizes.txt "archived: 0"
assert_file_has_content sizes.txt "unpacked: 0"
echo "ok size summary correct before pull"

${CMD_PREFIX} ostree --repo=repo pull origin main
${CMD_PREFIX} ostree --repo=repo size-summary origin main | tee sizes.txt
assert_file_has_content sizes.txt "files: 5/5 entries"
assert_file_has_content sizes.txt "archived: \([0-9]*\)/\1"
assert_file_has_content sizes.txt "unpacked: \([0-9]*\)/\1"
echo "ok size summary correct after pull"
