#!/bin/bash
#
# Copyright (C) 2014 Alexander Larsson <alexl@redhat.com>
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

# We don't want OSTREE_GPG_HOME used for these tests.
unset OSTREE_GPG_HOME

. $(dirname $0)/libtest.sh

skip_without_user_xattrs

echo "1..8"

setup_test_repository "archive"
echo "ok setup"

cd ${test_tmpdir}
mkdir repo2
ostree_repo_init repo2 --mode="bare-user"

${CMD_PREFIX} ostree --repo=repo2 pull-local repo
${CMD_PREFIX} ostree --repo=repo2 fsck
echo "ok pull-local z2 to bare-user"

mkdir repo3
ostree_repo_init repo3 --mode="archive"
${CMD_PREFIX} ostree --repo=repo3 pull-local repo2
${CMD_PREFIX} ostree --repo=repo3 fsck
echo "ok pull-local bare-user to z2"


# Verify the name + size + mode + type + symlink target + owner/group are the same
# for all checkouts
${CMD_PREFIX} ostree checkout --repo repo test2 checkout1
find checkout1 -printf '%P %s %#m %u/%g %y %l\n' | sort > checkout1.files

${CMD_PREFIX} ostree checkout --repo repo2 test2 checkout2
find checkout2 -printf '%P %s %#m %u/%g %y %l\n' | sort > checkout2.files

${CMD_PREFIX} ostree checkout --repo repo3 test2 checkout3
find checkout3 -printf '%P %s %#m %u/%g %y %l\n' | sort > checkout3.files

cmp checkout1.files checkout2.files
cmp checkout1.files checkout3.files
echo "ok checkouts same"

if has_gpgme; then
    # These tests are needed GPG support
    mkdir repo4
    ostree_repo_init repo4 --mode="archive"
    ${CMD_PREFIX} ostree --repo=repo4 remote add --gpg-import ${test_tmpdir}/gpghome/key1.asc origin repo

    if ${CMD_PREFIX} ostree --repo=repo4 pull-local --remote=origin --gpg-verify repo test2 2>&1; then
        assert_not_reached "GPG verification unexpectedly succeeded"
    fi
    echo "ok --gpg-verify with no signature"

    ${OSTREE} gpg-sign --gpg-homedir=${TEST_GPG_KEYHOME} test2  ${TEST_GPG_KEYID_1}

    mkdir repo5
    ostree_repo_init repo5 --mode="archive"
    ${CMD_PREFIX} ostree --repo=repo5 remote add --gpg-import ${test_tmpdir}/gpghome/key1.asc origin repo
    ${CMD_PREFIX} ostree --repo=repo5 pull-local --remote=origin --gpg-verify repo test2
    echo "ok --gpg-verify"

    mkdir repo6
    ostree_repo_init repo6 --mode="archive"
    ${CMD_PREFIX} ostree --repo=repo6 remote add --gpg-import ${test_tmpdir}/gpghome/key1.asc origin repo
    if ${CMD_PREFIX} ostree --repo=repo6 pull-local --remote=origin --gpg-verify-summary repo test2 2>&1; then
        assert_not_reached "GPG summary verification with no summary unexpectedly succeeded"
    fi

    ${OSTREE} summary --update

    if ${CMD_PREFIX} ostree --repo=repo6 pull-local --remote=origin --gpg-verify-summary repo test2 2>&1; then
        assert_not_reached "GPG summary verification with signed no summary unexpectedly succeeded"
    fi

    ${OSTREE} summary --update --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME}

    ${CMD_PREFIX} ostree --repo=repo6 pull-local --remote=origin --gpg-verify-summary repo test2 2>&1

    echo "ok --gpg-verify-summary"
else
    echo "ok --gpg-verify with no signature | # SKIP due GPG unavailability"
    echo "ok --gpg-verify | # SKIP due GPG unavailability"
    echo "ok --gpg-verify-summary | # SKIP due GPG unavailability"
fi

mkdir repo7
ostree_repo_init repo7 --mode="archive"
${CMD_PREFIX} ostree --repo=repo7 pull-local repo
${CMD_PREFIX} ostree --repo=repo7 fsck
for src_object in `find repo/objects -name '*.filez'`; do
    dst_object=${src_object/repo/repo7}
    assert_files_hardlinked "$src_object" "$dst_object"
done
echo "ok pull-local z2 to z2 default hardlink"
