// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2025 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __USB_DRV_H
#define __USB_DRV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "transport_drv.h"
#include "os_wrapper.h"

#define TO_SLAVE_QUEUE_SIZE               20
#define FROM_SLAVE_QUEUE_SIZE             20

#define USB_EP_DATA_IN                    0x81
#define USB_EP_DATA_OUT                   0x01
#define USB_EP_CTRL                       0x00

#define USB_MAX_PACKET_SIZE               512
#define USB_TIMEOUT_MS                    1000

typedef struct {
    bool is_host_mode;
    bool is_device_mode;
    bool cdc_acm_enabled;
    uint16_t bulk_endpoint_size;
    uint16_t vendor_id;
    uint16_t product_id;
    void *usb_handle;
    void *cdc_handle;
} usb_context_t;

#ifdef __cplusplus
}
#endif

#endif