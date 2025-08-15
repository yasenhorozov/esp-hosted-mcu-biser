// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2025 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_hosted_config.h"
#include "os_wrapper.h"
#include "usb_wrapper.h"

#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_types.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

static const char TAG[] = "usb_wrapper";

#define USB_FAIL_IF_NULL_CTX(x) do { \
		if (!x) return ESP_FAIL;      \
	} while (0);

typedef struct {
	bool is_host_mode;
	bool is_device_mode;
	bool is_connected;
	usb_host_client_handle_t usb_host_handle;
	cdc_acm_host_device_handle_t cdc_acm_handle;
	QueueHandle_t rx_queue;
	SemaphoreHandle_t tx_semaphore;
	SemaphoreHandle_t rx_semaphore;
	uint16_t vendor_id;
	uint16_t product_id;
	uint16_t bulk_endpoint_size;
	TaskHandle_t usb_host_task;
	bool usb_host_installed;
} usb_wrapper_ctx_t;

static usb_wrapper_ctx_t *g_usb_ctx = NULL;

// USB Host task for handling USB events
static void usb_host_lib_task(void *arg)
{
	while (1) {
		uint32_t event_flags;
		usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
		
		if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
			ESP_LOGD(TAG, "No USB clients");
		}
		if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
			ESP_LOGD(TAG, "All USB devices freed");
		}
	}
}

// CDC-ACM host event callback
static void cdc_acm_host_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t *)user_ctx;
	
	switch (event->type) {
		case CDC_ACM_HOST_ERROR:
			ESP_LOGE(TAG, "CDC-ACM error");
			usb_ctx->is_connected = false;
			break;
		case CDC_ACM_HOST_DEVICE_DISCONNECTED:
			ESP_LOGI(TAG, "CDC-ACM device disconnected");
			usb_ctx->is_connected = false;
			usb_ctx->cdc_acm_handle = NULL;
			break;
		case CDC_ACM_HOST_SERIAL_STATE:
			ESP_LOGD(TAG, "CDC-ACM serial state change");
			break;
		case CDC_ACM_HOST_NETWORK_CONNECTION:
			ESP_LOGD(TAG, "CDC-ACM network connection change");
			break;
		default:
			ESP_LOGW(TAG, "Unknown CDC-ACM event: %d", event->type);
			break;
	}
}

// CDC-ACM data received callback
static bool cdc_acm_host_data_callback(const uint8_t *data, size_t data_len, void *user_ctx)
{
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t *)user_ctx;
	
	if (!usb_ctx || !data || data_len == 0) {
		return false;
	}
	
	ESP_LOGV(TAG, "USB RX data: %zu bytes", data_len);
	
	// Try to send data to RX queue
	if (usb_ctx->rx_queue) {
		// For simplicity, we'll store the data length and signal the semaphore
		// In a real implementation, you'd need to handle the actual data buffer management
		if (xQueueSend(usb_ctx->rx_queue, data, 0) == pdTRUE) {
			xSemaphoreGive(usb_ctx->rx_semaphore);
		} else {
			ESP_LOGW(TAG, "RX queue full, dropping data");
		}
	}
	
	return true;
}

// TinyUSB CDC-ACM callbacks for device mode
static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
	if (!g_usb_ctx || !g_usb_ctx->is_device_mode) {
		return;
	}
	
	size_t rx_size = 0;
	uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
	
	esp_err_t ret = tinyusb_cdcacm_read(itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
	if (ret == ESP_OK && rx_size > 0) {
		ESP_LOGV(TAG, "TinyUSB RX data: %zu bytes", rx_size);
		
		// Send data to RX queue
		if (g_usb_ctx->rx_queue) {
			if (xQueueSend(g_usb_ctx->rx_queue, buf, 0) == pdTRUE) {
				xSemaphoreGive(g_usb_ctx->rx_semaphore);
			} else {
				ESP_LOGW(TAG, "RX queue full, dropping data");
			}
		}
	}
}

static void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
	if (!g_usb_ctx || !g_usb_ctx->is_device_mode) {
		return;
	}
	
	bool dtr = event->line_state_changed_data.dtr;
	bool rts = event->line_state_changed_data.rts;
	
	ESP_LOGI(TAG, "TinyUSB line state changed - DTR: %d, RTS: %d", dtr, rts);
	
	// Update connection state based on DTR
	g_usb_ctx->is_connected = dtr;
}

void * hosted_usb_init(void)
{
	ESP_LOGI(TAG, "Initializing USB wrapper");
	
	if (g_usb_ctx) {
		ESP_LOGW(TAG, "USB wrapper already initialized");
		return g_usb_ctx;
	}
	
	g_usb_ctx = calloc(1, sizeof(usb_wrapper_ctx_t));
	if (!g_usb_ctx) {
		ESP_LOGE(TAG, "Failed to allocate USB context");
		return NULL;
	}
	
	struct esp_hosted_usb_config *config;
	esp_hosted_usb_get_config(&config);
	
	g_usb_ctx->is_host_mode = config->host_mode;
	g_usb_ctx->is_device_mode = config->device_mode;
	g_usb_ctx->vendor_id = config->vendor_id;
	g_usb_ctx->product_id = config->product_id;
	g_usb_ctx->bulk_endpoint_size = config->bulk_endpoint_size;
	g_usb_ctx->is_connected = false;
	g_usb_ctx->usb_host_installed = false;
	
	// Create RX queue for incoming data
	g_usb_ctx->rx_queue = xQueueCreate(config->rx_queue_size, config->bulk_endpoint_size);
	if (!g_usb_ctx->rx_queue) {
		ESP_LOGE(TAG, "Failed to create RX queue");
		goto cleanup;
	}
	
	// Create semaphores for synchronization
	g_usb_ctx->tx_semaphore = xSemaphoreCreateMutex();
	g_usb_ctx->rx_semaphore = xSemaphoreCreateBinary();
	
	if (!g_usb_ctx->tx_semaphore || !g_usb_ctx->rx_semaphore) {
		ESP_LOGE(TAG, "Failed to create semaphores");
		goto cleanup;
	}
	
	if (g_usb_ctx->is_host_mode) {
		ESP_LOGI(TAG, "Initializing USB Host mode");
		
		// Install USB Host driver
		const usb_host_config_t host_config = {
			.skip_phy_setup = false,
			.intr_flags = ESP_INTR_FLAG_LEVEL1,
		};
		
		esp_err_t ret = usb_host_install(&host_config);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to install USB host driver: %s", esp_err_to_name(ret));
			goto cleanup;
		}
		g_usb_ctx->usb_host_installed = true;
		
		// Create USB host library task
		if (xTaskCreate(usb_host_lib_task, "usb_host_lib", 4096, NULL, 2, &g_usb_ctx->usb_host_task) != pdTRUE) {
			ESP_LOGE(TAG, "Failed to create USB host library task");
			goto cleanup;
		}
		
		// Register USB host client
		const usb_host_client_config_t client_config = {
			.is_synchronous = false,
			.max_num_event_msg = 5,
			.async = {
				.client_event_callback = NULL,
				.callback_arg = g_usb_ctx,
			}
		};
		
		ret = usb_host_client_register(&client_config, &g_usb_ctx->usb_host_handle);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to register USB host client: %s", esp_err_to_name(ret));
			goto cleanup;
		}
		
		// Install CDC-ACM host driver
		ret = cdc_acm_host_install(NULL);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to install CDC-ACM host driver: %s", esp_err_to_name(ret));
			goto cleanup;
		}
		
	} else if (g_usb_ctx->is_device_mode) {
		ESP_LOGI(TAG, "Initializing USB Device mode");
		
		// Initialize TinyUSB
		tinyusb_config_t tusb_cfg = {
			.device_descriptor = NULL,  // Use default descriptor
			.string_descriptor = NULL,  // Use default strings
			.external_phy = false,
			.configuration_descriptor = NULL, // Use default configuration
		};
		
		esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
			goto cleanup;
		}
		
		// Configure CDC-ACM interface
		tinyusb_config_cdcacm_t cdc_cfg = {
			.usb_dev = TINYUSB_USBDEV_0,
			.cdc_port = TINYUSB_CDC_ACM_0,
			.rx_unread_buf_sz = config->bulk_endpoint_size,
			.callback_rx = &tinyusb_cdc_rx_callback,
			.callback_rx_wanted_char = NULL,
			.callback_line_state_changed = &tinyusb_cdc_line_state_changed_callback,
			.callback_line_coding_changed = NULL
		};
		
		ret = tusb_cdc_acm_init(&cdc_cfg);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to initialize CDC-ACM: %s", esp_err_to_name(ret));
			goto cleanup;
		}
	}
	
	ESP_LOGI(TAG, "USB wrapper initialized successfully");
	return g_usb_ctx;
	
cleanup:
	hosted_usb_deinit(g_usb_ctx);
	return NULL;
}

esp_err_t hosted_usb_deinit(void *ctx)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	ESP_LOGI(TAG, "Deinitializing USB wrapper");
	
	// Disconnect if connected
	if (usb_ctx->is_connected) {
		if (usb_ctx->is_host_mode) {
			hosted_usb_host_close_device(ctx);
		} else if (usb_ctx->is_device_mode) {
			hosted_usb_device_disconnect(ctx);
		}
	}
	
	// Cleanup USB host resources
	if (usb_ctx->is_host_mode) {
		// Uninstall CDC-ACM host driver
		if (usb_ctx->cdc_acm_handle == NULL) {
			cdc_acm_host_uninstall();
		}
		
		// Deregister USB host client
		if (usb_ctx->usb_host_handle) {
			usb_host_client_deregister(usb_ctx->usb_host_handle);
		}
		
		// Delete USB host library task
		if (usb_ctx->usb_host_task) {
			vTaskDelete(usb_ctx->usb_host_task);
		}
		
		// Uninstall USB host driver
		if (usb_ctx->usb_host_installed) {
			usb_host_uninstall();
		}
	}
	
	// Cleanup queues and semaphores
	if (usb_ctx->rx_queue) {
		vQueueDelete(usb_ctx->rx_queue);
	}
	
	if (usb_ctx->tx_semaphore) {
		vSemaphoreDelete(usb_ctx->tx_semaphore);
	}
	
	if (usb_ctx->rx_semaphore) {
		vSemaphoreDelete(usb_ctx->rx_semaphore);
	}
	
	// Free context
	if (ctx == g_usb_ctx) {
		g_usb_ctx = NULL;
	}
	free(ctx);
	
	ESP_LOGI(TAG, "USB wrapper deinitialized");
	return ESP_OK;
}

int hosted_usb_read(void *ctx, uint8_t *data, uint16_t size)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	if (!usb_ctx->is_connected || !data || size == 0) {
		return -1;
	}
	
	ESP_LOGV(TAG, "USB read request: size=%u", size);
	
	if (usb_ctx->is_host_mode && usb_ctx->cdc_acm_handle) {
		// USB Host mode - use CDC-ACM host
		size_t actual_size = 0;
		esp_err_t ret = cdc_acm_host_data_rx_blocking(usb_ctx->cdc_acm_handle, data, size, &actual_size, 100);
		
		if (ret == ESP_OK) {
			ESP_LOGV(TAG, "USB host read success: %zu bytes", actual_size);
			return actual_size;
		} else if (ret == ESP_ERR_TIMEOUT) {
			return 0; // No data available
		} else {
			ESP_LOGE(TAG, "USB host read error: %s", esp_err_to_name(ret));
			return -1;
		}
	} else if (usb_ctx->is_device_mode) {
		// USB Device mode - use TinyUSB
		size_t actual_size = 0;
		esp_err_t ret = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, data, size, &actual_size);
		
		if (ret == ESP_OK) {
			ESP_LOGV(TAG, "USB device read success: %zu bytes", actual_size);
			return actual_size;
		} else {
			ESP_LOGV(TAG, "USB device read: no data available");
			return 0;
		}
	}
	
	return -1;
}

int hosted_usb_write(void *ctx, uint8_t *data, uint16_t size)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	if (!usb_ctx->is_connected || !data || size == 0) {
		return -1;
	}
	
	// Take TX semaphore for thread safety
	if (xSemaphoreTake(usb_ctx->tx_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
		ESP_LOGE(TAG, "Failed to take TX semaphore");
		return -1;
	}
	
	ESP_LOGV(TAG, "USB write request: size=%u", size);
	
	int result = -1;
	
	if (usb_ctx->is_host_mode && usb_ctx->cdc_acm_handle) {
		// USB Host mode - use CDC-ACM host
		esp_err_t ret = cdc_acm_host_data_tx_blocking(usb_ctx->cdc_acm_handle, data, size, 1000);
		
		if (ret == ESP_OK) {
			ESP_LOGV(TAG, "USB host write success: %u bytes", size);
			result = size;
		} else {
			ESP_LOGE(TAG, "USB host write error: %s", esp_err_to_name(ret));
			result = -1;
		}
	} else if (usb_ctx->is_device_mode) {
		// USB Device mode - use TinyUSB
		size_t written = 0;
		esp_err_t ret = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, size);
		
		if (ret == ESP_OK) {
			// Flush the write to ensure data is sent
			ret = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(1000));
			if (ret == ESP_OK) {
				ESP_LOGV(TAG, "USB device write success: %u bytes", size);
				result = size;
			} else {
				ESP_LOGE(TAG, "USB device write flush error: %s", esp_err_to_name(ret));
				result = -1;
			}
		} else {
			ESP_LOGE(TAG, "USB device write error: %s", esp_err_to_name(ret));
			result = -1;
		}
	}
	
	xSemaphoreGive(usb_ctx->tx_semaphore);
	return result;
}

int hosted_wait_usb_rx_data(uint32_t ticks_to_wait)
{
	if (!g_usb_ctx || !g_usb_ctx->is_connected) {
		return -1;
	}
	
	// Wait for RX semaphore to be signaled
	if (xSemaphoreTake(g_usb_ctx->rx_semaphore, ticks_to_wait) == pdTRUE) {
		// Return number of bytes available (placeholder implementation)
		return uxQueueMessagesWaiting(g_usb_ctx->rx_queue);
	}
	
	return 0; // Timeout
}

int hosted_usb_host_check_device(void *ctx)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	if (!usb_ctx->is_host_mode) {
		ESP_LOGE(TAG, "Not in host mode");
		return -1;
	}
	
	// This will be implemented in Phase 3 with actual USB host device enumeration
	ESP_LOGD(TAG, "Checking for USB device");
	return 0;
}

int hosted_usb_host_open_device(void *ctx, uint16_t vid, uint16_t pid)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	if (!usb_ctx->is_host_mode) {
		ESP_LOGE(TAG, "Not in host mode");
		return -1;
	}
	
	ESP_LOGI(TAG, "Opening USB device VID:0x%04X PID:0x%04X", vid, pid);
	
	// Wait for CDC-ACM device to be connected
	cdc_acm_host_device_config_t dev_config = {
		.connection_timeout_ms = 5000,
		.out_buffer_size = usb_ctx->bulk_endpoint_size,
		.in_buffer_size = usb_ctx->bulk_endpoint_size,
		.event_cb = cdc_acm_host_event_callback,
		.data_cb = cdc_acm_host_data_callback,
		.user_arg = usb_ctx,
	};
	
	esp_err_t ret = cdc_acm_host_open(vid, pid, 0, &dev_config, &usb_ctx->cdc_acm_handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open CDC-ACM device: %s", esp_err_to_name(ret));
		return -1;
	}
	
	// Start data reception
	ret = cdc_acm_host_data_rx_blocking(usb_ctx->cdc_acm_handle, NULL, 0, NULL, 0);
	if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
		ESP_LOGW(TAG, "Failed to start data reception: %s", esp_err_to_name(ret));
	}
	
	usb_ctx->is_connected = true;
	ESP_LOGI(TAG, "USB CDC-ACM device opened successfully");
	
	return 0;
}

int hosted_usb_host_close_device(void *ctx)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	if (!usb_ctx->is_host_mode) {
		ESP_LOGE(TAG, "Not in host mode");
		return -1;
	}
	
	ESP_LOGI(TAG, "Closing USB host device");
	
	if (usb_ctx->cdc_acm_handle) {
		esp_err_t ret = cdc_acm_host_close(usb_ctx->cdc_acm_handle);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to close CDC-ACM device: %s", esp_err_to_name(ret));
			return -1;
		}
		usb_ctx->cdc_acm_handle = NULL;
	}
	
	usb_ctx->is_connected = false;
	ESP_LOGI(TAG, "USB host device closed successfully");
	
	return 0;
}

int hosted_usb_device_connect(void *ctx)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	if (!usb_ctx->is_device_mode) {
		ESP_LOGE(TAG, "Not in device mode");
		return -1;
	}
	
	ESP_LOGI(TAG, "Connecting USB device");
	
	// For TinyUSB device mode, connection is handled automatically
	// We just mark as connected - the line state callback will update this
	usb_ctx->is_connected = true;
	
	ESP_LOGI(TAG, "USB device connection initiated");
	return 0;
}

int hosted_usb_device_disconnect(void *ctx)
{
	USB_FAIL_IF_NULL_CTX(ctx);
	
	usb_wrapper_ctx_t *usb_ctx = (usb_wrapper_ctx_t*)ctx;
	
	if (!usb_ctx->is_device_mode) {
		ESP_LOGE(TAG, "Not in device mode");
		return -1;
	}
	
	ESP_LOGI(TAG, "Disconnecting USB device");
	
	// For TinyUSB device mode, we can force disconnect by deinitializing
	usb_ctx->is_connected = false;
	
	ESP_LOGI(TAG, "USB device disconnected");
	return 0;
}