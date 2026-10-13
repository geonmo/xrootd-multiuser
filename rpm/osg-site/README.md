# Site RPM: xrootd-multiuser 2.2.1 + quiet Connect()/Disc()

OSG's `xrootd-multiuser-2.2.1-1.1.osg25` with one patch on top, built for
KISTI-GSDC until the change is available upstream
(https://github.com/opensciencegrid/xrootd-multiuser/issues/71).

| File | Content |
|---|---|
| `xrootd-multiuser.spec` | OSG spec (from `xrootd-multiuser-2.2.1-1.1.osg25.el9.src.rpm`) with `Release: 1.1.1`, `Patch0` and a changelog entry |
| `xrootd-multiuser-quiet-connect-disc.patch` | `Connect()`/`Disc()` pass straight through to the wrapped OSS without a `UserSentry` (same change as branch `quiet-connect-disc`, rebased on `v2.2.1-1`) |

## Build

```bash
mkdir -p ~/rpmbuild/SOURCES
# Source0: identical in content to the tarball in the OSG SRPM
git archive v2.2.1-1 --prefix=xrootd-multiuser-2.2.1/ | gzip -7 > ~/rpmbuild/SOURCES/xrootd-multiuser-2.2.1.tar.gz
cp rpm/osg-site/xrootd-multiuser-quiet-connect-disc.patch ~/rpmbuild/SOURCES/
env PATH=/usr/bin:/bin:/usr/sbin:/sbin \
    rpmbuild --define "dist .osg25.el9" -ba rpm/osg-site/xrootd-multiuser.spec
rpm -qlp ~/rpmbuild/RPMS/x86_64/xrootd-multiuser-2.2.1-1.1.1.osg25.el9.x86_64.rpm   # must list libXrdMultiuser-5.so
```

- **`PATH`:** CMake takes the plugin version from the first `xrootd-config` on `PATH`. If an XRootD 6 installation (for example in `/usr/local`) comes first, the library is built as `libXrdMultiuser-6.so` and still packaged (the spec uses a wildcard), but XRootD 5 will not load it. Check the file name after building.
- **`dist`:** `.osg25.el9` matches the OSG build. The release `1.1.1.osg25` sorts above the OSG `1.1.osg25` package and below any later OSG release (`1.2.osg25`, `2.2.2-…`), so `dnf update` returns to the official package once OSG ships a newer one.

## Install / roll back

```bash
dnf install xrootd-multiuser-2.2.1-1.1.1.osg25.el9.x86_64.rpm   # then restart xrootd
dnf downgrade xrootd-multiuser                                   # back to the OSG build, then restart
```
