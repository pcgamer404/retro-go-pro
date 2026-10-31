// Earphone buttons through the USB audio dongle's HID interface (used by usb.c, same USB client).
//   volume up / down  -> system volume, 5% steps, repeats while held
//   middle button     -> short press: mute on/off, long press: options menu (virtual OPTION key)
// usb.c calls attach() once the dongle streams, detach() when it goes away and process() from its task.
#include "rg_system.h"

#if defined(RG_AUDIO_USE_USB) && RG_AUDIO_USE_USB

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <usb/usb_host.h>
#include <string.h>

#define TAG_HID "usbhid"
#define HID_NOTIFY_BIT 4 // EV_HID in usb.c

#define LONG_PRESS_US   600000
#define REPEAT_DELAY_US 500000
#define REPEAT_US       150000
#define OPTION_PULSE_MS 100
#define MAX_FIELDS      16
#define MAX_USAGES      16

enum { BTN_VOL_UP = 1, BTN_VOL_DOWN = 2, BTN_MID = 4 };

typedef struct
{
    uint8_t id, size, count, array, btn[MAX_USAGES];
    uint16_t bit, page;
    int32_t lmin;
    uint32_t umin;
} field_t;

typedef struct
{
    uint8_t btn, down;
} hid_ev_t;

static struct
{
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    TaskHandle_t task;
    SemaphoreHandle_t ctl_sem;
    usb_transfer_t *x;
    bool claimed, has_ids;
    volatile bool stop;
    volatile int inflight;
    uint8_t intf;
    int errors, nfields;
    field_t f[MAX_FIELDS];
    uint32_t prev;
    hid_ev_t evq[16];
    volatile uint8_t evq_w, evq_r;
    // button logic (usb_hid_process)
    uint32_t vol_held;
    int64_t vol_next, mid_t0;
    bool mid_down, mid_long;
} h;

// usages (page << 16 | id) that mean one of our three buttons
static uint32_t button_for_usage(uint32_t u)
{
    switch (u)
    {
    case 0x000C00E9: return BTN_VOL_UP;
    case 0x000C00EA: return BTN_VOL_DOWN;
    case 0x000C00CD: // play/pause
    case 0x000C00B0: // play
    case 0x000C00B1: // pause
    case 0x000C00E2: // mute
    case 0x000B0020: // telephony hook switch
    case 0x000B002F: // telephony phone mute
        return BTN_MID;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// HID report descriptor -> list of button fields
// ---------------------------------------------------------------------------------------------

static uint32_t parse_report_desc(const uint8_t *d, int len)
{
    uint32_t page = 0, size = 0, count = 0, rid = 0, umin = 0, umax = 0, usages[MAX_USAGES], found = 0;
    int32_t lmin = 0;
    int nus = 0;
    bool have_range = false;
    uint8_t ids[4] = {0};
    uint16_t pos[4] = {0};
    int nids = 1;

    h.nfields = 0;
    h.has_ids = false;
    for (int i = 0; i < len;)
    {
        const uint8_t pfx = d[i++];
        if (pfx == 0xFE) // long item: skip
        {
            if (i < len)
                i += 2 + d[i];
            continue;
        }
        int sz = pfx & 3;
        sz = sz == 3 ? 4 : sz;
        if (i + sz > len)
            break;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++)
            v |= (uint32_t)d[i + k] << (8 * k);
        int32_t sv = (int32_t)v;
        if (sz == 1)
            sv = (int8_t)v;
        else if (sz == 2)
            sv = (int16_t)v;
        i += sz;
        const int type = (pfx >> 2) & 3, tag = pfx >> 4;

        if (type == 1) // global
        {
            switch (tag)
            {
            case 0: page = v; break;
            case 1: lmin = sv; break;
            case 7: size = v; break;
            case 8:
                rid = v;
                h.has_ids = true;
                break;
            case 9: count = v; break;
            }
        }
        else if (type == 2) // local
        {
            if (tag == 0 && nus < MAX_USAGES)
                usages[nus++] = sz == 4 ? v : (page << 16) | v;
            else if (tag == 1)
                umin = v, have_range = true;
            else if (tag == 2)
                umax = v;
        }
        else if (type == 0) // main
        {
            if (tag == 8) // Input
            {
                int slot = 0;
                while (slot < nids && ids[slot] != rid)
                    slot++;
                if (slot == nids && nids < 4)
                    ids[nids++] = rid;
                if (slot >= 4)
                    slot = 3;
                if (!(v & 1) && size > 0 && size <= 32 && count > 0 && h.nfields < MAX_FIELDS) // not constant
                {
                    field_t *f = &h.f[h.nfields];
                    memset(f, 0, sizeof(*f));
                    f->id = rid;
                    f->size = size;
                    f->count = count > MAX_USAGES ? MAX_USAGES : count;
                    f->bit = pos[slot];
                    f->array = !(v & 2);
                    f->lmin = lmin;
                    f->umin = have_range ? umin : 0;
                    f->page = page;
                    if (!f->array)
                    {
                        bool any = false;
                        for (uint32_t j = 0; j < f->count; j++)
                        {
                            uint32_t u = j < (uint32_t)nus ? usages[j] : (have_range && umin + j <= umax ? (page << 16) | (umin + j) : 0);
                            f->btn[j] = u ? button_for_usage(u) : 0;
                            any |= f->btn[j] != 0;
                            found |= f->btn[j];
                        }
                        if (any)
                            h.nfields++;
                    }
                    else
                    {
                        found |= BTN_VOL_UP | BTN_VOL_DOWN | BTN_MID; // an array can carry any usage of its range
                        h.nfields++;
                    }
                }
                pos[slot] += size * count;
            }
            nus = 0;
            have_range = false;
            umin = umax = 0;
        }
    }
    return found;
}

static uint32_t get_bits(const uint8_t *r, int bit, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)((r[(bit + i) >> 3] >> ((bit + i) & 7)) & 1) << i;
    return v;
}

// bitmask of the buttons that are down in this report
static uint32_t decode_report(const uint8_t *r, int n)
{
    int id = 0, base = 0;
    uint32_t mask = 0;
    if (h.has_ids)
    {
        id = r[0];
        base = 8;
    }
    for (int k = 0; k < h.nfields; k++)
    {
        const field_t *f = &h.f[k];
        if (f->id != id)
            continue;
        for (int j = 0; j < f->count; j++)
        {
            const int bit = base + f->bit + j * f->size;
            if (bit + f->size > n * 8)
                break;
            const uint32_t val = get_bits(r, bit, f->size);
            if (!val)
                continue;
            if (!f->array)
                mask |= f->btn[j];
            else
                mask |= button_for_usage(((uint32_t)f->page << 16) | (f->umin + val - f->lmin));
        }
    }
    return mask;
}

// ---------------------------------------------------------------------------------------------
// USB side
// ---------------------------------------------------------------------------------------------

static void push_event(uint8_t btn, uint8_t down)
{
    const uint8_t w = h.evq_w;
    if ((uint8_t)(w - h.evq_r) >= 16)
        return; // full: drop
    h.evq[w & 15] = (hid_ev_t){btn, down};
    __sync_synchronize();
    h.evq_w = w + 1;
}

static void int_cb(usb_transfer_t *x)
{
    if (x->status == USB_TRANSFER_STATUS_NO_DEVICE || x->status == USB_TRANSFER_STATUS_CANCELED)
    {
        h.inflight = 0;
        return;
    }
    if (x->status == USB_TRANSFER_STATUS_COMPLETED && x->actual_num_bytes > 0)
    {
        const uint8_t *r = x->data_buffer;
        const int n = x->actual_num_bytes;
        h.errors = 0;
        const uint32_t now = decode_report(r, n);
        const uint32_t previous = h.prev;
        const uint32_t diff = now ^ previous;
        if (diff)
            RG_LOGD("[" TAG_HID "] button state changed: decoded=0x%lx previous=0x%lx",
                    (unsigned long)now, (unsigned long)previous);
        h.prev = now;
        for (uint8_t b = 1; b <= BTN_MID; b <<= 1)
            if (diff & b)
                push_event(b, (now & b) != 0);
        if (diff && h.task)
            xTaskNotify(h.task, HID_NOTIFY_BIT, eSetBits);
    }
    else if (++h.errors > 50)
    {
        RG_LOGW("[" TAG_HID "] too many errors (status %d), buttons off", x->status);
        h.inflight = 0;
        return;
    }
    if (!h.stop && usb_host_transfer_submit(x) == ESP_OK)
        return;
    h.inflight = 0;
}

static void ctl_cb(usb_transfer_t *x)
{
    xSemaphoreGive((SemaphoreHandle_t)x->context);
}

// GET_DESCRIPTOR(report) of the HID interface, up to 256 bytes
static int read_report_desc(uint8_t *out, int len)
{
    const int dlen = (len + 63) & ~63; // IN data stage: multiple of the control endpoint's packet size
    usb_transfer_t *t = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + dlen, 0, &t) != ESP_OK)
        return -1;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = 0x81;
    s->bRequest = 0x06;
    s->wValue = 0x2200;
    s->wIndex = h.intf;
    s->wLength = len;
    t->num_bytes = sizeof(*s) + dlen;
    t->device_handle = h.dev;
    t->bEndpointAddress = 0;
    t->callback = ctl_cb;
    t->context = h.ctl_sem;
    int got = -1;
    bool done = true;
    if (usb_host_transfer_submit_control(h.client, t) == ESP_OK)
    {
        if (xSemaphoreTake(h.ctl_sem, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > (int)sizeof(*s))
            {
                got = t->actual_num_bytes - sizeof(*s);
                if (got > len)
                    got = len;
                memcpy(out, t->data_buffer + sizeof(*s), got);
            }
        }
        else
            done = false; // still owned by the USB stack: leak it
    }
    if (done)
        usb_host_transfer_free(t);
    return got;
}

void usb_hid_detach(void)
{
    h.stop = true;
    for (int i = 0; i < 100 && h.inflight > 0; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (h.x && h.inflight == 0)
        usb_host_transfer_free(h.x);
    if (h.claimed && h.inflight == 0)
        usb_host_interface_release(h.client, h.dev, h.intf);
    h.x = NULL;
    h.claimed = false;
    h.inflight = 0;
    h.prev = 0;
    h.vol_held = 0;
    h.mid_down = false;
}

void usb_hid_attach(usb_host_client_handle_t client, usb_device_handle_t dev, const usb_config_desc_t *cfg, TaskHandle_t task)
{
    if (h.claimed)
        return;
    if (!h.ctl_sem)
        h.ctl_sem = xSemaphoreCreateBinary();
    h.client = client;
    h.dev = dev;
    h.task = task;
    h.stop = false;
    h.errors = 0;
    h.prev = 0;
    h.evq_r = h.evq_w = 0;

    // the first HID interface with an interrupt IN endpoint
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    int intf = -1, rd_len = 0, ep = 0, mps = 0;
    bool in_hid = false;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end && ep == 0)
    {
        if (p[1] == 0x04 && p[0] >= 9)
            in_hid = p[5] == 0x03 && p[3] == 0, intf = p[2], rd_len = 0;
        else if (p[1] == 0x21 && in_hid && p[0] >= 9)
            rd_len = p[7] | p[8] << 8;
        else if (p[1] == 0x05 && in_hid && p[0] >= 7 && (p[2] & 0x80) && (p[3] & 3) == 3)
            ep = p[2], mps = p[4] | p[5] << 8;
        p += p[0];
    }
    if (!ep || mps <= 0 || mps > 64)
    {
        RG_LOGI("[" TAG_HID "] no HID interface: no earphone buttons");
        return;
    }
    h.intf = intf;
    if (usb_host_interface_claim(client, dev, intf, 0) != ESP_OK)
    {
        RG_LOGW("[" TAG_HID "] cannot claim HID interface %d", intf);
        return;
    }
    h.claimed = true;

    uint8_t rd[256];
    if (rd_len > (int)sizeof(rd))
        rd_len = sizeof(rd);
    const int n = rd_len > 0 ? read_report_desc(rd, rd_len) : -1;
    uint32_t found = 0;
    h.nfields = 0;
    if (n > 0)
    {
        found = parse_report_desc(rd, n);
        for (int i = 0; i < n; i += 24)
        {
            char s[24 * 3 + 1] = "";
            for (int j = 0; j < 24 && i + j < n; j++)
                snprintf(&s[j * 3], 4, "%02x ", rd[i + j]);
            RG_LOGD("[" TAG_HID "] report descriptor[%03d] %s", i, s);
        }
    }
    RG_LOGI("[" TAG_HID "] HID interface %d, endpoint 0x%02x (%d bytes), report descriptor %d bytes -> %d usable fields, volume up %s, volume down %s, middle %s",
            intf, ep, mps, n, h.nfields, found & BTN_VOL_UP ? "yes" : "no", found & BTN_VOL_DOWN ? "yes" : "no", found & BTN_MID ? "yes" : "no");

    if (usb_host_transfer_alloc(mps, 0, &h.x) != ESP_OK)
    {
        usb_hid_detach();
        return;
    }
    h.x->device_handle = dev;
    h.x->bEndpointAddress = ep;
    h.x->callback = int_cb;
    h.x->context = NULL;
    h.x->num_bytes = mps;
    h.inflight = 1;
    if (usb_host_transfer_submit(h.x) != ESP_OK)
    {
        h.inflight = 0;
        usb_hid_detach();
    }
}

// ---------------------------------------------------------------------------------------------
// Button actions (called from usb.c's task)
// ---------------------------------------------------------------------------------------------

static void volume_step(int delta)
{
    const char *driver = rg_audio_get_driver();
    if (!driver)
    {
        RG_LOGW("[" TAG_HID "] volume button ignored: no active audio driver (delta %d)", delta);
        return;
    }
    RG_LOGD("[" TAG_HID "] volume button delta %+d, driver=%s, current=%d",
            delta, driver, rg_audio_get_volume());
    int v = rg_audio_get_volume() + delta;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (rg_audio_get_mute())
        rg_audio_set_mute(false);
    rg_audio_set_volume(v);
}

static void toggle_mute(void)
{
    if (rg_audio_get_driver())
        rg_audio_set_mute(!rg_audio_get_mute());
}

static void open_options(void)
{
    rg_input_inject_keys(RG_KEY_OPTION); // the apps see a normal OPTION press; menus wait for its release
    vTaskDelay(pdMS_TO_TICKS(OPTION_PULSE_MS));
    rg_input_inject_keys(0);
}

// handles queued button events and the hold timers; returns ms until it must run again (-1 = no deadline)
int usb_hid_process(void)
{
    while (h.evq_r != h.evq_w)
    {
        const hid_ev_t ev = h.evq[h.evq_r & 15];
        __sync_synchronize();
        h.evq_r++;
        const int64_t now = rg_system_timer();
        if (ev.btn == BTN_VOL_UP || ev.btn == BTN_VOL_DOWN)
        {
            if (ev.down)
            {
                volume_step(ev.btn == BTN_VOL_UP ? 5 : -5);
                h.vol_held = ev.btn;
                h.vol_next = rg_system_timer() + REPEAT_DELAY_US;
            }
            else if (h.vol_held == ev.btn)
                h.vol_held = 0;
        }
        else if (ev.btn == BTN_MID)
        {
            if (ev.down)
            {
                h.mid_down = true;
                h.mid_long = false;
                h.mid_t0 = now;
            }
            else
            {
                if (h.mid_down && !h.mid_long)
                    toggle_mute();
                h.mid_down = false;
            }
        }
    }

    int64_t now = rg_system_timer();
    if (h.vol_held && now >= h.vol_next)
    {
        volume_step(h.vol_held == BTN_VOL_UP ? 5 : -5);
        h.vol_next = rg_system_timer() + REPEAT_US;
    }
    if (h.mid_down && !h.mid_long && now - h.mid_t0 >= LONG_PRESS_US)
    {
        h.mid_long = true;
        open_options();
    }

    int64_t next = INT64_MAX;
    if (h.vol_held)
        next = h.vol_next;
    if (h.mid_down && !h.mid_long && h.mid_t0 + LONG_PRESS_US < next)
        next = h.mid_t0 + LONG_PRESS_US;
    if (next == INT64_MAX)
        return -1;
    const int64_t ms = (next - rg_system_timer()) / 1000;
    return ms < 1 ? 1 : (int)ms;
}

#endif // RG_AUDIO_USE_USB
