// SPDX-License-Identifier: Apache-2.0
/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "drivers/bt/hci_drv.h"

#include "common.h"
#include "endian.h"
#include "esp_log.h"
#include "esp_hosted_log.h"
#include "transport_drv.h"
#include "stats.h"
#include "esp_hosted_power_save.h"
#include "esp_hosted_transport_config.h"
#include "power_save_drv.h"
#include "esp_hosted_bt.h"
#include "usb_drv.h"

static const char TAG[] = "H_USB_DRV";

static void h_usb_write_task(void const* pvParameters);
static void h_usb_read_task(void const* pvParameters);

extern transport_channel_t *chan_arr[ESP_MAX_IF];

static void * h_usb_write_task_info;
static void * h_usb_read_task_info;
static void * h_usb_process_rx_task_info;

static void * usb_handle = NULL;
static usb_context_t *usb_context = NULL;

static queue_handle_t to_slave_queue[MAX_PRIORITY_QUEUES];
static semaphore_handle_t sem_to_slave_queue;
static queue_handle_t from_slave_queue[MAX_PRIORITY_QUEUES];
static semaphore_handle_t sem_from_slave_queue;

static bool usb_start_write_thread = false;

static esp_hosted_transport_err_t process_capabilities_resp(uint8_t cap, uint8_t *raw_tp_in_use)
{
	ESP_LOGI(TAG, "Capabilities: 0x%x", cap);

	if (cap & ESP_WLAN_SDIO_SUPPORT) {
		ESP_LOGI(TAG, "ESP board supports SDIO");
	}

	if (cap & ESP_WLAN_SPI_SUPPORT) {
		ESP_LOGI(TAG, "ESP board supports SPI");
	}

	if (cap & ESP_WLAN_UART_SUPPORT) {
		ESP_LOGI(TAG, "ESP board supports UART");
	}

	if (cap & ESP_WLAN_USB_SUPPORT) {
		ESP_LOGI(TAG, "ESP board supports USB");
	}

	if (raw_tp_in_use) {
		*raw_tp_in_use = 0;
		if (cap & ESP_WLAN_USB_SUPPORT) {
			*raw_tp_in_use = ESP_TEST_RAW_TP__USB;
		}
	}

	return ESP_TRANSPORT_OK;
}

static void process_priv_communication(interface_buffer_handle_t *buf_handle)
{
	// Process private communication - placeholder implementation
	// This would handle ESP-Hosted protocol messages
	ESP_LOGD(TAG, "Processing private communication");
}

static void h_usb_process_rx_task(void const* pvParameters)
{
	ESP_LOGI(TAG, "Starting USB RX processing task");

	interface_buffer_handle_t buf_handle = {0};
	struct esp_hosted_usb_config *config;
	
	esp_hosted_usb_get_config(&config);
	
	while(1) {
		if (sem_from_slave_queue && 
			H_PLATFORM_SEM_WAIT(sem_from_slave_queue, portMAX_DELAY)) {
			
			for (int prio = 0; prio < MAX_PRIORITY_QUEUES; prio++) {
				if (from_slave_queue[prio] &&
					H_PLATFORM_Q_RECEIVE(from_slave_queue[prio],
					&buf_handle, 0)) {

					if (buf_handle.priv_buffer_handle &&
						buf_handle.free_buf_handle) {

						ESP_LOGV(TAG, "RX: prio:%u len:%u", prio, buf_handle.payload_len);

						if (buf_handle.if_type == ESP_PRIV_IF) {
							process_priv_communication(&buf_handle);
						} else {
							send_to_host_queue(&buf_handle, prio);
						}
					}
				}
			}
		}
	}
}

void *bus_init_internal(void)
{
	ESP_LOGI(TAG, "Initializing USB transport");
	
	struct esp_hosted_usb_config *config;
	esp_hosted_usb_get_config(&config);
	
	usb_context = (usb_context_t*)H_CALLOC(1, sizeof(usb_context_t));
	if (!usb_context) {
		ESP_LOGE(TAG, "Failed to allocate USB context");
		return NULL;
	}
	
	usb_context->is_host_mode = config->host_mode;
	usb_context->is_device_mode = config->device_mode;
	usb_context->cdc_acm_enabled = config->cdc_acm_class;
	usb_context->bulk_endpoint_size = config->bulk_endpoint_size;
	usb_context->vendor_id = config->vendor_id;
	usb_context->product_id = config->product_id;
	
	// Initialize queues
	for (int i = 0; i < MAX_PRIORITY_QUEUES; i++) {
		to_slave_queue[i] = H_PLATFORM_Q_CREATE(config->tx_queue_size,
			sizeof(interface_buffer_handle_t));
		from_slave_queue[i] = H_PLATFORM_Q_CREATE(config->rx_queue_size,
			sizeof(interface_buffer_handle_t));
			
		if (!to_slave_queue[i] || !from_slave_queue[i]) {
			ESP_LOGE(TAG, "Failed to create queues");
			goto cleanup;
		}
	}
	
	// Initialize semaphores
	sem_to_slave_queue = H_PLATFORM_SEM_CREATE(MAX_PRIORITY_QUEUES, 0);
	sem_from_slave_queue = H_PLATFORM_SEM_CREATE(MAX_PRIORITY_QUEUES, 0);
	
	if (!sem_to_slave_queue || !sem_from_slave_queue) {
		ESP_LOGE(TAG, "Failed to create semaphores");
		goto cleanup;
	}
	
	// Initialize USB host or device mode
	if (usb_context->is_host_mode) {
		ESP_LOGI(TAG, "Initializing USB Host mode");
		// USB Host initialization will be implemented in Phase 3
	} else if (usb_context->is_device_mode) {
		ESP_LOGI(TAG, "Initializing USB Device mode");
		// USB Device initialization will be implemented in Phase 3
	}
	
	// Create tasks
	h_usb_read_task_info = H_PLATFORM_TASK_CREATE(h_usb_read_task, "h_usb_read", 
		H_ESP_HOSTED_DFLT_TASK_STACK, NULL, DFLT_TASK_PRIO, NULL);
	h_usb_write_task_info = H_PLATFORM_TASK_CREATE(h_usb_write_task, "h_usb_write",
		H_ESP_HOSTED_DFLT_TASK_STACK, NULL, DFLT_TASK_PRIO, NULL);
	h_usb_process_rx_task_info = H_PLATFORM_TASK_CREATE(h_usb_process_rx_task, 
		"h_usb_process_rx", H_ESP_HOSTED_DFLT_TASK_STACK, NULL, DFLT_TASK_PRIO, NULL);
	
	if (!h_usb_read_task_info || !h_usb_write_task_info || !h_usb_process_rx_task_info) {
		ESP_LOGE(TAG, "Failed to create tasks");
		goto cleanup;
	}
	
	ESP_LOGI(TAG, "USB transport initialized successfully");
	return usb_context;
	
cleanup:
	bus_deinit_internal(usb_context);
	return NULL;
}

void bus_deinit_internal(void *bus_handle)
{
	ESP_LOGI(TAG, "Deinitializing USB transport");
	
	if (h_usb_read_task_info) {
		H_PLATFORM_TASK_DELETE(h_usb_read_task_info);
		h_usb_read_task_info = NULL;
	}
	
	if (h_usb_write_task_info) {
		H_PLATFORM_TASK_DELETE(h_usb_write_task_info);
		h_usb_write_task_info = NULL;
	}
	
	if (h_usb_process_rx_task_info) {
		H_PLATFORM_TASK_DELETE(h_usb_process_rx_task_info);
		h_usb_process_rx_task_info = NULL;
	}
	
	// Cleanup queues
	for (int i = 0; i < MAX_PRIORITY_QUEUES; i++) {
		if (to_slave_queue[i]) {
			H_PLATFORM_Q_DELETE(to_slave_queue[i]);
			to_slave_queue[i] = NULL;
		}
		if (from_slave_queue[i]) {
			H_PLATFORM_Q_DELETE(from_slave_queue[i]);
			from_slave_queue[i] = NULL;
		}
	}
	
	// Cleanup semaphores
	if (sem_to_slave_queue) {
		H_PLATFORM_SEM_DELETE(sem_to_slave_queue);
		sem_to_slave_queue = NULL;
	}
	
	if (sem_from_slave_queue) {
		H_PLATFORM_SEM_DELETE(sem_from_slave_queue);
		sem_from_slave_queue = NULL;
	}
	
	// Cleanup USB context
	if (usb_context) {
		H_FREE(usb_context);
		usb_context = NULL;
	}
	
	ESP_LOGI(TAG, "USB transport deinitialized");
}

int esp_hosted_tx(uint8_t iface_type, uint8_t iface_num,
		uint8_t *payload_buf, uint16_t payload_len, uint8_t buff_zerocopy,
		uint8_t *buffer_to_free, void (*free_buf_func)(void *ptr), uint8_t flags)
{
	ESP_LOGV(TAG, "USB TX: iface_type=%u, iface_num=%u, len=%u", 
		iface_type, iface_num, payload_len);
		
	interface_buffer_handle_t buf_handle = {0};
	uint8_t prio = get_priority_from_flag(flags);
	
	if (!usb_context) {
		ESP_LOGE(TAG, "USB transport not initialized");
		return ESP_FAIL;
	}
	
	buf_handle.if_type = iface_type;
	buf_handle.if_num = iface_num;
	buf_handle.payload_len = payload_len;
	buf_handle.payload = payload_buf;
	buf_handle.priv_buffer_handle = payload_buf;
	buf_handle.free_buf_handle = buffer_to_free;
	buf_handle.func = free_buf_func;
	
	if (to_slave_queue[prio] && 
		H_PLATFORM_Q_SEND(to_slave_queue[prio], &buf_handle, 0) == pdTRUE) {
		
		if (sem_to_slave_queue) {
			H_PLATFORM_SEM_POST(sem_to_slave_queue);
		}
		
		if (!usb_start_write_thread) {
			usb_start_write_thread = true;
		}
		
		return ESP_OK;
	}
	
	ESP_LOGE(TAG, "Failed to queue USB TX packet");
	return ESP_FAIL;
}

int ensure_slave_bus_ready(void *bus_handle)
{
	// USB devices are typically ready immediately after enumeration
	// This is a placeholder implementation
	ESP_LOGD(TAG, "Checking USB slave readiness");
	return ESP_OK;
}

static void h_usb_write_task(void const* pvParameters)
{
	ESP_LOGI(TAG, "Starting USB write task");
	
	interface_buffer_handle_t buf_handle = {0};
	
	while(1) {
		if (usb_start_write_thread && sem_to_slave_queue &&
			H_PLATFORM_SEM_WAIT(sem_to_slave_queue, portMAX_DELAY)) {
			
			for (int prio = 0; prio < MAX_PRIORITY_QUEUES; prio++) {
				if (to_slave_queue[prio] &&
					H_PLATFORM_Q_RECEIVE(to_slave_queue[prio], &buf_handle, 0)) {
					
					ESP_LOGV(TAG, "USB TX: prio=%u, len=%u", prio, buf_handle.payload_len);
					
					// Actual USB transmission will be implemented in Phase 3
					// For now, just acknowledge the packet
					if (buf_handle.func && buf_handle.free_buf_handle) {
						buf_handle.func(buf_handle.free_buf_handle);
					}
				}
			}
		}
	}
}

static void h_usb_read_task(void const* pvParameters)
{
	ESP_LOGI(TAG, "Starting USB read task");
	
	while(1) {
		// USB read implementation will be added in Phase 3
		// For now, just wait
		H_PLATFORM_DELAY_MS(1000);
	}
}