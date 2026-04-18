/*
 * SmartlinkTechnology M01 Linux Kernel Driver
 * 
 * Copyright (C) 2024 SmartlinkTechnology
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/kfifo.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/version.h>

#define DRIVER_NAME "smartlink_m01"
#define DEVICE_NAME "m01"
#define CLASS_NAME "smartlink"

#define M01_MAJOR 0
#define M01_MINOR_COUNT 1

#define M01_BUFFER_SIZE 4096
#define M01_MAX_DEVICES 4

/* IOCTL commands */
#define M01_IOC_MAGIC 0xA0

#define M01_IOC_RESET _IO(M01_IOC_MAGIC, 0)
#define M01_IOC_GET_STATUS _IOR(M01_IOC_MAGIC, 1, struct m01_status)
#define M01_IOC_SET_CONFIG _IOW(M01_IOC_MAGIC, 2, struct m01_config)
#define M01_IOC_GET_INFO _IOR(M01_IOC_MAGIC, 3, struct m01_info)
#define M01_IOC_FLUSH _IO(M01_IOC_MAGIC, 4)

#define M01_IOC_MAXNR 4

/* Device status structure */
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

/* Device configuration structure */
struct m01_config {
    __u32 baud_rate;
    __u32 data_bits;
    __u32 stop_bits;
    __u32 parity;
    __u32 flow_control;
    __u32 timeout_ms;
};

/* Device information structure */
struct m01_info {
    __u8 vendor_id[32];
    __u8 product_id[32];
    __u8 manufacturer[64];
    __u8 product_name[64];
    __u32 max_packet_size;
    __u32 protocol_version;
};

/* Error codes */
#define M01_ERR_NONE        0
#define M01_ERR_TIMEOUT     1
#define M01_ERR_COMM        2
#define M01_ERR_INVALID     3
#define M01_ERR_BUSY        4
#define M01_ERR_NOMEM       5
#define M01_ERR_NODEV       6

/* Default configuration */
#define M01_DEFAULT_BAUD_RATE   115200
#define M01_DEFAULT_DATA_BITS   8
#define M01_DEFAULT_STOP_BITS   1
#define M01_DEFAULT_PARITY      0
#define M01_DEFAULT_FLOW_CTRL   0
#define M01_DEFAULT_TIMEOUT     1000

/* Device private data */
struct m01_device {
    struct cdev cdev;
    struct class *dev_class;
    struct device *device;
    dev_t dev_num;
    
    struct mutex lock;
    spinlock_t spinlock;
    
    struct kfifo read_fifo;
    struct kfifo write_fifo;
    
    wait_queue_head_t read_wait;
    wait_queue_head_t write_wait;
    
    struct m01_status status;
    struct m01_config config;
    
    bool initialized;
    bool opened;
    atomic_t ref_count;
    
    /* USB specific (if USB device) */
    struct usb_device *udev;
    struct usb_interface *interface;
    struct usb_endpoint_descriptor *ep_in;
    struct usb_endpoint_descriptor *ep_out;
    
    /* Work queue for deferred processing */
    struct workqueue_struct *workqueue;
    struct work_struct work;
    
    /* Timer for timeouts */
    struct timer_list timer;
};

/* Global device array */
static struct m01_device *m01_devices[M01_MAX_DEVICES];
static int m01_major;
static struct class *m01_class;

/* Module parameters */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=info, 2=debug)");

static int max_devices = M01_MAX_DEVICES;
module_param(max_devices, int, 0644);
MODULE_PARM_DESC(max_devices, "Maximum number of M01 devices");

/* Logging macros */
#define m01_dbg(dev, fmt, ...) \
    do { if (debug >= 2) dev_dbg((dev)->device, fmt, ##__VA_ARGS__); } while (0)

#define m01_info(dev, fmt, ...) \
    do { if (debug >= 1) dev_info((dev)->device, fmt, ##__VA_ARGS__); } while (0)

#define m01_err(dev, fmt, ...) \
    dev_err((dev)->device, fmt, ##__VA_ARGS__)

/* Forward declarations */
static int m01_open(struct inode *inode, struct file *file);
static int m01_release(struct inode *inode, struct file *file);
static ssize_t m01_read(struct file *file, char __user *buf, size_t count, loff_t *ppos);
static ssize_t m01_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos);
static __poll_t m01_poll(struct file *file, poll_table *wait);
static long m01_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int m01_mmap(struct file *file, struct vm_area_struct *vma);

/* File operations */
static const struct file_operations m01_fops = {
    .owner = THIS_MODULE,
    .open = m01_open,
    .release = m01_release,
    .read = m01_read,
    .write = m01_write,
    .poll = m01_poll,
    .unlocked_ioctl = m01_ioctl,
    .mmap = m01_mmap,
    .llseek = no_llseek,
};

/* Initialize device structure */
static int m01_init_device(struct m01_device *m01_dev)
{
    int ret;
    
    memset(m01_dev, 0, sizeof(*m01_dev));
    
    mutex_init(&m01_dev->lock);
    spin_lock_init(&m01_dev->spinlock);
    
    ret = kfifo_alloc(&m01_dev->read_fifo, M01_BUFFER_SIZE, GFP_KERNEL);
    if (ret) {
        m01_err(m01_dev, "Failed to allocate read FIFO\n");
        goto err_read_fifo;
    }
    
    ret = kfifo_alloc(&m01_dev->write_fifo, M01_BUFFER_SIZE, GFP_KERNEL);
    if (ret) {
        m01_err(m01_dev, "Failed to allocate write FIFO\n");
        goto err_write_fifo;
    }
    
    init_waitqueue_head(&m01_dev->read_wait);
    init_waitqueue_head(&m01_dev->write_wait);
    
    /* Set default configuration */
    m01_dev->config.baud_rate = M01_DEFAULT_BAUD_RATE;
    m01_dev->config.data_bits = M01_DEFAULT_DATA_BITS;
    m01_dev->config.stop_bits = M01_DEFAULT_STOP_BITS;
    m01_dev->config.parity = M01_DEFAULT_PARITY;
    m01_dev->config.flow_control = M01_DEFAULT_FLOW_CTRL;
    m01_dev->config.timeout_ms = M01_DEFAULT_TIMEOUT;
    
    /* Initialize status */
    m01_dev->status.connected = 0;
    m01_dev->status.ready = 0;
    m01_dev->status.error_code = M01_ERR_NONE;
    m01_dev->status.bytes_available = 0;
    m01_dev->status.tx_pending = 0;
    
    m01_dev->initialized = false;
    m01_dev->opened = false;
    atomic_set(&m01_dev->ref_count, 0);
    
    /* Create workqueue */
    m01_dev->workqueue = create_singlethread_workqueue("m01_workqueue");
    if (!m01_dev->workqueue) {
        m01_err(m01_dev, "Failed to create workqueue\n");
        ret = -ENOMEM;
        goto err_workqueue;
    }
    
    /* Initialize timer */
    timer_setup(&m01_dev->timer, NULL, 0);
    
    return 0;

err_workqueue:
    kfifo_free(&m01_dev->write_fifo);
err_write_fifo:
    kfifo_free(&m01_dev->read_fifo);
err_read_fifo:
    mutex_destroy(&m01_dev->lock);
    return ret;
}

/* Cleanup device structure */
static void m01_cleanup_device(struct m01_device *m01_dev)
{
    if (!m01_dev)
        return;
    
    del_timer_sync(&m01_dev->timer);
    
    if (m01_dev->workqueue) {
        flush_workqueue(m01_dev->workqueue);
        destroy_workqueue(m01_dev->workqueue);
    }
    
    kfifo_free(&m01_dev->read_fifo);
    kfifo_free(&m01_dev->write_fifo);
    
    mutex_destroy(&m01_dev->lock);
}

/* Device open */
static int m01_open(struct inode *inode, struct file *file)
{
    struct m01_device *m01_dev;
    int ret = 0;
    
    m01_dev = container_of(inode->i_cdev, struct m01_device, cdev);
    
    if (mutex_lock_interruptible(&m01_dev->lock))
        return -ERESTARTSYS;
    
    if (m01_dev->opened) {
        ret = -EBUSY;
        goto out_unlock;
    }
    
    if (!m01_dev->initialized) {
        ret = -ENODEV;
        goto out_unlock;
    }
    
    m01_dev->opened = true;
    atomic_inc(&m01_dev->ref_count);
    
    file->private_data = m01_dev;
    
    m01_info(m01_dev, "Device opened\n");

out_unlock:
    mutex_unlock(&m01_dev->lock);
    return ret;
}

/* Device release */
static int m01_release(struct inode *inode, struct file *file)
{
    struct m01_device *m01_dev = file->private_data;
    
    if (!m01_dev)
        return -ENODEV;
    
    mutex_lock(&m01_dev->lock);
    
    m01_dev->opened = false;
    atomic_dec(&m01_dev->ref_count);
    
    /* Wake up any waiting readers/writers */
    wake_up_interruptible(&m01_dev->read_wait);
    wake_up_interruptible(&m01_dev->write_wait);
    
    m01_info(m01_dev, "Device closed\n");
    
    mutex_unlock(&m01_dev->lock);
    
    return 0;
}

/* Device read */
static ssize_t m01_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct m01_device *m01_dev = file->private_data;
    size_t bytes_read;
    int ret;
    
    if (!m01_dev)
        return -ENODEV;
    
    if (count == 0)
        return 0;
    
    /* Wait for data if none available */
    if (kfifo_is_empty(&m01_dev->read_fifo)) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        
        ret = wait_event_interruptible(m01_dev->read_wait,
                                       !kfifo_is_empty(&m01_dev->read_fifo));
        if (ret)
            return ret;
    }
    
    /* Read from FIFO */
    bytes_read = kfifo_to_user(&m01_dev->read_fifo, buf, count, &ret);
    if (ret)
        return ret;
    
    /* Update status */
    spin_lock(&m01_dev->spinlock);
    m01_dev->status.bytes_available = kfifo_len(&m01_dev->read_fifo);
    spin_unlock(&m01_dev->spinlock);
    
    m01_dbg(m01_dev, "Read %zu bytes\n", bytes_read);
    
    return bytes_read;
}

/* Device write */
static ssize_t m01_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    struct m01_device *m01_dev = file->private_data;
    size_t bytes_written;
    int ret;
    
    if (!m01_dev)
        return -ENODEV;
    
    if (count == 0)
        return 0;
    
    /* Check if there's space in the FIFO */
    if (kfifo_is_full(&m01_dev->write_fifo)) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        
        ret = wait_event_interruptible(m01_dev->write_wait,
                                       !kfifo_is_full(&m01_dev->write_fifo));
        if (ret)
            return ret;
    }
    
    /* Write to FIFO */
    bytes_written = kfifo_from_user(&m01_dev->write_fifo, buf, count, &ret);
    if (ret)
        return ret;
    
    /* Update status */
    spin_lock(&m01_dev->spinlock);
    m01_dev->status.tx_pending = kfifo_len(&m01_dev->write_fifo);
    spin_unlock(&m01_dev->spinlock);
    
    /* Schedule work to process the data */
    queue_work(m01_dev->workqueue, &m01_dev->work);
    
    m01_dbg(m01_dev, "Wrote %zu bytes\n", bytes_written);
    
    return bytes_written;
}

/* Device poll */
static __poll_t m01_poll(struct file *file, poll_table *wait)
{
    struct m01_device *m01_dev = file->private_data;
    __poll_t mask = 0;
    
    if (!m01_dev)
        return EPOLLERR;
    
    poll_wait(file, &m01_dev->read_wait, wait);
    poll_wait(file, &m01_dev->write_wait, wait);
    
    if (!kfifo_is_empty(&m01_dev->read_fifo))
        mask |= EPOLLIN | EPOLLRDNORM;
    
    if (!kfifo_is_full(&m01_dev->write_fifo))
        mask |= EPOLLOUT | EPOLLWRNORM;
    
    return mask;
}

/* Device ioctl */
static long m01_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct m01_device *m01_dev = file->private_data;
    void __user *argp = (void __user *)arg;
    int ret = 0;
    
    if (!m01_dev)
        return -ENODEV;
    
    if (_IOC_TYPE(cmd) != M01_IOC_MAGIC)
        return -EINVAL;
    
    if (_IOC_NR(cmd) > M01_IOC_MAXNR)
        return -EINVAL;
    
    switch (cmd) {
    case M01_IOC_RESET:
        m01_info(m01_dev, "Reset requested\n");
        /* Perform device reset */
        spin_lock(&m01_dev->spinlock);
        kfifo_reset(&m01_dev->read_fifo);
        kfifo_reset(&m01_dev->write_fifo);
        m01_dev->status.bytes_available = 0;
        m01_dev->status.tx_pending = 0;
        m01_dev->status.error_code = M01_ERR_NONE;
        spin_unlock(&m01_dev->spinlock);
        
        wake_up_interruptible(&m01_dev->read_wait);
        wake_up_interruptible(&m01_dev->write_wait);
        break;
        
    case M01_IOC_GET_STATUS:
        if (copy_to_user(argp, &m01_dev->status, sizeof(m01_dev->status)))
            return -EFAULT;
        m01_dbg(m01_dev, "Status returned\n");
        break;
        
    case M01_IOC_SET_CONFIG:
        if (copy_from_user(&m01_dev->config, argp, sizeof(m01_dev->config)))
            return -EFAULT;
        m01_info(m01_dev, "Configuration updated: baud=%u\n", 
                 m01_dev->config.baud_rate);
        /* Apply new configuration to hardware here */
        break;
        
    case M01_IOC_GET_INFO:
        {
            struct m01_info info;
            memset(&info, 0, sizeof(info));
            strncpy(info.vendor_id, "SmartlinkTechnology", sizeof(info.vendor_id) - 1);
            strncpy(info.product_id, "M01", sizeof(info.product_id) - 1);
            strncpy(info.manufacturer, "SmartlinkTechnology", sizeof(info.manufacturer) - 1);
            strncpy(info.product_name, "M01 Device", sizeof(info.product_name) - 1);
            info.max_packet_size = 64;
            info.protocol_version = 1;
            
            if (copy_to_user(argp, &info, sizeof(info)))
                return -EFAULT;
            m01_dbg(m01_dev, "Info returned\n");
        }
        break;
        
    case M01_IOC_FLUSH:
        m01_info(m01_dev, "Flush requested\n");
        spin_lock(&m01_dev->spinlock);
        kfifo_reset(&m01_dev->read_fifo);
        kfifo_reset(&m01_dev->write_fifo);
        m01_dev->status.bytes_available = 0;
        m01_dev->status.tx_pending = 0;
        spin_unlock(&m01_dev->spinlock);
        
        wake_up_interruptible(&m01_dev->read_wait);
        wake_up_interruptible(&m01_dev->write_wait);
        break;
        
    default:
        ret = -EINVAL;
        break;
    }
    
    return ret;
}

/* Device mmap */
static int m01_mmap(struct file *file, struct vm_area_struct *vma)
{
    /* Currently not supported */
    return -ENODEV;
}

/* USB probe function (if USB device) */
#if 1
static int m01_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    struct m01_device *m01_dev;
    int ret;
    int dev_idx;
    int i;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    
    /* Find available device slot */
    for (dev_idx = 0; dev_idx < M01_MAX_DEVICES; dev_idx++) {
        if (!m01_devices[dev_idx])
            break;
    }
    
    if (dev_idx >= M01_MAX_DEVICES) {
        dev_err(&intf->dev, "Maximum number of devices reached\n");
        return -ENODEV;
    }
    
    m01_dev = kzalloc(sizeof(*m01_dev), GFP_KERNEL);
    if (!m01_dev)
        return -ENOMEM;
    
    ret = m01_init_device(m01_dev);
    if (ret)
        goto err_init;
    
    m01_dev->udev = interface_to_usbdev(intf);
    m01_dev->interface = intf;
    
    /* Get endpoint descriptors */
    iface_desc = intf->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
        endpoint = &iface_desc->endpoint[i].desc;
        
        /* Check for BULK IN endpoint (0x81) */
        if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN &&
            (endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
            if ((endpoint->bEndpointAddress & 0x7F) == 0x01 || 
                m01_dev->ep_in == NULL) {
                m01_dev->ep_in = endpoint;
                m01_dbg(m01_dev, "Found BULK IN endpoint: 0x%02X\n", endpoint->bEndpointAddress);
            }
        }
        
        /* Check for BULK OUT endpoint (0x01) */
        if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT &&
            (endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
            if ((endpoint->bEndpointAddress & 0x7F) == 0x01 ||
                m01_dev->ep_out == NULL) {
                m01_dev->ep_out = endpoint;
                m01_dbg(m01_dev, "Found BULK OUT endpoint: 0x%02X\n", endpoint->bEndpointAddress);
            }
        }
    }
    
    /* Validate endpoints */
    if (!m01_dev->ep_in || !m01_dev->ep_out) {
        dev_err(&intf->dev, "Could not find required endpoints\n");
        ret = -ENODEV;
        goto err_endpoints;
    }
    
    /* Set max packet size from endpoint descriptors */
    m01_info(m01_dev, "Endpoint sizes - IN: %d, OUT: %d\n",
             le16_to_cpu(m01_dev->ep_in->wMaxPacketSize),
             le16_to_cpu(m01_dev->ep_out->wMaxPacketSize));
    
    /* Register character device */
    ret = cdev_add(&m01_dev->cdev, MKDEV(m01_major, dev_idx), 1);
    if (ret)
        goto err_cdev;
    
    /* Create device in /sys/class */
    m01_dev->device = device_create(m01_class, NULL, MKDEV(m01_major, dev_idx),
                                    NULL, DEVICE_NAME "%d", dev_idx);
    if (IS_ERR(m01_dev->device)) {
        ret = PTR_ERR(m01_dev->device);
        goto err_device;
    }
    
    m01_dev->initialized = true;
    m01_devices[dev_idx] = m01_dev;
    
    dev_info(&intf->dev, "Smartlink M01 device detected\n");
    
    return 0;

err_device:
    cdev_del(&m01_dev->cdev);
err_cdev:
err_endpoints:
    m01_cleanup_device(m01_dev);
err_init:
    kfree(m01_dev);
    return ret;
}

/* USB disconnect function */
static void m01_usb_disconnect(struct usb_interface *intf)
{
    struct m01_device *m01_dev = NULL;
    int i;
    
    /* Find the device */
    for (i = 0; i < M01_MAX_DEVICES; i++) {
        if (m01_devices[i] && m01_devices[i]->interface == intf) {
            m01_dev = m01_devices[i];
            break;
        }
    }
    
    if (!m01_dev)
        return;
    
    m01_dev->initialized = false;
    
    device_destroy(m01_class, m01_dev->dev_num);
    cdev_del(&m01_dev->cdev);
    
    m01_cleanup_device(m01_dev);
    
    m01_devices[i] = NULL;
    kfree(m01_dev);
    
    dev_info(&intf->dev, "Smartlink M01 device disconnected\n");
}

/* USB device ID table */
static const struct usb_device_id m01_usb_ids[] = {
    { USB_DEVICE(0x301a, 0x159b) },  /* SmartlinkTechnology M01 */
    { }
};
MODULE_DEVICE_TABLE(usb, m01_usb_ids);

/* USB driver structure */
static struct usb_driver m01_usb_driver = {
    .name = DRIVER_NAME,
    .probe = m01_usb_probe,
    .disconnect = m01_usb_disconnect,
    .id_table = m01_usb_ids,
};
#endif

/* Platform device support (for non-USB variants) */
static int m01_platform_probe(struct platform_device *pdev)
{
    struct m01_device *m01_dev;
    int ret;
    
    m01_dev = kzalloc(sizeof(*m01_dev), GFP_KERNEL);
    if (!m01_dev)
        return -ENOMEM;
    
    ret = m01_init_device(m01_dev);
    if (ret)
        goto err_init;
    
    /* Register character device */
    ret = cdev_add(&m01_dev->cdev, MKDEV(m01_major, 0), 1);
    if (ret)
        goto err_cdev;
    
    /* Create device in /sys/class */
    m01_dev->device = device_create(m01_class, &pdev->dev, MKDEV(m01_major, 0),
                                    NULL, DEVICE_NAME);
    if (IS_ERR(m01_dev->device)) {
        ret = PTR_ERR(m01_dev->device);
        goto err_device;
    }
    
    m01_dev->initialized = true;
    m01_devices[0] = m01_dev;
    
    dev_info(&pdev->dev, "Smartlink M01 platform device registered\n");
    
    return 0;

err_device:
    cdev_del(&m01_dev->cdev);
err_cdev:
    m01_cleanup_device(m01_dev);
err_init:
    kfree(m01_dev);
    return ret;
}

static int m01_platform_remove(struct platform_device *pdev)
{
    struct m01_device *m01_dev = m01_devices[0];
    
    if (!m01_dev)
        return 0;
    
    m01_dev->initialized = false;
    
    device_destroy(m01_class, m01_dev->dev_num);
    cdev_del(&m01_dev->cdev);
    
    m01_cleanup_device(m01_dev);
    
    m01_devices[0] = NULL;
    kfree(m01_dev);
    
    dev_info(&pdev->dev, "Smartlink M01 platform device removed\n");
    
    return 0;
}

static const struct platform_device_id m01_platform_ids[] = {
    { .name = "smartlink-m01" },
    { }
};
MODULE_DEVICE_TABLE(platform, m01_platform_ids);

static struct platform_driver m01_platform_driver = {
    .driver = {
        .name = DRIVER_NAME,
    },
    .probe = m01_platform_probe,
    .remove = m01_platform_remove,
    .id_table = m01_platform_ids,
};

/* Module initialization */
static int __init m01_init(void)
{
    dev_t dev;
    int ret;
    
    pr_info("SmartlinkTechnology M01 driver loading...\n");
    
    /* Allocate major number */
    ret = alloc_chrdev_region(&dev, M01_MINOR_COUNT, M01_MAX_DEVICES, DRIVER_NAME);
    if (ret < 0) {
        pr_err("Failed to allocate major number\n");
        return ret;
    }
    m01_major = MAJOR(dev);
    
    /* Create device class */
    m01_class = class_create(CLASS_NAME);
    if (IS_ERR(m01_class)) {
        ret = PTR_ERR(m01_class);
        goto err_class;
    }
    
    /* Register platform driver */
    ret = platform_driver_register(&m01_platform_driver);
    if (ret) {
        pr_err("Failed to register platform driver\n");
        goto err_platform;
    }
    
    /* Register USB driver */
    ret = usb_register(&m01_usb_driver);
    if (ret) {
        pr_err("Failed to register USB driver\n");
        goto err_usb;
    }
    
    pr_info("SmartlinkTechnology M01 driver loaded successfully (major %d)\n", m01_major);
    pr_info("Device node will be created at /dev/%s when device is detected\n", DEVICE_NAME);
    
    return 0;

err_usb:
    platform_driver_unregister(&m01_platform_driver);
err_platform:
    class_destroy(m01_class);
err_class:
    unregister_chrdev_region(MKDEV(m01_major, M01_MINOR_COUNT), M01_MAX_DEVICES);
    return ret;
}

/* Module cleanup */
static void __exit m01_exit(void)
{
    int i;
    
    /* Unregister USB driver */
    usb_deregister(&m01_usb_driver);
    
    /* Unregister platform driver */
    platform_driver_unregister(&m01_platform_driver);
    
    /* Clean up any remaining devices */
    for (i = 0; i < M01_MAX_DEVICES; i++) {
        if (m01_devices[i]) {
            m01_devices[i]->initialized = false;
            device_destroy(m01_class, m01_devices[i]->dev_num);
            cdev_del(&m01_devices[i]->cdev);
            m01_cleanup_device(m01_devices[i]);
            kfree(m01_devices[i]);
            m01_devices[i] = NULL;
        }
    }
    
    /* Destroy class */
    class_destroy(m01_class);
    
    /* Unregister character device region */
    unregister_chrdev_region(MKDEV(m01_major, M01_MINOR_COUNT), M01_MAX_DEVICES);
    
    pr_info("SmartlinkTechnology M01 driver unloaded\n");
}

module_init(m01_init);
module_exit(m01_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SmartlinkTechnology");
MODULE_DESCRIPTION("Linux kernel driver for SmartlinkTechnology M01 device");
MODULE_VERSION("1.0.0");
