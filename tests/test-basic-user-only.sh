#!/bin/bash
#
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

. $(dirname $0)/libtest.sh

mode="bare-user-only"
setup_test_repository "$mode"
extra_basic_tests=7
. $(dirname $0)/basic-test.sh

$CMD_PREFIX ostree --version > version.yaml
python3 -c 'import yaml; yaml.safe_load(open("version.yaml"))'
echo "ok yaml version"

# Reset things so we don't inherit a lot of state from earlier tests
cd ${test_tmpdir}
rm repo files -rf
ostree_repo_init repo init --mode=bare-user-only

# Init an archive repo where we'll store content that can't go into bare-user
cd ${test_tmpdir}
rm repo-input -rf
ostree_repo_init repo-input init --mode=archive

cd ${test_tmpdir}
cat > statoverride.txt <<EOF
2048 /some-setuid
EOF
mkdir -p files/
echo "a setuid file" > files/some-setuid
chmod 0644 files/some-setuid
$CMD_PREFIX ostree --repo=repo-input commit -b content-with-suid --statoverride=statoverride.txt --tree=dir=files
if $CMD_PREFIX ostree pull-local --repo=repo repo-input 2>err.txt; then
    assert_not_reached "copying suid file into bare-user worked?"
fi
assert_file_has_content err.txt "Content object.*invalid mode.*with bits 040.*"
echo "ok failed to commit suid"

cd ${test_tmpdir}
rm repo-input -rf
ostree_repo_init repo-input init --mode=archive
rm files -rf && mkdir files
if $CMD_PREFIX ostree --repo=repo-input commit -b metadata --tree=dir=files --add-metadata-string='=FOO' 2>err.txt; then
    assert_not_reached "committed an empty metadata key"
fi
assert_file_has_content err.txt "Empty metadata key"
$CMD_PREFIX ostree --repo=repo-input commit -b metadata --tree=dir=files --add-metadata-string='FOO='
echo "ok rejected invalid metadata"

cd ${test_tmpdir}
rm repo-input -rf
ostree_repo_init repo-input init --mode=archive
rm files -rf && mkdir files
echo "a group writable file" > files/some-group-writable
chmod 0664 files/some-group-writable
$CMD_PREFIX ostree --repo=repo-input commit -b content-with-group-writable --tree=dir=files
$OSTREE pull-local repo-input
$OSTREE checkout -U -H content-with-group-writable groupwritable-co
assert_file_has_mode groupwritable-co/some-group-writable 664
echo "ok supported group writable"

cd ${test_tmpdir}
rm repo-input -rf
ostree_repo_init repo-input init --mode=archive
rm files -rf && mkdir files
mkdir files/worldwritable-dir
chmod a+w files/worldwritable-dir
$CMD_PREFIX ostree --repo=repo-input commit -b content-with-dir-world-writable --tree=dir=files
$OSTREE pull-local repo-input
$OSTREE checkout -U -H content-with-dir-world-writable dir-co
assert_file_has_mode dir-co/worldwritable-dir 775
echo "ok didn't make world-writable dir"

if ! skip_one_without_user_xattrs; then
    cd ${test_tmpdir}
    rm repo-input -rf
    rm repo -rf
    ostree_repo_init repo init --mode=bare-user-only
    ostree_repo_init repo-input init --mode=bare-user
    rm files -rf && mkdir files
    echo afile > files/afile
    ln -s afile files/afile-link
    $CMD_PREFIX ostree --repo=repo-input commit --canonical-permissions -b testtree --tree=dir=files
    afile_relobjpath=$(ostree_file_path_to_relative_object_path repo-input testtree /afile)
    afile_link_relobjpath=$(ostree_file_path_to_relative_object_path repo-input testtree /afile-link)
    $CMD_PREFIX ostree pull-local --repo=repo repo-input
    assert_files_hardlinked repo/${afile_relobjpath} repo-input/${afile_relobjpath}
    if files_are_hardlinked repo/${afile_link_relobjpath} repo-input/${afile_link_relobjpath}; then
        assert_not_reached "symlinks hardlinked across bare-user?"
    fi
    $OSTREE fsck -q
    echo "ok hardlink pull from bare-user"
fi

cd ${test_tmpdir}
rm repo -rf
ostree_repo_init repo init --mode=bare-user-only
rm files -rf && mkdir files
echo afile > files/afile
chmod 0777 files/afile
$OSTREE commit ${COMMIT_ARGS} -b perms files
$OSTREE fsck
rm out -rf
$OSTREE checkout --force-copy perms out
assert_file_has_mode out/afile 755
$OSTREE checkout ${CHECKOUT_H_ARGS} --union-identical perms out
assert_file_has_mode out/afile 755
echo "ok automatic canonical perms for bare-user-only"
