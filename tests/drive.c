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

static void cards(void) {
  printf("  [%s] %s | status(%s): %s | apply_state %s\n", get("page_txt"), get("summary"), get("status_kind"), get("status"), get("apply_state"));
  for (int r = 1; r <= 3; r++) {
    char k[32];
    #define F(f) (snprintf(k, sizeof k, "r%d_" f, r), get(k))
    snprintf(k, sizeof k, "card%d_1", r);
    if (!strcmp(F("vis"), "0")) { printf("  card %d: -\n", r); continue; }
    printf("  card %d: %-13s [%s|%s] state=%s inst=%s chan=%s cpu=%s old=%s tested=%s(%s) %s %s %s %s | %s\n", r, get(k), F("init"), F("kindtxt"),
           F("state"), F("inst"), F("chan"), F("cpu"), F("old"), F("tested"), F("tested_txt"), F("ver"), F("from"), F("size"), F("sha"), F("meta"));
  }
}

static void wait_for(const char *key, const char *val) {
  for (int i = 0; i < 120; i++) { if (!strcmp(get(key), val)) return; sleep(1); }
  printf("  (timeout waiting for %s=%s)\n", key, val);
}

int main(int argc, char **argv) {
  e = mpc_engine(); m = e->create(NULL);
  for (int i = 0; i < 60 && !strstr(get("summary"), "available") ; i++) sleep(1);
  sleep(1);
  printf("net=%s problem=%s (%s) empty=%s disk=%s %s upd_badge=%s count=%s\n", get("net"), get("problem"), get("problem_txt"), get("empty"),
         get("disk"), get("disk_txt"), get("upd_badge"), get("upd_count"));
  if (argc > 1 && !strcmp(argv[1], "addins")) {   /* the offline fixture (tests/fixture.py, tests/addins.sh) */
    set("kind", "3"); printf("ADDINS\n"); cards();
    printf("remove alpha (card 1), install beta (card 2), remove gamma (card 3: made by hand, refused)\n");
    press("r1_more"); press("r1_remove"); press("r2_act"); press("r3_more"); press("r3_remove"); cards();
    press("apply"); wait_for("apply_state", "3"); cards();
    press("apply"); sleep(1); cards();
    e->destroy(m); return 0;
  }
  printf("DISCOVER\n"); cards(); press("page_next"); cards(); press("page_next"); cards();
  set("tab", "2"); printf("UPDATES\n"); cards();
  set("tab", "1"); printf("INSTALLED\n"); cards();
  set("tab", "0"); set("kind", "2"); printf("EFFECTS\n"); cards(); set("kind", "0");
  printf("menu on card 1 (if installed), then queue card 2 and UPDATE ALL\n");
  press("r1_more"); cards(); press("r1_remove"); press("r2_act"); press("update_all"); cards();
  press("apply"); wait_for("apply_state", "3"); cards();
  press("apply"); sleep(1); cards();
  e->destroy(m); return 0;
}
