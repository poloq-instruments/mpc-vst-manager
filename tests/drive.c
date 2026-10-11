/* Drives the engine like the screen would (tests/run.sh): waits for the catalog, prints each page of cards with
 * their fields and states, then queues an install and runs APPLY up to the (dry-run) restart. With "addins" it
 * runs the addin flow against the offline fixture instead: update, install and a refused removal. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
typedef struct { void *(*create)(const char *); void (*destroy)(void *); void (*midi)(void *, const uint8_t *, int);
  void (*set_param)(void *, const char *, const char *); int (*get_param)(void *, const char *, char *, int);
  void (*render)(void *, int16_t *, int); void *process; } E;
const E *mpc_engine(void);
static const E *e;
static void *m;

static const char *get(const char *k) { static char b[32][128]; static int i; i = (i + 1) % 32; b[i][0] = 0; e->get_param(m, k, b[i], 128); return b[i]; }
static void set(const char *k, const char *v) { e->set_param(m, k, v); }
static void press(const char *k) { set(k, "1"); set(k, "0"); }

static char cat = 'a';   /* the MPC tab being looked at: a(ll), i(nstruments), e(ffects), d (addins) */
static const char *cget(const char *f) { char k[64]; snprintf(k, sizeof k, "%c_%s", cat, f); return get(k); }
static void cset(const char *f, const char *v) { char k[64]; snprintf(k, sizeof k, "%c_%s", cat, f); set(k, v); }
static void cpress(const char *f) { cset(f, "1"); cset(f, "0"); }

static void cards(void) {
  printf("  [%c scroll %s] %s | foot %s: %s | apply_state %s\n", cat, cget("pos"), cget("count"), get("foot"), get("status"), get("apply_state"));
  for (int r = 1; r <= 4; r++) {
    char k[32];
    #define F(f) (snprintf(k, sizeof k, "r%d_" f, r), cget(k))
    snprintf(k, sizeof k, "card%d_1", r);
    if (!strcmp(F("vis"), "0")) { printf("  row %d: -\n", r); continue; }
    printf("  row %d: %-16s img=%s badges=%s state=%s trash=%s %s %s | %s\n", r, cget(k), F("img"), F("badges"),
           F("state"), F("trash"), F("ver"), F("size"), F("meta"));
  }
}

static void info(void) {
  printf("  INFO ov=%s img=%s %s | %s\n", cget("ov"), cget("info_bf"), cget("info_name"), cget("info_by"));
  char k[16];
  for (int d = 1; d <= 4; d++) { snprintf(k, sizeof k, "info_d%d", d); printf("    %s\n", cget(k)); }
  for (int l = 1; l <= 8; l++) { snprintf(k, sizeof k, "info_l%d", l); printf("    %s\n", cget(k)); }
}

static void wait_for(const char *key, const char *val) {
  for (int i = 0; i < 120; i++) { if (!strcmp(get(key), val)) return; sleep(1); }
  printf("  (timeout waiting for %s=%s)\n", key, val);
}

int main(int argc, char **argv) {
  e = mpc_engine(); m = e->create(NULL);
  for (int i = 0; i < 60 && !strstr(get("a_count"), "available") ; i++) sleep(1);
  sleep(1);
  printf("net=%s problem=%s (%s) empty=%s disk=%s %s upd_badge=%s count=%s\n", get("net"), get("problem"), get("problem_txt"), get("a_empty"),
         get("disk"), get("disk_txt"), get("upd_badge"), get("upd_count"));
  if (argc > 1 && !strcmp(argv[1], "addins")) {   /* the offline fixture (tests/fixture.py, tests/addins.sh) */
    cat = 'd'; printf("ADDINS\n"); cards();
    printf("remove alpha (row 1), install beta (row 2), remove gamma (row 3: made by hand, refused)\n");
    cpress("r1_del"); cpress("r1_remove"); cpress("r2_act"); cpress("r3_del"); cpress("r3_remove"); cards();
    press("apply"); wait_for("apply_state", "3"); cards();
    press("apply"); sleep(1); cards();
    e->destroy(m); return 0;
  }
  printf("ALL\n"); cards(); cpress("down"); cards(); cpress("up"); cards();
  printf("info on row 2\n"); cpress("r2_info"); info(); cards(); cpress("info_close"); printf("  closed: ov=%s\n", cget("ov"));
  set("tab", "2"); printf("UPDATES\n"); cards();
  set("tab", "1"); printf("INSTALLED\n"); cards();
  set("tab", "0"); cat = 'e'; printf("EFFECTS\n"); cards(); cat = 'i'; printf("INSTRUMENTS\n"); cards(); cat = 'a';
  cat = 'p'; printf("COMPANION\n"); cards(); cpress("r1_act"); printf("  NOTE ov=%s | %s | %s | %s\n", cget("ov"), cget("note_name"), cget("note_txt"), cget("note_hp")); cpress("info_close"); cpress("r1_info"); info(); cpress("info_close"); cat = 'a';
  printf("trash on row 1 (if installed), then queue row 2 and UPDATE ALL\n");
  set("tab", "1"); cpress("r1_del"); cards(); set("tab", "0"); cpress("r2_act"); press("update_all"); cards();
  press("apply"); wait_for("apply_state", "3"); cards();
  press("apply"); sleep(1); cards();
  e->destroy(m); return 0;
}
