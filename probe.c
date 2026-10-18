// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only diagnostic for the J1 (JP region-locked) language problem.
//
// The -j1 payload works around the ShellUI software error by writing the
// system language straight into the registry
// (sceRegMgrSetInt(0x02020000, id)). What is still unmeasured is whether that
// write survives a cold boot. Send this probe *before* injecting the -j1
// payload, reboot, send it again, and compare:
//
//   reg == written id, sys != written id
//       -> the registry keeps the value, something else re-derives the
//          applied language (fixable from ShellUI, worth chasing)
//   reg == init value
//       -> the system clamps the registry during boot (only re-injection
//          after every boot helps)
//
// Every sceRegMgr call below is a getter; this payload writes nothing.

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCE_SYSTEM_SERVICE_PARAM_ID_LANG 1
#define REGMGR_KEY_SYSTEM_LANGUAGE 0x02020000

// Number of ids dumped from the language key upwards; handy for spotting a
// neighbouring "supported language mask" / "first boot" entry.
#define REGMGR_SCAN_KEYS 8

#define LOG_PREFIX "[ps5-syslang-probe]"
#define NOTIFY_PREFIX "PS5 SysLang Probe:"

extern int sceSystemServiceParamGetInt(int param_id, int *value);
extern int sceKernelSendNotificationRequest(int device, void *request, size_t size,
                                            int blocking);
extern int sceKernelDebugOutText(int channel, const char *text);
extern int sceRegMgrGetInt(int regid, int *value);
extern int sceRegMgrGetIntInitVal(int regid, int *value);

typedef struct NotifyRequest {
  char reserved[45];
  char message[3075];
} NotifyRequest;

static const char *const kLanguageCodes[] = {
    "ja",     "en-US", "fr-FR",  "es-ES", "de",     "it",     "nl",    "pt-PT",
    "ru",     "ko",    "zh-Hant", "zh-Hans", "fi",  "sv",     "da",    "no",
    "pl",     "pt-BR", "en-GB",  "tr",    "es-419", "ar",     "fr-CA", "cs",
    "hu",     "el",    "ro",     "th",    "vi",     "in",     "uk",
};

static const char *language_code(int language_id)
{
  if (language_id < 0 ||
      language_id >= (int)(sizeof(kLanguageCodes) / sizeof(kLanguageCodes[0]))) {
    return "?";
  }
  return kLanguageCodes[language_id];
}

static void log_line(const char *fmt, ...)
{
  char line[512];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  char output[sizeof(line) + sizeof(LOG_PREFIX) + 4];
  snprintf(output, sizeof(output), LOG_PREFIX " %s\n", line);
  sceKernelDebugOutText(0, output);
}

static void notify(const char *fmt, ...)
{
  NotifyRequest request;
  char message[sizeof(request.message)];
  va_list ap;

  memset(&request, 0, sizeof(request));
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  snprintf(request.message, sizeof(request.message), NOTIFY_PREFIX " %s", message);
  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

int main(void)
{
  int sys_lang = -1;
  int reg_val = -1;
  int reg_init = -1;
  int scan[REGMGR_SCAN_KEYS];
  int scan_rc[REGMGR_SCAN_KEYS];

  int sys_rc = sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &sys_lang);
  int reg_rc = sceRegMgrGetInt(REGMGR_KEY_SYSTEM_LANGUAGE, &reg_val);
  int init_rc = sceRegMgrGetIntInitVal(REGMGR_KEY_SYSTEM_LANGUAGE, &reg_init);

  for (int i = 0; i < REGMGR_SCAN_KEYS; i++) {
    scan[i] = -1;
    scan_rc[i] = sceRegMgrGetInt(REGMGR_KEY_SYSTEM_LANGUAGE + i, &scan[i]);
  }

  log_line("sceSystemServiceParamGetInt(LANG): rc=0x%08x applied=%d (%s)", sys_rc,
           sys_lang, language_code(sys_lang));
  log_line("sceRegMgrGetInt(0x%08x): rc=0x%08x value=%d", REGMGR_KEY_SYSTEM_LANGUAGE,
           reg_rc, reg_val);
  log_line("sceRegMgrGetIntInitVal(0x%08x): rc=0x%08x value=%d",
           REGMGR_KEY_SYSTEM_LANGUAGE, init_rc, reg_init);

  char window[REGMGR_SCAN_KEYS * 12 + 1];
  bool neighbours = false;
  window[0] = '\0';

  for (int i = 0; i < REGMGR_SCAN_KEYS; i++) {
    char item[16];

    log_line("sceRegMgrGetInt(0x%08x): rc=0x%08x value=%d",
             REGMGR_KEY_SYSTEM_LANGUAGE + i, scan_rc[i], scan[i]);

    snprintf(item, sizeof(item), "%s%d", i ? "," : "",
             scan_rc[i] == 0 ? scan[i] : -999);
    strncat(window, item, sizeof(window) - strlen(window) - 1);

    if (i > 0 && scan_rc[i] == 0 && scan[i] != 0) {
      neighbours = true;
    }
  }

  if (sys_rc == 0 && reg_rc == 0) {
    notify("sys=%s(%d) reg=%d init=%d", language_code(sys_lang), sys_lang, reg_val,
           reg_init);
  } else {
    notify("sys rc=0x%08x reg rc=0x%08x", sys_rc, reg_rc);
  }

  if (neighbours) {
    notify("keys+0..+%d: %s", REGMGR_SCAN_KEYS - 1, window);
  }

  return 0;
}
