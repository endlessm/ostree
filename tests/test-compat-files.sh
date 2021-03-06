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

set -euo pipefail

. $(dirname $0)/libtest.sh

if ! has_gpgme; then
    echo "1..0 #SKIP no gpg support compiled in"
    exit 0
fi

setup_test_repository "archive-z2"

echo '1..6'

cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" \
    --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME} \
    --tree=dir=files
find repo/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^0$"

echo "ok commit sign no old signature"

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
assert_file_has_content sigcount "^0$"

echo "ok compat pull"

# Test pulls from a repo containing a commit made with old ostree.
# Create a couple needed directories not tracked in git.
cd ${test_tmpdir}
rm -rf compat-repo
cp -a ${test_srcdir}/compat-repo .
chmod -R u+w compat-repo
mkdir -p compat-repo/tmp compat-repo/refs/remotes

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init --mode=archive-z2
${CMD_PREFIX} ostree --repo=local pull-local compat-repo main
find local/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^0$"
find local/objects -name '*.sizes2' | wc -l > sizescount
assert_file_has_content sizescount "^0$"

echo "ok compat pull-local from old repo"

ln -s ${test_tmpdir}/compat-repo ${test_tmpdir}/httpd/

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init --mode=archive-z2
${CMD_PREFIX} ostree --repo=local remote add --set=gpg-verify=false origin \
    $(cat httpd-address)/compat-repo
${CMD_PREFIX} ostree --repo=local pull origin main
find local/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^0$"
find local/objects -name '*.sizes2' | wc -l > sizescount
assert_file_has_content sizescount "^0$"

echo "ok compat pull from old repo"

rm -rf local
mkdir local
${CMD_PREFIX} ostree --repo=local init --mode=archive-z2
${CMD_PREFIX} ostree --repo=local remote add origin \
    $(cat httpd-address)/compat-repo
if ${CMD_PREFIX} ostree --repo=local pull origin main; then
    fatal "pulled commit verified unexpectedly"
fi

echo "ok compat pull from old repo with verification fails"

${CMD_PREFIX} ostree --repo=compat-repo refs --delete main
${CMD_PREFIX} ostree --repo=compat-repo prune --depth=-1 --refs-only
find compat-repo/objects -type f | wc -l > objcount
assert_file_has_content objcount "^0$"

echo "ok compat prune"
