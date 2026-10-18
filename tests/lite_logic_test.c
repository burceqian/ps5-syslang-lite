// Host-side test for the ps5-syslang-lite decision logic.
//
// loader.c only talks to the console through a handful of extern functions, so
// the whole payload can be run on a PC with a fake registry: this checks that
// the "already in this language -> report and write nothing" rule really holds,
// that a real switch is written and verified, and that a failed write is
// reported instead of silently pretending to have switched.
//
//   gcc -O0 -g -DLANGUAGE_ID=11 -DSETTLE_SECONDS=0 -o lite_logic_test lite_logic_test.c
#include <stdio.h>
#include <string.h>

// --------------------------------------------------------------------------
// fake payload environment
// --------------------------------------------------------------------------
static int g_reg_value;
static int g_sys_value;
static int g_reg_set_fail;
static int g_reg_get_fail;
static int g_set_calls;
static int g_notifications;
static char g_last_notification[512];

int sceRegMgrGetInt(int regid, int *value)
{
  (void)regid;
  if (g_reg_get_fail) {
    return (int)0x80000002;
  }
  *value = g_reg_value;
  return 0;
}

int sceRegMgrSetInt(int regid, int value)
{
  (void)regid;
  g_set_calls++;
  if (g_reg_set_fail) {
    return (int)0x80000001;
  }
  g_reg_value = value;
  return 0;
}

int sceSystemServiceParamGetInt(int param_id, int *value)
{
  (void)param_id;
  *value = g_sys_value;
  return 0;
}

int sceKernelSendNotificationRequest(int device, void *request, size_t size,
                                     int blocking)
{
  (void)device;
  (void)size;
  (void)blocking;
  g_notifications++;
  const char *message = (const char *)request + 45;  // NotifyRequest.message
  snprintf(g_last_notification, sizeof(g_last_notification), "%s", message);
  printf("    notify: %s\n", message);
  return 0;
}

int sceKernelDebugOutText(int channel, const char *text)
{
  (void)channel;
  printf("    klog  : %s", text);
  return 0;
}

// --------------------------------------------------------------------------
// the payload itself, with its main() renamed
// --------------------------------------------------------------------------
#define main syslang_main
#include "../loader.c"
#undef main

static int g_failures;

static void check(int condition, const char *what)
{
  printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what);
  if (!condition) {
    g_failures++;
  }
}

static void reset(int reg, int sys)
{
  g_reg_value = reg;
  g_sys_value = sys;
  g_reg_set_fail = 0;
  g_reg_get_fail = 0;
  g_set_calls = 0;
  g_notifications = 0;
  g_last_notification[0] = '\0';
}

int main(void)
{
  printf("== 1) registry already 11 (zh-Hans): must report and write nothing ==\n");
  reset(11, 11);
  int rc = syslang_main();
  check(g_set_calls == 0, "sceRegMgrSetInt was never called");
  check(g_reg_value == 11, "registry left at 11");
  check(g_notifications == 2, "two notifications (already + hint)");
  printf("  returns %d (expect 0)\n", rc);
  check(rc == 0, "returns success");

  printf("== 2) system says 11 but registry says 0: registry is authority ==\n");
  reset(0, 11);
  rc = syslang_main();
  check(g_set_calls == 1, "the registry was written even though sys==11");
  check(g_reg_value == 11, "registry now 11");
  check(rc == 0, "returns success");

  printf("== 3) registry 0 -> 11: write, verify, report ==\n");
  reset(0, 0);
  rc = syslang_main();
  check(g_set_calls == 1, "one write");
  check(g_reg_value == 11, "registry now 11");
  check(g_notifications == 3, "three notifications (switched + hint + settled)");
  printf("  last notification: %s\n", g_last_notification);
  check(strstr(g_last_notification, "reg=11") != NULL, "settled report carries reg=11");
  check(rc == 0, "returns success");

  printf("== 4) the write fails: report it, do not claim a switch ==\n");
  reset(0, 0);
  g_reg_set_fail = 1;
  rc = syslang_main();
  check(g_set_calls == 2, "retried the write once");
  check(g_reg_value == 0, "registry unchanged");
  check(rc == 1, "returns failure");
  printf("  last notification: %s\n", g_last_notification);
  check(strstr(g_last_notification, "klog") != NULL, "points at the klog");

  printf("== 5) the registry cannot be read: write anyway ==\n");
  reset(0, 0);
  g_reg_get_fail = 1;
  rc = syslang_main();
  check(g_set_calls >= 1, "wrote instead of assuming 'already set'");
  check(rc == 1, "returns failure (the read still fails)");

  printf("\n%s (%d failures)\n", g_failures ? "RESULT: FAIL" : "RESULT: PASS",
         g_failures);
  return g_failures ? 1 : 0;
}
