# ESP-Hosted USB Transport Test Example

This example demonstrates how to use the ESP-Hosted USB transport layer for communication between host and slave ESP devices.

## Overview

The ESP-Hosted USB transport provides high-speed communication between ESP devices using USB CDC-ACM protocol. This example shows basic WiFi functionality over USB transport.

## Supported Transport Modes

### USB Host Mode
- ESP32 acts as USB host and connects to ESP slave device via USB cable
- Uses ESP-IDF USB Host CDC-ACM driver
- Suitable for applications where the ESP32 needs to connect to another ESP device over USB

### USB Device Mode  
- ESP32 acts as USB device and connects to host computer via USB cable
- Uses TinyUSB CDC-ACM device implementation
- Suitable for applications where ESP32 provides connectivity to a host computer

## Hardware Setup

### For USB Host Mode:
1. Connect USB cable between host ESP32 (this device) and slave ESP device
2. Ensure slave ESP device is configured with USB Device mode
3. Power both devices appropriately

### For USB Device Mode:
1. Connect USB cable between ESP32 (this device) and host computer
2. Ensure host computer has appropriate ESP-Hosted drivers
3. ESP32 will appear as USB CDC-ACM device on host computer

## Configuration

The example uses `sdkconfig.defaults` to configure USB transport. Key configurations:

- `CONFIG_ESP_HOSTED_USB_HOST_INTERFACE=y` - Enable USB transport
- `CONFIG_ESP_HOSTED_USB_HOST_MODE=y` - Configure as USB host (change to device mode if needed)
- `CONFIG_ESP_HOSTED_USB_BULK_ENDPOINT_SIZE=512` - USB bulk endpoint size
- `CONFIG_ESP_HOSTED_USB_CDC_ACM_CLASS=y` - Use CDC-ACM USB class

## Building and Running

1. Configure the project:
   ```bash
   idf.py set-target esp32s3  # USB requires ESP32-S2, S3, or C3
   idf.py menuconfig
   ```

2. Configure WiFi credentials in menuconfig if needed

3. Build and flash:
   ```bash
   idf.py build
   idf.py flash monitor
   ```

## Usage

Once the example is running:

1. The device will attempt to initialize USB transport
2. WiFi will be initialized and attempt to connect to configured AP
3. Console interface will be available for testing
4. Monitor logs to see transport status and connectivity information

## Console Commands

The example includes standard ESP console commands for system monitoring and WiFi control.

## USB Transport Benefits

- **High Speed**: USB provides higher throughput than UART
- **Reliable**: Built-in error detection and flow control
- **Standard Protocol**: Uses standard CDC-ACM USB class
- **Hot Plug**: Supports USB device connection/disconnection detection

## Supported ESP32 Variants

USB transport is supported on:
- ESP32-S2
- ESP32-S3  
- ESP32-C3
- ESP32-C6 (and newer variants with USB support)

Note: Original ESP32 does not have native USB support and cannot use this transport.

## Troubleshooting

1. **USB enumeration fails**: Check USB cable and connections
2. **Device not detected**: Verify USB drivers on host side
3. **Communication timeout**: Check USB configuration and clock settings
4. **Build errors**: Ensure target ESP variant supports USB

## Related Examples

- Basic WiFi examples for transport comparison
- USB host/device examples in ESP-IDF for USB protocol details
- Other ESP-Hosted transport examples (SPI, SDIO, UART)