#!/bin/sh

set -e

export VERBOSE=1

exit_status=0
dh_auto_test || exit_status=1

pkill --full "gpg-agent --homedir /var/tmp/tap-test\\.[^/]+/.*" || :

if pgrep lt-ostree || pgrep --full "gpg-agent --homedir /var/tmp/tap-test."; then \
    echo "WARNING: daemon processes were leaked"
    pgrep gpg-agent | xargs --no-run-if-empty ps ww
    pgrep lt-ostree | xargs --no-run-if-empty ps ww
fi

exit $exit_status

# vim:set et sw=4 sts=4:
