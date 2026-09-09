/*
 * StreamDeck4SW - ESP32-S3 USB-HID-Keyboard mit editierbarer Konfiguration
 *
 * Composite-USB-Geraet:
 *  - HID-Keyboard-Interface: liest digitale Eingaenge (Taster gegen GND,
 *    interner Pull-Up) ein und sendet die zugeordneten Tastencodes.
 *  - MSC-Interface: stellt eine kleine FAT-Partition als USB-Laufwerk
 *    bereit, auf der die Datei keymap.txt liegt. Darin laesst sich die
 *    GPIO-zu-Taste-Zuordnung am PC bearbeiten. Wird das Laufwerk am PC
 *    ausgeworfen (Eject), liest das Geraet die Datei automatisch neu ein.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "class/hid/hid_device.h"
#include "class/msc/msc.h"
#include "driver/gpio.h"

static const char *TAG = "hid_keyboard";

/************* Tasten-Zuordnung (laufzeitkonfigurierbar) *************/

#define MAX_KEY_ENTRIES        16
#define DEBOUNCE_STABLE_SCANS  3   // Anzahl gleicher Scans bis ein Pegel als stabil gilt
#define SCAN_INTERVAL_MS       10
#define MAX_KEYS_PER_BINDING   3   // max. "normale" Tasten je Zuordnung, zusaetzlich zu Modifiern

typedef struct {
    gpio_num_t gpio;
    uint8_t    modifier;                        // HID-Modifier-Bitmaske (KEYBOARD_MODIFIER_*), 0 = keine
    uint8_t    keycode[MAX_KEYS_PER_BINDING];    // gleichzeitig zu sendende Tasten dieser Zuordnung
    uint8_t    keycode_count;
} key_map_t;

static SemaphoreHandle_t s_key_map_mutex;
static key_map_t s_key_map[MAX_KEY_ENTRIES];
static size_t    s_key_map_count = 0;
static volatile bool s_key_map_changed = true;   // erzwingt Reset der Entprellung nach (Neu-)Laden

// Fallback-Belegung, falls die Konfigurationsdatei (noch) nicht verfuegbar ist
// 9 Tasten (3x3-Layout) fuer StreamDeck4SW
static const key_map_t s_fallback_key_map[] = {
    { GPIO_NUM_1,  0, { HID_KEY_1 }, 1 },
    { GPIO_NUM_2,  0, { HID_KEY_2 }, 1 },
    { GPIO_NUM_4,  0, { HID_KEY_3 }, 1 },
    { GPIO_NUM_5,  0, { HID_KEY_4 }, 1 },
    { GPIO_NUM_6,  0, { HID_KEY_5 }, 1 },
    { GPIO_NUM_7,  0, { HID_KEY_6 }, 1 },
    { GPIO_NUM_8,  0, { HID_KEY_7 }, 1 },
    { GPIO_NUM_9,  0, { HID_KEY_8 }, 1 },
    { GPIO_NUM_10, 0, { HID_KEY_9 }, 1 },
};

static void apply_key_map(const key_map_t *map, size_t count)
{
    uint64_t pin_mask = 0;
    for (size_t i = 0; i < count; i++) {
        pin_mask |= (1ULL << map[i].gpio);
    }
    const gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config fehlgeschlagen: %s", esp_err_to_name(err));
        return;
    }

    xSemaphoreTake(s_key_map_mutex, portMAX_DELAY);
    memcpy(s_key_map, map, count * sizeof(key_map_t));
    s_key_map_count = count;
    s_key_map_changed = true;
    xSemaphoreGive(s_key_map_mutex);

    ESP_LOGI(TAG, "Tastenbelegung aktiv: %u Eintraege", (unsigned) count);
    for (size_t i = 0; i < count; i++) {
        char keys_buf[48] = { 0 };
        size_t pos = 0;
        for (size_t k = 0; k < map[i].keycode_count && pos < sizeof(keys_buf); k++) {
            int n = snprintf(keys_buf + pos, sizeof(keys_buf) - pos, "%s0x%02X",
                              k ? "," : "", map[i].keycode[k]);
            if (n > 0) {
                pos += (size_t) n;
            }
        }
        ESP_LOGI(TAG, "  GPIO%d -> Modifier 0x%02X, Keycodes [%s]", map[i].gpio, map[i].modifier, keys_buf);
    }
}

/************* Konfigurationsdatei auf dem USB-Massenspeicher *********/

#define CFG_BASE_PATH  "/cfg"
#define CFG_FILE_PATH  CFG_BASE_PATH "/keymap.txt"

static const char *s_default_config_text =
    "# ESP32-S3 HID Keyboard (StreamDeck4SW) - Tastenbelegung\r\n"
    "# Eine Zeile pro Taste: GPIO=<Pin-Nummer> KEY=<Taste oder Kombination>\r\n"
    "#\r\n"
    "# Einzelne Taste:      KEY=A            KEY=F5           KEY=ENTER\r\n"
    "# Tastenkombination:   mehrere Namen durch '+' verbunden, OHNE Leerzeichen\r\n"
    "#                      KEY=CTRL+C       KEY=CTRL+ALT+DELETE\r\n"
    "#                      KEY=CTRL+SHIFT+ESC\r\n"
    "#\r\n"
    "# Tastennamen: A-Z, 0-9, ENTER, ESC, BACKSPACE, TAB, SPACE, MINUS,\r\n"
    "# EQUAL, LEFT, RIGHT, UP, DOWN, HOME, END, DELETE, CAPSLOCK, F1..F12\r\n"
    "# Modifier-Namen: CTRL/LCTRL, RCTRL, SHIFT/LSHIFT, RSHIFT, ALT/LALT,\r\n"
    "# ALTGR/RALT, GUI/WIN/SUPER/CMD/LGUI, RGUI\r\n"
    "#\r\n"
    "# Nach dem Bearbeiten: Datei speichern und das Laufwerk am PC\r\n"
    "# auswerfen (Eject/Sicher entfernen) - das Geraet liest die Datei\r\n"
    "# dann automatisch neu ein, ein Reset ist nicht noetig.\r\n"
    "GPIO=1 KEY=1\r\n"
    "GPIO=2 KEY=2\r\n"
    "GPIO=4 KEY=3\r\n"
    "GPIO=5 KEY=4\r\n"
    "GPIO=6 KEY=5\r\n"
    "GPIO=7 KEY=6\r\n"
    "GPIO=8 KEY=7\r\n"
    "GPIO=9 KEY=8\r\n"
    "GPIO=10 KEY=9\r\n";

static bool keycode_from_name(const char *name, uint8_t *out_code)
{
    typedef struct { const char *name; uint8_t code; } named_key_t;
    static const named_key_t named_keys[] = {
        { "ENTER",     HID_KEY_ENTER },
        { "ESC",       HID_KEY_ESCAPE },
        { "ESCAPE",    HID_KEY_ESCAPE },
        { "BACKSPACE", HID_KEY_BACKSPACE },
        { "TAB",       HID_KEY_TAB },
        { "SPACE",     HID_KEY_SPACE },
        { "MINUS",     HID_KEY_MINUS },
        { "EQUAL",     HID_KEY_EQUAL },
        { "LEFT",      HID_KEY_ARROW_LEFT },
        { "RIGHT",     HID_KEY_ARROW_RIGHT },
        { "UP",        HID_KEY_ARROW_UP },
        { "DOWN",      HID_KEY_ARROW_DOWN },
        { "HOME",      HID_KEY_HOME },
        { "END",       HID_KEY_END },
        { "DELETE",    HID_KEY_DELETE },
        { "CAPSLOCK",  HID_KEY_CAPS_LOCK },
    };

    size_t len = strlen(name);

    if (len == 1) {
        char c = (char) toupper((unsigned char) name[0]);
        if (c >= 'A' && c <= 'Z') {
            *out_code = HID_KEY_A + (uint8_t)(c - 'A');
            return true;
        }
        if (c >= '1' && c <= '9') {
            *out_code = HID_KEY_1 + (uint8_t)(c - '1');
            return true;
        }
        if (c == '0') {
            *out_code = HID_KEY_0;
            return true;
        }
    }

    if (len >= 2 && len <= 3 && (name[0] == 'F' || name[0] == 'f')) {
        int num = atoi(name + 1);
        if (num >= 1 && num <= 12) {
            *out_code = HID_KEY_F1 + (uint8_t)(num - 1);
            return true;
        }
    }

    for (size_t i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); i++) {
        if (strcasecmp(name, named_keys[i].name) == 0) {
            *out_code = named_keys[i].code;
            return true;
        }
    }

    return false;
}

static bool modifier_from_name(const char *name, uint8_t *out_modifier)
{
    typedef struct { const char *name; uint8_t modifier; } named_modifier_t;
    static const named_modifier_t named_modifiers[] = {
        { "CTRL",    KEYBOARD_MODIFIER_LEFTCTRL },
        { "LCTRL",   KEYBOARD_MODIFIER_LEFTCTRL },
        { "RCTRL",   KEYBOARD_MODIFIER_RIGHTCTRL },
        { "SHIFT",   KEYBOARD_MODIFIER_LEFTSHIFT },
        { "LSHIFT",  KEYBOARD_MODIFIER_LEFTSHIFT },
        { "RSHIFT",  KEYBOARD_MODIFIER_RIGHTSHIFT },
        { "ALT",     KEYBOARD_MODIFIER_LEFTALT },
        { "LALT",    KEYBOARD_MODIFIER_LEFTALT },
        { "ALTGR",   KEYBOARD_MODIFIER_RIGHTALT },
        { "RALT",    KEYBOARD_MODIFIER_RIGHTALT },
        { "GUI",     KEYBOARD_MODIFIER_LEFTGUI },
        { "WIN",     KEYBOARD_MODIFIER_LEFTGUI },
        { "SUPER",   KEYBOARD_MODIFIER_LEFTGUI },
        { "CMD",     KEYBOARD_MODIFIER_LEFTGUI },
        { "LGUI",    KEYBOARD_MODIFIER_LEFTGUI },
        { "RGUI",    KEYBOARD_MODIFIER_RIGHTGUI },
    };

    for (size_t i = 0; i < sizeof(named_modifiers) / sizeof(named_modifiers[0]); i++) {
        if (strcasecmp(name, named_modifiers[i].name) == 0) {
            *out_modifier = named_modifiers[i].modifier;
            return true;
        }
    }
    return false;
}

/* Zerlegt einen Ausdruck wie "CTRL+ALT+DELETE" (ohne Leerzeichen) in eine
 * Modifier-Bitmaske und bis zu MAX_KEYS_PER_BINDING "normale" Tastencodes. */
static bool parse_key_combo(const char *combo, uint8_t *out_modifier,
                             uint8_t out_keycodes[MAX_KEYS_PER_BINDING], uint8_t *out_keycode_count)
{
    *out_modifier = 0;
    *out_keycode_count = 0;

    char buf[32];
    strncpy(buf, combo, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    bool any_token = false;
    char *saveptr = NULL;
    char *token = strtok_r(buf, "+", &saveptr);
    while (token != NULL) {
        any_token = true;
        uint8_t modifier;
        uint8_t code;
        if (modifier_from_name(token, &modifier)) {
            *out_modifier |= modifier;
        } else if (keycode_from_name(token, &code)) {
            if (*out_keycode_count < MAX_KEYS_PER_BINDING) {
                out_keycodes[(*out_keycode_count)++] = code;
            } else {
                ESP_LOGW(TAG, "Zu viele Tasten in Kombination '%s' (max %d), '%s' ignoriert",
                         combo, MAX_KEYS_PER_BINDING, token);
            }
        } else {
            ESP_LOGW(TAG, "Unbekannter Tastenname in Kombination '%s': '%s'", combo, token);
            return false;
        }
        token = strtok_r(NULL, "+", &saveptr);
    }

    return any_token && (*out_modifier != 0 || *out_keycode_count != 0);
}

static void write_default_config(void)
{
    ESP_LOGI(TAG, "Erzeuge Standard-Konfigurationsdatei %s", CFG_FILE_PATH);
    FILE *f = fopen(CFG_FILE_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Konnte %s nicht anlegen", CFG_FILE_PATH);
        return;
    }
    fputs(s_default_config_text, f);
    fclose(f);
}

/* Liest CFG_FILE_PATH ein und aktiviert die neue Tastenbelegung.
 * Darf nur aufgerufen werden, waehrend der Speicher der Anwendung gehoert
 * (TINYUSB_MSC_STORAGE_MOUNT_APP). */
static void load_key_map_from_file(void)
{
    struct stat st;
    if (stat(CFG_FILE_PATH, &st) != 0) {
        write_default_config();
    }

    FILE *f = fopen(CFG_FILE_PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Konnte %s nicht lesen, behalte aktuelle Belegung", CFG_FILE_PATH);
        return;
    }

    key_map_t new_map[MAX_KEY_ENTRIES];
    size_t new_count = 0;
    char line[128];

    while (fgets(line, sizeof(line), f) != NULL && new_count < MAX_KEY_ENTRIES) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') {
            continue;
        }

        char *gpio_str = strstr(p, "GPIO=");
        char *key_str  = strstr(p, "KEY=");
        if (!gpio_str || !key_str) {
            continue;
        }

        int gpio_num = atoi(gpio_str + 5);

        char key_name[32] = { 0 };
        key_str += 4;
        size_t n = 0;
        while (*key_str && !isspace((unsigned char) *key_str) && n < sizeof(key_name) - 1) {
            key_name[n++] = *key_str++;
        }
        key_name[n] = '\0';

        uint8_t modifier = 0;
        uint8_t keycodes[MAX_KEYS_PER_BINDING] = { 0 };
        uint8_t keycode_count = 0;
        if (n == 0 || !parse_key_combo(key_name, &modifier, keycodes, &keycode_count)) {
            ESP_LOGW(TAG, "Ungueltige Taste/Kombination in Konfiguration: '%s'", key_name);
            continue;
        }
        if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX || !GPIO_IS_VALID_GPIO(gpio_num)) {
            ESP_LOGW(TAG, "Ungueltiger GPIO in Konfiguration: %d", gpio_num);
            continue;
        }

        new_map[new_count].gpio          = (gpio_num_t) gpio_num;
        new_map[new_count].modifier      = modifier;
        new_map[new_count].keycode_count = keycode_count;
        memcpy(new_map[new_count].keycode, keycodes, sizeof(keycodes));
        new_count++;
    }
    fclose(f);

    if (new_count == 0) {
        ESP_LOGW(TAG, "Keine gueltigen Eintraege in %s gefunden, behalte aktuelle Belegung", CFG_FILE_PATH);
        return;
    }

    apply_key_map(new_map, new_count);
}

/************* USB-Massenspeicher (FAT-Konfigurationsablage) *********/

static tinyusb_msc_storage_handle_t s_storage_hdl = NULL;

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    const esp_partition_t *data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "FAT-Partition 'storage' nicht gefunden - partitions.csv pruefen");
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(data_partition, wl_handle);
}

/* Wird aufgerufen, wenn der Speicher zwischen Host (USB) und Anwendung
 * wechselt. Wirft der Nutzer das Laufwerk am PC aus, bekommt die
 * Anwendung den Speicher automatisch zurueck - Gelegenheit, die
 * evtl. geaenderte Konfigurationsdatei neu einzulesen. */
static void storage_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void) arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE && event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ESP_LOGI(TAG, "Laufwerk vom Host ausgeworfen - lese Konfiguration neu ein");
        load_key_map_from_file();
        // Laufwerk dem Host sofort wieder zur Verfuegung stellen
        tinyusb_msc_set_storage_mount_point(handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    }
}

static void storage_init(void)
{
    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t err = storage_init_spiflash(&wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB-Massenspeicher nicht verfuegbar, verwende Fallback-Tastenbelegung");
        return;
    }

    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point       = TINYUSB_MSC_STORAGE_MOUNT_APP,  // zunaechst der Anwendung, um die Datei zu lesen/anzulegen
        .medium.wl_handle  = wl_handle,
        .fat_fs = {
            .base_path     = CFG_BASE_PATH,
            .config = {
                .format_if_mount_failed = true,
                .max_files              = 2,
                .allocation_unit_size   = 0,
            },
            .do_not_format = false,
            .format_flags  = 0,
        },
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_storage_hdl));
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_callback(storage_event_cb, NULL));

    load_key_map_from_file();

    // Speicher dem Host als USB-Laufwerk anbieten
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_mount_point(s_storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB));
}

/************* TinyUSB-Deskriptoren (Composite: HID + MSC) ***********/

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

#define EPNUM_HID_IN   0x81
#define EPNUM_MSC_OUT  0x02
#define EPNUM_MSC_IN   0x82

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)

// Reines Keyboard-HID-Reportdeskriptor (kein Report-ID-Byte noetig, da nur ein Report)
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

static const char *string_descriptor[] = {
    (char[]){ 0x09, 0x04 },     // 0: Sprache = Englisch (0x0409)
    "Espressif",                // 1: Hersteller
    "StreamDeck4SW",            // 2: Produkt
    "000001",                   // 3: Seriennummer
    "HID Keyboard Interface",   // 4: HID
    "Konfigurationsspeicher",   // 5: MSC
};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, false, sizeof(hid_report_descriptor), EPNUM_HID_IN, 16, 10),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

/********* TinyUSB HID-Callbacks ***************/

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    (void) instance; (void) report_id; (void) report_type; (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance; (void) report_id; (void) report_type; (void) buffer; (void) bufsize;
}

/* MSC-Callbacks werden von der esp_tinyusb-Komponente (tinyusb_msc.c)
 * bereitgestellt, sobald tinyusb_msc_new_storage_spiflash() aufgerufen
 * wurde - hier ist keine eigene Implementierung noetig. */

static void usb_init(void)
{
    ESP_LOGI(TAG, "USB-Initialisierung");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    tusb_cfg.descriptor.device            = NULL;
    tusb_cfg.descriptor.full_speed_config = configuration_descriptor;
    tusb_cfg.descriptor.string            = string_descriptor;
    tusb_cfg.descriptor.string_count      = sizeof(string_descriptor) / sizeof(string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = configuration_descriptor;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB-Initialisierung abgeschlossen");
}

/************* Anwendung: GPIO-Abfrage + HID-Report *******************/

void app_main(void)
{
    s_key_map_mutex = xSemaphoreCreateMutex();
    assert(s_key_map_mutex != NULL);

    // Sofort einsatzbereite Fallback-Belegung, bevor die Konfigurationsdatei
    // eingelesen werden konnte
    apply_key_map(s_fallback_key_map, sizeof(s_fallback_key_map) / sizeof(s_fallback_key_map[0]));

    storage_init();   // erstellt/liest keymap.txt und gibt das Laufwerk anschliessend an den Host frei
    usb_init();

    uint8_t  debounce_count[MAX_KEY_ENTRIES]  = { 0 };
    bool     stable_pressed[MAX_KEY_ENTRIES]  = { false };
    bool     last_raw_pressed[MAX_KEY_ENTRIES] = { false };

    uint8_t last_report_modifier = 0;
    uint8_t last_report_keycode[6] = { 0 };
    bool report_pending = false;

    while (1) {
        xSemaphoreTake(s_key_map_mutex, portMAX_DELAY);
        key_map_t local_map[MAX_KEY_ENTRIES];
        size_t local_count = s_key_map_count;
        memcpy(local_map, s_key_map, local_count * sizeof(key_map_t));
        bool map_changed = s_key_map_changed;
        s_key_map_changed = false;
        xSemaphoreGive(s_key_map_mutex);

        if (map_changed) {
            memset(debounce_count, 0, sizeof(debounce_count));
            memset(stable_pressed, 0, sizeof(stable_pressed));
            memset(last_raw_pressed, 0, sizeof(last_raw_pressed));
        }

        bool changed = false;

        // Entprellung je Taste (aktiv-low)
        for (size_t i = 0; i < local_count; i++) {
            bool raw_pressed = !gpio_get_level(local_map[i].gpio);

            if (raw_pressed == last_raw_pressed[i]) {
                if (debounce_count[i] < DEBOUNCE_STABLE_SCANS) {
                    debounce_count[i]++;
                }
            } else {
                debounce_count[i] = 0;
                last_raw_pressed[i] = raw_pressed;
            }

            if (debounce_count[i] >= DEBOUNCE_STABLE_SCANS && stable_pressed[i] != raw_pressed) {
                stable_pressed[i] = raw_pressed;
                changed = true;
            }
        }

        if (changed) {
            uint8_t modifier = 0;
            uint8_t keycode[6] = { 0 };
            size_t n = 0;
            for (size_t i = 0; i < local_count && n < 6; i++) {
                if (stable_pressed[i]) {
                    modifier |= local_map[i].modifier;
                    for (size_t k = 0; k < local_map[i].keycode_count && n < 6; k++) {
                        keycode[n++] = local_map[i].keycode[k];
                    }
                }
            }
            last_report_modifier = modifier;
            memcpy(last_report_keycode, keycode, sizeof(keycode));
            report_pending = true;
        }

        if (report_pending && tud_mounted() && tud_hid_ready()) {
            tud_hid_keyboard_report(0, last_report_modifier, last_report_keycode);
            report_pending = false;
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}
