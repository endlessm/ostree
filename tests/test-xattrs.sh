#!/bin/bash
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

skip "We don't really have a use case for committing user. xattrs right now. See also https://github.com/ostreedev/ostree/issues/758"

# Dead code below
skip_without_user_xattrs

echo "1..2"

setup_test_repository "archive"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=repo checkout test2 test2-checkout1
setfattr -n user.ostree-test -v testvalue test2-checkout1/firstfile
setfattr -n user.test0 -v moo test2-checkout1/firstfile
${CMD_PREFIX} ostree --repo=repo commit -b test2 -s xattrs --tree=dir=test2-checkout1
rm test2-checkout1 -rf
echo "ok commit with xattrs"

${CMD_PREFIX} ostree --repo=repo checkout test2 test2-checkout2
getfattr -m . test2-checkout2/firstfile > attrs
assert_file_has_content attrs '^user\.ostree-test'
assert_file_has_content attrs '^user\.test0'
getfattr -n user.ostree-test --only-values test2-checkout2/firstfile > v0
assert_file_has_content v0 '^testvalue$'
getfattr -n user.test0 --only-values test2-checkout2/firstfile > v1
assert_file_has_content v1 '^moo$'
echo "ok checkout with xattrs"
