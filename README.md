
Multi-user OSS and CKS (checksum) plugin for Xrootd
================================

A filesystem plugin to allow Xrootd to interact with underlying POSIX filesystems as
different Unix users.

This plugin will change the thread's filesystem UID to match Xrootd's user name, meaning a user with a login session
mapped to user name `atlas` will read and write to the filesystem as the UID associated with the Unix user `atlas`.
Without this plugin, Xrootd will always read and write as the Unix user `xrootd`.


Configuration
-------------

To configure the multi-user plugin, add the following line to the Xrootd configuration file (XRootD 5.0+):

```
ofs.osslib ++ libXrdMultiuser.so
```

To enable the checksum (only on XRootD 5.2+):

```
ofs.ckslib * libXrdMultiuser.so
```

The following optional directives can also be set in the Xrootd configuration file:

| Directive | Default | Description |
| --- | --- | --- |
| `multiuser.umask <octal>` | (unset) | Apply this umask to files and directories created through the plugin. |
| `multiuser.checksumonwrite <on\|off>` | `off` | Compute checksums while a file is being written. |
| `multiuser.minuid <n>` | `500` | Minimum UID a mapped username may resolve to; usernames mapping to a lower UID are treated as system accounts and denied. |
| `multiuser.mingid <n>` | `500` | Minimum GID a mapped username may resolve to; usernames mapping to a lower GID are treated as system accounts and denied. |

For example, to allow users and groups with IDs as low as 100 (e.g., groups
imported from a Lustre file system):

```
multiuser.minuid 100
multiuser.mingid 100
```

Third-party copy over root://
-----------------------------

HTTP-TPC writes through the plugin, so it switches to the mapped user.  A root:// TPC does not: the
destination xrootd runs a separate copy program (`ofs.tpc pgm`, by default `xrdcp --server`), which writes
the destination file directly with the daemon's IDs and fails in user-owned directories.

`xrdcp-multiuser` is a wrapper for that program.  The destination file has already been created through the
plugin, as the mapped user, when the copy starts; the wrapper switches to the owner of that file (UID, primary
GID and supplementary groups), drops all capabilities and runs `/usr/bin/xrdcp` with the same arguments.
Delegated credentials are handed over in a temporary file owned by that user.  To use it:

```
ofs.tpc pgm /usr/libexec/xrootd-multiuser/xrdcp-multiuser --server
```

Notes:

- The wrapper is installed `0750 root:xrootd` with file capabilities `cap_setuid,cap_setgid,cap_dac_override=ep`
  (all within the bounding set of `xrootd-privileged@.service`), and refuses callers other than the `xrootd`
  user.  `cap_dac_override` is used only to look up the destination's owner.
- It refuses destinations that are not regular files, have more than one link, or are owned by a UID/GID below
  500.  Set `XRDCP_MULTIUSER_MINUID` / `XRDCP_MULTIUSER_MINGID` in the daemon's environment to match
  `multiuser.minuid` / `multiuser.mingid` if you changed those.
- The identity is the destination file's owner.  For a new file that is the mapped user.  When an existing file
  is overwritten, the client must have been allowed to open it for writing, and the copy then runs as the
  file's owner.
- The copy writes outside the plugin, so `multiuser.checksumonwrite` does not apply to it; checksums are
  computed on demand afterwards, through the multiuser checksum plugin.

Startup
-------

The Xrootd process must be started with the privileged Linux capabilities in order to successfully
read and write as different users (i.e., execute the `setfsuid` and `setfsgid` calls).  To support this, we have a
separate systemd unit called `xrootd-privileged@.service`.

To start the configuration in `/etc/xrootd/xrootd-clustered.cfg` with the multiuser plugin enabled, execute:

```
systemctl start xrootd-privileged@clustered
```
