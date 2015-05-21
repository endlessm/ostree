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

echo '1..1'

cd ${test_tmpdir}
${OSTREE} commit -b test2 -s "A GPG signed commit" -m "Signed commit body" \
    --gpg-sign=${TEST_GPG_KEYID_1} --gpg-homedir=${TEST_GPG_KEYHOME} \
    --tree=dir=files
find repo/objects -name '*.sig' | wc -l > sigcount
assert_file_has_content sigcount "^1$"

echo "ok compat sign"
