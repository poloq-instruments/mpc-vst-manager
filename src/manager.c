/* Plugin Manager: browse the mpc-vst-plugins catalog from the MPC screen, queue installs, updates and removals, and
 * apply them with one MPC restart. The screen (vst/layout.conf) is three plugin cards per page with tabs, filters,
 * state-dependent buttons and badges, a status line and a progress bar, all driven from here: every value the skin
 * shows or switches on is a parameter computed in get_param, and "display_rev" tells the wrapper when to re-read them
 * (HAS_DISPLAY_REV: text refreshes and changed values are reported, so when= panels follow; docs/NOTES.md).
 *
 * Everything slow runs on a worker thread: the catalog and zips come over HTTPS through the system libcurl
 * (dlopen, nothing bundled); sha256sum and unzip run as children with a clean environment (MPC's LD_PRELOAD
 * dropped). Applying writes a script that stops MPC, runs each package's own install.sh / uninstall.sh and starts
 * MPC again; it is launched with systemd-run so it lives outside acvs.service's cgroup and survives the stop
 * (docs/NOTES.md, "Restarting MPC from inside a plugin via systemd-run"). The plugin makes no sound.
 *
 * Addins (catalog kind "addin": libraries MPC loads at start through LD_PRELOAD, docs/ADDINS.md) go through the same
 * flow. One installs to its own folder under ADDINS_DIR with the install.sh in its zip; what is installed is read from
 * the addin.manifest in each folder; a removal runs the uninstall.sh the folder carries. MPC.settings is not touched. */
#define _GNU_SOURCE   /* dladdr */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* mpc-vst-plugins wrapper/engine.h, positional */
typedef struct {
    void *(*create)(const char *data_dir);
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);
    void (*render)(void *inst, int16_t *out_lr, int frames);
    void (*process)(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames);
} mpc_engine_t;

#ifndef CATALOG_URL
#define CATALOG_URL "https://sd88me.github.io/mpc-vst-plugins/catalog.json"   /* tests: a file:// fixture */
#endif
#define WORK "/tmp/pluginmgr"
#define MAXPKG 64
#define ROWS 3      /* plugin cards per page */
#ifndef ADDINS_DIR
#define ADDINS_DIR "/data/mpc-addins"   /* one folder per addin id, as mpc-vst-plugins' addin installer lays them out */
#endif
#ifndef FS_ANY
#define FS_ANY 0   /* tests: accept the container's overlay filesystem as internal storage */
#endif

typedef struct {
    char id[48], name[48], author[32], kind[16], dist[16], latest[24], url[512], sha[72];
    char style[32], tags[96], channel[16], cpu[8], tested[48];   /* the card's badges and meta line */
    long long size;
    char installed[24];   /* "" not installed, "?" installed (version unknown), else the version */
    char synths[200];     /* the Synths folder it is installed in; an addin: its own folder under ADDINS_DIR */
    char uids[4][16];     /* build-yourself components' VST uids (catalog "components") */
    int legacy;           /* registered, but not from a plugin folder (old /sdcard/vst layout); an addin folder
                           * without uninstall.sh (made by hand): either is updated in place, not removed */
    int queued;           /* 0, Q_INSTALL, Q_REMOVE */
    char dir[256];        /* unpacked package (install.sh / uninstall.sh), set while preparing */
} pkg_t;
enum { Q_INSTALL = 1, Q_REMOVE = 2 };
enum { J_NONE, J_REFRESH, J_PREPARE, J_LAUNCH };

typedef struct device device_t;
typedef struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int quit, job, busy, ready;   /* ready: a prepared apply.sh waits for the second APPLY press */
    pkg_t pkg[MAXPKG];
    int npkg, page, sel, menu;    /* sel, menu: pkg indexes (-1 none); menu = the card showing Reinstall/Remove */
    int tab, kindf;               /* 0 discover, 1 installed, 2 updates; 0 all, 1 instruments, 2 effects, 3 addins */
    int online, loaded, failed;   /* catalog reachable; ever loaded; the last APPLY failed (offer RETRY) */
    int sticky, status_kind;      /* status set by the worker or an action (else the queue summary); 0 ok, 1 warn, 2 error */
    int downloading;
    float progress, disk_used;
    char disk_free[32];
    unsigned rev;   /* bumped whenever the worker changes text, polled by the wrapper as "display_rev" */
    char status[160];
    device_t *dev;   /* probed by the worker on each refresh */
} mgr_t;

/* ---- small helpers ---- */

enum { OK, WARN, ERR };

static void vsay(mgr_t *m, int kind, const char *fmt, va_list ap) {   /* called with mu held */
    vsnprintf(m->status, sizeof m->status, fmt, ap);
    m->status_kind = kind;
    m->sticky = 1;
    m->rev++;
}

static void say_locked(mgr_t *m, int kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(m, kind, fmt, ap);
    va_end(ap);
}

static void say(mgr_t *m, int kind, const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&m->mu);
    va_start(ap, fmt);
    vsay(m, kind, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&m->mu);
}

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(WORK "/manager.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static int is_addin(const pkg_t *p) { return !strcmp(p->kind, "addin"); }
static int safe(const char *s) { return !strpbrk(s, "'\\\n`$"); }   /* fits in a single-quoted shell word */

/* A child with MPC's LD_PRELOAD and the rest of its environment dropped; output goes to the log. */
static int run(char *const argv[]) {
    static char *const env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", NULL};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, WORK "/manager.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { logf_("spawn %s: %s", argv[0], strerror(rc)); return -1; }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int sh(const char *cmd) { char *argv[] = {"/bin/sh", "-c", (char *)cmd, NULL}; return run(argv); }

/* ---- HTTPS through the system libcurl ---- */

typedef void CURL;
static struct {
    int tried, ok;
    CURL *(*init)(void);
    int (*setopt)(CURL *, int, ...);
    int (*perform)(CURL *);
    void (*cleanup)(CURL *);
    const char *(*strerror)(int);
} cu;
enum { CU_WRITEDATA = 10001, CU_URL = 10002, CU_USERAGENT = 10018, CU_CAINFO = 10065, CU_WRITEFUNCTION = 20011,
       CU_XFERINFOFUNCTION = 20219, CU_XFERINFODATA = 10057, CU_NOPROGRESS = 43, CU_FAILONERROR = 45,
       CU_FOLLOWLOCATION = 52, CU_CONNECTTIMEOUT = 78, CU_LOW_SPEED_LIMIT = 19, CU_LOW_SPEED_TIME = 20, CU_RESUME_FROM_LARGE = 30116 };

static int curl_load(void) {
    if (cu.tried) return cu.ok;
    cu.tried = 1;
    void *h = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
    cu.init = (CURL * (*)(void)) dlsym(h, "curl_easy_init");
    cu.setopt = (int (*)(CURL *, int, ...))dlsym(h, "curl_easy_setopt");
    cu.perform = (int (*)(CURL *))dlsym(h, "curl_easy_perform");
    cu.cleanup = (void (*)(CURL *))dlsym(h, "curl_easy_cleanup");
    cu.strerror = (const char *(*)(int))dlsym(h, "curl_easy_strerror");
    cu.ok = cu.init && cu.setopt && cu.perform && cu.cleanup && cu.strerror;
    return cu.ok;
}

typedef struct { char *buf; size_t len, cap; FILE *f; } sink_t;

static size_t on_data(char *p, size_t sz, size_t n, void *ud) {
    sink_t *s = ud;
    size_t k = sz * n;
    if (s->f) return fwrite(p, 1, k, s->f);
    if (s->len + k + 1 > s->cap) {
        size_t cap = (s->len + k + 1) * 2;
        char *b = realloc(s->buf, cap);
        if (!b) return 0;
        s->buf = b, s->cap = cap;
    }
    memcpy(s->buf + s->len, p, k);
    s->len += k;
    s->buf[s->len] = 0;
    return k;
}

typedef struct { mgr_t *m; const char *label; long long base; int pct; } prog_t;

/* the footer's progress bar and status line, once per percent */
static int on_progress(void *ud, long long total, long long now, long long ut, long long un) {
    prog_t *g = ud;
    (void)ut, (void)un;
    if (g->label && total > 0) {
        int pct = (int)((g->base + now) * 100 / (g->base + total));
        if (pct != g->pct) {
            g->pct = pct;
            pthread_mutex_lock(&g->m->mu);
            g->m->progress = pct / 100.0f;
            say_locked(g->m, OK, "Downloading %s  \xc2\xb7  %d%%  \xc2\xb7  then checking its sha256", g->label, pct);
            pthread_mutex_unlock(&g->m->mu);
        }
    }
    return g->m->quit;   /* non-zero aborts the transfer when the plugin is removed */
}

/* to memory (s->f NULL) or to a file, resuming from byte `from`; 0 on success */
static int fetch_from(mgr_t *m, const char *url, sink_t *s, const char *label, long long from) {
    prog_t g = {m, label, from, -1};
    if (!curl_load()) { logf_("libcurl.so.4 not available"); return -1; }
    CURL *c = cu.init();
    if (!c) return -1;
    cu.setopt(c, CU_URL, url);
    cu.setopt(c, CU_FOLLOWLOCATION, 1L);
    cu.setopt(c, CU_FAILONERROR, 1L);
    cu.setopt(c, CU_USERAGENT, "mpc-plugin-manager/0.1");
    if (is_file("/etc/ssl/certs/ca-certificates.crt")) cu.setopt(c, CU_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    cu.setopt(c, CU_CONNECTTIMEOUT, 15L);
    cu.setopt(c, CU_LOW_SPEED_LIMIT, 100L);
    cu.setopt(c, CU_LOW_SPEED_TIME, 60L);
    if (from > 0) cu.setopt(c, CU_RESUME_FROM_LARGE, (long long)from);
    cu.setopt(c, CU_WRITEFUNCTION, on_data);
    cu.setopt(c, CU_WRITEDATA, s);
    cu.setopt(c, CU_NOPROGRESS, 0L);
    cu.setopt(c, CU_XFERINFOFUNCTION, on_progress);
    cu.setopt(c, CU_XFERINFODATA, &g);
    int rc = cu.perform(c);
    if (rc) logf_("fetch %s: %s", url, cu.strerror(rc));
    cu.cleanup(c);
    return rc ? -1 : 0;
}

static int fetch(mgr_t *m, const char *url, sink_t *s) { return fetch_from(m, url, s, NULL, 0); }

static int sha_ok(const char *sha, const char *path) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "echo '%s  %s' | sha256sum -c -", sha, path);
    return is_file(path) && sh(cmd) == 0;
}

/* Download to path until its sha256 matches: a kept zip that already matches is reused, a broken transfer resumes
 * where it stopped, a wrong file starts over. 0 on success. */
static int download(mgr_t *m, const pkg_t *p, const char *path) {
    for (int attempt = 1; attempt <= 4 && !m->quit; attempt++) {
        if (sha_ok(p->sha, path)) return 0;
        struct stat st;
        long long have = attempt > 1 && stat(path, &st) == 0 ? (long long)st.st_size : 0;
        if (attempt > 1) say(m, WARN, "%s: connection stalled  \xc2\xb7  resuming, retry %d/3", p->name, attempt - 1);
        sink_t s = {0};
        s.f = fopen(path, have ? "ab" : "wb");
        if (!s.f) return -1;
        int rc = fetch_from(m, p->url, &s, p->name, have);
        fclose(s.f);
        if (rc == 0 && !sha_ok(p->sha, path)) unlink(path);   /* complete but wrong (a server that ignored the range) */
        else if (rc == 0) return 0;
    }
    return -1;
}

/* ---- just enough JSON for catalog.json ---- */

static const char *ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static const char *jstr(const char *p, char *out, size_t n) {   /* p at '"'; returns past the closing quote */
    size_t k = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            c = *++p;
            if (!c) return NULL;
            if (c == 'n') c = ' ';
            else if (c == 'u') { c = '?'; for (int i = 0; i < 4 && p[1]; i++) p++; }
        }
        if (out && k + 1 < n) out[k++] = c;
    }
    if (out && n) out[k] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static const char *jskip(const char *p) {
    p = ws(p);
    if (*p == '"') return jstr(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') { p = jstr(ws(p), NULL, 0); if (!p) return NULL; p = ws(p); if (*p++ != ':') return NULL; }
            p = jskip(p);
            if (!p) return NULL;
            p = ws(p);
            if (*p == ',') { p++; continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    while (*p && !strchr(",}] \n\r\t", *p)) p++;
    return p;
}

/* Iterate an object: calls f(key, value_ptr, ud) for each member; f returns past the value (or NULL to skip it). */
typedef const char *(*member_fn)(const char *key, const char *v, void *ud);
static const char *jobject(const char *p, member_fn f, void *ud) {
    char key[64];
    p = ws(p);
    if (*p != '{') return NULL;
    p = ws(p + 1);
    if (*p == '}') return p + 1;
    for (;;) {
        p = jstr(ws(p), key, sizeof key);
        if (!p) return NULL;
        p = ws(p);
        if (*p++ != ':') return NULL;
        p = ws(p);
        const char *q = f(key, p, ud);
        p = q ? q : jskip(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p == ',') { p++; continue; }
        return *p == '}' ? p + 1 : NULL;
    }
}

typedef struct { char version[24], url[512], sha[72], channel[16], cpu[8], tested[48]; long long size; int yanked; } ver_t;
typedef struct { char s[48]; } str_t;

static const char *verdict_member(const char *k, const char *v, void *ud) {
    return *v == '"' && !strcmp(k, "verdict") ? jstr(v, ((str_t *)ud)->s, sizeof ((str_t *)ud)->s) : NULL;
}
static const char *device_member(const char *k, const char *v, void *ud) {
    return *v == '"' && !strcmp(k, "device") ? jstr(v, ((str_t *)ud)->s, sizeof ((str_t *)ud)->s) : NULL;
}

static const char *ver_member(const char *k, const char *v, void *ud) {
    ver_t *x = ud;
    if (!strcmp(k, "channel") && *v == '"') return jstr(v, x->channel, sizeof x->channel);
    if (!strcmp(k, "size") && *v >= '0' && *v <= '9') { x->size = atoll(v); return NULL; }
    if (!strcmp(k, "cpu") && *v == '{') {   /* the bench verdict: PASS, WARN or FAIL */
        str_t t = {{0}};
        const char *q = jobject(v, verdict_member, &t);
        snprintf(x->cpu, sizeof x->cpu, "%s", t.s);
        return q;
    }
    if (!strcmp(k, "tested") && *v == '[') {   /* devices it was tested on: the first one */
        const char *q = ws(v + 1);
        while (q && *q == '{') {
            str_t t = {{0}};
            q = jobject(q, device_member, &t);
            if (q && !x->tested[0]) snprintf(x->tested, sizeof x->tested, "%s", t.s);
            q = q ? ws(q) : NULL;
            if (q && *q == ',') q = ws(q + 1);
        }
        return q && *q == ']' ? q + 1 : NULL;
    }
    if (!strcmp(k, "version")) return jstr(v, x->version, sizeof x->version);
    if (!strcmp(k, "url")) return jstr(v, x->url, sizeof x->url);
    if (!strcmp(k, "sha256")) return jstr(v, x->sha, sizeof x->sha);
    if (!strcmp(k, "yanked")) { x->yanked = !strncmp(v, "true", 4); return NULL; }
    return NULL;
}

typedef struct { char uid[16]; } comp_t;
static const char *comp_member(const char *k, const char *v, void *ud) {
    return *v == '"' && !strcmp(k, "uid") ? jstr(v, ((comp_t *)ud)->uid, sizeof ((comp_t *)ud)->uid) : NULL;
}

/* "MdOn" (a 4-character VST id, as the catalog writes it) -> "4d644f6e" (as MPC.settings writes it) */
static void uid_hex(const char *u, char *out) {
    out[0] = 0;
    if (strlen(u) == 4) snprintf(out, 16, "%02x%02x%02x%02x", (unsigned char)u[0], (unsigned char)u[1], (unsigned char)u[2], (unsigned char)u[3]);
    else if (strlen(u) == 8) snprintf(out, 16, "%s", u);
}

static const char *pkg_member(const char *k, const char *v, void *ud) {
    pkg_t *x = ud;
    if (*v == '"') {
        if (!strcmp(k, "id")) return jstr(v, x->id, sizeof x->id);
        if (!strcmp(k, "name")) return jstr(v, x->name, sizeof x->name);
        if (!strcmp(k, "author")) return jstr(v, x->author, sizeof x->author);
        if (!strcmp(k, "kind")) return jstr(v, x->kind, sizeof x->kind);
        if (!strcmp(k, "distribution")) return jstr(v, x->dist, sizeof x->dist);
        if (!strcmp(k, "latest")) return jstr(v, x->latest, sizeof x->latest);
        if (!strcmp(k, "style")) return jstr(v, x->style, sizeof x->style);
    }
    if (!strcmp(k, "tags") && *v == '[') {   /* the first two, for the meta line */
        const char *q = ws(v + 1);
        for (int c = 0; q && *q == '"'; c++) {
            char t[32];
            q = jstr(q, t, sizeof t);
            if (q && c < 2) snprintf(x->tags + strlen(x->tags), sizeof x->tags - strlen(x->tags), "%s%s", c ? "  \xc2\xb7  " : "", t);
            q = q ? ws(q) : NULL;
            if (q && *q == ',') q = ws(q + 1);
        }
        return q && *q == ']' ? q + 1 : NULL;
    }
    if (!strcmp(k, "components") && *v == '[') {   /* build-yourself: the VST uids it installs */
        const char *p = ws(v + 1);
        for (int c = 0; *p != ']'; c++) {
            comp_t cp = {{0}};
            p = jobject(p, comp_member, &cp);
            if (!p) return NULL;
            if (c < 4) uid_hex(cp.uid, x->uids[c]);
            p = ws(p);
            if (*p == ',') p = ws(p + 1);
        }
        return p + 1;
    }
    if (!strcmp(k, "versions") && *v == '[') {   /* the latest version's download, unless it was yanked */
        const char *p = ws(v + 1);
        if (*p == ']') return p + 1;
        for (;;) {
            ver_t ver = {0};
            p = jobject(p, ver_member, &ver);
            if (!p) return NULL;
            if (!ver.yanked && !x->url[0] && ver.url[0] && (!x->latest[0] || !strcmp(ver.version, x->latest))) {
                snprintf(x->url, sizeof x->url, "%s", ver.url);
                snprintf(x->sha, sizeof x->sha, "%s", ver.sha);
                snprintf(x->channel, sizeof x->channel, "%s", ver.channel);
                snprintf(x->cpu, sizeof x->cpu, "%s", ver.cpu);
                snprintf(x->tested, sizeof x->tested, "%s", ver.tested);
                x->size = ver.size;
                if (!x->latest[0]) snprintf(x->latest, sizeof x->latest, "%s", ver.version);
            }
            p = ws(p);
            if (*p == ',') { p = ws(p + 1); continue; }
            return *p == ']' ? p + 1 : NULL;
        }
    }
    return NULL;
}

typedef struct { pkg_t *pkg; int n; } list_t;

static const char *top_member(const char *k, const char *v, void *ud) {
    list_t *l = ud;
    if (strcmp(k, "plugins") || *v != '[') return NULL;
    const char *p = ws(v + 1);
    if (*p == ']') return p + 1;
    for (;;) {
        pkg_t x;
        memset(&x, 0, sizeof x);
        p = jobject(p, pkg_member, &x);
        if (!p) return NULL;
        if (x.id[0] && l->n < MAXPKG) l->pkg[l->n++] = x;
        p = ws(p);
        if (*p == ',') { p = ws(p + 1); continue; }
        return *p == ']' ? p + 1 : NULL;
    }
}

/* ---- the device: everything model-specific is read from it, nothing is assumed ---- */

/* Models differ in where plugins live (MPC One: /media/az01-internal/Synths, /sdcard is a stub on the full root fs;
 * Force: /sdcard/Synths), in what MPC preloads into its children, and possibly in their tools. So: install
 * locations come from MPC.settings' SynthContentLocations, filtered to internal, writable, exec-allowed filesystems;
 * what is installed comes from the pluginList-arm entries (uid, file=) and the mpc-plugin.json next to each .so;
 * missing tools are reported on screen and in WORK/device.txt, which testers attach to a report. */

typedef struct { char name[64], uid[16], file[256]; } entry_t;
typedef struct { char id[48], version[24], folder[256]; int removable; } addin_t;   /* an installed addin folder */
struct device {
    char settings[200], base[200], state[220], arch[32], target[200], problem[24], keys[160], via[48];
    char loc[8][200];
    char addins[64];   /* ADDINS_DIR when its parent is a writable folder, else "" (no addin can be installed) */
    int nloc, nent, naddin, systemd_run, cgroup2;
    entry_t ent[128];
    addin_t addin[32];
};

static int on_path(const char *tool) {
    static const char *dirs[] = {"/usr/bin", "/bin", "/usr/sbin", "/sbin"};
    char p[64];
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", dirs[i], tool);
        if (access(p, X_OK) == 0) return 1;
    }
    return 0;
}

static char *slurp(const char *path, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

/* an attribute of one <PLUGIN .../> tag (tag..end, whitespace already flattened to spaces) */
static void attr(const char *tag, const char *end, const char *name, char *out, size_t n) {
    char pat[32];
    snprintf(pat, sizeof pat, " %s=\"", name);
    out[0] = 0;
    const char *p = strstr(tag, pat), *q;
    if (!p || p >= end) return;
    p += strlen(pat);
    if ((q = strchr(p, '"')) && q < end) snprintf(out, n, "%.*s", (int)(q - p), p);
}

/* the filesystem a path is on, from /proc/mounts (longest matching mount point); why it can't hold a plugin, or NULL */
static const char *bad_fs(const char *path, char *why, size_t n) {
    FILE *f = fopen("/proc/mounts", "r");
    char dev[128], mnt[256], type[32], opts[512];
    size_t best = 0;
    int ok = 0;
    snprintf(why, n, "not on a mounted filesystem");
    while (f && fscanf(f, "%127s %255s %31s %511s %*d %*d", dev, mnt, type, opts) == 4) {
        size_t l = strlen(mnt);
        if (strncmp(path, mnt, l) || (path[l] && path[l] != '/' && l > 1) || l < best) continue;
        best = l;
        int rw = !strncmp(opts, "rw", 2), exec = !strstr(opts, "noexec");
        int fs = FS_ANY || !strcmp(type, "ext4") || !strcmp(type, "ext3") || !strcmp(type, "f2fs") || !strcmp(type, "btrfs");
        ok = rw && exec && fs;
        snprintf(why, n, "%s is %s%s%s", mnt, type, rw ? "" : ", read-only", exec ? "" : ", noexec");
    }
    if (f) fclose(f);
    return ok ? NULL : why;
}

/* why a Synths folder can't take a plugin (.so), or NULL if it can */
static const char *bad_target(const char *path, char *why, size_t n) {
    char loc[200], parent[200];
    struct statvfs sv;
    snprintf(loc, sizeof loc, "%s", path);
    for (size_t l = strlen(loc); l > 1 && loc[l - 1] == '/'; ) loc[--l] = 0;
    snprintf(parent, sizeof parent, "%s", loc);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0;
    const char *leaf = strrchr(loc, '/');   /* plugin folders go in a Synths folder, never Akai's Expansions content */
    if (!leaf || strcmp(leaf, "/Synths")) return "not a Synths folder";
    if (!strncmp(loc, "/usr/", 5)) return "factory content";
    if (!safe(loc) || strchr(loc, '&') || strchr(loc, '|')) return "unsupported characters";
    const char *at = is_dir(loc) ? loc : parent;
    if (!is_dir(at)) return "missing";
    if (bad_fs(at, why, n)) return why;
    if (statvfs(at, &sv) || (unsigned long long)sv.f_bavail * sv.f_frsize < 64ull << 20) return "under 64 MB free";
    return NULL;
}

static int good_target(const char *loc) {
    char why[320];
    return !bad_target(loc, why, sizeof why);
}

/* the Synths folder this plugin runs from: proof that MPC loads plugins (and their skins) from there */
static void own_synths(char *out, size_t n) {
    Dl_info di;
    out[0] = 0;
    if (!dladdr((void *)own_synths, &di) || !di.dli_fname || di.dli_fname[0] != '/') return;
    snprintf(out, n, "%s", di.dli_fname);
    for (int i = 0; i < 2; i++) { char *sl = strrchr(out, '/'); if (sl && sl != out) *sl = 0; }
}

static void probe_device(device_t *d) {
    memset(d, 0, sizeof *d);
    struct utsname u;
    snprintf(d->arch, sizeof d->arch, "%s", uname(&u) == 0 ? u.machine : "?");
    DIR *sd = opendir("/media/az01-internal/Settings");   /* the same glob as install.sh */
    struct dirent *e;
    while (sd && (e = readdir(sd)) && !d->settings[0]) {
        char p[300];
        snprintf(p, sizeof p, "/media/az01-internal/Settings/%s/MPC.settings", e->d_name);
        if (e->d_name[0] != '.' && is_file(p)) snprintf(d->settings, sizeof d->settings, "%s", p);
    }
    if (sd) closedir(sd);
    char *x = d->settings[0] ? slurp(d->settings, 4 << 20) : NULL;
    if (x) {
        char *a = strstr(x, "<SynthContentLocations>"), *b = a ? strstr(a, "</SynthContentLocations>") : NULL;
        for (char *p = a; b && d->nloc < 8 && (p = strstr(p, "<Location>")) && p < b; ) {
            p += 10;
            char *q = strstr(p, "</Location>");
            if (!q || q > b) break;
            snprintf(d->loc[d->nloc++], sizeof d->loc[0], "%.*s", (int)(q - p), p);
        }
        for (char *p = x; (p = strstr(p, "<VALUE name=\"pluginList")); p++) {   /* which plugin lists exist (Gen2 may differ) */
            char *k = p + 13, *q = strchr(k, '"');   /* past '<VALUE name="' */
            if (q && strlen(d->keys) + (size_t)(q - k) + 2 < sizeof d->keys) strncat(d->keys, k, q - k), strcat(d->keys, " ");
        }
        a = strstr(x, "<VALUE name=\"pluginList-arm\">");
        b = a ? strstr(a, "</VALUE>") : NULL;
        for (char *p = a; b && d->nent < 128 && (p = strstr(p, "<PLUGIN ")) && p < b; p++) {
            char *q = strstr(p, "/>");
            if (!q) break;
            entry_t *en = &d->ent[d->nent++];
            for (char *c = p; c < q; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
            attr(p, q, "name", en->name, sizeof en->name);
            attr(p, q, "uid", en->uid, sizeof en->uid);
            attr(p, q, "file", en->file, sizeof en->file);
        }
        free(x);
    }
    /* internal storage first (One: /media/az01-internal, Force: /sdcard = /media/az01-internal-sd): a plugin on a USB
     * stick or card disappears with it; any other good location only as a last resort */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < d->nloc && !d->target[0]; i++) {
            int internal = !strncmp(d->loc[i], "/media/az01-internal", 20) || !strncmp(d->loc[i], "/sdcard/", 8);
            if ((pass || internal) && good_target(d->loc[i])) snprintf(d->target, sizeof d->target, "%s", d->loc[i]);
        }
    if (d->target[0]) snprintf(d->via, sizeof d->via, "SynthContentLocations");
    /* none listed is usable (a trailing "/", only card or factory locations, ...): the folder this plugin was installed
     * in, then where other plugin folders are, then the internal drive's default */
    char cand[200];
    own_synths(cand, sizeof cand);
    if (!d->target[0] && cand[0] && good_target(cand))
        snprintf(d->target, sizeof d->target, "%s", cand), snprintf(d->via, sizeof d->via, "the Plugin Manager's folder");
    for (int i = 0; i < d->nent && !d->target[0]; i++) {
        snprintf(cand, sizeof cand, "%s", d->ent[i].file);
        char *sl = strrchr(cand, '/');
        if (sl) *sl = 0;
        sl = strrchr(cand, '/');
        if (sl && strstr(sl, " - VST - ") && (*sl = 0, good_target(cand)))
            snprintf(d->target, sizeof d->target, "%s", cand), snprintf(d->via, sizeof d->via, "an installed plugin");
    }
    static const char *defaults[] = {"/media/az01-internal/Synths", "/sdcard/Synths"};
    for (int i = 0; i < 2 && !d->target[0]; i++)
        if (good_target(defaults[i])) snprintf(d->target, sizeof d->target, "%s", defaults[i]), snprintf(d->via, sizeof d->via, "default");
    for (size_t l = strlen(d->target); l > 1 && d->target[l - 1] == '/'; ) d->target[--l] = 0;
    /* the manager's own record of versions it installed, next to the Settings folder */
    snprintf(d->base, sizeof d->base, "%s", d->settings);
    char *cut = strstr(d->base, "/Settings/");
    if (cut) *cut = 0; else snprintf(d->base, sizeof d->base, "/media/az01-internal");
    snprintf(d->state, sizeof d->state, "%s/.pluginmgr-installed", d->base);
    d->systemd_run = on_path("systemd-run");
    d->cgroup2 = is_file("/sys/fs/cgroup/cgroup.procs");
    /* addins: one folder per id under ADDINS_DIR, each with the addin.manifest the installer wrote (ADDIN_ID,
     * ADDIN_VERSION, plain shell assignments) and a copy of its uninstall.sh; a folder without one was made by hand */
    char parent[64];
    snprintf(parent, sizeof parent, "%s", ADDINS_DIR);
    char *ps = strrchr(parent, '/');
    if (ps && ps != parent) *ps = 0;
    if (is_dir(parent) && access(parent, W_OK) == 0) snprintf(d->addins, sizeof d->addins, "%s", ADDINS_DIR);
    DIR *ad = d->addins[0] ? opendir(d->addins) : NULL;
    while (ad && (e = readdir(ad)) && d->naddin < 32) {
        addin_t *a = &d->addin[d->naddin];
        char mp[300];
        if (e->d_name[0] == '.' || !safe(e->d_name)) continue;
        snprintf(a->folder, sizeof a->folder, "%s/%s", d->addins, e->d_name);
        snprintf(mp, sizeof mp, "%s/addin.manifest", a->folder);
        char *mf = slurp(mp, 64 << 10);
        if (!mf) continue;
        a->id[0] = a->version[0] = 0;
        for (char *l = mf; l; ) {
            char *nl = strchr(l, '\n'), *val = NULL, *out = NULL;
            size_t n = 0;
            if (!strncmp(l, "ADDIN_ID=", 9)) val = l + 9, out = a->id, n = sizeof a->id;
            else if (!strncmp(l, "ADDIN_VERSION=", 14)) val = l + 14, out = a->version, n = sizeof a->version;
            if (val) {
                size_t len = strcspn(val, "\r\n");
                if (len >= 2 && (*val == '"' || *val == '\'')) val++, len -= 2;   /* the installer quotes values */
                snprintf(out, n, "%.*s", (int)len, val);
            }
            l = nl ? nl + 1 : NULL;
        }
        snprintf(mp, sizeof mp, "%s/uninstall.sh", a->folder);
        a->removable = is_file(mp);
        free(mf);
        if (a->id[0] && safe(a->id)) d->naddin++;
    }
    if (ad) closedir(ad);
    const char *pb = !d->settings[0] ? "No MPC.settings"
                   : !d->target[0] ? "No install location"
                   : !on_path("systemctl") ? "No systemctl"
                   : !d->systemd_run && !d->cgroup2 ? "No systemd-run"
                   : !on_path("unzip") ? "No unzip"
                   : !on_path("sha256sum") ? "No sha256sum"
                   : !curl_load() ? "No libcurl" : "";
    snprintf(d->problem, sizeof d->problem, "%s", pb);
}

static void write_report(const device_t *d) {
    FILE *f = fopen(WORK "/device.txt", "w");
    if (!f) return;
    struct utsname u;
    if (uname(&u) == 0) fprintf(f, "uname: %s %s %s\n", u.machine, u.release, u.version);
    char *os = slurp("/etc/os-release", 4096);
    if (os) { char *v = strstr(os, "VERSION="); if (v) fprintf(f, "os: %.*s\n", (int)strcspn(v, "\n"), v); free(os); }
    fprintf(f, "settings: %s\nplugin lists: %s\nstate: %s\n", d->settings[0] ? d->settings : "(not found)", d->keys, d->state);
    char why[320], own[200];
    for (int i = 0; i < d->nloc; i++) {
        const char *bad = bad_target(d->loc[i], why, sizeof why);
        fprintf(f, "location: %s  (%s)\n", d->loc[i], bad ? bad : "usable");
    }
    own_synths(own, sizeof own);
    fprintf(f, "manager folder: %s\ninstall target: %s%s%s%s\n", own[0] ? own : "(unknown)", d->target[0] ? d->target : "(none)",
            d->via[0] ? "  (from " : "", d->via, d->via[0] ? ")" : "");
    fprintf(f, "addins: %s\n", d->addins[0] ? d->addins : "(" ADDINS_DIR " not usable)");
    for (int i = 0; i < d->naddin; i++)
        fprintf(f, "addin: %s %s %s%s\n", d->addin[i].id, d->addin[i].version, d->addin[i].folder, d->addin[i].removable ? "" : "  (no uninstall.sh)");
    fprintf(f, "tools: systemctl=%d systemd-run=%d cgroup2=%d unzip=%d sha256sum=%d libcurl=%d ca-bundle=%d\n",
            on_path("systemctl"), d->systemd_run, d->cgroup2, on_path("unzip"), on_path("sha256sum"), curl_load(),
            is_file("/etc/ssl/certs/ca-certificates.crt"));
    char *env = getenv("LD_PRELOAD");
    fprintf(f, "MPC LD_PRELOAD: %s\n", env ? env : "(none)");
    for (int i = 0; i < d->nent; i++)
        fprintf(f, "plugin: %s uid=%s file=%s%s\n", d->ent[i].name, d->ent[i].uid, d->ent[i].file, is_file(d->ent[i].file) ? "" : "  (MISSING)");
    fprintf(f, "problem: %s\n", d->problem[0] ? d->problem : "none");
    fclose(f);
}

typedef struct { char id[48], version[24]; } manifest_t;
static const char *manifest_member(const char *k, const char *v, void *ud) {
    manifest_t *x = ud;
    if (*v == '"' && !strcmp(k, "id")) return jstr(v, x->id, sizeof x->id);
    if (*v == '"' && !strcmp(k, "version")) return jstr(v, x->version, sizeof x->version);
    return NULL;
}

/* Match each catalog plugin to a plugin-list entry: by the mpc-plugin.json id in its folder, else a component uid,
 * else the name. The Synths folder is the entry's file= minus "<vendor> - VST - <name>/<so>"; an entry whose .so is
 * not in such a folder is an old-layout install (synths left empty, shown as "old"). An addin is matched by id to
 * the folders under ADDINS_DIR; one without uninstall.sh is shown as "old" too (it can be updated, not removed). */
static void scan_installed(const device_t *d, pkg_t *pkg, int n) {
    for (int i = 0; i < n; i++) pkg[i].installed[0] = pkg[i].synths[0] = 0, pkg[i].legacy = 0;
    for (int k = 0; k < d->nent; k++) {
        const entry_t *en = &d->ent[k];
        char folder[256], mpath[300];
        snprintf(folder, sizeof folder, "%s", en->file);
        char *sl = strrchr(folder, '/');
        if (!sl) continue;
        *sl = 0;
        manifest_t mf = {{0}, {0}};
        snprintf(mpath, sizeof mpath, "%s/mpc-plugin.json", folder);
        char *js = slurp(mpath, 64 << 10);
        if (js) { jobject(js, manifest_member, &mf); free(js); }
        for (int i = 0; i < n; i++) {
            pkg_t *p = &pkg[i];
            if (is_addin(p)) continue;
            int hit = mf.id[0] ? !strcmp(mf.id, p->id) : !strcmp(en->name, p->name);
            for (int c = 0; c < 4 && !hit; c++) hit = p->uids[c][0] && !strcasecmp(p->uids[c], en->uid);
            if (!hit || p->installed[0]) continue;
            snprintf(p->installed, sizeof p->installed, "%s", mf.version[0] ? mf.version : "?");
            const char *base = strrchr(folder, '/');
            if (base && strstr(base, " - VST - ")) snprintf(p->synths, sizeof p->synths, "%.*s", (int)(base - folder), folder);
            else p->legacy = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        pkg_t *p = &pkg[i];
        if (!is_addin(p)) continue;
        for (int k = 0; k < d->naddin; k++)
            if (!strcmp(d->addin[k].id, p->id) && !p->installed[0]) {
                snprintf(p->installed, sizeof p->installed, "%s", d->addin[k].version[0] ? d->addin[k].version : "?");
                snprintf(p->synths, sizeof p->synths, "%s", d->addin[k].folder);
                p->legacy = !d->addin[k].removable;
            }
    }
    FILE *f = fopen(d->state, "r");
    char id[64], ver[32];
    while (f && fscanf(f, "%63s %31s", id, ver) == 2)
        for (int i = 0; i < n; i++)
            if (!strcmp(pkg[i].id, id) && !strcmp(pkg[i].installed, "?")) snprintf(pkg[i].installed, sizeof pkg[i].installed, "%s", ver);
    if (f) fclose(f);
}

/* ---- jobs ---- */

static void do_refresh(mgr_t *m) {
    say(m, OK, "Loading the plugin catalog\xe2\x80\xa6");
    sink_t s = {0};
    int got = fetch(m, CATALOG_URL, &s) == 0;
    pkg_t *tmp = calloc(MAXPKG, sizeof *tmp);
    list_t l = {tmp, 0};
    if (!got || !tmp || !jobject(s.buf, top_member, &l)) {
        free(s.buf);
        free(tmp);
        pthread_mutex_lock(&m->mu);
        m->online = 0;
        say_locked(m, ERR, got ? "The plugin catalog could not be read" : "Can't reach the plugin catalog");
        pthread_mutex_unlock(&m->mu);
        return;
    }
    free(s.buf);
    probe_device(m->dev);
    write_report(m->dev);
    scan_installed(m->dev, tmp, l.n);
    struct statvfs sv;   /* the header's storage readout: the drive plugins go to */
    const char *disk = m->dev->target[0] ? m->dev->target : "/media/az01-internal";
    float used = 0;
    char freebuf[32] = "";
    if (statvfs(disk, &sv) == 0 && sv.f_blocks) {
        double fr = (double)sv.f_bavail * sv.f_frsize;
        used = 1.0f - (float)sv.f_bavail / (float)sv.f_blocks;
        if (fr >= 1e9) snprintf(freebuf, sizeof freebuf, "%.1f GB free", fr / 1e9);
        else snprintf(freebuf, sizeof freebuf, "%.0f MB free", fr / 1e6);
    }
    pthread_mutex_lock(&m->mu);
    char sel[48] = "";
    if (m->sel >= 0 && m->sel < m->npkg) snprintf(sel, sizeof sel, "%s", m->pkg[m->sel].id);
    for (int i = 0; i < l.n; i++)   /* keep what was queued and selected */
        for (int j = 0; j < m->npkg; j++)
            if (!strcmp(tmp[i].id, m->pkg[j].id)) tmp[i].queued = m->pkg[j].queued;
    memcpy(m->pkg, tmp, sizeof(pkg_t) * l.n);
    m->npkg = l.n;
    m->sel = m->menu = -1;
    for (int i = 0; i < l.n; i++) if (!strcmp(tmp[i].id, sel)) m->sel = i;
    m->online = m->loaded = 1;
    m->disk_used = used;
    snprintf(m->disk_free, sizeof m->disk_free, "%s", freebuf);
    if (m->dev->problem[0]) say_locked(m, ERR, "Installing is turned off on this MPC (%s)", m->dev->problem);
    else { m->sticky = 0; m->rev++; }
    pthread_mutex_unlock(&m->mu);
    free(tmp);
}

/* Download, check and unpack every queued package, then write apply.sh. Nothing on the device changes yet. */
static void prepare(mgr_t *m, pkg_t *q) {
    int nq = 0;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < m->npkg; i++) if (m->pkg[i].queued) q[nq++] = m->pkg[i];
    m->failed = 0;
    m->downloading = 1;
    m->progress = 0;
    pthread_mutex_unlock(&m->mu);
    char path[512], cmd[1400];
    const char *err = NULL;
    int i;
    for (i = 0; i < nq && !err; i++) {
        pkg_t *p = &q[i];
        if (is_addin(p) && p->queued == Q_REMOVE) continue;   /* removed by the uninstall.sh in its folder: nothing to fetch */
        if (!p->url[0] || strlen(p->sha) != 64 || !safe(p->id) || !safe(p->name)) { err = "has no download in the catalog"; break; }
        snprintf(path, sizeof path, WORK "/%s.zip", p->id);
        if (download(m, p, path)) { err = "download failed after 3 retries"; break; }
        snprintf(cmd, sizeof cmd, "rm -rf '" WORK "/%s' && unzip -q -o '%s' -d '" WORK "/%s'", p->id, path, p->id);
        if (sh(cmd)) { err = "could not be unpacked"; break; }
        p->dir[0] = 0;   /* the zip holds one top folder with install.sh / uninstall.sh */
        snprintf(path, sizeof path, WORK "/%s", p->id);
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char f[600];
            snprintf(f, sizeof f, "%s/%s/install.sh", path, e->d_name);
            if (e->d_name[0] != '.' && is_file(f) && safe(e->d_name)) snprintf(p->dir, sizeof p->dir, "%s/%s", path, e->d_name);
        }
        if (d) closedir(d);
        if (!p->dir[0]) err = "is not a valid package";
    }
    FILE *f = err ? NULL : fopen(WORK "/apply.sh", "w");
    if (!err && !f) err = "apply.sh could not be written";
    if (f) {
        fprintf(f, "#!/bin/sh\n# written by the Plugin Manager; runs as a transient systemd unit, outside MPC\n"
                   "exec >>" WORK "/apply.log 2>&1\necho \"== apply $(date)\"\nSTATE='%s'\n"
                   "systemctl stop acvs\ni=0; while pidof MPC >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done\n"
                   "setstate() { grep -v \"^$1 \" \"$STATE\" 2>/dev/null > \"$STATE.new\"; [ -n \"$2\" ] && echo \"$1 $2\" >> \"$STATE.new\"; mv \"$STATE.new\" \"$STATE\"; }\n"
                   "batch() { grep -q -- '-n)' \"$1\" && printf '%%s' -n; }   # older installers lack -n and restart MPC themselves\n",
                m->dev->state);
        for (i = 0; i < nq && !err; i++) {
            pkg_t *p = &q[i];
            /* a remove goes where it is; an update stays where it is if that folder can hold a plugin, else it (and a new
             * install) goes to the first good SynthContentLocations entry. The installer moves the entry by uid.
             * An addin's folder is ADDINS_DIR/<id>: its installer is told that folder, and its removal runs the
             * uninstall.sh kept there (-n: MPC is already stopped; no STATE record, the folder's manifest has it). */
            char target[280];
            if (is_addin(p) && p->queued == Q_REMOVE) snprintf(target, sizeof target, "%s", p->synths);
            else if (is_addin(p)) snprintf(target, sizeof target, "%s/%s", m->dev->addins, p->id);
            else snprintf(target, sizeof target, "%s", p->queued == Q_REMOVE ? p->synths
                          : p->synths[0] && good_target(p->synths) ? p->synths : m->dev->target);
            const char *synths = target;
            if (!synths[0] || !safe(synths) || (is_addin(p) && !m->dev->addins[0])) { err = "has no install location"; break; }
            if (is_addin(p) && p->queued == Q_INSTALL)
                fprintf(f, "echo '-- install %s %s'\nsh '%s/install.sh' -y -n -t '%s' && rm -rf '" WORK "/%s' '" WORK "/%s.zip'\n",
                        p->id, p->latest, p->dir, synths, p->id, p->id);
            else if (is_addin(p))
                fprintf(f, "echo '-- remove %s'\nsh '%s/uninstall.sh' -y -n -t '%s'\n", p->id, synths, synths);
            else if (p->queued == Q_INSTALL)
                fprintf(f, "echo '-- install %s %s'\nsh '%s/install.sh' -y $(batch '%s/install.sh') -t '%s' && setstate '%s' '%s' && rm -rf '" WORK "/%s' '" WORK "/%s.zip'\n",
                        p->id, p->latest, p->dir, p->dir, synths, p->id, p->latest, p->id, p->id);
            else
                fprintf(f, "echo '-- remove %s'\nsh '%s/uninstall.sh' -y $(batch '%s/uninstall.sh') -t '%s' && setstate '%s' '' && rm -rf '" WORK "/%s' '" WORK "/%s.zip'\n",
                        p->id, p->dir, p->dir, synths, p->id, p->id, p->id);
        }
        fprintf(f, "pidof MPC >/dev/null || systemctl start acvs\nrm -f \"$0\"\necho \"== done $(date)\"\n");
        fclose(f);
    }
    pthread_mutex_lock(&m->mu);
    m->downloading = 0;
    if (err) {
        m->failed = 1;
        say_locked(m, ERR, "%s: %s. Nothing on the MPC was changed.", i < nq ? q[i].name : "APPLY", err);
    } else {
        m->ready = 1;
        say_locked(m, WARN, "Ready. Save your project: APPLY restarts MPC.");
    }
    pthread_mutex_unlock(&m->mu);
}

static void do_prepare(mgr_t *m) {
    pkg_t *q = calloc(MAXPKG, sizeof *q);   /* 64 KB: kept off MPC's small thread stacks */
    if (q) prepare(m, q);
    free(q);
}

static void do_launch(mgr_t *m) {
    say(m, OK, "Restarting MPC\xe2\x80\xa6 it will be back in a few seconds");
    logf_("launching apply.sh");
#ifdef __arm__
    if (m->dev->systemd_run) {
        char unit[64];
        snprintf(unit, sizeof unit, "--unit=pluginmgr-apply-%ld", (long)time(NULL));
        char *argv[] = {"systemd-run", unit, "--collect", "/bin/sh", WORK "/apply.sh", NULL};
        if (run(argv)) say(m, ERR, "MPC could not be restarted (systemd-run failed). Nothing was changed.");
    } else {
        /* untested fallback: leave acvs.service's cgroup by hand (cgroup v2 root) so stopping MPC doesn't kill us */
        if (sh("echo $$ > /sys/fs/cgroup/cgroup.procs && (setsid /bin/sh " WORK "/apply.sh </dev/null >/dev/null 2>&1 &)"))
            say(m, ERR, "MPC could not be restarted. Nothing was changed.");
    }
#else
    say(m, OK, "Dry run: apply.sh written");   /* host tests: never touch the build machine */
#endif
}

static void *worker(void *ud) {
    mgr_t *m = ud;
    mkdir(WORK, 0755);
    pthread_mutex_lock(&m->mu);
    m->job = J_REFRESH;
    for (;;) {
        while (!m->quit && m->job == J_NONE) pthread_cond_wait(&m->cv, &m->mu);
        if (m->quit) break;
        int job = m->job;
        m->job = J_NONE;
        m->busy = job;
        m->rev++;
        pthread_mutex_unlock(&m->mu);
        if (job == J_REFRESH) do_refresh(m);
        else if (job == J_PREPARE) do_prepare(m);
        else if (job == J_LAUNCH) do_launch(m);
        pthread_mutex_lock(&m->mu);
        m->busy = 0;
        m->rev++;
    }
    pthread_mutex_unlock(&m->mu);
    return NULL;
}

/* ---- what the screen shows: everything is computed from the state on each read ---- */

static int installed(const pkg_t *p) { return p->installed[0] != 0; }
static int has_update(const pkg_t *p) {   /* a known older version, or an old-layout install */
    return p->url[0] && installed(p) && (p->legacy || (strcmp(p->installed, "?") && strcmp(p->installed, p->latest)));
}

/* the cards of the current tab and filter; build-yourself plugins (no download) are left out: not plug and play */
static int visible(const mgr_t *m, int *idx) {
    int n = 0;
    for (int i = 0; i < m->npkg; i++) {
        const pkg_t *p = &m->pkg[i];
        if (!p->url[0]) continue;
        if (m->kindf == 1 && strcmp(p->kind, "instrument")) continue;
        if (m->kindf == 2 && strcmp(p->kind, "effect")) continue;
        if (m->kindf == 3 && !is_addin(p)) continue;
        if (m->tab == 1 && !installed(p)) continue;
        if (m->tab == 2 && !has_update(p)) continue;
        idx[n++] = i;
    }
    return n;
}

enum { S_NONE, S_INSTALL, S_UPDATE, S_INSTALLED, S_Q_INSTALL, S_Q_UPDATE, S_Q_REMOVE, S_DISABLED, S_MENU };

static int card_state(const mgr_t *m, int i) {
    const pkg_t *p = &m->pkg[i];
    if (m->menu == i) return S_MENU;
    if (p->queued == Q_REMOVE) return S_Q_REMOVE;
    if (p->queued == Q_INSTALL) return installed(p) ? S_Q_UPDATE : S_Q_INSTALL;
    if (m->dev->problem[0] && !installed(p)) return S_DISABLED;
    if (!installed(p)) return S_INSTALL;
    return has_update(p) ? S_UPDATE : S_INSTALLED;
}

static int count_updates(const mgr_t *m) {
    int n = 0;
    for (int i = 0; i < m->npkg; i++) n += has_update(&m->pkg[i]);
    return n;
}

static int count_queued(const mgr_t *m) {
    int n = 0;
    for (int i = 0; i < m->npkg; i++) n += m->pkg[i].queued != 0;
    return n;
}

static void queue_text(const mgr_t *m, char *b, int n) {
    int q = count_queued(m), k;
    if (!q) { snprintf(b, n, "Nothing queued. Pick INSTALL or UPDATE on a plugin."); return; }
    k = snprintf(b, n, "%d change%s queued:", q, q > 1 ? "s" : "");
    for (int i = 0; i < m->npkg && k < n; i++)
        if (m->pkg[i].queued)
            k += snprintf(b + k, n - k, "  %s%s", m->pkg[i].queued == Q_REMOVE ? "\xe2\x88\x92" : "+", m->pkg[i].name);
}

static void initials(const char *name, char *b) {   /* first letters of the first two words that start with a letter */
    int k = 0, word = 1;
    for (const char *c = name; *c && k < 2; c++) {
        int al = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'), an = al || (*c >= '0' && *c <= '9');
        if (al && word) b[k++] = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
        word = !an;
    }
    if (k == 1 && name[1]) b[k++] = (name[1] >= 'a' && name[1] <= 'z') ? name[1] - 32 : name[1];
    b[k] = 0;
}

static void spaced(const char *in, char *out, int n) {   /* catalog slugs ("fm-synth") as words */
    int k = 0;
    for (; *in && k < n - 1; in++) out[k++] = *in == '-' ? ' ' : *in;
    out[k] = 0;
}

/* a card's field (key after "rN_"), for the plugin at pkg index i (or -1: an empty card) */
static void card_field(const mgr_t *m, int i, const char *f, char *b, int n) {
    const pkg_t *p = i >= 0 ? &m->pkg[i] : NULL;
    b[0] = 0;
    if (!strcmp(f, "vis")) snprintf(b, n, "%d", p != NULL && m->loaded);
    else if (!p) snprintf(b, n, !strcmp(f, "state") || !strcmp(f, "inst") || !strcmp(f, "chan") || !strcmp(f, "cpu") ||
                                 !strcmp(f, "old") || !strcmp(f, "tested") ? "0" : " ");
    else if (!strcmp(f, "state")) snprintf(b, n, "%d", card_state(m, i));
    else if (!strcmp(f, "inst")) snprintf(b, n, "%d", installed(p));
    else if (!strcmp(f, "init")) initials(p->name, b);
    else if (!strcmp(f, "kindtxt")) snprintf(b, n, "%s", is_addin(p) ? "ADDIN" : !strcmp(p->kind, "effect") ? "FX" : "INST");
    else if (!strcmp(f, "meta")) {
        char st[32], tg[96];
        spaced(p->style, st, sizeof st);
        spaced(p->tags, tg, sizeof tg);
        snprintf(b, n, "by %s%s%s%s%s", p->author, st[0] ? "  \xc2\xb7  " : "", st, tg[0] ? "  \xc2\xb7  " : "", tg);
    }
    else if (!strcmp(f, "chan")) snprintf(b, n, "%d", !strcmp(p->channel, "beta") ? 2 : 1);   /* 0: no card */
    else if (!strcmp(f, "cpu")) snprintf(b, n, "%d", !strcmp(p->cpu, "WARN") || !strcmp(p->cpu, "FAIL"));
    else if (!strcmp(f, "old")) snprintf(b, n, "%d", p->legacy);
    else if (!strcmp(f, "tested")) snprintf(b, n, "%d", p->tested[0] != 0);
    else if (!strcmp(f, "tested_txt")) snprintf(b, n, "Tested: %s", !strncmp(p->tested, "Akai ", 5) ? p->tested + 5 : p->tested);
    else if (!strcmp(f, "ver")) snprintf(b, n, "v%s", p->latest);
    else if (!strcmp(f, "from")) snprintf(b, n, "%s%s", has_update(p) && !p->legacy ? "from " : " ",
                                          has_update(p) && !p->legacy ? p->installed : "");
    else if (!strcmp(f, "size")) {
        if (p->size >= 1000000) snprintf(b, n, "%.1f MB", p->size / 1e6);
        else snprintf(b, n, "%lld KB", p->size / 1000);
    } else if (!strcmp(f, "sha")) snprintf(b, n, "sha %.7s", p->sha);
    else snprintf(b, n, "0");   /* the triggers */
}

/* ---- engine interface ---- */

static void *mgr_create(const char *data_dir) {
    (void)data_dir;
    mgr_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->sel = m->menu = -1;
    m->online = 1;
    snprintf(m->status, sizeof m->status, "Starting\xe2\x80\xa6");
    m->sticky = 1;
    m->dev = calloc(1, sizeof *m->dev);
    if (!m->dev) { free(m); return NULL; }
    if (pthread_create(&m->th, NULL, worker, m)) { free(m->dev); free(m); return NULL; }
    return m;
}

static void mgr_destroy(void *inst) {
    mgr_t *m = inst;
    pthread_mutex_lock(&m->mu);
    m->quit = 1;
    pthread_cond_signal(&m->cv);
    pthread_mutex_unlock(&m->mu);
    pthread_join(m->th, NULL);
    free(m->dev);
    free(m);
}

static void post(mgr_t *m, int job) {   /* called with mu held */
    if (m->busy || m->job) return;
    m->job = job;
    pthread_cond_signal(&m->cv);
}

static void requeue(mgr_t *m, pkg_t *p, int q) {   /* any change to the queue drops a prepared or failed APPLY */
    p->queued = q;
    m->ready = m->failed = 0;
    m->sticky = 0;
    m->menu = -1;
}

static void mgr_set_param(void *inst, const char *key, const char *val) {
    mgr_t *m = inst;
    double v = atof(val);
    pthread_mutex_lock(&m->mu);
    int idx[MAXPKG], nv = visible(m, idx), pages = nv ? (nv + ROWS - 1) / ROWS : 1;
    int busy = m->busy == J_PREPARE || m->busy == J_LAUNCH;
    if (!strcmp(key, "tab") || !strcmp(key, "kind")) {   /* option params: the index as a number */
        int o = (int)(v + 0.5);
        if (key[0] == 't') m->tab = o; else m->kindf = o;
        m->page = 0;
        m->menu = -1;
    } else if (!strncmp(key, "card", 4)) {   /* a tap anywhere on a card selects it (both toggle directions) */
        int r = key[4] - '1', k = m->page * ROWS + r;
        if (k >= 0 && k < nv) { m->sel = idx[k]; if (m->menu != idx[k]) m->menu = -1; }
    } else if (v < 0.5) {
        /* the other params are triggers: act on press only */
    } else if (!strcmp(key, "page_prev") && m->page > 0) { m->page--; m->menu = -1; }
    else if (!strcmp(key, "page_next") && m->page + 1 < pages) { m->page++; m->menu = -1; }
    else if (!strcmp(key, "refresh")) post(m, J_REFRESH);
    else if (!strcmp(key, "update_all") && !busy) {
        for (int i = 0; i < m->npkg; i++) if (has_update(&m->pkg[i]) && !m->pkg[i].queued) requeue(m, &m->pkg[i], Q_INSTALL);
    } else if (!strcmp(key, "apply") && !busy) {
        if (m->ready) { m->ready = 0; post(m, J_LAUNCH); }
        else if (count_queued(m) && !m->dev->problem[0]) post(m, J_PREPARE);
    } else if (key[0] == 'r' && key[1] >= '1' && key[1] <= '3' && key[2] == '_') {
        int k = m->page * ROWS + key[1] - '1';
        const char *f = key + 3;
        pkg_t *p = k < nv ? &m->pkg[idx[k]] : NULL;
        if (p && !busy) {
            int i = idx[k], st = card_state(m, i);
            m->sel = i;
            if (!strcmp(f, "act")) {
                if (st == S_INSTALL || st == S_UPDATE) requeue(m, p, Q_INSTALL);
                else if (st == S_Q_INSTALL || st == S_Q_UPDATE || st == S_Q_REMOVE) requeue(m, p, 0);
                else if (st == S_DISABLED) say_locked(m, ERR, "Installing is turned off on this MPC (%s)", m->dev->problem);
            } else if (!installed(p)) {
                /* the menu (Reinstall, Remove) is only on installed plugins */
            } else if (!strcmp(f, "more")) m->menu = m->menu == i ? -1 : i;
            else if (!strcmp(f, "reinstall")) requeue(m, p, Q_INSTALL);
            else if (!strcmp(f, "remove")) {
                if (p->legacy && is_addin(p)) { m->menu = -1; say_locked(m, ERR, "%s was installed by hand: update it first, then remove it", p->name); }
                else if (p->legacy) { m->menu = -1; say_locked(m, ERR, "%s is an old-style install: update it first, then remove it", p->name); }
                else requeue(m, p, Q_REMOVE);
            }
        }
    }
    m->rev++;
    pthread_mutex_unlock(&m->mu);
}

static int mgr_get_param(void *inst, const char *key, char *b, int n) {
    mgr_t *m = inst;
    pthread_mutex_lock(&m->mu);
    int idx[MAXPKG], nv = visible(m, idx), pages = nv ? (nv + ROWS - 1) / ROWS : 1, ok = 1;
    if (m->page >= pages) m->page = pages - 1;
    int upd = count_updates(m), q = count_queued(m), busy = m->busy == J_PREPARE || m->busy == J_LAUNCH;
    if (!strcmp(key, "display_rev")) snprintf(b, n, "%u", m->rev);
    else if (!strcmp(key, "tab")) snprintf(b, n, "%d", m->tab);
    else if (!strcmp(key, "kind")) snprintf(b, n, "%d", m->kindf);
    else if (!strcmp(key, "net")) snprintf(b, n, "%d", !m->online);
    else if (!strcmp(key, "disk")) snprintf(b, n, "%d", (int)(m->disk_used * 10 + 0.5));   /* bar steps 0..10 */
    else if (!strcmp(key, "disk_txt")) snprintf(b, n, "%s", m->disk_free[0] ? m->disk_free : " ");
    else if (!strcmp(key, "upd_badge")) snprintf(b, n, "%d", upd > 0);
    else if (!strcmp(key, "upd_count")) snprintf(b, n, "%d", upd);
    else if (!strcmp(key, "problem")) snprintf(b, n, "%d", m->dev->problem[0] != 0);
    else if (!strcmp(key, "problem_txt"))
        snprintf(b, n, "Can't install on this MPC: %s  \xc2\xb7  details in " WORK "/device.txt", m->dev->problem);
    else if (!strcmp(key, "empty")) snprintf(b, n, "%d", !m->loaded ? (m->online || m->busy == J_REFRESH ? 0 : 1) : nv == 0 ? 2 : 0);
    else if (!strcmp(key, "empty_txt"))
        snprintf(b, n, "%s", m->tab == 2 ? "Everything is up to date" : m->tab == 1 ? "No catalog plugins installed yet"
                                                                       : m->kindf == 3 ? "No addins in the catalog yet" : "No plugins match this filter");
    else if (!strcmp(key, "nav_prev")) snprintf(b, n, "%d", m->page > 0);
    else if (!strcmp(key, "nav_next")) snprintf(b, n, "%d", m->page + 1 < pages);
    else if (!strcmp(key, "page_txt")) snprintf(b, n, "%d / %d", m->page + 1, pages);
    else if (!strcmp(key, "summary"))
        snprintf(b, n, "%d %s%s %s  \xc2\xb7  %d update%s available", nv, m->kindf == 3 ? "addin" : "plugin", nv == 1 ? "" : "s",
                 m->tab == 1 ? "installed" : m->tab == 2 ? "to update" : "available", upd, upd == 1 ? "" : "s");
    else if (!strcmp(key, "status")) { if (m->sticky) snprintf(b, n, "%s", m->status); else queue_text(m, b, n); }
    else if (!strcmp(key, "status_kind")) snprintf(b, n, "%d", m->sticky ? m->status_kind : OK);
    else if (!strcmp(key, "busy")) snprintf(b, n, "%d", m->downloading);
    else if (!strcmp(key, "progress"))   /* bar steps 1..20 while downloading, 0 (no bar) otherwise */
        snprintf(b, n, "%d", m->downloading ? 1 + (int)(m->progress * 19 + 0.5) : 0);
    else if (!strcmp(key, "upd_all")) snprintf(b, n, "%d", upd > 0 && !busy);
    else if (!strcmp(key, "apply_state"))   /* 0 off, 1 apply, 2 preparing, 3 ready (restart), 4 retry */
        snprintf(b, n, "%d", busy ? 2 : m->ready ? 3 : m->failed && q ? 4 : q && !m->dev->problem[0] ? 1 : 0);
    else if (!strncmp(key, "card", 4) && key[4] >= '1' && key[4] <= '3') {   /* card<r>_1: the name, and its selection */
        int k = m->page * ROWS + key[4] - '1', i = k < nv ? idx[k] : -1;
        if (strstr(key, "_on")) snprintf(b, n, "%d", i >= 0 && i == m->sel);
        else snprintf(b, n, "%s", i >= 0 ? m->pkg[i].name : " ");
    } else if (key[0] == 'r' && key[1] >= '1' && key[1] <= '3' && key[2] == '_') {
        int k = m->page * ROWS + key[1] - '1';
        card_field(m, k < nv ? idx[k] : -1, key + 3, b, n);
    } else if (!strcmp(key, "refresh") || !strcmp(key, "page_prev") || !strcmp(key, "page_next") ||
               !strcmp(key, "update_all") || !strcmp(key, "apply") || !strcmp(key, "noop")) snprintf(b, n, "0");
    else ok = 0;
    pthread_mutex_unlock(&m->mu);
    return ok;
}

static void mgr_midi(void *inst, const uint8_t *msg, int len) { (void)inst, (void)msg, (void)len; }
static void mgr_render(void *inst, int16_t *out, int frames) { (void)inst; memset(out, 0, sizeof(int16_t) * 2 * frames); }

static const mpc_engine_t ENGINE = {mgr_create, mgr_destroy, mgr_midi, mgr_set_param, mgr_get_param, mgr_render, NULL};
__attribute__((visibility("default"))) const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
