/*
 * ============================================================================
 * usb_driver_complete.c - A complete, compile-ready Linux USB driver
 *                         demonstrating Bulk and Interrupt transfers
 *                         (Line-by-Line Annotated & Bug-Fixed Edition)
 * ============================================================================
 *
 * HIGH-LEVEL OVERVIEW:
 * This Linux kernel module demonstrates a production-patterned USB device
 * driver architecture that supports:
 * 1. Dynamic endpoint discovery (no hardcoded endpoint numbers):
 *    Iterates over the active USB interface's endpoint descriptors at probe
 *    time via usb_find_endpoint() (line 564: usb_find_endpoint).
 * 2. Bulk IN/OUT transfers for high-throughput, error-checked data:
 *    - Bulk IN runs continuously via asynchronous URB resubmission into a
 *      kernel FIFO (line 675: bulk_in_completion).
 *    - Bulk OUT transmits user-space writes on demand, serialized by a
 *      completion object so concurrent/back-to-back writes never corrupt the
 *      in-flight URB (line 1069: usb_write, line 793: bulk_out_completion).
 * 3. Interrupt IN transfers for low-latency periodic data (optional):
 *    Discovered dynamically and polled by the host controller at the endpoint's
 *    descriptor bInterval (line 826: int_in_completion).
 * 4. Proper spinlock & mutex usage:
 *    - spin_lock_irqsave() / spin_unlock_irqrestore() protect the kfifos
 *      between softirq/interrupt URB callbacks and process-context readers.
 *    - mutex_lock() / mutex_unlock() protect disconnect state transitions
 *      against concurrent process-context I/O.
 * 5. Kernel FIFO (struct kfifo) buffering:
 *    Decouples fast asynchronous USB packet arrivals in softirq context from
 *    slower synchronous user-space read() calls in process context.
 * 6. Completion synchronization:
 *    - bulk_in_ready / int_in_ready allow readers to sleep cleanly until data
 *      arrives or a timeout expires.
 *    - bulk_out_done prevents overlapping submissions of bulk_out_urb.
 * 7. Safe cleanup & lifetime management on disconnect:
 *    - Registers/deregisters a /dev/usb_complete%d node via usb_class_driver.
 *    - Uses struct kref reference counting (line 478: usb_delete) so unplugging
 *      the device while a user-space process still holds an open fd cannot
 *      cause a use-after-free.
 *    - Synchronously cancels all in-flight URBs with usb_kill_urb() and wakes
 *      all sleeping waiters with complete_all().
 *
 * BUILD & USAGE COMMANDS:
 * - Minimal Makefile in the same directory:
 *     obj-m += usb_driver_complete.o
 * - To compile against the running kernel headers:
 *     make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 * - To load the compiled kernel object (.ko):
 *     sudo insmod usb_driver_complete.ko
 * - To unload the module:
 *     sudo rmmod usb_driver_complete
 * - To monitor kernel logs (dev_info / dev_err) in real time:
 *     dmesg -w
 */

/* ============================================================================
 * SECTION 1: KERNEL HEADER INCLUDES
 * ============================================================================
 */

/*
 * <linux/module.h>:
 * Core header required by all loadable kernel modules (LKMs).
 * Provides:
 *   - MODULE_LICENSE(), MODULE_AUTHOR(), MODULE_DESCRIPTION(), MODULE_VERSION()
 *   - MODULE_DEVICE_TABLE() for exporting hotplug device-ID aliases
 *   - THIS_MODULE macro (pointer to this module's struct module)
 */
#include <linux/module.h>

/*
 * <linux/kernel.h>:
 * General kernel utility declarations and macros.
 * Provides:
 *   - printk() and log levels (KERN_INFO, KERN_ERR, etc.)
 *   - container_of() (used in line 478: usb_delete to recover struct usb_device_data)
 *   - min_t() (type-safe minimum macro used in line 916: usb_read)
 *   - Fixed-width kernel integer types (u8, u16, u32, etc. via <linux/types.h>)
 */
#include <linux/kernel.h>

/*
 * <linux/fs.h>:
 * Virtual File System (VFS) definitions required for character-device file
 * operations.
 * Provides:
 *   - struct file, struct inode, struct file_operations
 *   - iminor(const struct inode *) to extract the minor device number in
 *     line 1190: usb_open so we can locate the matching struct usb_interface
 */
#include <linux/fs.h>

/*
 * <linux/usb.h>:
 * Main Linux USB subsystem header.
 * Provides:
 *   - Core structs: struct usb_device, struct usb_interface, struct urb,
 *     struct usb_host_interface, struct usb_endpoint_descriptor,
 *     struct usb_device_id, struct usb_driver, struct usb_class_driver
 *   - Endpoint helpers: usb_endpoint_type(), usb_endpoint_dir_in(),
 *     usb_endpoint_maxp(), USB_ENDPOINT_XFER_BULK, USB_ENDPOINT_XFER_INT,
 *     USB_DIR_IN, USB_DIR_OUT
 *   - URB & DMA APIs: usb_alloc_urb(), usb_free_urb(), usb_fill_bulk_urb(),
 *     usb_fill_int_urb(), usb_submit_urb(), usb_kill_urb(),
 *     usb_alloc_coherent(), usb_free_coherent(), URB_NO_TRANSFER_DMA_MAP
 *   - Pipe builders: usb_rcvbulkpipe(), usb_sndbulkpipe(), usb_rcvintpipe()
 *   - Interface & class helpers: usb_set_intfdata(), usb_get_intfdata(),
 *     interface_to_usbdev(), usb_get_dev(), usb_put_dev(),
 *     usb_register_dev(), usb_deregister_dev(), usb_find_interface(),
 *     module_usb_driver()
 *   - Device logging helpers (via <linux/device.h>): dev_info(), dev_err()
 */
#include <linux/usb.h>

/*
 * <linux/spinlock.h>:
 * Spinlock concurrency primitives for mutual exclusion in atomic/interrupt
 * context where sleeping is strictly forbidden.
 * Provides:
 *   - spinlock_t type
 *   - spin_lock_init(), spin_lock_irqsave(), spin_unlock_irqrestore()
 */
#include <linux/spinlock.h>

/*
 * <linux/mutex.h>:
 * Sleeping mutual-exclusion lock primitives for process context.
 * Provides:
 *   - struct mutex
 *   - mutex_init(), mutex_lock(), mutex_unlock()
 * Used to synchronize process-context I/O (line 916: usb_read, line 1069: usb_write)
 * with device teardown in line 1619: usb_disconnect.
 */
#include <linux/mutex.h>

/*
 * <linux/kref.h>:
 * Atomic kernel reference counting primitive.
 * Provides:
 *   - struct kref
 *   - kref_init(), kref_get(), kref_put()
 * Ensures struct usb_device_data is freed in line 478: usb_delete only after BOTH
 * line 1619: usb_disconnect and all open file descriptors (line 1260: usb_release) are done.
 */
#include <linux/kref.h>

/*
 * <linux/kfifo.h>:
 * Generic kernel circular byte/element FIFO (ring buffer) implementation.
 * Provides:
 *   - struct kfifo
 *   - kfifo_alloc(), kfifo_free(), kfifo_in(), kfifo_out(), kfifo_is_empty()
 */
#include <linux/kfifo.h>

/*
 * <linux/completion.h>:
 * Event synchronization primitive allowing a task in process context to sleep
 * until another context signals an event.
 * Provides:
 *   - struct completion
 *   - init_completion(), reinit_completion(), complete(), complete_all(),
 *     wait_for_completion_interruptible_timeout()
 */
#include <linux/completion.h>

/*
 * <linux/slab.h>:
 * Kernel slab allocator APIs for dynamic heap memory allocation.
 * Provides:
 *   - kmalloc(), kzalloc(), kfree()
 *   - Allocation flags: GFP_KERNEL (may sleep), GFP_ATOMIC (cannot sleep)
 */
#include <linux/slab.h>

/*
 * <linux/jiffies.h>:
 * Kernel tick-counter definitions.
 * Provides:
 *   - HZ (number of timer ticks in 1 second, used as timeout in line 916: usb_read
 *     and line 1069: usb_write)
 */
#include <linux/jiffies.h>

/*
 * <linux/uaccess.h>:
 * Architecture-independent wrapper for user-space memory access helpers
 * (preferred over <asm/uaccess.h> in all modern Linux kernels).
 * Provides:
 *   - copy_to_user() and copy_from_user() for safe kernel <-> user transfers
 *     with address validation and page-fault handling
 *   - __user sparse address-space annotation
 */
#include <linux/uaccess.h>

/* ============================================================================
 * SECTION 2: MODULE METADATA DECLARATIONS
 * ============================================================================
 * These macros embed key=value strings into the ELF `.modinfo` section of the
 * compiled `.ko` file (inspectable via `modinfo usb_driver_complete.ko`).
 */

/*
 * MODULE_LICENSE("GPL"):
 * Declares this module as GNU General Public License compatible.
 * Required to link against EXPORT_SYMBOL_GPL USB core symbols without tainting
 * the kernel.
 */
MODULE_LICENSE("GPL");

/*
 * MODULE_AUTHOR("Your Name"):
 * Embeds the module author's name into `.modinfo`.
 */
MODULE_AUTHOR("Your Name");

/*
 * MODULE_DESCRIPTION(...):
 * Embeds a human-readable summary of the driver into `.modinfo`.
 */
MODULE_DESCRIPTION("Complete USB Driver with Bulk and Interrupt Transfer Support");

/*
 * MODULE_VERSION("1.0"):
 * Embeds a version string into `.modinfo` (/sys/module/<name>/version).
 */
MODULE_VERSION("1.0");

/* ============================================================================
 * SECTION 3: CONFIGURATION CONSTANTS (PREPROCESSOR MACROS)
 * ============================================================================
 */

/*
 * MAX_PACKET_SIZE:
 * Preprocessor constant expanding to 512 (standard High-Speed USB 2.0 Bulk
 * endpoint wMaxPacketSize). Used to size the FIFOs; actual URB DMA buffers
 * are sized dynamically from each endpoint's hardware descriptor.
 */
#define MAX_PACKET_SIZE 512

/*
 * FIFO_SIZE:
 * Preprocessor constant expanding to (512 * 4) = 2048 bytes.
 * Must be a power of 2 (2^11) because Linux kfifo wraps head/tail indices via
 * bitmasking (`& (size - 1)`), and kfifo_alloc() rounds up to a power of 2.
 */
#define FIFO_SIZE (MAX_PACKET_SIZE * 4)

/*
 * USB_COMPLETE_MINOR_BASE:
 * Starting USB minor number requested when calling usb_register_dev() in
 * line 1319: usb_probe. USB character devices share major number 180 (USB_MAJOR).
 * 192 is the conventional base used by sample/custom drivers (e.g., usb-skeleton).
 */
#define USB_COMPLETE_MINOR_BASE 192

/* ============================================================================
 * SECTION 4: PER-DEVICE PRIVATE DATA STRUCTURE & FORWARD DECLARATIONS
 * ============================================================================
 */

/*
 * Forward declaration of `static struct usb_driver usb_driver`
 * (full definition at line 1717: usb_driver).
 * Required so line 1190: usb_open can pass `&usb_driver` to `usb_find_interface()`
 * before the `struct usb_driver` initializer appears at the end of the file.
 */
static struct usb_driver usb_driver;

/*
 * struct usb_device_data:
 * Holds all per-instance runtime state for one bound USB interface.
 * Lifecycle:
 *   - Allocated with kzalloc() in line 1319: usb_probe with kref initialized to 1.
 *   - Attached to struct usb_interface via usb_set_intfdata().
 *   - Each open() increments kref; each release() and disconnect() decrements it.
 *   - Freed in line 478: usb_delete when kref reaches 0.
 */
struct usb_device_data {
    /*
     * udev:
     * Reference-counted pointer to the kernel's struct usb_device representing
     * the physical USB device (bus address, speed, descriptors, pipes).
     * Acquired via usb_get_dev(interface_to_usbdev(intf)) in line 1319: usb_probe
     * and released via usb_put_dev() in line 478: usb_delete.
     */
    struct usb_device *udev;

    /*
     * intf:
     * Pointer to the kernel's struct usb_interface bound to this driver.
     * Cleared to NULL under io_mutex during line 1619: usb_disconnect so process-
     * context code never dereferences a torn-down interface.
     */
    struct usb_interface *intf;

    /*
     * bulk_in_urb:
     * Pointer to struct urb allocated via usb_alloc_urb() for asynchronous
     * Bulk IN transfers (device -> host).
     */
    struct urb *bulk_in_urb;

    /*
     * bulk_out_urb:
     * Pointer to struct urb allocated for asynchronous Bulk OUT transfers
     * (host -> device), submitted on demand by line 1069: usb_write.
     */
    struct urb *bulk_out_urb;

    /*
     * int_in_urb:
     * Pointer to struct urb allocated for optional periodic Interrupt IN
     * transfers (device -> host). NULL if the device lacks an Interrupt IN ep.
     */
    struct urb *int_in_urb;

    /*
     * bulk_in_buf:
     * Kernel virtual address of the DMA-coherent transfer buffer for Bulk IN
     * data, allocated by usb_alloc_coherent().
     */
    unsigned char *bulk_in_buf;

    /*
     * bulk_out_buf:
     * Kernel virtual address of the DMA-coherent transfer buffer for Bulk OUT
     * data. line 1069: usb_write copies user-space bytes here prior to submission.
     */
    unsigned char *bulk_out_buf;

    /*
     * int_in_buf:
     * Kernel virtual address of the DMA-coherent transfer buffer for Interrupt
     * IN data. NULL if no Interrupt IN endpoint exists.
     */
    unsigned char *int_in_buf;

    /*
     * bulk_in_fifo:
     * Kernel byte-stream FIFO buffering Bulk IN payloads between
     * line 675: bulk_in_completion (producer, softirq) and line 916: usb_read (consumer).
     */
    struct kfifo bulk_in_fifo;

    /*
     * int_in_fifo:
     * Kernel byte-stream FIFO buffering Interrupt IN payloads produced by
     * line 826: int_in_completion.
     */
    struct kfifo int_in_fifo;

    /*
     * bulk_in_lock:
     * Spinlock serializing access to bulk_in_fifo between softirq context
     * (line 675: bulk_in_completion) and process context (line 916: usb_read).
     */
    spinlock_t bulk_in_lock;

    /*
     * int_in_lock:
     * Spinlock serializing access to int_in_fifo between line 826: int_in_completion
     * and any process-context reader.
     */
    spinlock_t int_in_lock;

    /*
     * io_mutex:
     * Process-context mutex serializing line 916: usb_read and line 1069: usb_write
     * against line 1619: usb_disconnect so disconnect never races with active I/O
     * or URB submission.
     */
    struct mutex io_mutex;

    /*
     * bulk_in_ready:
     * Completion object signaled via complete() in line 675: bulk_in_completion
     * when Bulk IN data arrives (or complete_all() in line 1619: usb_disconnect).
     */
    struct completion bulk_in_ready;

    /*
     * int_in_ready:
     * Completion object signaled when Interrupt IN data arrives.
     */
    struct completion int_in_ready;

    /*
     * bulk_out_done:
     * Completion object indicating that bulk_out_urb is idle and safe to
     * populate and submit in line 1069: usb_write. Initialized to signaled in
     * line 1319: usb_probe, consumed by line 1069: usb_write, and re-signaled by
     * line 793: bulk_out_completion (or line 1619: usb_disconnect).
     */
    struct completion bulk_out_done;

    /*
     * kref:
     * Reference counter governing the lifetime of this struct usb_device_data.
     * When kref drops to 0, kref_put() invokes line 478: usb_delete.
     */
    struct kref kref;

    /*
     * dma_handle_bulk_in:
     * Hardware DMA bus address corresponding to bulk_in_buf, returned by
     * usb_alloc_coherent(), assigned to bulk_in_urb->transfer_dma, and passed
     * to usb_free_coherent() on teardown.
     */
    dma_addr_t dma_handle_bulk_in;

    /*
     * dma_handle_bulk_out:
     * Hardware DMA bus address corresponding to bulk_out_buf.
     */
    dma_addr_t dma_handle_bulk_out;

    /*
     * dma_handle_int_in:
     * Hardware DMA bus address corresponding to int_in_buf.
     */
    dma_addr_t dma_handle_int_in;

    /*
     * bulk_in_maxpkt:
     * Unsigned 16-bit max packet size (bytes) of the Bulk IN endpoint.
     */
    u16 bulk_in_maxpkt;

    /*
     * bulk_out_maxpkt:
     * Unsigned 16-bit max packet size (bytes) of the Bulk OUT endpoint.
     */
    u16 bulk_out_maxpkt;

    /*
     * int_in_maxpkt:
     * Unsigned 16-bit max packet size (bytes) of the Interrupt IN endpoint.
     */
    u16 int_in_maxpkt;

    /*
     * int_in_interval:
     * Unsigned 8-bit raw bInterval polling interval from the Interrupt IN
     * endpoint descriptor.
     */
    u8 int_in_interval;

    /*
     * has_int_in:
     * Explicit boolean flag set to true in line 1319: usb_probe if an optional
     * Interrupt IN endpoint was discovered on the active alternate setting.
     */
    bool has_int_in;

    /*
     * connected:
     * Boolean flag indicating whether the USB device is currently attached
     * (true after line 1319: usb_probe, set to false under io_mutex in
     * line 1619: usb_disconnect).
     */
    bool connected;
};

/* ============================================================================
 * SECTION 5: REFERENCE-COUNTED DESTRUCTOR (usb_delete)
 * ============================================================================
 * Function: usb_delete()
 * Linkage:  static (internal to this file).
 * Context:  Process context (invoked automatically by kref_put() when the last
 *           reference from line 1619: usb_disconnect and all line 1260: usb_release calls
 *           has been dropped).
 * Parameter:
 *   kref - Pointer to the embedded `struct kref` member inside our
 *          `struct usb_device_data`.
 */
static void usb_delete(struct kref *kref)
{
    /*
     * Macro `container_of(ptr, type, member)`:
     *   Computes the base address of the enclosing `struct usb_device_data`
     *   from the pointer `kref` to its embedded `.kref` field by subtracting
     *   `offsetof(struct usb_device_data, kref)`.
     */
    struct usb_device_data *dev = container_of(kref, struct usb_device_data, kref);

    /*
     * Free all DMA-coherent buffers that were allocated by usb_alloc_coherent().
     * Dereferences:
     *   - `dev->bulk_in_buf` / `dev->bulk_out_buf` / `dev->int_in_buf`: checked
     *     non-NULL before freeing (safe even on partial setup).
     *   - `dev->udev`: `struct usb_device *` (still valid because we hold a
     *     usb_get_dev() reference until usb_put_dev() below!).
     *   - `dev->*_maxpkt`: exact byte size passed to usb_alloc_coherent().
     *   - `dev->dma_handle_*`: exact DMA bus handle returned at allocation.
     */
    if (dev->bulk_in_buf)
        usb_free_coherent(dev->udev, dev->bulk_in_maxpkt,
                          dev->bulk_in_buf, dev->dma_handle_bulk_in);
    if (dev->bulk_out_buf)
        usb_free_coherent(dev->udev, dev->bulk_out_maxpkt,
                          dev->bulk_out_buf, dev->dma_handle_bulk_out);
    if (dev->int_in_buf)
        usb_free_coherent(dev->udev, dev->int_in_maxpkt,
                          dev->int_in_buf, dev->dma_handle_int_in);

    /*
     * API `usb_free_urb(struct urb *urb)`:
     *   Decrements each URB's refcount and frees it. `usb_free_urb(NULL)` is
     *   explicitly a no-op, so it is safe even if `dev->int_in_urb` is NULL.
     */
    usb_free_urb(dev->bulk_in_urb);
    usb_free_urb(dev->bulk_out_urb);
    usb_free_urb(dev->int_in_urb);

    /*
     * API `kfifo_free(fifo)`:
     *   Frees the internal ring-buffer memory allocated by `kfifo_alloc()`.
     *   Dereferences `&dev->int_in_fifo` and `&dev->bulk_in_fifo`.
     */
    kfifo_free(&dev->int_in_fifo);
    kfifo_free(&dev->bulk_in_fifo);

    /*
     * API `usb_put_dev(struct usb_device *dev)`:
     *   Releases the reference on `dev->udev` acquired via `usb_get_dev()` in
     *   line 1319: usb_probe. Must happen AFTER `usb_free_coherent()` uses `dev->udev`.
     */
    usb_put_dev(dev->udev);

    /*
     * API `kfree(dev)`:
     *   Frees the `struct usb_device_data` slab object itself. After this line,
     *   `dev` is invalid and must never be dereferenced again.
     */
    kfree(dev);
}

/* ============================================================================
 * SECTION 6: DYNAMIC ENDPOINT DISCOVERY HELPER
 * ============================================================================
 * Function: usb_find_endpoint()
 * Linkage:  static (internal to this translation unit).
 * Context:  Called from line 1319: usb_probe in process context.
 * Purpose:  Scans all endpoints in the interface's currently active alternate
 *           setting and returns metadata for the first endpoint matching the
 *           requested transfer type and direction.
 *
 * Parameters:
 *   intf      - Pointer to the USB interface whose descriptors are inspected.
 *   ep_addr   - Output pointer: receives raw bEndpointAddress byte (bit 7 = dir,
 *               bits 0..3 = endpoint number). Must be non-NULL.
 *   max_pkt   - Output pointer: receives decoded max packet size. Must be non-NULL.
 *   interval  - Optional output pointer (may be NULL): receives bInterval for
 *               interrupt/isochronous endpoints.
 *   xfer_type - Desired transfer type (USB_ENDPOINT_XFER_BULK or _INT).
 *   direction - Desired direction (USB_DIR_IN [0x80] or USB_DIR_OUT [0x00]).
 *
 * Returns:
 *   0       - Matching endpoint found and output parameters populated.
 *   -ENODEV - Negative kernel errno ("No such device") if no match exists.
 */
static int usb_find_endpoint(struct usb_interface *intf,
                             u8 *ep_addr, u16 *max_pkt, u8 *interval,
                             u8 xfer_type, u8 direction)
{
    /*
     * Local variable `alt`:
     *   Pointer to const struct usb_host_interface representing the active
     *   alternate setting and its array of endpoints.
     * Dereference `intf->cur_altsetting`:
     *   Follows pointer `intf` to read its `cur_altsetting` pointer member,
     *   guaranteed non-NULL by the USB core during probe.
     */
    const struct usb_host_interface *alt = intf->cur_altsetting;

    /* Local signed int loop counter indexing `alt->endpoint[]`. */
    int i;

    /*
     * Iterate over every non-EP0 endpoint in this alternate setting.
     * Dereference `alt->desc.bNumEndpoints`:
     *   1. `alt->desc` dereferences `alt` to access embedded `struct
     *      usb_interface_descriptor desc` (value member, so `.` follows).
     *   2. `.bNumEndpoints` reads the u8 endpoint count.
     */
    for (i = 0; i < alt->desc.bNumEndpoints; i++) {
        /*
         * Local variable `epd`:
         *   Pointer to const struct usb_endpoint_descriptor for endpoint `i`.
         * Dereference `&alt->endpoint[i].desc`:
         *   1. `alt->endpoint` reads pointer to array of `struct usb_host_endpoint`.
         *   2. `[i]` selects the i-th element.
         *   3. `.desc` accesses its embedded `struct usb_endpoint_descriptor`.
         *   4. `&` takes its address for passing to descriptor helper APIs.
         */
        const struct usb_endpoint_descriptor *epd = &alt->endpoint[i].desc;

        /*
         * API `usb_endpoint_type(epd)`:
         *   Inline helper evaluating `(epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)`
         *   (bits 0..1) -> USB_ENDPOINT_XFER_CONTROL (0), _ISOC (1), _BULK (2),
         *   or _INT (3). Stored in local u8 `type`.
         */
        u8 type = usb_endpoint_type(epd);

        /*
         * API `usb_endpoint_dir_in(epd)`:
         *   Inline helper testing bit 7 of `epd->bEndpointAddress`
         *   (`(epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN`).
         * Ternary normalizes boolean result into `USB_DIR_IN` (0x80) or
         * `USB_DIR_OUT` (0x00), stored in local u8 `dir`.
         */
        u8 dir = usb_endpoint_dir_in(epd) ? USB_DIR_IN : USB_DIR_OUT;

        /* Check whether this endpoint matches both requested type and direction. */
        if (type == xfer_type && dir == direction) {
            /*
             * Dereference `*ep_addr` (write) and `epd->bEndpointAddress` (read):
             *   Copies raw u8 endpoint address byte into caller's output var.
             */
            *ep_addr = epd->bEndpointAddress;

            /*
             * API `usb_endpoint_maxp(epd)`:
             *   Converts little-endian `epd->wMaxPacketSize` via `le16_to_cpu()`
             *   and masks bits 0..10 (`USB_ENDPOINT_MAXP_MASK`) to return the
             *   true packet size in host byte order.
             * Dereference `*max_pkt` (write): stores decoded u16 size.
             */
            *max_pkt = usb_endpoint_maxp(epd);

            /*
             * Null-check `interval`:
             *   Bulk callers pass NULL because Bulk endpoints have no polling
             *   interval; Interrupt callers pass `&int_in_interval`.
             */
            if (interval) {
                /*
                 * Dereference `*interval` (write) and `epd->bInterval` (read):
                 *   Copies u8 polling interval into caller's output variable.
                 */
                *interval = epd->bInterval;
            }

            /* Return 0 indicating matching endpoint found. */
            return 0;
        }
    }

    /* No matching endpoint found in active alternate setting; return -ENODEV. */
    return -ENODEV;
}

/* ============================================================================
 * SECTION 7: URB COMPLETION CALLBACKS (ATOMIC / SOFTIRQ CONTEXT)
 * ============================================================================
 * Invoked by the USB Host Controller Driver (HCD) when a submitted URB finishes,
 * fails, or is canceled.
 * Execution context: softirq / interrupt context (`in_interrupt()` is true).
 * Strict rules:
 *   - NEVER sleep (no mutex_lock, no msleep, no GFP_KERNEL, no copy_to_user).
 *   - Use spin_lock_irqsave() to protect shared FIFO state.
 *   - Use GFP_ATOMIC if submitting URBs.
 *   - ALWAYS return immediately (do NOT resubmit) on -ENOENT, -ECONNRESET, or
 *     -ESHUTDOWN so disconnect/kill terminates cleanly.
 */

/*
 * Function: bulk_in_completion()
 * Parameter:
 *   urb - Pointer to the completed Bulk IN `struct urb`.
 */
static void bulk_in_completion(struct urb *urb)
{
    /*
     * Dereference `urb->context`:
     *   Recovers our `struct usb_device_data *` stored when `usb_fill_bulk_urb()`
     *   configured this URB in line 1319: usb_probe.
     */
    struct usb_device_data *dev = urb->context;

    /* Saved CPU interrupt flags for spin_lock_irqsave() / spin_unlock_irqrestore(). */
    unsigned long flags;

    /* Return code for checking usb_submit_urb() resubmission errors. */
    int ret;

    /*
     * Dereference `urb->status`:
     *   Inspects transfer completion status written by the USB HCD:
     *     0           = success
     *     -ENOENT     = synchronously canceled by usb_kill_urb()
     *     -ECONNRESET = asynchronously unlinked by usb_unlink_urb()
     *     -ESHUTDOWN  = device unplugged or host controller shut down
     *     Other (< 0) = transient USB bus/protocol error (e.g., -EPIPE, -EPROTO)
     */
    if (urb->status) {
        /*
         * Normal teardown/unplug statuses: stop resubmitting immediately so
         * usb_kill_urb() in line 1619: usb_disconnect finishes cleanly.
         */
        if (urb->status == -ENOENT ||
            urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN) {
            return;
        }

        /*
         * Unexpected USB transfer error: log with interface device prefix.
         * Chained dereference `&dev->intf->dev`:
         *   1. `dev->intf` reads `struct usb_interface *` from `*dev`.
         *   2. `->dev` accesses its embedded `struct device`.
         *   3. `&` passes `struct device *` to `dev_err()`.
         */
        dev_err(&dev->intf->dev, "Bulk IN transfer failed: %d\n", urb->status);
        goto resubmit;
    }

    /*
     * Dereference `urb->actual_length`:
     *   `u32 actual_length` holds the exact number of bytes received from the
     *   device. If a Zero-Length Packet (ZLP) arrived, skip FIFO insertion and
     *   re-arm the URB.
     */
    if (urb->actual_length == 0) {
        goto resubmit;
    }

    /*
     * API `spin_lock_irqsave(&dev->bulk_in_lock, flags)`:
     *   Saves local CPU IRQ state into `flags`, disables local IRQs, and
     *   acquires `dev->bulk_in_lock` (dereferenced via `&dev->bulk_in_lock`).
     *   Protects `dev->bulk_in_fifo` against concurrent access from line 916: usb_read.
     */
    spin_lock_irqsave(&dev->bulk_in_lock, flags);

    /*
     * API `kfifo_in(&dev->bulk_in_fifo, dev->bulk_in_buf, urb->actual_length)`:
     *   Copies up to `urb->actual_length` bytes from DMA buffer `dev->bulk_in_buf`
     *   into the ring buffer `dev->bulk_in_fifo`, returning the exact count of
     *   bytes stored.
     * Dereferences:
     *   - `&dev->bulk_in_fifo`: address of embedded `struct kfifo` in `*dev`.
     *   - `dev->bulk_in_buf`: virtual pointer to Bulk IN DMA buffer.
     *   - `urb->actual_length`: received byte count (`u32`, printed with `%u`).
     */
    if (kfifo_in(&dev->bulk_in_fifo, dev->bulk_in_buf, urb->actual_length) !=
        urb->actual_length) {
        dev_err(&dev->intf->dev, "FIFO full, dropping %u bytes\n",
                urb->actual_length);
    }

    /*
     * API `complete(&dev->bulk_in_ready)`:
     *   Wakes one task sleeping in `wait_for_completion_interruptible_timeout()`
     *   inside line 916: usb_read. Safe in atomic/softirq context.
     * Dereference `&dev->bulk_in_ready`: address of embedded `struct completion`.
     */
    complete(&dev->bulk_in_ready);

    /*
     * API `spin_unlock_irqrestore(&dev->bulk_in_lock, flags)`:
     *   Releases `dev->bulk_in_lock` and restores prior local IRQ state.
     */
    spin_unlock_irqrestore(&dev->bulk_in_lock, flags);

/* Label reached on transient errors, ZLPs, or normal completion to keep reading. */
resubmit:
    /*
     * API `usb_submit_urb(urb, GFP_ATOMIC)`:
     *   Re-submits the Bulk IN URB asynchronously to the USB core.
     *   `GFP_ATOMIC` is mandatory because we are in softirq context and cannot
     *   sleep. Checks `ret` and logs unexpected failures (ignoring -ENODEV and
     *   -EPERM, which naturally occur if disconnect races with resubmission).
     */
    ret = usb_submit_urb(urb, GFP_ATOMIC);
    if (ret && ret != -ENODEV && ret != -EPERM) {
        dev_err(&dev->intf->dev, "Failed to resubmit bulk IN URB: %d\n", ret);
    }
}

/*
 * Function: bulk_out_completion()
 * Purpose:  Invoked by the USB core when a Bulk OUT transfer initiated by
 *           line 1069: usb_write finishes. Does NOT resubmit (Bulk OUT is driven on
 *           demand by user-space writes); instead signals `dev->bulk_out_done`
 *           so the next write can reuse `dev->bulk_out_urb` and `bulk_out_buf`.
 * Parameter:
 *   urb - Pointer to the completed Bulk OUT `struct urb`.
 */
static void bulk_out_completion(struct urb *urb)
{
    /* Dereference `urb->context`: recovers our `struct usb_device_data *`. */
    struct usb_device_data *dev = urb->context;

    /*
     * Dereference `urb->status`:
     *   Logs genuine Bulk OUT transfer failures while ignoring normal
     *   cancellation/unplug codes (-ENOENT, -ECONNRESET, -ESHUTDOWN).
     */
    if (urb->status &&
        urb->status != -ENOENT &&
        urb->status != -ECONNRESET &&
        urb->status != -ESHUTDOWN) {
        dev_err(&dev->intf->dev, "Bulk OUT transfer failed: %d\n", urb->status);
    }

    /*
     * API `complete(&dev->bulk_out_done)`:
     *   Marks `dev->bulk_out_urb` as idle again and wakes any writer waiting
     *   in line 1069: usb_write. Dereferences `&dev->bulk_out_done`.
     */
    complete(&dev->bulk_out_done);
}

/*
 * Function: int_in_completion()
 * Purpose:  Invoked by the USB core when a periodic Interrupt IN transfer
 *           completes. Enqueues any received bytes into `dev->int_in_fifo`,
 *           signals `dev->int_in_ready`, and re-arms the URB.
 * Parameter:
 *   urb - Pointer to the completed Interrupt IN `struct urb`.
 */
static void int_in_completion(struct urb *urb)
{
    /* Dereference `urb->context`: recovers our `struct usb_device_data *`. */
    struct usb_device_data *dev = urb->context;

    /* Saved CPU IRQ flags for spinlock critical section. */
    unsigned long flags;

    /* Return code for checking usb_submit_urb() resubmission. */
    int ret;

    /*
     * Dereference `urb->status`:
     *   Stops immediately without resubmitting on cancel/unplug statuses;
     *   logs other bus errors and jumps to `resubmit:`.
     */
    if (urb->status) {
        if (urb->status == -ENOENT ||
            urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN) {
            return;
        }
        dev_err(&dev->intf->dev, "Interrupt IN transfer failed: %d\n", urb->status);
        goto resubmit;
    }

    /*
     * Dereference `urb->actual_length`:
     *   Only acquires `int_in_lock` and writes to `int_in_fifo` when >= 1 byte
     *   of interrupt data was actually received.
     */
    if (urb->actual_length > 0) {
        /* Acquires `dev->int_in_lock` and saves/disables local IRQs. */
        spin_lock_irqsave(&dev->int_in_lock, flags);

        /*
         * API `kfifo_in(&dev->int_in_fifo, dev->int_in_buf, urb->actual_length)`:
         *   Enqueues received interrupt bytes into `dev->int_in_fifo` and
         *   reports overflow if fewer bytes fit than arrived.
         */
        if (kfifo_in(&dev->int_in_fifo, dev->int_in_buf, urb->actual_length) !=
            urb->actual_length) {
            dev_err(&dev->intf->dev, "Interrupt FIFO full, dropping %u bytes\n",
                    urb->actual_length);
        }

        /* Wakes any task waiting on `&dev->int_in_ready`. */
        complete(&dev->int_in_ready);

        /* Releases `dev->int_in_lock` and restores saved local IRQ state. */
        spin_unlock_irqrestore(&dev->int_in_lock, flags);
    }

/* Label to re-arm the Interrupt IN URB for the next polling interval. */
resubmit:
    /* Re-submits Interrupt IN URB in atomic context via `GFP_ATOMIC`. */
    ret = usb_submit_urb(urb, GFP_ATOMIC);
    if (ret && ret != -ENODEV && ret != -EPERM) {
        dev_err(&dev->intf->dev, "Failed to resubmit interrupt IN URB: %d\n", ret);
    }
}

/* ============================================================================
 * SECTION 8: CHARACTER DEVICE FILE OPERATIONS (PROCESS CONTEXT)
 * ============================================================================
 * Implements VFS open(), release(), read(), and write() for `/dev/usb_completeN`
 * registered via `usb_register_dev()` in line 1319: usb_probe.
 * Execution context: process context (sleeping is permitted EXCEPT while
 * holding a spinlock).
 */

/*
 * Function: usb_read()
 * Purpose:  Handles user-space `read(fd, buf, count)` by draining bytes from
 *           `dev->bulk_in_fifo` (waiting up to 1 second if the FIFO is empty)
 *           and copying them safely to user space.
 * Parameters:
 *   file  - Pointer to kernel `struct file` for this open file descriptor.
 *   buf   - User-space destination pointer (`__user` sparse annotation).
 *   count - Maximum byte count requested by user space.
 *   ppos  - Pointer to file offset (unused for streaming USB I/O).
 * Returns:
 *   > 0          - Number of bytes copied to user space.
 *   0            - Caller requested 0 bytes.
 *   -ENODEV      - Device was disconnected.
 *   -ENOMEM      - Temporary heap buffer allocation failed.
 *   -ERESTARTSYS - Waiting process was interrupted by a signal (e.g., Ctrl-C).
 *   -ETIMEDOUT   - No data arrived within HZ (1 second).
 *   -EFAULT      - User-space `buf` pointer was invalid/unwritable.
 */
static ssize_t usb_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    /*
     * Dereference `file->private_data`:
     *   Retrieves our `struct usb_device_data *` saved during line 1190: usb_open.
     */
    struct usb_device_data *dev = file->private_data;

    /*
     * Local pointer `temp`:
     *   Dynamically allocated staging buffer (replaces the unsafe 2048-byte
     *   kernel stack array so we avoid kernel stack overflow).
     */
    unsigned char *temp;

    /* Clamped transfer size: never try to dequeue more than FIFO_SIZE bytes. */
    size_t to_read;

    /* Number of bytes actually dequeued from `dev->bulk_in_fifo`. */
    unsigned int len;

    /* Signed long to hold return value of wait_for_completion_interruptible_timeout(). */
    long wait_ret;

    /* Saved CPU IRQ flags for spin_lock_irqsave(). */
    unsigned long flags;

    /* If user space asked for 0 bytes, return 0 immediately (POSIX semantics). */
    if (count == 0) {
        return 0;
    }

    /*
     * Macro `min_t(size_t, count, FIFO_SIZE)`:
     *   Caps `to_read` at `FIFO_SIZE` (2048) so `kmalloc()` and `kfifo_out()`
     *   never allocate or copy an unbounded user-supplied `count`.
     */
    to_read = min_t(size_t, count, FIFO_SIZE);

    /*
     * API `kmalloc(to_read, GFP_KERNEL)`:
     *   Allocates `to_read` bytes on the kernel heap (sleeping allowed here
     *   because we are in process context and hold no spinlock).
     */
    temp = kmalloc(to_read, GFP_KERNEL);
    if (!temp) {
        return -ENOMEM;
    }

    /*
     * Loop until either data is available in `bulk_in_fifo` or we exit due to
     * disconnect, signal, or timeout. Checking `kfifo_is_empty()` first ensures
     * bytes already sitting in the FIFO from a previous partial read are
     * returned immediately without blocking!
     */
    while (1) {
        /*
         * Acquire `dev->bulk_in_lock` (disabling local IRQs) to inspect and
         * dequeue from `dev->bulk_in_fifo` atomically against line 675: bulk_in_completion.
         */
        spin_lock_irqsave(&dev->bulk_in_lock, flags);

        /*
         * API `kfifo_out(&dev->bulk_in_fifo, temp, to_read)`:
         *   Copies up to `to_read` bytes out of `dev->bulk_in_fifo` into `temp`
         *   and returns the exact number of bytes extracted (0 if FIFO is empty).
         */
        len = kfifo_out(&dev->bulk_in_fifo, temp, to_read);
        if (len > 0) {
            /* Data obtained; release spinlock BEFORE calling copy_to_user(). */
            spin_unlock_irqrestore(&dev->bulk_in_lock, flags);
            break;
        }

        /*
         * FIFO is empty: reset `dev->bulk_in_ready` while still holding
         * `bulk_in_lock` so we do not miss a `complete()` from
         * line 675: bulk_in_completion between releasing the lock and sleeping.
         */
        reinit_completion(&dev->bulk_in_ready);
        spin_unlock_irqrestore(&dev->bulk_in_lock, flags);

        /*
         * Check `dev->connected` under `dev->io_mutex`:
         *   If line 1619: usb_disconnect has run and the FIFO is empty, fail with
         *   -ENODEV instead of sleeping for a timeout.
         */
        mutex_lock(&dev->io_mutex);
        if (!dev->connected) {
            mutex_unlock(&dev->io_mutex);
            kfree(temp);
            return -ENODEV;
        }
        mutex_unlock(&dev->io_mutex);

        /*
         * API `wait_for_completion_interruptible_timeout(&dev->bulk_in_ready, HZ)`:
         *   Sleeps (TASK_INTERRUPTIBLE) until line 675: bulk_in_completion calls
         *   `complete()`, line 1619: usb_disconnect calls `complete_all()`, a signal
         *   arrives, or `HZ` jiffies (1 second) expire.
         *   Returns:
         *     > 0          = remaining jiffies (signaled before timeout)
         *     0            = timed out
         *     -ERESTARTSYS = interrupted by a signal (e.g., SIGINT / Ctrl-C)
         */
        wait_ret = wait_for_completion_interruptible_timeout(&dev->bulk_in_ready, HZ);
        if (wait_ret == 0) {
            kfree(temp);
            return -ETIMEDOUT;
        }
        if (wait_ret < 0) {
            kfree(temp);
            return wait_ret; /* -ERESTARTSYS */
        }
        /* Woken up: loop back to check FIFO (or disconnect if complete_all woke us). */
    }

    /*
     * API `copy_to_user(buf, temp, len)`:
     *   Copies `len` bytes from kernel heap buffer `temp` to user-space `buf`.
     *   May sleep on page fault, which is safe because no spinlock is held.
     *   Returns number of bytes that could NOT be copied (0 = full success).
     */
    if (copy_to_user(buf, temp, len)) {
        kfree(temp);
        return -EFAULT;
    }

    /* Free temporary staging buffer and return byte count read to user space. */
    kfree(temp);
    return len;
}

/*
 * Function: usb_write()
 * Purpose:  Handles user-space `write(fd, buf, count)` by waiting until
 *           `dev->bulk_out_urb` is idle, copying user data into `bulk_out_buf`,
 *           and submitting `bulk_out_urb` to the USB core.
 * Parameters:
 *   file  - Pointer to open `struct file`.
 *   buf   - Const user-space source buffer pointer (`__user`).
 *   count - Number of bytes to send to the device.
 *   ppos  - Pointer to file offset (unused).
 * Returns:
 *   count        - Number of bytes submitted on success.
 *   0            - Caller requested 0 bytes.
 *   -EINVAL      - `count` exceeds `dev->bulk_out_maxpkt`.
 *   -ENODEV      - Device was disconnected.
 *   -ETIMEDOUT   - Previous Bulk OUT URB did not finish within 1 second.
 *   -ERESTARTSYS - Interrupted by a signal while waiting for URB availability.
 *   -EFAULT      - User-space pointer `buf` could not be read.
 *   < 0          - Negative errno from `usb_submit_urb()`.
 */
static ssize_t usb_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    /* Dereference `file->private_data`: recovers our `struct usb_device_data *`. */
    struct usb_device_data *dev = file->private_data;

    /* Holds return value of `wait_for_completion_interruptible_timeout()`. */
    long wait_ret;

    /* Holds return value of `usb_submit_urb()`. */
    int ret;

    /* Zero-byte write succeeds immediately. */
    if (count == 0) {
        return 0;
    }

    /*
     * Acquire `dev->io_mutex` (dereferenced via `&dev->io_mutex`):
     *   Serializes concurrent writers and prevents line 1619: usb_disconnect from
     *   racing with URB submission.
     */
    mutex_lock(&dev->io_mutex);

    /*
     * Dereference `dev->connected`:
     *   Rejects writes immediately if the device has already been unplugged.
     */
    if (!dev->connected) {
        mutex_unlock(&dev->io_mutex);
        return -ENODEV;
    }

    /*
     * Dereference `dev->bulk_out_maxpkt`:
     *   Ensures `count` fits inside `dev->bulk_out_buf` (which was allocated
     *   to `dev->bulk_out_maxpkt` bytes in line 1319: usb_probe).
     */
    if (count > dev->bulk_out_maxpkt) {
        dev_err(&dev->intf->dev, "Write size %zu exceeds max packet %u\n",
                count, dev->bulk_out_maxpkt);
        mutex_unlock(&dev->io_mutex);
        return -EINVAL;
    }

    /*
     * Wait for any previous Bulk OUT URB to finish in line 793: bulk_out_completion
     * before modifying `dev->bulk_out_buf` and `dev->bulk_out_urb`.
     * Dereference `&dev->bulk_out_done`: address of embedded `struct completion`.
     */
    wait_ret = wait_for_completion_interruptible_timeout(&dev->bulk_out_done, HZ);
    if (wait_ret == 0) {
        mutex_unlock(&dev->io_mutex);
        return -ETIMEDOUT;
    }
    if (wait_ret < 0) {
        mutex_unlock(&dev->io_mutex);
        return wait_ret; /* -ERESTARTSYS */
    }

    /* Re-check `dev->connected` in case line 1619: usb_disconnect woke us up. */
    if (!dev->connected) {
        mutex_unlock(&dev->io_mutex);
        return -ENODEV;
    }

    /*
     * API `copy_from_user(dev->bulk_out_buf, buf, count)`:
     *   Safely copies `count` bytes from user-space `buf` into kernel DMA
     *   buffer `dev->bulk_out_buf`. If it fails, re-signal `bulk_out_done`
     *   so future writers are not permanently blocked!
     */
    if (copy_from_user(dev->bulk_out_buf, buf, count)) {
        complete(&dev->bulk_out_done);
        mutex_unlock(&dev->io_mutex);
        return -EFAULT;
    }

    /*
     * Chained dereferences into `*dev->bulk_out_urb`:
     *   - `dev->bulk_out_urb->transfer_buffer`: virtual pointer to DMA buffer.
     *   - `dev->bulk_out_urb->transfer_buffer_length`: exact byte count to send.
     *   - `dev->bulk_out_urb->transfer_dma`: DMA bus handle (`dma_handle_bulk_out`).
     *   - `dev->bulk_out_urb->transfer_flags`: ensures `URB_NO_TRANSFER_DMA_MAP`
     *     is set because `bulk_out_buf` came from `usb_alloc_coherent()`.
     */
    dev->bulk_out_urb->transfer_buffer = dev->bulk_out_buf;
    dev->bulk_out_urb->transfer_buffer_length = count;
    dev->bulk_out_urb->transfer_dma = dev->dma_handle_bulk_out;
    dev->bulk_out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    /*
     * API `usb_submit_urb(dev->bulk_out_urb, GFP_KERNEL)`:
     *   Submits the Bulk OUT URB asynchronously (`GFP_KERNEL` is safe in
     *   process context). On error, re-signal `bulk_out_done` before returning.
     */
    ret = usb_submit_urb(dev->bulk_out_urb, GFP_KERNEL);
    if (ret) {
        dev_err(&dev->intf->dev, "Failed to submit bulk OUT URB: %d\n", ret);
        complete(&dev->bulk_out_done);
        mutex_unlock(&dev->io_mutex);
        return ret;
    }

    /* Release `dev->io_mutex` and return the number of bytes submitted. */
    mutex_unlock(&dev->io_mutex);
    return count;
}

/*
 * Function: usb_open()
 * Purpose:  Handles user-space `open("/dev/usb_completeN", ...)` calls.
 *           Locates the bound `struct usb_interface` via its minor number,
 *           retrieves `struct usb_device_data`, increments `kref`, and stores
 *           `dev` in `file->private_data`.
 * Parameters:
 *   inode - Pointer to VFS `struct inode` of `/dev/usb_completeN`.
 *   file  - Pointer to newly created `struct file` for this open descriptor.
 * Returns:
 *   0       - Success.
 *   -ENODEV - No matching interface or device is already disconnected.
 */
static int usb_open(struct inode *inode, struct file *file)
{
    /* Pointer to the `struct usb_interface` corresponding to `iminor(inode)`. */
    struct usb_interface *intf;

    /* Pointer to our per-device private state retrieved from `intf`. */
    struct usb_device_data *dev;

    /*
     * API `usb_find_interface(&usb_driver, iminor(inode))`:
     *   - `iminor(inode)`: extracts the minor device number from `inode->i_rdev`.
     *   - `usb_find_interface()`: searches interfaces currently bound to
     *     `usb_driver` (line 1717: usb_driver) for the one owning that minor number
     *     (assigned by `usb_register_dev()` in line 1319: usb_probe).
     *   Returns `struct usb_interface *` or NULL if not found.
     */
    intf = usb_find_interface(&usb_driver, iminor(inode));
    if (!intf) {
        return -ENODEV;
    }

    /*
     * API `usb_get_intfdata(intf)`:
     *   Retrieves the `struct usb_device_data *` pointer stored on `intf` by
     *   `usb_set_intfdata()` in line 1319: usb_probe.
     */
    dev = usb_get_intfdata(intf);
    if (!dev) {
        return -ENODEV;
    }

    /*
     * Acquire `dev->io_mutex` to check `dev->connected` atomically against
     * line 1619: usb_disconnect and increment `dev->kref` before disconnect can
     * drop the final reference.
     */
    mutex_lock(&dev->io_mutex);
    if (!dev->connected) {
        mutex_unlock(&dev->io_mutex);
        return -ENODEV;
    }

    /*
     * API `kref_get(&dev->kref)`:
     *   Atomically increments the reference count in `dev->kref` so `*dev`
     *   remains alive even if the USB device is unplugged while this fd is open.
     *   Paired with `kref_put(&dev->kref, usb_delete)` in line 1260: usb_release.
     */
    kref_get(&dev->kref);
    mutex_unlock(&dev->io_mutex);

    /*
     * Dereference `file->private_data` (write):
     *   Caches `dev` on the open `struct file` for line 916: usb_read,
     *   line 1069: usb_write, and line 1260: usb_release.
     */
    file->private_data = dev;
    return 0;
}

/*
 * Function: usb_release()
 * Purpose:  Handles user-space `close(fd)` when the last reference to the open
 *           `struct file` is closed. Drops the `kref` taken in line 1190: usb_open.
 * Parameters:
 *   inode - Pointer to VFS `struct inode`.
 *   file  - Pointer to `struct file` being closed.
 * Returns:
 *   0 - Always succeeds.
 */
static int usb_release(struct inode *inode, struct file *file)
{
    /* Dereference `file->private_data`: retrieves our `struct usb_device_data *`. */
    struct usb_device_data *dev = file->private_data;

    /* Clear `file->private_data` so no dangling pointer remains on `*file`. */
    file->private_data = NULL;

    /*
     * API `kref_put(&dev->kref, usb_delete)`:
     *   Atomically decrements `dev->kref`. If this was the last reference
     *   (i.e., line 1619: usb_disconnect already ran), `kref_put()` immediately
     *   invokes line 478: usb_delete to free URBs, DMA buffers, FIFOs, and `dev`.
     */
    if (dev) {
        kref_put(&dev->kref, usb_delete);
    }

    return 0;
}

/*
 * Definition: `static const struct file_operations usb_fops`
 * VFS dispatch table mapping user-space syscalls on `/dev/usb_completeN` to
 * this driver's callbacks. Referenced by `usb_class` (line 1299: usb_class).
 */
static const struct file_operations usb_fops = {
    .owner = THIS_MODULE,   /* Prevents `rmmod` while any `/dev/usb_completeN` fd is open */
    .open = usb_open,       /* Called on open()  (line 1190: usb_open) */
    .release = usb_release, /* Called on close() (line 1260: usb_release) */
    .read = usb_read,       /* Called on read()  (line 916: usb_read) */
    .write = usb_write,     /* Called on write() (line 1069: usb_write) */
};

/*
 * Definition: `static struct usb_class_driver usb_class`
 * Describes the character-device node created by `usb_register_dev()` in
 * line 1319: usb_probe and removed by `usb_deregister_dev()` in line 1619: usb_disconnect.
 */
static struct usb_class_driver usb_class = {
    .name = "usb_complete%d",          /* Device node name template -> /dev/usb_complete0, 1, ... */
    .fops = &usb_fops,                 /* File operations table (line 1286: usb_fops) */
    .minor_base = USB_COMPLETE_MINOR_BASE, /* Starting minor number under USB_MAJOR (180) */
};

/* ============================================================================
 * SECTION 9: USB PROBE FUNCTION (DEVICE ATTACHMENT / INITIALIZATION)
 * ============================================================================
 * Function: usb_probe()
 * Context:  Process context (called by USB core when a matching device plugs in
 *           or when the module loads). Sleeping (`GFP_KERNEL`) is permitted.
 * Parameters:
 *   intf - Pointer to the `struct usb_interface` being bound.
 *   id   - Pointer to the matching entry in `usb_id_table[]` (line 1693: usb_id_table).
 * Returns:
 *   0   - Device successfully initialized and claimed.
 *   < 0 - Negative errno; all partially allocated/submitted resources are
 *         unwound cleanly via the `err_*` goto ladder.
 */
static int usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    /* Newly allocated per-device private state structure. */
    struct usb_device_data *dev;

    /* Discovered raw endpoint address bytes (bEndpointAddress). */
    u8 bulk_in_addr, bulk_out_addr, int_in_addr = 0;

    /* Discovered endpoint max packet sizes (bytes). */
    u16 bulk_in_maxpkt, bulk_out_maxpkt, int_in_maxpkt = 0;

    /* Discovered optional Interrupt IN polling interval (bInterval). */
    u8 int_in_interval = 0;

    /* Generic return code for error checking and unwinding. */
    int ret;

    /*
     * API `kzalloc(sizeof(*dev), GFP_KERNEL)`:
     *   Allocates zero-initialized slab memory for `struct usb_device_data`.
     *   `sizeof(*dev)` is evaluated at compile time (no runtime dereference).
     */
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        return -ENOMEM;
    }

    /*
     * API `kref_init(&dev->kref)`:
     *   Initializes reference count to 1 (representing the driver's binding,
     *   dropped in line 1619: usb_disconnect).
     */
    kref_init(&dev->kref);

    /*
     * API `mutex_init(&dev->io_mutex)`:
     *   Initializes the unlocked process-context I/O vs. disconnect mutex.
     */
    mutex_init(&dev->io_mutex);

    /*
     * API `spin_lock_init(...)`:
     *   Initializes both FIFO spinlocks to unlocked state.
     *   Dereferences `&dev->bulk_in_lock` and `&dev->int_in_lock`.
     */
    spin_lock_init(&dev->bulk_in_lock);
    spin_lock_init(&dev->int_in_lock);

    /*
     * API `init_completion(...)`:
     *   Initializes all three completion objects (`done = 0`, empty waitqueue).
     *   `complete(&dev->bulk_out_done)` immediately sets `bulk_out_done` to
     *   signaled because `bulk_out_urb` starts out idle and ready for the first
     *   line 1069: usb_write call.
     */
    init_completion(&dev->bulk_in_ready);
    init_completion(&dev->int_in_ready);
    init_completion(&dev->bulk_out_done);
    complete(&dev->bulk_out_done);

    /*
     * API `kfifo_alloc(&dev->bulk_in_fifo, FIFO_SIZE, GFP_KERNEL)`:
     *   Dynamically allocates a 2048-byte power-of-2 ring buffer for Bulk IN
     *   data. Returns 0 on success or `-ENOMEM` on failure.
     */
    ret = kfifo_alloc(&dev->bulk_in_fifo, FIFO_SIZE, GFP_KERNEL);
    if (ret) {
        kfree(dev);
        return ret;
    }

    /* Allocates a 2048-byte ring buffer for Interrupt IN data. */
    ret = kfifo_alloc(&dev->int_in_fifo, FIFO_SIZE, GFP_KERNEL);
    if (ret) {
        kfifo_free(&dev->bulk_in_fifo);
        kfree(dev);
        return ret;
    }

    /*
     * Store interface pointer, take a reference on parent `struct usb_device`
     * via `usb_get_dev(interface_to_usbdev(intf))`, and mark connected.
     * Dereferences `dev->intf`, `dev->udev`, and `dev->connected` (writes).
     */
    dev->intf = intf;
    dev->udev = usb_get_dev(interface_to_usbdev(intf));
    dev->connected = true;

    /* Discover mandatory Bulk IN endpoint via line 564: usb_find_endpoint. */
    ret = usb_find_endpoint(intf, &bulk_in_addr, &bulk_in_maxpkt, NULL,
                            USB_ENDPOINT_XFER_BULK, USB_DIR_IN);
    if (ret) {
        dev_err(&intf->dev, "No bulk IN endpoint found\n");
        goto err_put_dev;
    }
    dev->bulk_in_maxpkt = bulk_in_maxpkt;

    /* Discover mandatory Bulk OUT endpoint via line 564: usb_find_endpoint. */
    ret = usb_find_endpoint(intf, &bulk_out_addr, &bulk_out_maxpkt, NULL,
                            USB_ENDPOINT_XFER_BULK, USB_DIR_OUT);
    if (ret) {
        dev_err(&intf->dev, "No bulk OUT endpoint found\n");
        goto err_put_dev;
    }
    dev->bulk_out_maxpkt = bulk_out_maxpkt;

    /* Discover optional Interrupt IN endpoint via line 564: usb_find_endpoint. */
    ret = usb_find_endpoint(intf, &int_in_addr, &int_in_maxpkt, &int_in_interval,
                            USB_ENDPOINT_XFER_INT, USB_DIR_IN);
    if (ret) {
        dev_info(&intf->dev, "No interrupt IN endpoint found (optional)\n");
        dev->has_int_in = false;
    } else {
        dev->has_int_in = true;
        dev->int_in_maxpkt = int_in_maxpkt;
        dev->int_in_interval = int_in_interval;
    }

    /* Allocate Bulk IN `struct urb` (0 isochronous packets, `GFP_KERNEL`). */
    dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->bulk_in_urb) {
        ret = -ENOMEM;
        goto err_put_dev;
    }

    /*
     * Allocate DMA-coherent buffer for Bulk IN transfers.
     * Dereferences `dev->udev`, `dev->bulk_in_maxpkt`, `&dev->dma_handle_bulk_in`,
     * and writes returned virtual pointer to `dev->bulk_in_buf`.
     */
    dev->bulk_in_buf = usb_alloc_coherent(dev->udev, dev->bulk_in_maxpkt,
                                          GFP_KERNEL, &dev->dma_handle_bulk_in);
    if (!dev->bulk_in_buf) {
        ret = -ENOMEM;
        goto err_free_bulk_in_urb;
    }

    /*
     * Configure Bulk IN URB:
     *   - `usb_rcvbulkpipe(dev->udev, bulk_in_addr & 0x7F)`: masks off bit 7
     *     (direction bit) and builds the Bulk IN pipe handle.
     *   - Callback: line 675: bulk_in_completion, context: `dev`.
     */
    usb_fill_bulk_urb(dev->bulk_in_urb, dev->udev,
                      usb_rcvbulkpipe(dev->udev, bulk_in_addr & 0x7F),
                      dev->bulk_in_buf, dev->bulk_in_maxpkt,
                      bulk_in_completion, dev);

    /*
     * Bind the coherent DMA bus handle and set `URB_NO_TRANSFER_DMA_MAP` so
     * the USB core uses `dma_handle_bulk_in` directly instead of performing
     * redundant streaming DMA mapping on an already-coherent buffer.
     * Chained dereferences: `dev->bulk_in_urb->transfer_dma` and `->transfer_flags`.
     */
    dev->bulk_in_urb->transfer_dma = dev->dma_handle_bulk_in;
    dev->bulk_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    /* Allocate Bulk OUT `struct urb` (submitted on demand in line 1069: usb_write). */
    dev->bulk_out_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->bulk_out_urb) {
        ret = -ENOMEM;
        goto err_free_bulk_in_buf;
    }

    /* Allocate DMA-coherent buffer for Bulk OUT transfers. */
    dev->bulk_out_buf = usb_alloc_coherent(dev->udev, dev->bulk_out_maxpkt,
                                           GFP_KERNEL, &dev->dma_handle_bulk_out);
    if (!dev->bulk_out_buf) {
        ret = -ENOMEM;
        goto err_free_bulk_out_urb;
    }

    /*
     * Configure Bulk OUT URB (`usb_sndbulkpipe` for OUT direction, callback
     * line 793: bulk_out_completion). Note: NOT submitted here—line 1069: usb_write
     * submits it only when user space writes real payload bytes.
     */
    usb_fill_bulk_urb(dev->bulk_out_urb, dev->udev,
                      usb_sndbulkpipe(dev->udev, bulk_out_addr & 0x7F),
                      dev->bulk_out_buf, dev->bulk_out_maxpkt,
                      bulk_out_completion, dev);
    dev->bulk_out_urb->transfer_dma = dev->dma_handle_bulk_out;
    dev->bulk_out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    /*
     * Allocate and configure optional Interrupt IN URB if `dev->has_int_in`
     * was set to true during endpoint discovery above.
     */
    if (dev->has_int_in) {
        dev->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!dev->int_in_urb) {
            ret = -ENOMEM;
            goto err_free_bulk_out_buf;
        }

        dev->int_in_buf = usb_alloc_coherent(dev->udev, dev->int_in_maxpkt,
                                             GFP_KERNEL, &dev->dma_handle_int_in);
        if (!dev->int_in_buf) {
            ret = -ENOMEM;
            goto err_free_int_in_urb;
        }

        /*
         * Configure Interrupt IN URB via `usb_fill_int_urb()` with
         * `usb_rcvintpipe()`, callback line 826: int_in_completion, context `dev`,
         * and `int_in_interval` (decoded for FS/HS/SS internally).
         */
        usb_fill_int_urb(dev->int_in_urb, dev->udev,
                         usb_rcvintpipe(dev->udev, int_in_addr & 0x7F),
                         dev->int_in_buf, dev->int_in_maxpkt,
                         int_in_completion, dev,
                         int_in_interval);
        dev->int_in_urb->transfer_dma = dev->dma_handle_int_in;
        dev->int_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    }

    /*
     * Attach `dev` to `intf` BEFORE submitting URBs or registering `/dev/...`
     * so both `usb_open()` and any early callback/disconnect see valid intfdata.
     */
    usb_set_intfdata(intf, dev);

    /*
     * Submit Bulk IN URB to begin asynchronous reception from the device.
     * If this fails, clear intfdata and unwind cleanly.
     */
    ret = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
    if (ret) {
        dev_err(&intf->dev, "Failed to submit bulk IN URB: %d\n", ret);
        goto err_clear_intfdata;
    }

    /* Submit optional Interrupt IN URB if present. */
    if (dev->has_int_in) {
        ret = usb_submit_urb(dev->int_in_urb, GFP_KERNEL);
        if (ret) {
            dev_err(&intf->dev, "Failed to submit interrupt IN URB: %d\n", ret);
            goto err_kill_bulk_in;
        }
    }

    /*
     * API `usb_register_dev(intf, &usb_class)`:
     *   Registers a USB minor number under USB_MAJOR (180) and asks udev to
     *   create `/dev/usb_completeN` wired to `usb_fops` (line 1299: usb_class).
     */
    ret = usb_register_dev(intf, &usb_class);
    if (ret) {
        dev_err(&intf->dev, "Failed to get a minor for this device: %d\n", ret);
        goto err_kill_int_in;
    }

    /* Log successful probe with the assigned minor number (`intf->minor`). */
    dev_info(&intf->dev, "Device probed successfully (minor %d)\n", intf->minor);
    return 0;

/*
 * REVERSE-ORDER ERROR UNWIND LADDER:
 * Safely kills any already-submitted URBs BEFORE freeing DMA buffers and URBs,
 * preventing use-after-free if a late initialization step fails.
 */
err_kill_int_in:
    if (dev->has_int_in)
        usb_kill_urb(dev->int_in_urb);
err_kill_bulk_in:
    usb_kill_urb(dev->bulk_in_urb);
err_clear_intfdata:
    usb_set_intfdata(intf, NULL);
    if (dev->int_in_buf)
        usb_free_coherent(dev->udev, dev->int_in_maxpkt,
                          dev->int_in_buf, dev->dma_handle_int_in);
err_free_int_in_urb:
    usb_free_urb(dev->int_in_urb);
err_free_bulk_out_buf:
    usb_free_coherent(dev->udev, dev->bulk_out_maxpkt,
                      dev->bulk_out_buf, dev->dma_handle_bulk_out);
err_free_bulk_out_urb:
    usb_free_urb(dev->bulk_out_urb);
err_free_bulk_in_buf:
    usb_free_coherent(dev->udev, dev->bulk_in_maxpkt,
                      dev->bulk_in_buf, dev->dma_handle_bulk_in);
err_free_bulk_in_urb:
    usb_free_urb(dev->bulk_in_urb);
err_put_dev:
    usb_put_dev(dev->udev);
    kfifo_free(&dev->int_in_fifo);
    kfifo_free(&dev->bulk_in_fifo);
    kfree(dev);
    return ret;
}

/* ============================================================================
 * SECTION 10: USB DISCONNECT FUNCTION (DEVICE REMOVAL / CLEANUP)
 * ============================================================================
 * Function: usb_disconnect()
 * Context:  Process context (called by USB core when device is unplugged or
 *           when `rmmod` calls `usb_deregister()`). Sleeping is allowed.
 * Parameter:
 *   intf - Pointer to the `struct usb_interface` being unbound.
 */
static void usb_disconnect(struct usb_interface *intf)
{
    /* Retrieve our `struct usb_device_data *` stored on `intf`. */
    struct usb_device_data *dev = usb_get_intfdata(intf);

    /* Guard against NULL in case disconnect is called on an unbound interface. */
    if (!dev) {
        return;
    }

    /*
     * API `usb_deregister_dev(intf, &usb_class)`:
     *   Removes `/dev/usb_completeN` and releases the assigned minor number so
     *   no new processes can open this disconnected device.
     */
    usb_deregister_dev(intf, &usb_class);

    /*
     * Acquire `dev->io_mutex` to synchronize with any active line 1190: usb_open,
     * line 916: usb_read, or line 1069: usb_write:
     *   - Mark `dev->connected = false` so no new I/O or URB submissions start.
     *   - Clear `usb_set_intfdata(intf, NULL)`.
     * Note: `dev->intf` is intentionally kept valid until AFTER `usb_kill_urb()`
     * returns below so any in-flight completion callback logging via
     * `&dev->intf->dev` cannot dereference a NULL `dev->intf`.
     */
    mutex_lock(&dev->io_mutex);
    dev->connected = false;
    usb_set_intfdata(intf, NULL);
    mutex_unlock(&dev->io_mutex);

    /*
     * Synchronously cancel all active URBs and wait until their completion
     * callbacks have completely exited (and will not resubmit).
     */
    usb_kill_urb(dev->bulk_in_urb);
    usb_kill_urb(dev->bulk_out_urb);
    if (dev->int_in_urb) {
        usb_kill_urb(dev->int_in_urb);
    }

    /* Now that all URB callbacks have finished, clear `dev->intf` under io_mutex. */
    mutex_lock(&dev->io_mutex);
    dev->intf = NULL;
    mutex_unlock(&dev->io_mutex);

    /*
     * Wake ALL processes currently sleeping on any completion object so they
     * observe `dev->connected == false` and return `-ENODEV` immediately.
     */
    complete_all(&dev->bulk_in_ready);
    complete_all(&dev->int_in_ready);
    complete_all(&dev->bulk_out_done);

    /*
     * Drop the probe reference via `kref_put(&dev->kref, usb_delete)`.
     * If no user-space file descriptors are currently open, line 478: usb_delete
     * runs immediately; otherwise it runs when the last line 1260: usb_release runs.
     */
    kref_put(&dev->kref, usb_delete);

    /* Log disconnect using `&intf->dev` (still valid during disconnect). */
    dev_info(&intf->dev, "Device disconnected\n");
}

/* ============================================================================
 * SECTION 11: USB DEVICE ID TABLE & DRIVER REGISTRATION
 * ============================================================================
 */

/*
 * Definition: `static const struct usb_device_id usb_id_table[]`
 * Null-terminated array of Vendor ID / Product ID pairs claimed by this driver.
 */
static const struct usb_device_id usb_id_table[] = {
    /*
     * Macro `USB_DEVICE(vend, prod)`:
     *   Expands to `{ .match_flags = USB_DEVICE_ID_MATCH_DEVICE,
     *                 .idVendor = 0x1234, .idProduct = 0x5678 }`.
     *   Replace 0x1234 (VID) and 0x5678 (PID) with your actual hardware IDs.
     */
    { USB_DEVICE(0x1234, 0x5678) },

    /* All-zero sentinel entry terminating the USB ID table. */
    { }
};

/*
 * Macro `MODULE_DEVICE_TABLE(usb, usb_id_table)`:
 *   Exports `usb:v1234p5678...` modalias metadata into `.modinfo` so `depmod`
 *   and `udev` automatically load this module when a matching device plugs in.
 */
MODULE_DEVICE_TABLE(usb, usb_id_table);

/*
 * Definition: `static struct usb_driver usb_driver`
 * Main driver descriptor registered with the USB core.
 */
static struct usb_driver usb_driver = {
    .name       = "usb_driver_complete", /* Driver name in /sys/bus/usb/drivers/ */
    .id_table   = usb_id_table,          /* Supported VID/PID table (line 1693: usb_id_table) */
    .probe      = usb_probe,             /* Attach callback (line 1319: usb_probe) */
    .disconnect = usb_disconnect,        /* Detach callback (line 1619: usb_disconnect) */
};

/*
 * Macro `module_usb_driver(usb_driver)`:
 *   Boilerplate helper macro from <linux/usb.h> that generates both
 *   `module_init()` (calling `usb_register(&usb_driver)`) and `module_exit()`
 *   (calling `usb_deregister(&usb_driver)`), making this module loadable with
 *   `insmod` and unloadable with `rmmod`.
 */
module_usb_driver(usb_driver);