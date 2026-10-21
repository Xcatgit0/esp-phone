#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "esp_vfs_fat.h"

static const char *TAG = "USB_HOST_APP";

#define MOUNT_PATH "/usb"

typedef enum {
    APP_EVT_CONNECTED,
    APP_EVT_DISCONNECTED,
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    union {
        uint8_t address;                 // ใช้กับ CONNECTED
        msc_host_device_handle_t handle; // ใช้กับ DISCONNECTED
    };
} app_evt_t;

static QueueHandle_t app_queue = NULL;
static msc_host_vfs_handle_t vfs_handle = NULL;
static msc_host_device_handle_t msc_device = NULL;

/* ------------------------------------------------------------------ */
/* Callback จาก MSC driver: ห้ามทำงานหนักที่นี่ ส่งเข้า queue ไปทำใน app task */
/* ------------------------------------------------------------------ */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    app_evt_t evt = {0};
    switch (event->event) {
    case MSC_DEVICE_CONNECTED:
        evt.type = APP_EVT_CONNECTED;
        evt.address = event->device.address;
        break;
    case MSC_DEVICE_DISCONNECTED:
        evt.type = APP_EVT_DISCONNECTED;
        evt.handle = event->device.handle;
        break;
    default:
        return;
    }
    xQueueSend(app_queue, &evt, 0);
}

/* ------------------------------------------------------------------ */
/* แสดงข้อมูลอุปกรณ์                                                    */
/* ------------------------------------------------------------------ */
static void print_device_info(msc_host_device_handle_t dev)
{
    msc_host_device_info_t info;
    if (msc_host_get_device_info(dev, &info) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read device info");
        return;
    }

    uint64_t total = (uint64_t)info.sector_count * info.sector_size;
    ESP_LOGI(TAG, "---------- Device Info ----------");
    ESP_LOGI(TAG, "VID:PID       : %04X:%04X", info.idVendor, info.idProduct);
    ESP_LOGI(TAG, "Sector size   : %" PRIu32 " bytes", (uint32_t)info.sector_size);
    ESP_LOGI(TAG, "Sector count  : %" PRIu32, (uint32_t)info.sector_count);
    ESP_LOGI(TAG, "Total size    : %" PRIu64 " MB", total / (1024 * 1024));
    ESP_LOGI(TAG, "---------------------------------");

    // พิมพ์ USB descriptor เต็ม ๆ (manufacturer, product, serial ฯลฯ)
    msc_host_print_descriptors(dev);
}

/* ------------------------------------------------------------------ */
/* แสดงรายชื่อไฟล์ (ชื่อ, ชนิด, ขนาด)                                    */
/* ------------------------------------------------------------------ */
static void list_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open dir: %s", path);
        return;
    }

    ESP_LOGI(TAG, "Listing %s", path);
    int files = 0, dirs = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full[300];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                ESP_LOGI(TAG, "  [DIR ] %s", entry->d_name);
                dirs++;
            } else {
                ESP_LOGI(TAG, "  [FILE] %-32s %10" PRIu32 " bytes",
                         entry->d_name, (uint32_t)st.st_size);
                files++;
            }
        } else {
            ESP_LOGI(TAG, "  [ ?  ] %s", entry->d_name);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Total: %d file(s), %d dir(s)", files, dirs);
}

/* ------------------------------------------------------------------ */
/* Mount / Unmount                                                      */
/* ------------------------------------------------------------------ */
static void handle_connected(uint8_t address)
{
    ESP_LOGI(TAG, ">>> USB drive connected (addr %d) <<<", address);

    if (msc_device) {
        ESP_LOGW(TAG, "A device is already mounted, ignoring");
        return;
    }

    esp_err_t err = msc_host_install_device(address, &msc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
        msc_device = NULL;
        return;
    }

    print_device_info(msc_device);

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 128,
        .allocation_unit_size = 8 * 1024,
    };

    err = msc_host_vfs_register(msc_device, MOUNT_PATH, &mount_cfg, &vfs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s (ระบบไฟล์ต้องเป็น FAT/FAT32/exFAT)",
                 esp_err_to_name(err));
        msc_host_uninstall_device(msc_device);
        msc_device = NULL;
        vfs_handle = NULL;
        return;
    }

    ESP_LOGI(TAG, ">>> Mounted at %s <<<", MOUNT_PATH);
    list_dir(MOUNT_PATH);
}

static void handle_disconnected(void)
{
    ESP_LOGI(TAG, ">>> USB drive disconnected <<<");

    if (vfs_handle) {
        msc_host_vfs_unregister(vfs_handle);
        vfs_handle = NULL;
        ESP_LOGI(TAG, "Unmounted %s", MOUNT_PATH);
    }
    if (msc_device) {
        msc_host_uninstall_device(msc_device);
        msc_device = NULL;
    }
    ESP_LOGI(TAG, "Waiting for USB drive...");
}

/* ------------------------------------------------------------------ */
/* Tasks                                                                */
/* ------------------------------------------------------------------ */
static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            continue;
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "USB Host: no clients, freeing all devices");
            usb_host_device_free_all();
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB Host: all devices freed");
        }
    }
}

static void app_task(void *arg)
{
    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true, // (สะกดผิดตาม API ของ ESP-IDF)
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));
    ESP_LOGI(TAG, "MSC Host installed. Waiting for USB drive...");

    app_evt_t evt;
    while (1) {
        if (xQueueReceive(app_queue, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.type) {
            case APP_EVT_CONNECTED:
                handle_connected(evt.address);
                break;
            case APP_EVT_DISCONNECTED:
                handle_disconnected();
                break;
            }
        }
    }
}

void usb_main(void)
{
    ESP_LOGI(TAG, "Initializing USB Host stack...");

    app_queue = xQueueCreate(4, sizeof(app_evt_t));
    assert(app_queue);

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 6, NULL);
    // app_task ทำ mount + อ่านไดเรกทอรี ใช้ stack เยอะกว่า จึงให้ 6144
    xTaskCreate(app_task, "usb_app", 6144, NULL, 5, NULL);
}