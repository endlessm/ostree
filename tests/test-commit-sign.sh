#!/bin/bash
#
# Copyright (C) 2013 Jeremy Whiting <jeremy.whiting@collabora.com>
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

set -e

if ! ostree --version | grep -q -e '\+gpgme'; then
    exit 77
fi

. $(dirname $0)/libtest.sh

# Make a copy of the keyrings to be usable unprivileged
cp -r ${SRCDIR}/gpghome ${test_tmpdir}
chmod 0700 ${test_tmpdir}/gpghome

keyid="472CDAFA"
    oldpwd=`pwd`
    mkdir ostree-srv
    cd ostree-srv
    mkdir gnomerepo
    ${CMD_PREFIX} ostree --repo=gnomerepo init --mode="archive-z2"
    mkdir gnomerepo-files
    cd gnomerepo-files 
    echo first > firstfile
    mkdir baz
    echo moo > baz/cow
    echo alien > baz/saucer
    ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "A remote commit" -m "Some Commit body" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome
    mkdir baz/deeper
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "Add deeper" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome
    echo hi > baz/deeper/ohyeah
    mkdir baz/another/
    echo x > baz/another/y
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "The rest" --gpg-sign=$keyid --gpg-homedir=${test_tmpdir}/gpghome
    cd ..
    rm -rf gnomerepo-files

    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir}/ostree-srv ostree
    ostree trivial-httpd --daemonize -p ${test_tmpdir}/httpd-port
    port=$(cat ${test_tmpdir}/httpd-port)
    echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
    cd ${oldpwd} 

    export OSTREE="ostree --repo=repo"

repopath=${test_tmpdir}/ostree-srv/gnomerepo
cp -a ${repopath} ${repopath}.orig

cd ${test_tmpdir}
rm repo -rf

mkdir repo
${CMD_PREFIX} ostree --repo=repo init
${CMD_PREFIX} ostree --repo=repo config  set core.gpgkeyring ${test_tmpdir}/gpghome/pubring.gpg
${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull --no-verify-commits origin main
rm repo -rf

mkdir repo
${CMD_PREFIX} ostree --repo=repo init --gpg-homedir=${test_tmpdir}/gpghome --gpg-keyring=${test_tmpdir}/gpghome/pubring.gpg
${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=repo pull origin main
rm repo -rf
