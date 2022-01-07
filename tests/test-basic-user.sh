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

skip_without_user_xattrs

mode="bare-user"
setup_test_repository "$mode"

extra_basic_tests=6
. $(dirname $0)/basic-test.sh

# Reset things so we don't inherit a lot of state from earlier tests
rm repo files -rf
setup_test_repository "bare-user"

cd ${test_tmpdir}
objpath_nonexec=$(ostree_file_path_to_object_path repo test2 baz/cow)
assert_file_has_mode ${objpath_nonexec} 644
objpath_ro=$(ostree_file_path_to_object_path repo test2 baz/cowro)
assert_file_has_mode ${objpath_ro} 600
objpath_exec=$(ostree_file_path_to_object_path repo test2 baz/deeper/ohyeahx)
assert_file_has_mode ${objpath_exec} 755
echo "ok bare-user committed modes"

rm test2-checkout -rf
$OSTREE checkout -U -H test2 test2-checkout
cd test2-checkout
assert_file_has_mode baz/cow 644
assert_file_has_mode baz/cowro 600
assert_file_has_mode baz/deeper/ohyeahx 755
echo "ok bare-user checkout modes"

rm test2-checkout -rf
$OSTREE checkout -U -H test2 test2-checkout
touch test2-checkout/unwritable
chmod 0400 test2-checkout/unwritable
$OSTREE commit -b test2-unwritable --tree=dir=test2-checkout
chmod 0600 test2-checkout/unwritable
rm test2-checkout -rf
$OSTREE checkout -U -H test2-unwritable test2-checkout
cd test2-checkout
assert_file_has_mode unwritable 400
echo "ok bare-user unwritable"

rm test2-checkout -rf
$OSTREE checkout -U -H test2 test2-checkout
cat > statoverride.txt <<EOF
=0 /unreadable
EOF
touch test2-checkout/unreadable
$OSTREE commit -b test2-unreadable --statoverride=statoverride.txt --tree=dir=test2-checkout
$OSTREE fsck
rm test2-checkout -rf
$OSTREE checkout -U -H test2-unreadable test2-checkout
assert_file_has_mode test2-checkout/unreadable 400
# Should not be hardlinked
assert_streq $(stat -c "%h" test2-checkout/unreadable) 1
echo "ok bare-user handled unreadable file"

cd ${test_tmpdir}
mkdir -p components/{dbus,systemd}/usr/{bin,lib}
echo dbus binary > components/dbus/usr/bin/dbus-daemon
chmod a+x components/dbus/usr/bin/dbus-daemon
echo dbus lib > components/dbus/usr/lib/libdbus.so.1
echo dbus helper > components/dbus/usr/lib/dbus-daemon-helper
chmod a+x components/dbus/usr/lib/dbus-daemon-helper
echo systemd binary > components/systemd/usr/bin/systemd
chmod a+x components/systemd/usr/bin/systemd
echo systemd lib > components/systemd/usr/lib/libsystemd.so.1

# Make the gid on dbus 81 like fedora
$OSTREE commit -b component-dbus --owner-uid 0 --owner-gid 81 --tree=dir=components/dbus
$OSTREE commit -b component-systemd --owner-uid 0 --owner-gid 0 --tree=dir=components/systemd
rm rootfs -rf
for component in dbus systemd; do
    $OSTREE checkout -U -H component-${component} --union rootfs
done
echo 'some rootfs data' > rootfs/usr/lib/cache.txt
$OSTREE commit -b rootfs --link-checkout-speedup --tree=dir=rootfs
$OSTREE ls rootfs /usr/bin/systemd >ls.txt
assert_file_has_content ls.txt '^-007.. 0 0 .*/usr/bin/systemd'
$OSTREE ls rootfs /usr/lib/dbus-daemon-helper >ls.txt
assert_file_has_content ls.txt '^-007.. 0 81 .*/usr/lib/dbus-daemon-helper'
echo "ok bare-user link-checkout-speedup maintains uids"

cd ${test_tmpdir}
rm -rf test2-checkout
$OSTREE checkout -H -U test2 test2-checkout
# With --link-checkout-speedup, specifying --owner-uid should "win" by default.
myuid=$(id -u)
mygid=$(id -g)
newuid=$((${myuid} + 1))
newgid=$((${mygid} + 1))
$OSTREE commit ${COMMIT_ARGS} --owner-uid ${newuid} --owner-gid ${newgid} \
        --link-checkout-speedup -b test2-linkcheckout-test --tree=dir=test2-checkout
$OSTREE ls test2-linkcheckout-test /baz/cow > ls.txt
assert_file_has_content ls.txt "^-006.. ${newuid} ${newgid} .*/baz/cow"

# But --devino-canonical should override that
$OSTREE commit ${COMMIT_ARGS} --owner-uid ${newuid} --owner-gid ${newgid} \
        -I -b test2-devino-test --table-output --tree=dir=test2-checkout > out.txt
$OSTREE ls test2-devino-test /baz/cow > ls.txt
assert_file_has_content ls.txt "^-006.. ${myuid} ${mygid} .*/baz/cow"
assert_file_has_content out.txt "Content Cache Hits: [1-9][0-9]*"

$OSTREE refs --delete test2-{linkcheckout,devino}-test
echo "ok commit with -I"
