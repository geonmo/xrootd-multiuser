/*
 * xrdcp-multiuser: run the xrootd-protocol (root://) third-party-copy program
 * as the user that owns the destination file.
 *
 * For a root:// TPC, the destination xrootd creates the destination file when
 * the client opens it (through xrootd-multiuser, so as the mapped user), then
 * runs the copy program ("ofs.tpc pgm", by default "xrdcp --server") as
 *
 *     <pgm> [-C cks] [-S streams] <source URL> <destination PFN>
 *
 * The program writes the PFN directly, outside the OSS stack, and therefore
 * with the daemon's IDs, which fails in user-owned directories.  No identity
 * is passed to the program (XRD_TIDENT is client-controlled), but the
 * destination file was created a moment earlier as the mapped user, so its
 * owner is the identity to use.  This wrapper switches to that owner (UID,
 * primary GID and supplementary groups), drops all capabilities and runs the
 * real xrdcp with the same arguments.
 *
 * Use it as:   ofs.tpc pgm /usr/libexec/xrootd-multiuser/xrdcp-multiuser --server
 *
 * It needs file capabilities cap_setuid,cap_setgid,cap_dac_override=ep (the
 * xrootd-privileged units keep these in the bounding set) and must only be
 * executable by the xrootd group (mode 0750 root:xrootd).  It refuses to run
 * for any caller other than the daemon user.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef XRDCP_PATH
#define XRDCP_PATH "/usr/bin/xrdcp"
#endif
#ifndef DAEMON_USER
#define DAEMON_USER "xrootd"
#endif
#ifndef DEFAULT_MIN_UID
#define DEFAULT_MIN_UID 500      /* same default as multiuser.minuid */
#endif
#define MAX_CREDS (1024 * 1024)

static const char *g_prog = "xrdcp-multiuser";
static pid_t g_child = -1;

/* XrdOfsTPCProg reports the last "<word>: <text>" line as the copy error. */
static void die(int rc, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", g_prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(rc);
}

static void forward_signal(int sig)
{
    if (g_child > 0) {kill(g_child, sig);}
}

static int set_caps(const cap_value_t *list, int n)
{
    cap_t caps = cap_init();   /* everything cleared */
    if (!caps) {return -1;}
    int rc = 0;
    if (n > 0 && (cap_set_flag(caps, CAP_PERMITTED, n, list, CAP_SET) ||
                  cap_set_flag(caps, CAP_EFFECTIVE, n, list, CAP_SET))) {rc = -1;}
    if (!rc) {rc = cap_set_proc(caps);}
    cap_free(caps);
    return rc;
}

static unsigned long env_ulong(const char *name, unsigned long dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) {return dflt;}
    char *end = NULL;
    errno = 0;
    unsigned long val = strtoul(v, &end, 10);
    if (errno || *end) {die(EINVAL, "invalid %s=%s", name, v);}
    return val;
}

/* The credentials file xrootd exports for delegated TPC is owned by the
 * daemon (0600) and named "<dir>/...#<n>.creds"; find it in the environment. */
static char *find_creds_var(void)
{
    extern char **environ;
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        size_t len = eq ? strlen(eq + 1) : 0;
        if (len > 6 && !strcmp(eq + 1 + len - 6, ".creds") && strchr(eq + 1, '#')) {return *e;}
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) {die(EINVAL, "usage: %s [xrdcp options] <source> <destination pfn>", g_prog);}
    const char *dst = argv[argc - 1];
    if (dst[0] != '/') {die(EINVAL, "destination is not a local path: %s", dst);}

    /* Only the daemon may use this. */
    struct passwd *dpw = getpwnam(DAEMON_USER);
    if (!dpw) {die(EPERM, "daemon user %s not found", DAEMON_USER);}
    if (getuid() != dpw->pw_uid && getuid() != 0) {die(EPERM, "may only be run by %s", DAEMON_USER);}
    unsigned long min_uid = env_ulong("XRDCP_MULTIUSER_MINUID", DEFAULT_MIN_UID);
    unsigned long min_gid = env_ulong("XRDCP_MULTIUSER_MINGID", DEFAULT_MIN_UID);

    /* Find the owner; CAP_DAC_OVERRIDE (and fsuid 0, for file systems that
     * only look at the fsuid) lets us reach it in any user's directory. */
    struct stat st;
    int prev_fsuid = setfsuid(0);
    int rc = lstat(dst, &st);
    int err = errno;
    setfsuid(prev_fsuid);
    if (rc) {errno = err; die(err, "cannot stat destination %s: %s", dst, strerror(err));}
    if (!S_ISREG(st.st_mode)) {die(EPERM, "destination %s is not a regular file", dst);}
    if (st.st_nlink != 1) {die(EPERM, "destination %s has %lu links", dst, (unsigned long)st.st_nlink);}
    if (st.st_uid < min_uid) {die(EPERM, "destination %s is owned by system UID %u", dst, (unsigned)st.st_uid);}

    char pwbuf[16384];
    struct passwd pw, *res = NULL;
    if (getpwuid_r(st.st_uid, &pw, pwbuf, sizeof(pwbuf), &res) || !res) {
        die(EPERM, "owner UID %u of %s has no account", (unsigned)st.st_uid, dst);
    }
    if (pw.pw_gid < min_gid) {die(EPERM, "owner %s has system GID %u", pw.pw_name, (unsigned)pw.pw_gid);}
    int ngroups = 64;
    gid_t *groups = malloc(sizeof(gid_t) * (size_t)ngroups);
    if (!groups) {die(ENOMEM, "out of memory");}
    if (getgrouplist(pw.pw_name, pw.pw_gid, groups, &ngroups) < 0) {
        gid_t *more = realloc(groups, sizeof(gid_t) * (size_t)ngroups);
        if (!more) {die(ENOMEM, "out of memory");}
        groups = more;
        if (getgrouplist(pw.pw_name, pw.pw_gid, groups, &ngroups) < 0) {
            die(EPERM, "cannot get the groups of %s", pw.pw_name);
        }
    }

    /* Read delegated credentials while we can still read the daemon's file. */
    char *creds_var = find_creds_var(), *creds = NULL, *creds_name = NULL;
    ssize_t creds_len = 0;
    if (creds_var) {
        const char *path = strchr(creds_var, '=') + 1;
        creds_name = strndup(creds_var, (size_t)(path - 1 - creds_var));
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {die(errno, "cannot read credentials %s: %s", path, strerror(errno));}
        creds = malloc(MAX_CREDS);
        if (!creds) {die(ENOMEM, "out of memory");}
        creds_len = read(fd, creds, MAX_CREDS);
        close(fd);
        if (creds_len < 0) {die(EIO, "cannot read credentials %s", path);}
    }

    /* Become the owner, for good, and drop every capability. */
    if (setgroups((size_t)ngroups, groups) || setresgid(pw.pw_gid, pw.pw_gid, pw.pw_gid) ||
        setresuid(pw.pw_uid, pw.pw_uid, pw.pw_uid)) {
        die(EPERM, "cannot switch to %s: %s", pw.pw_name, strerror(errno));
    }
    if (set_caps(NULL, 0) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        die(EPERM, "cannot drop capabilities: %s", strerror(errno));
    }
    if (getuid() != pw.pw_uid || geteuid() != pw.pw_uid || getegid() != pw.pw_gid) {
        die(EPERM, "identity switch to %s did not take effect", pw.pw_name);
    }

    /* Hand the credentials over in a file the user can read. */
    char creds_path[] = "/tmp/xrdcp-multiuser-XXXXXX";
    if (creds) {
        int fd = mkstemp(creds_path);   /* 0600, owned by the user */
        if (fd < 0 || write(fd, creds, (size_t)creds_len) != creds_len || close(fd)) {
            die(EIO, "cannot pass credentials: %s", strerror(errno));
        }
        if (setenv(creds_name, creds_path, 1)) {die(EIO, "cannot set %s", creds_name);}
        memset(creds, 0, (size_t)creds_len);
    }

    /* Run xrdcp; stay around to forward signals and remove the credentials. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    pid_t parent = getpid();
    g_child = fork();
    if (g_child < 0) {
        if (creds) {unlink(creds_path);}
        die(errno, "fork: %s", strerror(errno));
    }
    if (g_child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);   /* do not outlive the wrapper */
        if (getppid() != parent) {_exit(127);}
        argv[0] = (char *)"xrdcp";
        execv(XRDCP_PATH, argv);
        fprintf(stderr, "%s: cannot run %s: %s\n", g_prog, XRDCP_PATH, strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(g_child, &status, 0) < 0 && errno == EINTR) {}
    if (creds) {unlink(creds_path);}
    if (WIFEXITED(status)) {return WEXITSTATUS(status);}
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s: xrdcp killed by signal %d\n", g_prog, WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}
