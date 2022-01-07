#!/bin/bash
#
# Copyright (C) 2015 Red Hat, Inc.
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

# We don't want OSTREE_GPG_HOME used for these tests.
unset OSTREE_GPG_HOME

setup_fake_remote_repo1 "archive"

# Some tests require an appropriate gpg
num_non_gpg_tests=4
num_gpg_tests=2
num_tests=$((num_non_gpg_tests + num_gpg_tests))

echo "1..${num_tests}"

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo

#----------------------------------------------
# Test synchronicity of keyring file and remote
#----------------------------------------------

assert_not_has_file repo/R1.trustedkeys.gpg

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

assert_not_has_file repo/R1.trustedkeys.gpg

# Import one valid key ID
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_1} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

assert_has_file repo/R1.trustedkeys.gpg

${OSTREE} remote delete R1

assert_not_has_file repo/R1.trustedkeys.gpg

#---------------------------------------
# Test gpg-import with --keyring option
#---------------------------------------

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

# Import one valid key ID
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_1} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

# Import multiple valid key IDs
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_2} ${TEST_GPG_KEYID_3} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 2 GPG key'

# Import key IDs we already have, make sure they're caught
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 ${TEST_GPG_KEYID_1} ${TEST_GPG_KEYID_3} | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 0 GPG key'

${OSTREE} remote delete R1

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

# Import all keys from keyring
${OSTREE} remote gpg-import --keyring ${test_tmpdir}/gpghome/trusted/pubring.gpg R1 | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 3 GPG key'

${OSTREE} remote delete R1

#-------------------------------------
# Test gpg-import with --stdin option
#-------------------------------------

${OSTREE} remote add R1 $(cat httpd-address)/ostree/gnomerepo

# Import ASCII-armored keys thru stdin
cat ${test_tmpdir}/gpghome/key{1,2,3}.asc | ${OSTREE} remote gpg-import --stdin R1 | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 3 GPG key'

${OSTREE} remote delete R1

#------------------------------------------------------------
# Divide keys across multiple remotes, test GPG verification
# For testing purposes the remotes all point to the same URL
# This also tests "remote add" with --gpg-import.
#------------------------------------------------------------

${OSTREE} remote add --gpg-import ${test_tmpdir}/gpghome/key1.asc R1 $(cat httpd-address)/ostree/gnomerepo | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

${OSTREE} remote add --gpg-import ${test_tmpdir}/gpghome/key2.asc R2 $(cat httpd-address)/ostree/gnomerepo | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

${OSTREE} remote add --gpg-import ${test_tmpdir}/gpghome/key3.asc R3 $(cat httpd-address)/ostree/gnomerepo | grep -o 'Imported [[:digit:]] GPG key' > result
assert_file_has_content result 'Imported 1 GPG key'

# Checkout the "remote" repo so we can add more commits
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo checkout main workdir

# Sign a new commit with key1 and try pulling from each remote
echo shadow > workdir/blinky
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "Add blinky" --gpg-sign ${TEST_GPG_KEYID_1} --gpg-homedir ${test_tmpdir}/gpghome workdir
if ${OSTREE} pull R2:main >/dev/null 2>&1; then
    assert_not_reached "(key1/R2) GPG verification unexpectedly succeeded"
fi
if ${OSTREE} pull R3:main >/dev/null 2>&1; then
    assert_not_reached "(key1/R3) GPG verification unexpectedly succeeded"
fi
${OSTREE} pull R1:main >/dev/null

# Sign a new commit with key2 and try pulling from each remote
echo speedy > workdir/pinky
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "Add pinky" --gpg-sign ${TEST_GPG_KEYID_2} --gpg-homedir ${test_tmpdir}/gpghome workdir
if ${OSTREE} pull R1:main >/dev/null 2>&1; then
    assert_not_reached "(key2/R1) GPG verification unexpectedly succeeded"
fi
if ${OSTREE} pull R3:main >/dev/null 2>&1; then
    assert_not_reached "(key2/R3) GPG verification unexpectedly succeeded"
fi
${OSTREE} pull R2:main >/dev/null

# Sign a new commit with key3 and try pulling from each remote
echo bashful > workdir/inky
${CMD_PREFIX} ostree --repo=ostree-srv/gnomerepo commit -b main -s "Add inky" --gpg-sign ${TEST_GPG_KEYID_3} --gpg-homedir ${test_tmpdir}/gpghome workdir
if ${OSTREE} pull R1:main >/dev/null 2>&1; then
    assert_not_reached "(key3/R1) GPG verification unexpectedly succeeded"
fi
if ${OSTREE} pull R2:main >/dev/null 2>&1; then
    assert_not_reached "(key3/R2) GPG verification unexpectedly succeeded"
fi
${OSTREE} pull R3:main >/dev/null

echo "ok"

rm repo/refs/remotes/* -rf
${OSTREE} prune --refs-only

# Test the successful gpgkeypath option
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/key3.asc R4 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} pull R4:main >/dev/null

# Test gpgkeypath success with multiple keys to try
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/key1.asc,${test_tmpdir}/gpghome/key2.asc,${test_tmpdir}/gpghome/key3.asc R7 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} pull R7:main >/dev/null

# Test gpgkeypath failure with multiple keys but none in keyring
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/key1.asc,${test_tmpdir}/gpghome/key2.asc R8 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R8:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with different key"
fi
assert_file_has_content err.txt "public key not found"

# Test gpgkeypath success with directory containing a valid key
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/ R9 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} pull R9:main >/dev/null

# Test gpgkeypath failure with nonexistent directory
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/INVALIDKEYDIRPATH/ R10 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R10:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with nonexistent key directory"
fi
assert_file_has_content err.txt "INVALIDKEYDIRPATH.*No such file or directory"

# Test gpgkeypath failure with a directory containing a valid key, and a nonexistent key
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/,${test_tmpdir}/gpghome/INVALIDKEYPATH.asc R11 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R11:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with nonexistent key"
fi
assert_file_has_content err.txt "INVALIDKEYPATH.*No such file or directory"

# Test gpgkeypath success with a directory containing a valid key, and a key not in keyring
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/,${test_tmpdir}/gpghome/key1.asc R12 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} pull R12:main >/dev/null

# Test gpgkeypath failure with a nonexistent directory, and a valid key
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/INVALIDKEYDIRPATH/,${test_tmpdir}/gpghome/key3.asc R13 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R13:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with nonexistent key directory"
fi
assert_file_has_content err.txt "INVALIDKEYDIRPATH.*No such file or directory"

# Test gpgkeypath failure with a nonexistent directory and a nonexistent key
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/INVALIDKEYDIRPATH/,${test_tmpdir}/gpghome/INVALIDKEYPATH.asc R14 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R14:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with nonexistent key"
fi
assert_file_has_content err.txt "INVALIDKEYDIRPATH.*No such file or directory"

# Test gpgkeypath success for no trailing slash in directory path
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome R15 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} pull R15:main >/dev/null

# Test gpgkeypath failure with prefixed separator giving an empty path, and a nonexistent key
${OSTREE} remote add --set=gpgkeypath=,${test_tmpdir}/gpghome/INVALIDKEYPATH.asc R16 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R16:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with nonexistent key"
fi
assert_file_has_content err.txt "().*No such file or directory"

# Test gpgkeypath success with suffixed separator
${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/key3.asc, R17 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} pull R17:main >/dev/null

# Test gpgkeypath success with multiple keys specified, with semicolons
${OSTREE} remote add --set=gpgkeypath="${test_tmpdir}/gpghome/key1.asc;${test_tmpdir}/gpghome/key2.asc;${test_tmpdir}/gpghome/key3.asc" R18 $(cat httpd-address)/ostree/gnomerepo
${OSTREE} pull R18:main >/dev/null

# Test gpgkeypath failure multiple keys specified, with mix of commas and semicolons
${OSTREE} remote add --set=gpgkeypath="${test_tmpdir}/gpghome/key1.asc,${test_tmpdir}/gpghome/key2.asc;${test_tmpdir}/gpghome/key3.asc" R19 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R19:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with invalid gpgkeypath value"
fi
assert_file_has_content err.txt ".*key value list contains more than one separator"

rm repo/refs/remotes/* -rf
${OSTREE} prune --refs-only

${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/INVALIDKEYPATH.asc R5 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R5:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with nonexistent key"
fi
assert_file_has_content err.txt "INVALIDKEYPATH.*No such file or directory"

rm repo/refs/remotes/* -rf
${OSTREE} prune --refs-only

${OSTREE} remote add --set=gpgkeypath=${test_tmpdir}/gpghome/key2.asc R6 $(cat httpd-address)/ostree/gnomerepo
if ${OSTREE} pull R6:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling with different key"
fi
assert_file_has_content err.txt "public key not found"

echo "ok"

# Test deltas with signed commits; this test is a bit
# weird here but this file has separate per-remote keys.
cd ${test_tmpdir}
rm repo/refs/remotes/* -rf
${OSTREE} prune --refs-only
echo $(date) > workdir/testfile-for-deltas-1
# Sign with keyid 1 for first commit
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main --gpg-sign ${TEST_GPG_KEYID_1} --gpg-homedir ${test_tmpdir}/gpghome workdir
prevrev=$(${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo rev-parse main)
# Pull the previous revision
${OSTREE} pull R1:main
assert_streq $(${OSTREE} rev-parse R1:main) ${prevrev}
# Sign with keyid 2, but use remote r1
echo $(date) > workdir/testfile-for-deltas-2
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main --gpg-sign ${TEST_GPG_KEYID_2} --gpg-homedir ${test_tmpdir}/gpghome workdir
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo static-delta generate main
# Summary is signed with key1
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u --gpg-sign ${TEST_GPG_KEYID_1} --gpg-homedir ${test_tmpdir}/gpghome
newrev=$(${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo rev-parse main)
if ${OSTREE} pull --require-static-deltas R1:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling commit signed with untrusted key"
fi
assert_file_has_content err.txt "public key not found"

echo "ok gpg untrusted signed commit for delta upgrades"

${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo reset main{,^}
${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main --gpg-sign ${TEST_GPG_KEYID_1} --gpg-homedir ${test_tmpdir}/gpghome workdir
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo static-delta generate main
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo summary -u --gpg-sign ${TEST_GPG_KEYID_1} --gpg-homedir ${test_tmpdir}/gpghome
${OSTREE} pull --require-static-deltas R1:main

echo "ok gpg trusted signed commit for delta upgrades"

# Run some more tests if an appropriate gpg is available
GPG=$(which_gpg)
if [ -z "${GPG}" ]; then
  # Print a skip message per skipped test
  for (( i = 0; i < num_gpg_tests; i++ )); do
    echo "ok # SKIP this test requires gpg"
  done
else
  # Create a commit signed with keyid 1
  echo $(date) > workdir/testfile-for-key-mangling
  ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main --gpg-sign ${TEST_GPG_KEYID_1} --gpg-homedir ${test_tmpdir}/gpghome workdir

  # Re-add the remote
  ${OSTREE} remote delete R1
  ${OSTREE} remote add --gpg-import ${test_tmpdir}/gpghome/key1.asc R1 $(cat httpd-address)/ostree/gnomerepo | grep -o 'Imported [[:digit:]] GPG key' > result
  assert_file_has_content result 'Imported 1 GPG key'

  # Expire key 1, wait for it to be expired and import the expired key. Only
  # new keys are reported.
  ${GPG} --homedir=${test_tmpdir}/gpghome --quick-set-expire ${TEST_GPG_KEYFPR_1} seconds=1
  sleep 2
  ${GPG} --homedir=${test_tmpdir}/gpghome --armor --export ${TEST_GPG_KEYID_1} > ${test_tmpdir}/key1expired.asc
  ${OSTREE} remote gpg-import --keyring ${test_tmpdir}/key1expired.asc R1 | grep -o 'Imported [[:digit:]] GPG key' > result
  assert_file_has_content result 'Imported 0 GPG key'

  # Pulling should fail since the key is expired
  rm repo/refs/remotes/* -rf
  ${OSTREE} prune --refs-only
  if ${OSTREE} pull R1:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling commit signed with expired key"
  fi
  assert_file_has_content err.txt "Key expired"

  echo "ok imported expired key"

  # Unexpire keyid 1 and revoke it. Revoking is done by importing the
  # pre-generated revocation certificate.
  ${GPG} --homedir=${test_tmpdir}/gpghome --quick-set-expire ${TEST_GPG_KEYFPR_1} seconds=0
  ${GPG} --homedir=${TEST_GPG_KEYHOME} --import ${TEST_GPG_KEYHOME}/revocations/key1.rev
  ${GPG} --homedir=${test_tmpdir}/gpghome --armor --export ${TEST_GPG_KEYID_1} > ${test_tmpdir}/key1revoked.asc
  ${OSTREE} remote gpg-import --keyring ${test_tmpdir}/key1revoked.asc R1 | grep -o 'Imported [[:digit:]] GPG key' > result
  assert_file_has_content result 'Imported 0 GPG key'

  # Pulling should fail since the key is revoked
  rm repo/refs/remotes/* -rf
  ${OSTREE} prune --refs-only
  if ${OSTREE} pull R1:main 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at pulling commit signed with revoked key"
  fi
  assert_file_has_content err.txt "Key revoked"

  echo "ok imported revoked key"
fi
