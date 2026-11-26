/*
 * pam_surface_ir.c - Lightweight C PAM module for Surface IPU3 IR Face Unlock
 * Connects to pre-warmed surface-ir-hellod Unix socket (/run/surface-ir-hello/auth.sock).
 * Zero Python/OpenCV overhead inside PAM client processes; safe fallback on SSH/closed lid.
 */
#define PAM_SM_AUTH
#include <errno.h>
#include <glob.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#define SOCK_PATH "/run/surface-ir-hello/auth.sock"

static int is_lid_closed(void) {
    glob_t g;
    int closed = 0;
    if (glob("/proc/acpi/button/lid/*/state", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r");
            if (f) {
                char buf[128];
                if (fgets(buf, sizeof(buf), f) && strstr(buf, "closed")) {
                    closed = 1;
                }
                fclose(f);
            }
        }
        globfree(&g);
    }
    return closed;
}

static int is_remote_session(pam_handle_t *pamh) {
    const void *rhost = NULL;
    if (pam_get_item(pamh, PAM_RHOST, &rhost) == PAM_SUCCESS && rhost && *((const char *)rhost) != '\0') {
        return 1;
    }
    if (getenv("SSH_CONNECTION") || getenv("SSH_TTY") || getenv("SSH_CLIENT")) {
        return 1;
    }
    return 0;
}

static int valid_username(const char *u) {
    if (!u || !*u || strlen(u) > 64) return 0;
    for (const char *p = u; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) {
            return 0;
        }
    }
    return 1;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)flags;
    int quiet = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "quiet") == 0) quiet = 1;
    }

    if (is_remote_session(pamh) || is_lid_closed()) {
        return PAM_IGNORE;
    }

    const char *user = NULL;
    if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !valid_username(user)) {
        return PAM_IGNORE;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return PAM_IGNORE;

    struct timeval tv_snd = { .tv_sec = 1, .tv_usec = 0 };
    struct timeval tv_rcv = { .tv_sec = 4, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return PAM_IGNORE;
    }

    char req[128];
    int n = snprintf(req, sizeof(req), "AUTH %s\n", user);
    if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) {
        close(fd);
        return PAM_IGNORE;
    }

    char resp[128];
    ssize_t r = recv(fd, resp, sizeof(resp) - 1, 0);
    close(fd);
    if (r <= 0) return PAM_IGNORE;
    resp[r] = '\0';

    if (strncmp(resp, "OK", 2) == 0) {
        if (!quiet) {
            pam_info(pamh, "Surface IR: Face recognized.");
        }
        return PAM_SUCCESS;
    }
    return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_IGNORE;
}
