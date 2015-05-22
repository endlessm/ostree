#!/bin/bash
#
# Copyright (C) 2015 Dan Nicholson <nicholson@endlessm.com>
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

if ! ${CMD_PREFIX} ostree --version | grep -q -e '\+gpgme'; then
    exit 77
fi

. $(dirname $0)/libtest.sh

setup_test_repository "archive-z2"

echo '1..7'

cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" \
    --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME} \
    --tree=dir=files
find repo/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^1$"

echo "ok compat sign"

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init
${CMD_PREFIX} ostree --repo=local pull-local repo test2
find local/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^1$"

echo "ok compat pull-local"

setup_fake_remote_repo1 "archive-z2"

cd ${test_tmpdir}
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo \
    commit -b main -s "A GPG signed commit" -m "Signed commit body" \
    --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME} \
    --tree=dir=files

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init
${CMD_PREFIX} ostree --repo=local remote add --set=gpg-verify=false origin \
    $(cat httpd-address)/ostree/gnomerepo
${CMD_PREFIX} ostree --repo=local pull origin main
find local/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^1$"

echo "ok compat pull"

# Test pulls from a repo containing a commit made with old ostree
cd ${test_tmpdir}
rm -rf compat-repo
cp -a ${test_srcdir}/compat-repo .
mkdir -p compat-repo/tmp

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init --mode=archive-z2
${CMD_PREFIX} ostree --repo=local pull-local compat-repo main
find local/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^1$"
find local/objects -name '*.sizes2' | wc -l > sizescount
assert_file_has_content sizescount "^1$"

echo "ok compat pull-local from old repo"

ln -s ${test_tmpdir}/compat-repo ${test_tmpdir}/httpd/

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init --mode=archive-z2
${CMD_PREFIX} ostree --repo=local remote add --set=gpg-verify=false origin \
    $(cat httpd-address)/compat-repo
${CMD_PREFIX} ostree --repo=local pull origin main
find local/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^1$"
find local/objects -name '*.sizes2' | wc -l > sizescount
assert_file_has_content sizescount "^1$"

echo "ok compat pull from old repo"

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init --mode=archive-z2
${CMD_PREFIX} ostree --repo=local remote add origin \
    $(cat httpd-address)/compat-repo
${CMD_PREFIX} ostree --repo=local pull origin main

echo "ok compat pull from old repo with verification"

${CMD_PREFIX} ostree --repo=local refs --delete origin:main
${CMD_PREFIX} ostree --repo=local prune --depth=-1 --refs-only
find local/objects -type f | wc -l > objcount
assert_file_has_content objcount "^0$"

echo "ok compat prune"
