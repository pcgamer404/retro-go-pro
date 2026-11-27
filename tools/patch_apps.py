path = 'D:/Games/E/retro-go-pro/cores/launcher/main/applications.c'
with open(path, 'r', encoding='utf-8') as f:
    text = f.read()

if '#include <freertos/task.h>' not in text:
    old_inc = '#include <errno.h>'
    new_inc = '#include <errno.h>\n#ifdef ESP_PLATFORM\n#include <freertos/FreeRTOS.h>\n#include <freertos/task.h>\n#endif'
    text = text.replace(old_inc, new_inc, 1)

safe_fn = (
    "static void safe_switch_app(const char *partition, const char *name, const char *args, int save_slot, uint32_t flags)\n"
    "{\n"
    "#ifdef ESP_PLATFORM\n"
    "    TaskHandle_t sysmon = xTaskGetHandle(\"rg_sysmon\");\n"
    "    if (sysmon) {\n"
    "        vTaskDelete(sysmon);\n"
    "    }\n"
    "#endif\n"
    "    rg_system_switch_app(partition, name, args, save_slot, flags);\n"
    "}\n\n"
)

if 'safe_switch_app' not in text:
    text = text.replace('static void application_start(', safe_fn + 'static void application_start(', 1)
    text = text.replace('rg_system_switch_app(part, name, path, load_state, flags);', 'safe_switch_app(part, name, path, load_state, flags);', 1)
    text = text.replace('rg_system_switch_app(part, name, args, -1, flags);', 'safe_switch_app(part, name, args, -1, flags);', 1)

with open(path, 'w', encoding='utf-8') as f:
    f.write(text)

print('Patch applied cleanly!')
