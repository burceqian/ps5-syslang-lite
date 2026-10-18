// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5-syslang-lite -- switch the PS5 system language by writing one registry
// value, and nothing else.
//
//     sceRegMgrSetInt(0x02020000, language_id)
//
// Measured on a J1 (JP region-locked) console, and the reason this payload
// exists in this shape:
//
//   * the bare registry write *does* switch the system language,
//   * it never freezes SceShellUI,
//   * it never triggers the whole-database rebuild,
//   * SceShellUI is never injected into, never reloaded, never restarted.
//
// What it does not do: it does not repaint the home screen that is *already*
// drawn. Text rendered after the switch -- newly opened apps, a settings page
// that is entered again, the notification area -- is already in the new
// language; only the already-rendered home screen keeps the old strings.
// Loading OnionHEN once repaints it (it reloads the home screen's React Native
// scene, "NPXS40002", from the UI thread).
//
// The alternatives were measured and rejected (see 测试记录.md):
//   * calling the managed ChangeSystemLanguage / ReloadApplicationForChange
//     SystemLanguage inside SceShellUI (the upstream approach, `-j1` builds):
//     freezes the console and triggers the database rebuild.
//   * the system's own "pshomeui:navigateToHome" navigation (`setlang` builds):
//     safe, but a no-op when the home screen is already in front, so the UI
//     stays in the old language.
//   * reloading the home screen's RN scene via a hook into SceShellUI
//     (`reload` builds): it left the console's UI broken -- pages stopped
//     showing the new language at all.
//
// Everything here is idempotent and read-only apart from the single registry
// value, so it can be run again at any time.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int sceSystemServiceParamGetInt(int param_id, int *value);
extern int sceKernelSendNotificationRequest(int device, void *request, size_t size,
                                            int blocking);
extern int sceKernelDebugOutText(int channel, const char *text);
extern int sceRegMgrSetInt(int regid, int value);
extern int sceRegMgrGetInt(int regid, int *value);

#ifndef LANGUAGE_ID
#define LANGUAGE_ID 11
#endif

#ifndef SYS_LANG_VERSION
#define SYS_LANG_VERSION "unknown"
#endif

#if LANGUAGE_ID < 0 || LANGUAGE_ID > 30
#error "LANGUAGE_ID must be in the 0-30 range"
#endif

#define SCE_SYSTEM_SERVICE_PARAM_ID_LANG 1
#define REGMGR_KEY_SYSTEM_LANGUAGE 0x02020000

#define LOG_PREFIX "[ps5-syslang-lite]"
#define NOTIFY_PREFIX "PS5 SysLang:"

// Seconds to wait before the final "state after the write" notification. The
// system service reads the registry on a timer (OnionHEN's daemon polls every
// 5 s), so a shorter wait can still report the previous language.
#ifndef SETTLE_SECONDS
#define SETTLE_SECONDS 6
#endif

typedef struct NotifyRequest {
  char reserved[45];
  char message[3075];
} NotifyRequest;

typedef struct LanguageText {
  const char *name;
  const char *already_fmt;
} LanguageText;

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static const LanguageText kLanguageTexts[] = {
    {"日本語", "すでに%sです"},
    {"English (United States)", "Already %s"},
    {"Français", "Déjà en %s"},
    {"Español", "Ya está en %s"},
    {"Deutsch", "Bereits %s"},
    {"Italiano", "Già %s"},
    {"Nederlands", "Al %s"},
    {"Português (Portugal)", "Já está em %s"},
    {"Русский", "Уже %s"},
    {"한국어", "이미 %s입니다"},
    {"繁體中文", "已經是%s"},
    {"简体中文", "已经是%s"},
    {"Suomi", "Jo %s"},
    {"Svenska", "Redan %s"},
    {"Dansk", "Allerede %s"},
    {"Norsk", "Allerede %s"},
    {"Polski", "Już %s"},
    {"Português (Brasil)", "Já está em %s"},
    {"English (United Kingdom)", "Already %s"},
    {"Türkçe", "Zaten %s"},
    {"Español (Latinoamérica)", "Ya está en %s"},
    {"العربية", "بالفعل %s"},
    {"Français (Canada)", "Déjà en %s"},
    {"Čeština", "Již %s"},
    {"Magyar", "Már %s"},
    {"Ελληνικά", "Ήδη %s"},
    {"Română", "Deja %s"},
    {"ไทย", "เป็น%sอยู่แล้ว"},
    {"Tiếng Việt", "Đã là %s"},
    {"Bahasa Indonesia", "Sudah %s"},
    {"Українська", "Вже %s"},
};

static int is_valid_language_id(int language_id)
{
  return language_id >= 0 && language_id < (int)ARRAY_SIZE(kLanguageTexts);
}

static const char *language_name_from_id(int language_id)
{
  if (!is_valid_language_id(language_id)) {
    return "Unknown";
  }

  return kLanguageTexts[language_id].name;
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
  static const char kPrefix[] = NOTIFY_PREFIX " ";
  NotifyRequest request;
  va_list ap;

  memset(&request, 0, sizeof(request));
  memcpy(request.message, kPrefix, sizeof(kPrefix));

  va_start(ap, fmt);
  vsnprintf(request.message + sizeof(kPrefix) - 1,
            sizeof(request.message) - (sizeof(kPrefix) - 1), fmt, ap);
  va_end(ap);

  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

// "Already in this language", in that language (the same phrasing the console
// itself uses for its own language switch).
static void notify_already_language(int language_id)
{
  if (!is_valid_language_id(language_id)) {
    notify("invalid language_id %d", language_id);
    return;
  }

  notify(kLanguageTexts[language_id].already_fmt,
         language_name_from_id(language_id));
}

int main(void)
{
  const int language_id = LANGUAGE_ID;
  int sys_lang = -1;
  int reg_value = -1;

  const int sys_rc =
      sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &sys_lang);
  const int get_rc = sceRegMgrGetInt(REGMGR_KEY_SYSTEM_LANGUAGE, &reg_value);

  log_line("%s: target=%s(%d) sys_rc=0x%08x sys=%d reg_rc=0x%08x reg=%d",
           SYS_LANG_VERSION, language_name_from_id(language_id), language_id,
           (unsigned int)sys_rc, sys_lang, (unsigned int)get_rc, reg_value);

  // Nothing to do when the registry already holds the target language: say so
  // and leave both the value and the system alone. Reported in the target
  // language, the way the console words its own "already in this language".
  if (get_rc == 0 && reg_value == language_id) {
    log_line("already %d (reg=%d sys=%d); nothing written", language_id,
             reg_value, sys_lang);
    notify_already_language(language_id);
    notify("未做任何修改｜重进设置页或加载一次 OnionHEN 可刷新主界面");
    return 0;
  }

  int set_rc = sceRegMgrSetInt(REGMGR_KEY_SYSTEM_LANGUAGE, language_id);
  int reg_after = -1;
  int read_rc = sceRegMgrGetInt(REGMGR_KEY_SYSTEM_LANGUAGE, &reg_after);

  // One retry (write again, then read back): a write that does not stick means
  // the registry was not reachable in this process, which has never been
  // observed -- but the payload reports that instead of pretending to have
  // switched anything.
  if (read_rc != 0 || reg_after != language_id) {
    log_line("write unconfirmed (set=0x%08x read=0x%08x value=%d); retrying once",
             (unsigned int)set_rc, (unsigned int)read_rc, reg_after);
    usleep(200000);
    set_rc = sceRegMgrSetInt(REGMGR_KEY_SYSTEM_LANGUAGE, language_id);
    read_rc = sceRegMgrGetInt(REGMGR_KEY_SYSTEM_LANGUAGE, &reg_after);
  }

  if (read_rc == 0 && reg_after == language_id) {
    log_line("switched to %d (set=0x%08x reg=%d)", language_id,
             (unsigned int)set_rc, reg_after);
    notify("已切换为%s (%d)", language_name_from_id(language_id), language_id);
    notify("重进设置页或加载一次 OnionHEN 可刷新主界面");
  } else {
    log_line("FAILED: set=0x%08x read=0x%08x value=%d target=%d",
             (unsigned int)set_rc, (unsigned int)read_rc, reg_after, language_id);
    notify("写入注册表失败 set=0x%08x reg=%d (target=%d)", (unsigned int)set_rc,
           read_rc == 0 ? reg_after : -1, language_id);
    notify("没有做任何切换；请重试，并查看 klog 里的 %s", LOG_PREFIX);
    return 1;
  }

  // Give the system service time to pick the new value up, then report what it
  // is actually using -- this is the "settled" state, not the write itself.
  for (int i = 0; i < SETTLE_SECONDS * 10; i++) {
    usleep(100000);
  }

  int sys_after = -1;
  int reg_final = -1;
  const int final_rc = sceRegMgrGetInt(REGMGR_KEY_SYSTEM_LANGUAGE, &reg_final);
  sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &sys_after);

  log_line("settled after %ds: reg_rc=0x%08x reg=%d sys=%d", SETTLE_SECONDS,
           (unsigned int)final_rc, reg_final, sys_after);

  notify("写入完成 reg=%d sys=%s(%d)", final_rc == 0 ? reg_final : -1,
         language_name_from_id(sys_after), sys_after);

  return 0;
}
