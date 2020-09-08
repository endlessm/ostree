# Testing ostree-boot on an existing system

Start from an amd64 system running Debian 10 or later - it will be
switched to a Debian unstable (sid) OSTree-based installation as part
of following these instructions. A VM is obviously most convenient,
but bare metal should work equally.

The installation needs to satisfy the following requirements:

* The root partition must not be encrypted.
* It must use GRUB and BIOS booting. Other bootloaders or EFI may require
  different bootloader setup steps; see [old Fedora instructions][]
* If you start from Debian 10 or older, it must have a separate
  /boot partition due to limitations in ostree 2020.5 and older.
  If the initial installation was with ostree 2020.6 or later, this
  is no longer required.

[old Fedora instructions]: https://pagure.io/workstation-ostree-config/blob/5b574d39c63b82b397df789eb4a75a5bdcc13dd0/f/README-install-inside.md

Using a caching apt proxy is suggested; in these examples it's
assumed to be http://192.168.122.1:3142 (apt-cacher-ng on the host system,
as seen from a virtual machine in a default libvirt configuration).
Remove or amend the `http_proxy` if your configuration does not match.

We need the bootloader integration files on the non-OSTree system from
which we are switching, as well as in the OSTree-based installation,
so the bootloader gets configured properly when we "ostree admin deploy":

    # apt-get update
    # apt-get install ostree ostree-boot multistrap

Create the ostree system repository and a stateroot:

    # ostree admin init-fs /
    # ostree admin os-init debian

Copy modified-deb-ostree-builder, ostree-1.conf and ostree-2.conf from
debian/ostree-boot-examples/ to the test machine, and run the builder
script:

    # chmod +x ./deb-ostree-builder
    # http_proxy=http://192.168.122.1:3142 ./deb-ostree-builder \
        ./ostree-1.conf sid-1 /ostree/repo
    # http_proxy=http://192.168.122.1:3142 ./deb-ostree-builder \
        ./ostree-2.conf sid-2 /ostree/repo

Then we deploy the first of those commits:

    # ostree admin deploy --karg-proc-cmdline --os=debian os/debian/amd64/sid-1
    # deploy=$(find /ostree/deploy/debian/deploy/* -maxdepth 0 -type d)

Next, we set the root password and copy a few essential configuration files
into the initial deployment:

    # chroot $deploy passwd root
    # : > $deploy/etc/machine-id
    # for f in etc/fstab etc/default/grub; do cp /$f $deploy/$f; done

Finally, we set up the bootloader by pointing GRUB at the configuration file
managed by ostree. Alternatively, you can run update-grub by hand after every
new ostree deployment. This step may be different or unnecessary for other
bootloaders.

    # mv /boot/grub/grub.cfg /boot/grub/grub.cfg.backup
    # ln -s ../loader/grub.cfg /boot/grub/grub.cfg

Now reboot. Make sure to select the new ostree entry in the bootloader. Log in
as root using the password you selected before. The system is rather
unconfigured (network access can be set up in /etc/network/interfaces). This is
the commit built from the ostree-1.conf file, so it doesn't have the hello
package:

    # hello
    -bash: hello: command not found

Now we can deploy the second commit from inside the ostree system:

    # ostree admin deploy --karg-proc-cmdline --os=debian os/debian/amd64/sid-2

Reboot again. Note that your new deployment will again be the first ostree
entry in the menu, labelled `ostree:0`. Your old deployment has been moved
down the menu to `ostree:1`. The root password and the other configuration
was copied from the first deployment, so you can log in as before.

Now try running the hello command again:

    # hello
    Hello, world!

This shows we've now booted the second commit built from ostree-2.conf, which
includes the hello package.
