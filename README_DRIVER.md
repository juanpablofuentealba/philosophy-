# SmartlinkTechnology M01 Linux Kernel Driver

A complete Linux kernel driver for the SmartlinkTechnology M01 device.

## Features

- **Character Device Interface**: Standard `/dev/m01` device node
- **USB Support**: Ready for USB-based M01 devices (VID/PID configurable)
- **Platform Device Support**: For non-USB variants (SPI, I2C, UART)
- **IOCTL Commands**: Device control and status queries
- **Thread-Safe Operations**: Mutex and spinlock protection
- **Asynchronous I/O**: Poll/select support
- **Configurable Parameters**: Debug level, max devices

## Files

- `smartlink_m01.c` - Main driver source code
- `Makefile` - Kernel module build system
- `README.md` - This documentation

## Building

### Prerequisites

```bash
# Install kernel headers (Debian/Ubuntu)
sudo apt-get install linux-headers-$(uname -r) build-essential

# Install kernel headers (RHEL/CentOS/Fedora)
sudo dnf install kernel-devel kernel-headers
```

### Build Commands

```bash
# Build the module
make

# Build with debug enabled
make debug

# Clean build artifacts
make clean
```

## Installation

```bash
# Install the module
sudo make install

# Load the module
sudo make load

# Or manually
sudo insmod smartlink_m01.ko debug=1
```

## Usage

### Device Node

The driver creates a character device at `/dev/m01`.

### Basic Operations

```bash
# Read from device
cat /dev/m01

# Write to device
echo "data" > /dev/m01

# Check device status using ioctl (requires user-space tool)
./m01_tool --status
```

### Module Parameters

```bash
# Load with debug level 2 (maximum verbosity)
sudo insmod smartlink_m01.ko debug=2

# Load with custom max devices
sudo insmod smartlink_m01.ko max_devices=2
```

## IOCTL Interface

The driver supports the following IOCTL commands:

| Command | Description |
|---------|-------------|
| `M01_IOC_RESET` | Reset the device |
| `M01_IOC_GET_STATUS` | Get device status |
| `M01_IOC_SET_CONFIG` | Set device configuration |
| `M01_IOC_GET_INFO` | Get device information |
| `M01_IOC_FLUSH` | Flush read/write buffers |

### Status Structure

```c
struct m01_status {
    __u32 connected;
    __u32 ready;
    __u32 error_code;
    __u32 bytes_available;
    __u32 tx_pending;
    __u8 firmware_version[16];
    __u8 hardware_version[16];
    __u8 serial_number[32];
};
```

### Configuration Structure

```c
struct m01_config {
    __u32 baud_rate;
    __u32 data_bits;
    __u32 stop_bits;
    __u32 parity;
    __u32 flow_control;
    __u32 timeout_ms;
};
```

## User-Space Example

```c
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>

#define M01_IOC_MAGIC 0xA0
#define M01_IOC_GET_STATUS _IOR(M01_IOC_MAGIC, 1, struct m01_status)

struct m01_status {
    __u32 connected;
    __u32 ready;
    __u32 error_code;
    __u32 bytes_available;
    __u32 tx_pending;
    __u8 firmware_version[16];
    __u8 hardware_version[16];
    __u8 serial_number[32];
};

int main() {
    int fd = open("/dev/m01", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    struct m01_status status;
    if (ioctl(fd, M01_IOC_GET_STATUS, &status) == 0) {
        printf("Connected: %u\n", status.connected);
        printf("Ready: %u\n", status.ready);
        printf("Error Code: %u\n", status.error_code);
        printf("Bytes Available: %u\n", status.bytes_available);
    }
    
    close(fd);
    return 0;
}
```

## USB Configuration

To enable USB support:

1. Uncomment the USB code sections in `smartlink_m01.c`
2. Update the VID/PID in `m01_usb_ids[]` with actual values:
   ```c
   static const struct usb_device_id m01_usb_ids[] = {
       { USB_DEVICE(0xXXXX, 0xYYYY) }, /* Replace with actual VID/PID */
       { }
   };
   ```
3. Rebuild the module

## Platform Device Registration

For platform devices, register via device tree or board file:

```c
// Example platform device registration
static struct platform_device m01_platform_device = {
    .name = "smartlink-m01",
    .id = 0,
};

platform_device_register(&m01_platform_device);
```

## Debugging

```bash
# View kernel messages
dmesg | grep smartlink

# Load with maximum debug output
sudo insmod smartlink_m01.ko debug=2

# Monitor in real-time
dmesg -w | grep smartlink
```

## Troubleshooting

### Module fails to load
- Check kernel version matches headers: `uname -r` vs installed headers
- Verify module signature if secure boot is enabled
- Check `dmesg` for specific error messages

### Device node not created
- Check if module loaded successfully: `lsmod | grep smartlink`
- Verify major number assigned: `cat /proc/devices | grep smartlink`
- Check permissions: `ls -l /dev/m01`

### No data transfer
- Verify device is connected and powered
- Check configuration settings (baud rate, etc.)
- Enable debug mode and check `dmesg` output

## License

This driver is licensed under the GNU General Public License version 2 (GPL v2).

## Author

SmartlinkTechnology

## Version

1.0.0
