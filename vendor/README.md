# vendor/ — the bundled P2P wpa_supplicant

Imira needs Wi-Fi Direct (P2P), but the stock Sailfish OS `wpa_supplicant`
ships with `CONFIG_P2P` compiled out — upstream keeps it disabled because it
would break AP (hotspot) mode on some devices (Jolla C2, Xperia X). So the
imira service brings its **own** P2P-capable build and runs it side by side
with the system one; nothing system-wide is touched.

The build is the unmodified [sailfishos/wpa_supplicant](https://github.com/sailfishos/wpa_supplicant)
packaging plus exactly one change: `wpa-p2p-enable.patch` (this directory)
turns `CONFIG_P2P=y` on.

Reproduce it with the Sailfish Platform SDK:

```sh
git clone --recurse-submodules https://github.com/sailfishos/wpa_supplicant vendor/wpa_supplicant-p2p
cd vendor/wpa_supplicant-p2p
git apply ../wpa-p2p-enable.patch
mb2 -t SailfishOS-5.0.0.62-aarch64 build
# unpack the built RPM so the packaging step finds the binaries:
mkdir -p extracted && cd extracted
rpm2cpio ../RPMS/wpa_supplicant-*.aarch64.rpm | cpio -idm
```

`rpm/harbour-imira.spec` copies `vendor/wpa_supplicant-p2p/extracted/usr/sbin/wpa_supplicant`
and `wpa_cli` into the package as `wpa_supplicant-p2p` / `wpa_cli-p2p`.
The checkout itself stays untracked (`.gitignore`) — only the patch and this
recipe are part of the repository.
