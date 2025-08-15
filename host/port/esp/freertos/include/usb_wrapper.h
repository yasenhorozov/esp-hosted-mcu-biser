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

/* Wrapper interfaces for USB to communicate with slave using USB */

#ifndef __USB_WRAPPER_H_
#define __USB_WRAPPER_H_

/* Hosted init function to init the USB interface
 * returns a pointer to the USB context */
void * hosted_usb_init(void);

/* Hosted USB deinit function
 * expects a pointer to the USB context */
esp_err_t hosted_usb_deinit(void *ctx);

/* Hosted USB functions to read / write
 * Returns -1 (error) or number of bytes read / written */
int hosted_usb_read(void *ctx, uint8_t *data, uint16_t size);
int hosted_usb_write(void *ctx, uint8_t *data, uint16_t size);

/* Hosted USB function to wait until there is Rx data
 * Returns -1 (error) or number of bytes to read */
int hosted_wait_usb_rx_data(uint32_t ticks_to_wait);

/* USB Host mode specific functions */
int hosted_usb_host_check_device(void *ctx);
int hosted_usb_host_open_device(void *ctx, uint16_t vid, uint16_t pid);
int hosted_usb_host_close_device(void *ctx);

/* USB Device mode specific functions */
int hosted_usb_device_connect(void *ctx);
int hosted_usb_device_disconnect(void *ctx);

#endif