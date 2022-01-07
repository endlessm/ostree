# This file is to be sourced, not executed

# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

$OSTREE checkout test2 checkout-test2
echo "ok checkout"

cd checkout-test2
assert_has_file firstfile
assert_has_file baz/cow
assert_file_has_content baz/cow moo
assert_has_file baz/deeper/ohyeah
echo "ok content"

cd ${test_tmpdir}
mkdir repo2
ostree_repo_init repo2
${CMD_PREFIX} ostree --repo=repo2 pull-local repo
echo "ok local clone"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo2 checkout test2 test2-checkout-from-local-clone
cd test2-checkout-from-local-clone
assert_file_has_content baz/cow moo
cd ${test_tmpdir}
rm repo2 -rf
echo "ok local clone checkout"

$OSTREE checkout -U test2 checkout-user-test2
echo "ok user checkout"

cd ${test_tmpdir}/checkout-test2
$OSTREE commit -b test2-uid0 -s 'UID 0 test' --owner-uid=0 --owner-gid=0
echo "ok uid0 commit"

cd ${test_tmpdir}
$OSTREE ls test2-uid0 /firstfile > uid0-ls-output.txt
assert_file_has_content uid0-ls-output.txt "-006[64]4 0 0      6 /firstfile" 
echo "ok uid0 ls"

$OSTREE checkout -U test2-uid0 checkout-user-test2-uid0
echo "ok user checkout from uid 0"

cd ${test_tmpdir}
$OSTREE cat test2 /baz/cow > cow-contents
assert_file_has_content cow-contents "moo"
echo "ok cat-file"

cd ${test_tmpdir}
$OSTREE fsck
echo "ok fsck"

mkdir -p test-overlays
date > test-overlays/overlaid-file
$OSTREE commit ${COMMIT_ARGS} -b test-base --base test2 --owner-uid 42 --owner-gid 42 test-overlays/
$OSTREE ls -R test-base > ls.txt
assert_streq "$(wc -l < ls.txt)" 14
assert_streq "$(grep '42.*42' ls.txt | wc -l)" 2
echo "ok commit overlay base"
