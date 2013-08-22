#!/bin/bash
#
# Copyright © 2013 Vivek Dasmohaptra <vivek@collabora.com>
#
# This program is free software; you can redistribute it and/or
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

passed=0;

function ok ()
{
    passed=$((passed + 1));
    echo "ok $passed: $@";
}

function count_dirents ()
{
    count=$(ls -1d "$1" | wc -l);
}

function extract_stat ()
{
    repo=$1
    rev=$2;
    format=$3;
    owd=$(pwd);

    cd $test_tmpdir;
    rm -rf stat.cache;
    mkdir stat.cache;

    ostree --repo="$repo" ls -RC $rev | grep -v '^d' |
    while read mode uid gid size checksum ignored;
    do
        # symlinks are indexed as size 0
        if [ x${mode:0:1} = xl ]; then size=0; fi;

        if [ x$format = xarchive-z2 ];
        then
            # find the filez object so we can grab the archived filesize via stat:
            path=$test_tmpdir/repo/objects/${checksum:0:2}/${checksum:2}.filez;
            read X archived X < <(stat -t $path);
            echo $archived $size > stat.cache/$checksum;
        else
            echo 0 $size > stat.cache/$checksum;
        fi
    done;

    count_dirents stat.cache;
    cd "$owd";
}

function extract_index ()
{
    repo=$1;
    rev=$2;
    format=$3;
    owd=$(pwd);

    cd $test_tmpdir;
    rm -rf index.cache;
    mkdir index.cache;
    go="no";

    ostree --repo="$repo" summary -d $rev |
    while read checksum archived unpacked;
    do
        if [ $go = "yes" ];
        then
            if [ x$checksum = xdetails-end ]; then break; fi;
            if [ x$format = xbare ];
            then # bare repos don't have archived objects:
                echo 0 $unpacked > index.cache/$checksum;
            else
                echo $archived $unpacked > index.cache/$checksum;
            fi
        fi;
        if [ x$checksum = xchecksum ] && [ x$archived = xarchived ];
        then
            go=yes;
        fi
    done;

    count_dirents index.cache;
    cd "$owd";
}

function assert ()
{
    echo "$@" 1>&2;
    exit 1;
}

function check_index ()
{
    index_repo=$1;
    rev=$2;
    stat_repo=${3:-$index_repo};
    format=$(ostree --repo="$stat_repo" config get core.mode);

    extract_index $index_repo $rev $format;
    index_entries=$count;
    extract_stat  $stat_repo $rev $format;
    stat_entries=$count;

    if [ -z "$index_entries"  ]; then assert "No index data in repo" ; fi;
    if [ -z "$stat_entries"   ]; then assert "No ls -CR data in repo"; fi;
    if [ $index_entries -le 0 ]; then assert "Index is empty"        ; fi;
    if [ $stat_entries -ne $index_entries ];
    then
        assert "Index and ls -CR indicate different commit contents";
    fi;

    diff -ur index.cache stat.cache || assert "Index and ls -CR contents differ";
}

function setup_local_mirror ()
{
    local=$1;
    remote=$2;
    url=$3;

    cd $test_tmpdir;
    mkdir $local;
    ostree --repo=$local init --mode=bare;
    ostree --repo=$local remote add $remote $url;
}

function check_pull_meta ()
{
    local=$1;
    remote=$2;
    branch=$3;

    cd $test_tmpdir;
    ostree --repo=$local pull -m $remote $branch;
    count=$(find $local/objects -type f | wc -l)

    if [ -z "$count"  ]; then assert "Malformed repo after metadata pull"     ; fi;
    if [ $count -ne 2 ]; then assert "Unexpected contents after metadata pull"; fi;
}

function check_pull_data ()
{
    local=$1;
    remote=$2;
    branch=$3;

    cd $test_tmpdir;
    ostree --repo=$local pull $remote $branch;
    count=$(find $local/objects -type f | wc -l)

    if [ -z "$count"  ]; then assert "Malformed repo after contents pull" ; fi;
    if [ $count -le 2 ]; then assert "Missing objects after contents pull"; fi;
}

. $(dirname $0)/libtest.sh

echo "1..2";

setup_test_repository archive-z2;
ok setup;

check_index "$test_tmpdir"/repo test2;
ok index;

http_serve_docroot "$test_tmpdir"/repo index-test-remote;
setup_local_mirror index-pull-test test-remote $http_uri;
ok local-mirror $http_uri

check_pull_meta index-pull-test test-remote test2;
ok pull-index;

check_index index-pull-test test2 "$test_tmpdir"/repo
ok pulled-index;

check_pull_data index-pull-test test-remote test2;
ok pull-content-after-index;

check_index index-pull-test test2;
ok pulled-content-after-index;

http_unserve_docroot index-test-remote;
