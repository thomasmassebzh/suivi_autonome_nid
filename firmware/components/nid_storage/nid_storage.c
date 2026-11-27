/*
 * nid_storage — support de stockage (carte microSD ou clé USB), nommage des
 * fichiers, journal
 *
 * Supports (menuconfig « Suivi nid — Stockage ») :
 *  - carte microSD : SDMMC emplacement 0, domaine d'E/S alimenté par le LDO
 *    interne, carte alimentée par un interrupteur sur la Waveshare ;
 *  - clé ou disque USB : hôte USB 2.0 haut débit de l'ESP32-P4, pilote
 *    usb_host_msc (motif de l'exemple msc d'ESP-IDF), 5 V USB commandé par un
 *    interrupteur facultatif et coupé au démontage. Le câble USB peut atteindre
 *    5 m (USB 2.0) : clé et batterie au pied de l'arbre, relève sans grimper ;
 *  - automatique : clé USB si elle répond, sinon carte microSD.
 * Les deux supports sont montés au même endroit (/sdcard) : le reste du
 * firmware ne voit pas la différence. FAT sans jamais formater. Les médias sont écrits sous « .tmp » puis renommés ; au
 * démarrage, les « .tmp » restants deviennent « _INCOMPLET ». Le journal CSV
 * est ouvert, écrit, fsync puis fermé à chaque ligne : une coupure ne peut
 * perdre que la ligne en cours.
 *
 * Verrous (ordre : verrou fichiers, puis état, jamais l'inverse) :
 *  - s_fs_lock  : mutex qui sérialise montage, démontage et écritures du
 *                 journal (appelé par plusieurs tâches) ;
 *  - s_state_mux : section critique de quelques microsecondes (identifiants,
 *                 batterie, température), jamais tenue pendant un accès carte.
 *
 * Limite FAT connue : f_rename crée la nouvelle entrée de répertoire avant
 * d'effacer l'ancienne. Une coupure dans cette fenêtre de quelques ms peut
 * laisser « X.mp4 » et « X.mp4.tmp » sur les mêmes clusters : nid_storage_recover()
 * ne touche alors à aucun des deux (supprimer l'un libérerait les clusters de l'autre).
 */
#include "nid_storage.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nid_board.h"
#include "nid_storage_calc.h"

#if CONFIG_NID_STORAGE_USB || CONFIG_NID_STORAGE_AUTO
#define NID_STORAGE_HAS_USB 1
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#else
#define NID_STORAGE_HAS_USB 0
#endif

static const char *TAG = "nid_storage";

#define ID_MAX_LEN          15      /* longueur maximale de <SYS> et de <NID> */
#define DEFAULT_SYSTEM_ID   "CAM01"
#define DEFAULT_NEST_ID     "NID01"
#define BASE_DIR_MAX        48      /* "/sdcard/" + 15 + "_" + 15 + fin de chaîne = 40 */
#define MAX_FILES_OPEN      5
#define MAX_SAME_NAME       99      /* suffixes _1 à _99 si le nom existe déjà */
#define TMP_SUFFIX          ".tmp"
#define INCOMPLETE_TAG      "_INCOMPLET"
#define JOURNAL_FILE        "journal.csv"
#define JOURNAL_HEADER      "date_heure;evenement;details;batterie_mV;temperature_C\n"
#define LINE_TAIL_RESERVE   40      /* guillemet fermant, batterie, température, fin de ligne */
#define TIME_VALID_MIN_UTC  1735689600LL   /* 2025-01-01 : en deçà, l'heure est perdue */
#define SD_POWER_SETTLE_MS  20      /* montée de l'alimentation de la carte (interrupteur Waveshare) */
#define USB_VBUS_SETTLE_MS  100     /* montée du 5 V USB avant l'énumération */
#define USB_TASK_STACK      4096
#define USB_TASK_PRIO       5
#define USB_STOP_TIMEOUT_MS 2000

static StaticSemaphore_t s_fs_lock_buf;
static SemaphoreHandle_t s_fs_lock;                 /* créé au premier usage (voir fs_lock()) */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr_handle;
static volatile bool s_mounted;
static nid_storage_medium_t s_medium = NID_STORAGE_MEDIUM_NONE;

/* Protégés par s_state_mux. */
static char s_system_id[ID_MAX_LEN + 1];
static char s_nest_id[ID_MAX_LEN + 1];
static char s_base_dir[BASE_DIR_MAX];               /* vide tant que nid_storage_set_ids() n'a pas réussi */
static int32_t s_batt_mv = -1;
static float s_temp_c = NAN;

/* ------------------------------------------------------------------------- */
/* Utilitaires                                                                */
/* ------------------------------------------------------------------------- */

static SemaphoreHandle_t fs_lock(void)
{
    portENTER_CRITICAL(&s_state_mux);
    if (s_fs_lock == NULL) {
        s_fs_lock = xSemaphoreCreateMutexStatic(&s_fs_lock_buf);
    }
    portEXIT_CRITICAL(&s_state_mux);
    return s_fs_lock;
}

/** Copie le dossier de base et les identifiants ; false si les identifiants ne sont pas posés. */
static bool snapshot_ids(char *base, size_t base_size, char *sys, char *nid)
{
    bool known;
    portENTER_CRITICAL(&s_state_mux);
    known = (s_base_dir[0] != '\0');
    strlcpy(base, s_base_dir, base_size);
    if (sys != NULL) {
        strlcpy(sys, s_system_id, ID_MAX_LEN + 1);
    }
    if (nid != NULL) {
        strlcpy(nid, s_nest_id, ID_MAX_LEN + 1);
    }
    portEXIT_CRITICAL(&s_state_mux);
    return known;
}

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/** Crée le dossier s'il n'existe pas (un seul niveau). */
static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }
    const int err = errno;
    struct stat st;
    if (err == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "création du dossier %s impossible (errno %d)", path, err);
    return ESP_FAIL;
}

static bool has_tmp_suffix(const char *name)
{
    const size_t len = strlen(name);
    const size_t suffix_len = sizeof(TMP_SUFFIX) - 1;
    return len > suffix_len && strcasecmp(name + len - suffix_len, TMP_SUFFIX) == 0;
}

/** Assainit un identifiant comme nid_config_sanitize_id() : [A-Za-z0-9_-], 15 caractères au plus. */
static void sanitize_id(char *out, size_t out_size, const char *in, const char *fallback)
{
    size_t n = 0;
    if (in != NULL) {
        while (*in == ' ' || *in == '\t') {
            in++;
        }
        size_t end = strlen(in);
        while (end > 0 && (in[end - 1] == ' ' || in[end - 1] == '\t')) {
            end--;
        }
        size_t i = 0;
        while (i < end && n + 1 < out_size) {
            const unsigned char c = (unsigned char)in[i++];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                out[n++] = (char)c;
            } else {
                out[n++] = '_';
                if (c >= 0xC0) {
                    /* Caractère UTF-8 multi-octet (lettre accentuée...) : un seul « _ ». */
                    while (i < end && ((unsigned char)in[i] & 0xC0) == 0x80) {
                        i++;
                    }
                }
            }
        }
    }
    out[n] = '\0';
    if (n == 0) {
        strlcpy(out, fallback, out_size);
    }
}

static void log_volume(void);

/* ------------------------------------------------------------------------- */
/* Carte microSD                                                              */
/* ------------------------------------------------------------------------- */

static void release_power(void)
{
    nid_board_sd_power(false);
    if (s_pwr_handle != NULL) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_handle);
        s_pwr_handle = NULL;
    }
}

/**
 * Diagnostic d'une carte dont le volume ne convient pas (verrou fichiers pris) :
 * lit le secteur 0 et décrit la table de partitions. Cas typique : carte
 * d'un Raspberry Pi (petite partition FAT « boot » + partition Linux), ou
 * carte de 64 Go et plus livrée en exFAT (non pris en charge par FatFs ici).
 */
static void log_partitions(sdmmc_card_t *card)
{
    if (card == NULL) {
        return;
    }
    uint8_t *s0 = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s0 == NULL) {
        return;
    }
    if (sdmmc_read_sectors(card, s0, 0, 1) == ESP_OK) {
        nid_mbr_part_t p[4];
        const int n = nid_mbr_parse(s0, p);
        if (n < 0) {
            ESP_LOGW(TAG, "carte sans table de partitions (volume unique)");
        } else {
            ESP_LOGW(TAG, "table de partitions de la carte : %d partition(s)", n);
            bool fat = false;
            for (int i = 0; i < n; i++) {
                ESP_LOGW(TAG, "  partition %d : %s (type 0x%02X), %" PRIu64 " Mo", i + 1, nid_mbr_type_name(p[i].type),
                         p[i].type, (uint64_t)p[i].sectors * 512u / 1000000u);
                fat = fat || nid_mbr_is_fat(p[i].type);
                if (p[i].type == 0x07) {
                    ESP_LOGW(TAG, "  → exFAT n'est pas lu par le firmware : reformater la carte en FAT32");
                }
            }
            if (n > 1) {
                ESP_LOGW(TAG, "  → seule la première partition FAT est utilisée : reformater la carte "
                         "en une seule partition FAT32 (SD Card Formatter)");
            } else if (n == 1 && !fat) {
                ESP_LOGW(TAG, "  → pas de partition FAT : reformater la carte en FAT32");
            }
        }
    }
    heap_caps_free(s0);
}

/** Monte la carte (verrou fichiers pris, aucun support monté). Inutilisée en mode « clé USB » seul. */
__attribute__((unused)) static esp_err_t mount_card(void)
{
    const nid_board_pins_t *pins = nid_board_pins();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = CONFIG_NID_SD_FREQ_KHZ;

    if (CONFIG_NID_SD_LDO_CHAN >= 0) {
        /* Le LDO interne alimente le domaine d'E/S de la carte (VO4). */
        const sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = CONFIG_NID_SD_LDO_CHAN };
        const esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LDO %d de la carte SD indisponible : %s", CONFIG_NID_SD_LDO_CHAN, esp_err_to_name(err));
            s_pwr_handle = NULL;
            return err;
        }
        host.pwr_ctrl_handle = s_pwr_handle;
    }
    /* Interrupteur d'alimentation de la carte (Waveshare, GPIO45) : après le LDO,
     * la broche étant dans le domaine d'E/S qu'il alimente. */
    if (nid_board_sd_power(true)) {
        vTaskDelay(pdMS_TO_TICKS(SD_POWER_SETTLE_MS));
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = CONFIG_NID_SD_BUS_WIDTH;
    slot.clk = pins->sd_clk;
    slot.cmd = pins->sd_cmd;
    slot.d0 = pins->sd_d0;
    slot.d1 = pins->sd_d1;
    slot.d2 = pins->sd_d2;
    slot.d3 = pins->sd_d3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,    /* jamais de formatage : la carte de l'utilisateur reste intacte */
        .max_files = MAX_FILES_OPEN,
    };
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(NID_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "système de fichiers illisible : la carte doit être formatée en FAT32 "
                     "(les cartes de 64 Go et plus sont livrées en exFAT, non pris en charge)");
        } else {
            ESP_LOGE(TAG, "carte SD absente ou illisible : %s", esp_err_to_name(err));
        }
        s_card = NULL;
        release_power();
        return err;
    }
    s_medium = NID_STORAGE_MEDIUM_SD;
    s_mounted = true;
    ESP_LOGI(TAG, "carte SD montée : %llu Mo, bus %d bit(s), %d kHz",
             (unsigned long long)((uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size / (1024 * 1024)),
             slot.width, s_card->real_freq_khz);
    return ESP_OK;
}

static esp_err_t unmount_card(void)
{
    esp_err_t err = ESP_OK;
    if (s_card != NULL) {
        err = esp_vfs_fat_sdcard_unmount(NID_SD_MOUNT_POINT, s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "démontage de la carte SD : %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "carte SD démontée");
        }
        s_card = NULL;
    }
    release_power();    /* coupe la carte puis le LDO : le domaine d'E/S n'est plus alimenté */
    return err;
}

/* ------------------------------------------------------------------------- */
/* Clé ou disque USB (hôte USB 2.0 haut débit de l'ESP32-P4, classe MSC)      */
/* ------------------------------------------------------------------------- */

#if NID_STORAGE_HAS_USB

typedef struct {
    bool                     connected;     /*!< true : branchement, false : retrait */
    uint8_t                  address;
    msc_host_device_handle_t handle;
} usb_evt_t;

static QueueHandle_t            s_usb_q;
static SemaphoreHandle_t        s_usb_lib_done;
static volatile bool            s_usb_no_clients;
static volatile bool            s_usb_quit;
static bool                     s_usb_msc_installed;
static msc_host_device_handle_t s_msc_dev;
static msc_host_vfs_handle_t    s_msc_vfs;
static uint64_t                 s_usb_capacity;
static volatile bool            s_usb_lost;

/** Rappel du pilote MSC (tâche du pilote) : transmis à la tâche qui monte le support. */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    usb_evt_t ev = { 0 };
    if (event->event == MSC_DEVICE_CONNECTED) {
        ev.connected = true;
        ev.address = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        ev.connected = false;
        ev.handle = event->device.handle;
        if (ev.handle == s_msc_dev && s_msc_dev != NULL) {
            s_usb_lost = true;
            ESP_LOGE(TAG, "support USB retiré pendant l'utilisation");
        }
    } else {
        return;
    }
    xQueueSend(s_usb_q, &ev, 0);
}

/** Tâche de la bibliothèque hôte USB (motif de l'exemple msc d'ESP-IDF). */
static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            s_usb_no_clients = true;
            if (usb_host_device_free_all() == ESP_OK) {
                break;
            }
        }
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) && s_usb_no_clients) {
            break;
        }
        if (s_usb_quit && !s_usb_msc_installed) {
            /* Pilote MSC jamais installé : aucun client ne se désinscrira. */
            s_usb_no_clients = true;
            if (usb_host_device_free_all() == ESP_OK) {
                break;
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    usb_host_uninstall();
    xSemaphoreGive(s_usb_lib_done);
    vTaskDelete(NULL);
}

static void usb_stack_stop(void)
{
    if (s_usb_msc_installed) {
        msc_host_uninstall();       /* client désinscrit : la tâche voit NO_CLIENTS */
        s_usb_msc_installed = false;
    }
    s_usb_quit = true;
    usb_host_lib_unblock();
    if (xSemaphoreTake(s_usb_lib_done, pdMS_TO_TICKS(USB_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "arrêt de la pile USB : délai dépassé");
    }
    nid_board_usb_vbus(false);
}

/** Monte la clé USB (verrou fichiers pris, aucun support monté). */
static esp_err_t mount_usb(uint32_t connect_timeout_ms)
{
    if (s_usb_q == NULL) {
        s_usb_q = xQueueCreate(4, sizeof(usb_evt_t));
        s_usb_lib_done = xSemaphoreCreateBinary();
        if (s_usb_q == NULL || s_usb_lib_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xQueueReset(s_usb_q);
    xSemaphoreTake(s_usb_lib_done, 0);
    s_usb_no_clients = false;
    s_usb_quit = false;
    s_usb_lost = false;
    s_msc_dev = NULL;
    s_msc_vfs = NULL;

    const int64_t t0 = esp_timer_get_time();
    if (nid_board_usb_vbus(true)) {
        vTaskDelay(pdMS_TO_TICKS(USB_VBUS_SETTLE_MS));
    }
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hôte USB : %s", esp_err_to_name(err));
        nid_board_usb_vbus(false);
        return err;
    }
    if (xTaskCreatePinnedToCore(usb_lib_task, "nid_usb", USB_TASK_STACK, NULL, USB_TASK_PRIO, NULL, 1) != pdPASS) {
        usb_host_uninstall();
        nid_board_usb_vbus(false);
        return ESP_ERR_NO_MEM;
    }
    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority = USB_TASK_PRIO,
        .stack_size = USB_TASK_STACK,
        .callback = msc_event_cb,
    };
    err = msc_host_install(&msc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pilote de stockage USB : %s", esp_err_to_name(err));
        usb_stack_stop();
        return err;
    }
    s_usb_msc_installed = true;

    /* Attente du branchement (énumération de la clé). */
    usb_evt_t ev = { 0 };
    bool found = false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(connect_timeout_ms);
    while (!found) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0 || xQueueReceive(s_usb_q, &ev, deadline - now) != pdTRUE) {
            break;
        }
        found = ev.connected;
    }
    if (!found) {
        ESP_LOGW(TAG, "aucune clé USB détectée en %" PRIu32 " ms", connect_timeout_ms);
        usb_stack_stop();
        return ESP_ERR_NOT_FOUND;
    }

    err = msc_host_install_device(ev.address, &s_msc_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clé USB non prise en charge : %s", esp_err_to_name(err));
        s_msc_dev = NULL;
        usb_stack_stop();
        return err;
    }
    msc_host_device_info_t info;
    if (msc_host_get_device_info(s_msc_dev, &info) == ESP_OK) {
        s_usb_capacity = (uint64_t)info.sector_size * info.sector_count;
        ESP_LOGI(TAG, "clé USB %04X:%04X, %" PRIu64 " Mo, secteurs de %" PRIu32 " octets", info.idVendor,
                 info.idProduct, s_usb_capacity / 1000000u, info.sector_size);
    }
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,    /* jamais de formatage */
        .max_files = MAX_FILES_OPEN,
        .allocation_unit_size = 0,
    };
    err = msc_host_vfs_register(s_msc_dev, NID_SD_MOUNT_POINT, &mount_cfg, &s_msc_vfs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "système de fichiers de la clé USB illisible (FAT32 attendu) : %s", esp_err_to_name(err));
        msc_host_uninstall_device(s_msc_dev);
        s_msc_dev = NULL;
        s_msc_vfs = NULL;
        usb_stack_stop();
        return err;
    }
    s_medium = NID_STORAGE_MEDIUM_USB;
    s_mounted = true;
    ESP_LOGI(TAG, "clé USB montée en %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

static esp_err_t unmount_usb(void)
{
    if (s_msc_vfs != NULL) {
        if (msc_host_vfs_unregister(s_msc_vfs) != ESP_OK) {
            ESP_LOGW(TAG, "démontage du système de fichiers USB");
        }
        s_msc_vfs = NULL;
    }
    if (s_msc_dev != NULL) {
        msc_host_uninstall_device(s_msc_dev);
        s_msc_dev = NULL;
    }
    usb_stack_stop();
    ESP_LOGI(TAG, "clé USB démontée, 5 V USB %s", nid_board_pins()->usb_vbus != GPIO_NUM_NC ? "coupé" : "permanent");
    return ESP_OK;
}

#endif /* NID_STORAGE_HAS_USB */

/* ------------------------------------------------------------------------- */
/* Montage                                                                    */
/* ------------------------------------------------------------------------- */

/** Monte le support choisi dans menuconfig (verrou fichiers pris). */
static esp_err_t mount_medium(void)
{
    esp_err_t err;
#if CONFIG_NID_STORAGE_USB
    err = mount_usb(CONFIG_NID_USB_CONNECT_TIMEOUT_MS);
#elif CONFIG_NID_STORAGE_AUTO
    err = mount_usb(CONFIG_NID_USB_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "pas de clé USB : carte microSD");
        err = mount_card();
    }
#else
    err = mount_card();
#endif
    if (err != ESP_OK) {
        return err;
    }
    log_volume();
    /* Identifiants déjà connus (remontage après une pause) : le support a pu être remplacé. */
    char base[BASE_DIR_MAX];
    if (snapshot_ids(base, sizeof(base), NULL, NULL)) {
        ensure_dir(base);
    }
    return ESP_OK;
}

esp_err_t nid_storage_mount(void)
{
    SemaphoreHandle_t lock = fs_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    const esp_err_t err = s_mounted ? ESP_OK : mount_medium();
    xSemaphoreGive(lock);
    return err;
}

esp_err_t nid_storage_unmount(void)
{
    SemaphoreHandle_t lock = fs_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    s_mounted = false;
#if NID_STORAGE_HAS_USB
    if (s_medium == NID_STORAGE_MEDIUM_USB) {
        err = unmount_usb();
    } else
#endif
    {
        err = unmount_card();
    }
    s_medium = NID_STORAGE_MEDIUM_NONE;
    xSemaphoreGive(lock);
    return err;
}

bool nid_storage_is_mounted(void)
{
#if NID_STORAGE_HAS_USB
    if (s_medium == NID_STORAGE_MEDIUM_USB && s_usb_lost) {
        return false;
    }
#endif
    return s_mounted;
}

nid_storage_medium_t nid_storage_medium(void)
{
    return s_mounted ? s_medium : NID_STORAGE_MEDIUM_NONE;
}

const char *nid_storage_medium_str(void)
{
    switch (nid_storage_medium()) {
    case NID_STORAGE_MEDIUM_SD:
        return "carte SD";
    case NID_STORAGE_MEDIUM_USB:
        return "clé USB";
    default:
        return "aucun";
    }
}

/**
 * Volume FAT monté (verrou fichiers pris) : carte SD, f_getfree sur le lecteur
 * de la carte, tailles calculées en 64 bits (nid_storage_calc.h) ; clé USB,
 * esp_vfs_fat_info.
 */
static esp_err_t read_volume(nid_storage_volume_t *v, bool full_scan)
{
    memset(v, 0, sizeof(*v));
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    v->medium = s_medium;
#if NID_STORAGE_HAS_USB
    if (s_medium == NID_STORAGE_MEDIUM_USB) {
        uint64_t total = 0, free_b = 0;
        const esp_err_t err = esp_vfs_fat_info(NID_SD_MOUNT_POINT, &total, &free_b);
        if (err != ESP_OK) {
            return err;
        }
        v->volume_bytes = total;
        v->free_bytes = free_b;
        v->card_bytes = s_usb_capacity;
        return ESP_OK;
    }
#endif
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xFF) {
        return ESP_ERR_INVALID_STATE;
    }
    const char drv[3] = { (char)('0' + pdrv), ':', '\0' };
    FATFS *fs = NULL;
    DWORD free_clst = 0;
    FRESULT res = f_getfree(drv, &free_clst, &fs);
    if (res == FR_OK && (full_scan || free_clst > fs->n_fatent - 2)) {
        /* Compteur FSINFO ignoré (ou invraisemblable) : recomptage complet de la FAT. */
        fs->free_clst = 0xFFFFFFFF;
        res = f_getfree(drv, &free_clst, &fs);
    }
    if (res != FR_OK || fs == NULL) {
        ESP_LOGE(TAG, "lecture de l'espace libre impossible (FatFs %d)", (int)res);
        return ESP_FAIL;
    }
#if FF_MAX_SS != FF_MIN_SS
    const uint32_t sector_size = fs->ssize;
#else
    const uint32_t sector_size = FF_MIN_SS;
#endif
    v->volume_bytes = nid_fat_bytes(nid_fat_clusters(fs->n_fatent), fs->csize, sector_size);
    v->free_bytes = nid_fat_bytes(free_clst, fs->csize, sector_size);
    v->cluster_bytes = (uint32_t)fs->csize * sector_size;
    v->first_sector = (uint32_t)fs->volbase;
    v->fat_type = fs->fs_type == FS_FAT32 ? 32 : (fs->fs_type == FS_FAT16 ? 16 : 12);
    v->card_bytes = (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size;
    return ESP_OK;
}

esp_err_t nid_storage_get_volume(nid_storage_volume_t *out, bool full_scan)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t lock = fs_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    const esp_err_t err = read_volume(out, full_scan);
    xSemaphoreGive(lock);
    return err;
}

esp_err_t nid_storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    nid_storage_volume_t v;
    const esp_err_t err = nid_storage_get_volume(&v, false);
    if (total_bytes != NULL) {
        *total_bytes = v.volume_bytes;
    }
    if (free_bytes != NULL) {
        *free_bytes = v.free_bytes;
    }
    return err;
}

/**
 * Journal série du volume monté ; si le volume FAT ne couvre pas la carte,
 * description de la table de partitions (cause habituelle : carte de
 * Raspberry Pi, « 42 Mo libres » sur une carte de 120 Go au banc du 14/10).
 */
static void log_volume(void)
{
    nid_storage_volume_t v;
    if (read_volume(&v, false) != ESP_OK) {
        return;
    }
    if (v.medium == NID_STORAGE_MEDIUM_USB) {
        ESP_LOGI(TAG, "volume FAT de la clé USB : %" PRIu64 " Mo dont %" PRIu64 " Mo libres ; clé %" PRIu64 " Mo",
                 v.volume_bytes / 1000000ULL, v.free_bytes / 1000000ULL, v.card_bytes / 1000000ULL);
    } else {
        ESP_LOGI(TAG, "volume FAT%u : %" PRIu64 " Mo dont %" PRIu64 " Mo libres, grappes de %" PRIu32 " ko, "
                 "début secteur %" PRIu32 " ; carte %" PRIu64 " Mo", v.fat_type, v.volume_bytes / 1000000ULL,
                 v.free_bytes / 1000000ULL, v.cluster_bytes / 1024u, v.first_sector, v.card_bytes / 1000000ULL);
    }
    if (nid_fat_volume_too_small(v.volume_bytes, v.card_bytes)) {
        ESP_LOGW(TAG, "le volume FAT monté (%" PRIu64 " Mo) ne couvre qu'une partie du support (%" PRIu64 " Mo)",
                 v.volume_bytes / 1000000ULL, v.card_bytes / 1000000ULL);
        if (v.medium == NID_STORAGE_MEDIUM_SD) {
            log_partitions(s_card);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Identifiants et dossier de base                                            */
/* ------------------------------------------------------------------------- */

esp_err_t nid_storage_set_ids(const char *system_id, const char *nest_id)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    char sys[ID_MAX_LEN + 1];
    char nid[ID_MAX_LEN + 1];
    char base[BASE_DIR_MAX];
    sanitize_id(sys, sizeof(sys), system_id, DEFAULT_SYSTEM_ID);
    sanitize_id(nid, sizeof(nid), nest_id, DEFAULT_NEST_ID);
    snprintf(base, sizeof(base), NID_SD_MOUNT_POINT "/%s_%s", sys, nid);

    const esp_err_t err = ensure_dir(base);
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_state_mux);
    strlcpy(s_system_id, sys, sizeof(s_system_id));
    strlcpy(s_nest_id, nid, sizeof(s_nest_id));
    strlcpy(s_base_dir, base, sizeof(s_base_dir));
    portEXIT_CRITICAL(&s_state_mux);
    ESP_LOGI(TAG, "dossier de base : %s", base);
    return ESP_OK;
}

const char *nid_storage_base_dir(void)
{
    return (s_base_dir[0] != '\0') ? s_base_dir : NID_SD_MOUNT_POINT;
}

/* ------------------------------------------------------------------------- */
/* Fichiers médias                                                            */
/* ------------------------------------------------------------------------- */

esp_err_t nid_storage_media_begin(nid_media_kind_t kind, time_t ts, uint32_t seq, nid_media_file_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (kind != NID_MEDIA_VIDEO && kind != NID_MEDIA_PHOTO) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    char base[BASE_DIR_MAX];
    char sys[ID_MAX_LEN + 1];
    char nid[ID_MAX_LEN + 1];
    if (!snapshot_ids(base, sizeof(base), sys, nid)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Dossier du jour, en heure locale. Heure perdue (avant 2025) : dossier et noms
     * « SANSHEURE » plutôt qu'une date fausse (1969-12-31 au banc du 14/10). */
    struct tm tm;
    localtime_r(&ts, &tm);
    char day[12];
    char hms[8];
    if ((int64_t)ts < TIME_VALID_MIN_UTC) {
        strlcpy(day, "SANSHEURE", sizeof(day));
        strlcpy(hms, "000000", sizeof(hms));
    } else if (strftime(day, sizeof(day), "%Y%m%d", &tm) == 0 || strftime(hms, sizeof(hms), "%H%M%S", &tm) == 0) {
        return ESP_FAIL;
    }
    char dir[BASE_DIR_MAX + 12];
    int len = snprintf(dir, sizeof(dir), "%s/%s", base, day);
    if (len < 0 || (size_t)len >= sizeof(dir)) {
        return ESP_FAIL;
    }
    ensure_dir(base);   /* recréé si la carte a été remplacée ; l'échec réel apparaît sur le dossier du jour */
    esp_err_t err = ensure_dir(dir);
    if (err != ESP_OK) {
        return err;
    }

    /* <SYS>_<NID>_<AAAAMMJJ>_<HHMMSS>_V0042 */
    const char kind_char = (kind == NID_MEDIA_VIDEO) ? 'V' : 'P';
    const char *ext = (kind == NID_MEDIA_VIDEO) ? ".mp4" : ".jpg";
    char stem[64];
    len = snprintf(stem, sizeof(stem), "%s_%s_%s_%s_%c%04u", sys, nid, day, hms, kind_char, (unsigned)(seq % 10000));
    if (len < 0 || (size_t)len >= sizeof(stem)) {
        return ESP_FAIL;
    }

    /* Jamais d'écrasement : « _1 », « _2 »... avant l'extension si le nom (ou son .tmp) existe. */
    for (unsigned n = 0; n <= MAX_SAME_NAME; n++) {
        if (n == 0) {
            len = snprintf(out->name, sizeof(out->name), "%s%s", stem, ext);
        } else {
            len = snprintf(out->name, sizeof(out->name), "%s_%u%s", stem, n, ext);
        }
        const int len_final = snprintf(out->final_path, sizeof(out->final_path), "%s/%s", dir, out->name);
        const int len_tmp = snprintf(out->tmp_path, sizeof(out->tmp_path), "%s" TMP_SUFFIX, out->final_path);
        if (len < 0 || (size_t)len >= sizeof(out->name) || len_final < 0 || (size_t)len_final >= sizeof(out->final_path) ||
            len_tmp < 0 || (size_t)len_tmp >= sizeof(out->tmp_path)) {
            break;
        }
        if (!path_exists(out->final_path) && !path_exists(out->tmp_path)) {
            return ESP_OK;
        }
    }
    memset(out, 0, sizeof(*out));
    ESP_LOGE(TAG, "trop de fichiers portant le nom %s%s", stem, ext);
    return ESP_FAIL;
}

esp_err_t nid_storage_media_commit(const nid_media_file_t *f)
{
    if (f == NULL || f->tmp_path[0] == '\0' || f->final_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rename(f->tmp_path, f->final_path) != 0) {
        const int err = errno;
        ESP_LOGE(TAG, "renommage %s -> %s impossible (errno %d)", f->tmp_path, f->final_path, err);
        return (err == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * Renomme tmp_path en « <final sans extension>_INCOMPLET.<ext> » ; si ce nom
 * existe déjà (cas très improbable), ajoute _1, _2... avant l'extension.
 */
static esp_err_t keep_as_incomplete(const char *tmp_path, const char *final_path)
{
    struct stat st;
    if (stat(tmp_path, &st) != 0) {
        /* Échec avant la création du fichier (encodeur, mémoire...) : rien à garder. */
        ESP_LOGW(TAG, "%s n'a pas été créé : rien à renommer", tmp_path);
        return ESP_OK;
    }
    const char *slash = strrchr(final_path, '/');
    const char *dot = strrchr(final_path, '.');
    size_t stem_len = strlen(final_path);
    const char *ext = "";
    if (dot != NULL && (slash == NULL || dot > slash)) {
        stem_len = (size_t)(dot - final_path);
        ext = dot;
    }
    char dst[NID_PATH_MAX + 24];
    for (int n = 0; n < 10; n++) {
        if (n == 0) {
            snprintf(dst, sizeof(dst), "%.*s" INCOMPLETE_TAG "%s", (int)stem_len, final_path, ext);
        } else {
            snprintf(dst, sizeof(dst), "%.*s" INCOMPLETE_TAG "_%d%s", (int)stem_len, final_path, n, ext);
        }
        if (rename(tmp_path, dst) == 0) {
            return ESP_OK;
        }
        if (errno != EEXIST) {
            break;
        }
    }
    const int err = errno;
    ESP_LOGE(TAG, "renommage de %s en _INCOMPLET impossible (errno %d)", tmp_path, err);
    return (err == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

esp_err_t nid_storage_media_abort(const nid_media_file_t *f, bool keep_partial)
{
    if (f == NULL || f->tmp_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (keep_partial) {
        return keep_as_incomplete(f->tmp_path, f->final_path);
    }
    if (unlink(f->tmp_path) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "suppression de %s impossible (errno %d)", f->tmp_path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Renomme les « *.tmp » d'un dossier en « *_INCOMPLET.<ext> » ; retourne le nombre de fichiers traités. */
static int recover_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_DIR || !has_tmp_suffix(ent->d_name)) {
            continue;
        }
        char tmp_path[NID_PATH_MAX];
        if (snprintf(tmp_path, sizeof(tmp_path), "%s/%s", dir_path, ent->d_name) >= (int)sizeof(tmp_path)) {
            continue;   /* nom trop long : ce n'est pas un de nos fichiers */
        }
        char final_path[NID_PATH_MAX];
        strlcpy(final_path, tmp_path, sizeof(final_path));
        final_path[strlen(final_path) - (sizeof(TMP_SUFFIX) - 1)] = '\0';
        if (path_exists(final_path)) {
            /* Renommage interrompu : deux noms sur les mêmes clusters. Ne rien supprimer ni renommer. */
            ESP_LOGW(TAG, "%s et %s coexistent : laissés en l'état (à vérifier sur un ordinateur)", tmp_path, final_path);
            continue;
        }
        if (keep_as_incomplete(tmp_path, final_path) == ESP_OK) {
            ESP_LOGW(TAG, "fichier interrompu conservé : %s", tmp_path);
            count++;
        }
    }
    closedir(dir);
    return count;
}

int nid_storage_recover(void)
{
    char base[BASE_DIR_MAX];
    if (!s_mounted || !snapshot_ids(base, sizeof(base), NULL, NULL)) {
        return -1;
    }
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return (errno == ENOENT) ? 0 : -1;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char path[NID_PATH_MAX];
        if (ent->d_type != DT_DIR || strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", base, ent->d_name) < (int)sizeof(path)) {
            count += recover_dir(path);
        }
    }
    closedir(dir);
    count += recover_dir(base);
    return count;
}

/* ------------------------------------------------------------------------- */
/* Journal                                                                    */
/* ------------------------------------------------------------------------- */

static void put_char(char *buf, size_t *n, size_t limit, char c)
{
    if (*n < limit) {
        buf[(*n)++] = c;
    }
}

/**
 * Compose « date locale;événement;"détails";batterie;température\n » et
 * retourne la longueur. Les « " » des détails sont doublés, les sauts de ligne
 * remplacés par des espaces ; batterie < 0 et température inconnue : vides.
 */
static size_t format_journal_line(char *line, size_t size, const char *event, const char *details,
                                  int32_t batt_mv, float temp_c)
{
    char stamp[24];
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    size_t n = (size_t)snprintf(line, size, "%s;", stamp);
    const size_t limit = size - LINE_TAIL_RESERVE;
    for (const char *p = event; *p != '\0'; p++) {
        put_char(line, &n, limit, (*p == ';' || *p == '"' || *p == '\r' || *p == '\n') ? '_' : *p);
    }
    put_char(line, &n, limit, ';');
    put_char(line, &n, limit, '"');
    for (const char *p = details; *p != '\0'; p++) {
        char c = *p;
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
        if (c == '"') {
            put_char(line, &n, limit, '"');
        }
        put_char(line, &n, limit, c);
    }
    line[n++] = '"';
    line[n++] = ';';
    if (batt_mv >= 0) {
        n += (size_t)snprintf(line + n, size - n, "%" PRId32, batt_mv);
    }
    line[n++] = ';';
    if (!isnan(temp_c) && fabsf(temp_c) < 1000.0f) {
        n += (size_t)snprintf(line + n, size - n, "%.1f", (double)temp_c);
    }
    line[n++] = '\n';
    line[n] = '\0';
    return n;
}

/** Ajoute la ligne au journal (verrou fichiers pris) : écriture, fflush, fsync, fermeture. */
static esp_err_t append_journal(const char *base, const char *line, size_t len)
{
    char path[BASE_DIR_MAX + 16];
    snprintf(path, sizeof(path), "%s/" JOURNAL_FILE, base);

    struct stat st;
    const bool is_new = (stat(path, &st) != 0) || (st.st_size == 0);

    FILE *f = fopen(path, "a");
    if (f == NULL && errno == ENOENT && ensure_dir(base) == ESP_OK) {
        f = fopen(path, "a");   /* dossier de base recréé (carte remplacée) */
    }
    if (f == NULL) {
        ESP_LOGW(TAG, "ouverture de %s impossible (errno %d)", path, errno);
        return ESP_FAIL;
    }
    bool ok = !is_new || fputs(JOURNAL_HEADER, f) >= 0;
    ok = ok && fwrite(line, 1, len, f) == len;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGW(TAG, "écriture du journal impossible (carte pleine ou retirée ?)");
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t nid_storage_journal(const char *event, const char *fmt, ...)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char details[192] = "";
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(details, sizeof(details), fmt, ap);
        va_end(ap);
    }

    char base[BASE_DIR_MAX];
    int32_t batt_mv;
    float temp_c;
    const bool ids_known = snapshot_ids(base, sizeof(base), NULL, NULL);
    portENTER_CRITICAL(&s_state_mux);
    batt_mv = s_batt_mv;
    temp_c = s_temp_c;
    portEXIT_CRITICAL(&s_state_mux);

    char line[512];
    const size_t len = format_journal_line(line, sizeof(line), event, details, batt_mv, temp_c);
    ESP_LOGI(TAG, "[journal] %.*s", (int)(len - 1), line);

    if (!s_mounted || !ids_known) {
        return ESP_ERR_INVALID_STATE;
    }
    SemaphoreHandle_t lock = fs_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    const esp_err_t err = s_mounted ? append_journal(base, line, len) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(lock);
    return err;
}

void nid_storage_journal_context(int32_t batt_mv, float temp_c)
{
    portENTER_CRITICAL(&s_state_mux);
    s_batt_mv = batt_mv;
    s_temp_c = temp_c;
    portEXIT_CRITICAL(&s_state_mux);
}

/* ------------------------------------------------------------------------- */
/* Écriture atomique                                                          */
/* ------------------------------------------------------------------------- */

esp_err_t nid_storage_write_atomic(const char *path, const void *data, size_t len)
{
    if (path == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    char tmp[NID_PATH_MAX + sizeof(TMP_SUFFIX)];
    if (snprintf(tmp, sizeof(tmp), "%s" TMP_SUFFIX, path) >= (int)sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "création de %s impossible (errno %d)", tmp, errno);
        return ESP_FAIL;
    }
    bool ok = (len == 0) || fwrite(data, 1, len, f) == len;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "écriture de %s impossible (carte pleine ou retirée ?)", tmp);
        unlink(tmp);
        return ESP_FAIL;
    }

    /* FAT refuse de renommer sur un fichier existant. Entre unlink et rename, les données
     * complètes restent dans « path.tmp » (jamais supprimé en cas d'échec du renommage). */
    unlink(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "renommage %s -> %s impossible (errno %d)", tmp, path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}
