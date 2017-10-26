#!/bin/sh

set -e

export VERBOSE=1

# Ubuntu autopkgtest infra provides internet access via a proxy, and
# buildds could conceivably do the same, but libostree doesn't need
# that. However, libostree also doesn't support no_proxy, so it will try
# to use Ubuntu's proxy for localhost, and fail to reach itself.
unset ftp_proxy
unset http_proxy
unset https_proxy
unset no_proxy

failed=0
make check || failed=1

pkill --full "gpg-agent --homedir /var/tmp/tap-test\\.[^/]+/.*" || :
pkill --full '\.libs/ostree-trivial-httpd' || :

if pgrep lt-ostree || pgrep --full '\.libs/ostree-trivial-httpd' || pgrep --full "gpg-agent --homedir /var/tmp/tap-test."; then \
    echo "WARNING: daemon processes were leaked"
    pgrep gpg-agent | xargs --no-run-if-empty ps ww
    pgrep --full '\.libs/ostree-trivial-httpd' | xargs --no-run-if-empty ps ww
    pgrep lt-ostree | xargs --no-run-if-empty ps ww
fi

exit $failed

# vim:set et sw=4 sts=4:
