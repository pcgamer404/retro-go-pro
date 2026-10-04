// USB audio sink for retro-go: plays through a USB-C / USB-A "DAC dongle" plugged into the
// ESP32-S3's native USB (OTG) port. ESP32-S3 acts as USB host, the dongle must be a plain
// USB Audio Class 1.0 playback device (almost every 3.5mm USB-C dongle is: 2ch, 16 bit, 48 or 44.1 kHz).
//
// How it works
//  - daemon task (core 1): installs the ESP-IDF USB host library and pumps its events.
//  - client task (core 1): receives "device plugged / unplugged" and the isochronous transfer
//    callbacks. Each callback refills one 10 ms transfer from the ring buffer and resubmits it.
//  - setup task: parses the dongle's descriptors, picks 2ch/16bit at 48000 (else 44100), selects the
//    alternate setting, sets the sample rate and starts 4 queued transfers (40 ms in the pipe).
//  - submit() (emulator task): resamples the core's rate (32768 Hz for gpSP) to the dongle's rate with
//    linear interpolation, applies the volume and writes into the ring. While the ring is full it waits,
//    so the dongle's clock paces the emulator exactly like an I2S DAC would.
//  - No dongle (or unplugged): submit() just sleeps for the duration of the audio, like the Dummy sink.
//    Hot-plug works: plug it in at any time.
#include "rg_system.h"

#if defined(RG_AUDIO_USE_USB) && RG_AUDIO_USE_USB

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_intr_alloc.h>
#include <usb/usb_host.h>
#include <string.h>

#define TAG_USB "usbaudio"

#define XFER_COUNT    4          // transfers queued at once
#define XFER_PACKETS  10         // 1 ms (full speed frame) each -> 10 ms per transfer
#define RING_FRAMES   4096       // stereo frames at the dongle's rate (power of 2)
#define RING_MASK     (RING_FRAMES - 1)
#define HIGH_WATER    3072       // submit() waits while more than this is queued (~64 ms at 48 kHz)
#define PREFILL       1440       // after an underrun, play again only when this much is queued (~30 ms)

// FIFO split. The ESP32-S3 USB core has 200 lines (4 bytes each) of FIFO shared by RX, non-periodic TX and
// periodic TX (the numbers in ESP-IDF's Kconfig bias help add up to exactly 200; install asserts if the sum is
// larger). IDF's default split gives isochronous OUT only 32 lines = 128 bytes per packet, but this kind of
// dongle declares a 384-byte endpoint (48 kHz stereo 16-bit needs only 192):
//   "EP MPS (384) exceeds supported limit (128)" -> "Claiming interface error: ESP_ERR_NOT_SUPPORTED".
// So give the periodic TX FIFO nearly everything: RX 32 lines (IN limit 120 bytes, enough for 64-byte control
// packets), non-periodic TX 16 lines (64 bytes, the control endpoint's size), periodic TX 140 lines (560 bytes).
// 188 of 200 lines used, 12 spare. Set here in code so it applies in every app, whatever the Kconfig bias is.
#define FIFO_RX_LINES    32
#define FIFO_NPTX_LINES  16
#define FIFO_PTX_LINES   140
#define MAX_OUT_MPS      (FIFO_PTX_LINES * 4)
_Static_assert(FIFO_RX_LINES + FIFO_NPTX_LINES + FIFO_PTX_LINES <= 200, "USB FIFO split larger than the hardware FIFO");

enum { EV_NEW_DEV = 1, EV_GONE = 2 };

typedef struct
{
    uint8_t intf, alt, ep;
    uint16_t mps;
    uint32_t rate;
} uac_fmt_t;

static struct
{
    // setup / state
    bool started, enabled;
    volatile bool streaming, stop;
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    TaskHandle_t uac_task;
    SemaphoreHandle_t ctl_sem, space_sem, ready_sem;
    uint8_t new_addr;
    uac_fmt_t fmt;
    usb_transfer_t *xfers[XFER_COUNT];
    volatile int inflight;
    bool intf_claimed;
    // audio
    uint32_t ring[RING_FRAMES];
    volatile uint32_t wr, rd;
    bool buffering;
    uint32_t wait_frames;         // consumer: silent frames output while waiting for PREFILL
    uint32_t acc;                 // consumer: fractional frames per ms
    uint32_t src_rate, dst_rate;
    uint32_t step, pos;           // producer: resampler step and position, 16.16
    int pl, pr;                   // producer: previous source frame
    int gain;                     // 0..256
    int volume;
    bool muted;
    uint32_t overruns, underruns;
    uint32_t iso_ok, iso_err, iso_bad_packets;   // how the USB controller completes our transfers
    int peak_in, peak_out;                       // loudest sample from the emulator / sent to the dongle (0 = silence)
    uint32_t submits, last_count;
    int ctl_status;
    const char *error;
} u;

typedef struct
{
    uint8_t id, ac_intf, ctrl[3];   // feature unit id, audio control interface, bmaControls of master/left/right
} fu_info_t;

// ---------------------------------------------------------------------------------------------
// Descriptor parsing (UAC 1.0)
// ---------------------------------------------------------------------------------------------

static bool rate_supported(const uint8_t *fmt, uint32_t rate)
{
    int type = fmt[7];
    if (type == 0) // continuous: lower, upper
    {
        uint32_t lo = fmt[8] | fmt[9] << 8 | fmt[10] << 16;
        uint32_t hi = fmt[11] | fmt[12] << 8 | fmt[13] << 16;
        return rate >= lo && rate <= hi;
    }
    for (int i = 0; i < type; i++)
    {
        const uint8_t *f = &fmt[8 + i * 3];
        if ((uint32_t)(f[0] | f[1] << 8 | f[2] << 16) == rate)
            return true;
    }
    return false;
}

// the whole configuration descriptor as hex in the log: if a dongle is ever refused, this shows why
static void dump_config(const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const int total = cfg->wTotalLength;
    char line[24 * 3 + 1];
    for (int i = 0; i < total; i += 24)
    {
        const int n = total - i < 24 ? total - i : 24;
        for (int j = 0; j < n; j++)
            snprintf(&line[j * 3], 4, "%02x ", p[i + j]);
        RG_LOGI("[" TAG_USB "] cfg[%03d] %s", i, line);
    }
}

static bool parse_config(const usb_config_desc_t *cfg, uac_fmt_t *best)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    int intf = -1, alt = 0;
    bool in_as = false, pcm = false, fmt_ok = false, uac2 = false;
    uint32_t rate = 0;
    int found_as = 0;

    memset(best, 0, sizeof(*best));

    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end)
    {
        const uint8_t len = p[0], type = p[1];

        if (type == 0x04 && len >= 9) // interface
        {
            intf = p[2];
            alt = p[3];
            in_as = (p[5] == 0x01 && p[6] == 0x02); // audio / streaming
            if (in_as)
            {
                found_as++;
                if (p[7] == 0x20)
                    uac2 = true;
            }
            pcm = fmt_ok = false;
            rate = 0;
        }
        else if (type == 0x24 && in_as && len >= 6 && p[2] == 0x01) // AS_GENERAL
        {
            pcm = (p[5] | p[6] << 8) == 1; // PCM
        }
        else if (type == 0x24 && in_as && len >= 8 && p[2] == 0x02 && p[3] == 0x01) // FORMAT_TYPE_I
        {
            // p[4]=channels p[5]=subframe bytes p[6]=bits
            fmt_ok = pcm && p[4] == 2 && p[5] == 2 && p[6] == 16;
            rate = 0;
            if (fmt_ok && len >= 8 + (p[7] ? p[7] * 3 : 6))
            {
                if (rate_supported(p, 48000))
                    rate = 48000;
                else if (rate_supported(p, 44100))
                    rate = 44100;
            }
        }
        else if (type == 0x05 && in_as && fmt_ok && rate && len >= 7) // endpoint
        {
            const uint8_t addr = p[2], attr = p[3];
            const uint16_t mps = (p[4] | p[5] << 8) & 0x7FF;
            const uint16_t need = ((rate + 999) / 1000) * 4;
            if (!(addr & 0x80) && (attr & 3) == 1 && mps >= need && mps <= MAX_OUT_MPS)
            {
                // prefer 48000, then the smallest packet size that fits
                bool better = !best->rate || (rate == 48000 && best->rate != 48000)
                              || (rate == best->rate && mps < best->mps);
                if (better)
                    *best = (uac_fmt_t){.intf = intf, .alt = alt, .ep = addr, .mps = mps, .rate = rate};
            }
        }
        p += len;
    }

    if (!best->rate)
    {
        u.error = uac2 ? "UAC2 device (only USB Audio Class 1 is supported)"
                       : found_as ? "no 2ch/16-bit 48k/44.1k playback format" : "not a USB audio device";
        return false;
    }
    return true;
}

// Find the feature unit (mute / volume) of the playback path: output terminal of a speaker/headphone type
// (0x03xx) <- feature unit. Many dongles power up quiet or muted until the host sets them.
static bool find_playback_fu(const usb_config_desc_t *cfg, fu_info_t *out)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    struct { uint8_t id, src, n, ctrl[3]; } fu[8];
    struct { uint16_t type; uint8_t src; } ot[8];
    int nfu = 0, not_ = 0, ac_intf = -1;
    bool in_ac = false;

    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end)
    {
        const uint8_t len = p[0], type = p[1];
        if (type == 0x04 && len >= 9)
        {
            in_ac = (p[5] == 0x01 && p[6] == 0x01); // audio / control
            if (in_ac && ac_intf < 0)
                ac_intf = p[2];
        }
        else if (type == 0x24 && in_ac && len >= 9 && p[2] == 0x03 && not_ < 8) // OUTPUT_TERMINAL
        {
            ot[not_].type = p[4] | p[5] << 8;
            ot[not_].src = p[7];
            not_++;
        }
        else if (type == 0x24 && in_ac && len >= 8 && p[2] == 0x06 && nfu < 8 && p[5] >= 1) // FEATURE_UNIT
        {
            fu[nfu].id = p[3];
            fu[nfu].src = p[4];
            const int csize = p[5];
            fu[nfu].n = 0;
            for (int ch = 0; ch < 3; ch++)
            {
                fu[nfu].ctrl[ch] = (6 + (ch + 1) * csize <= len) ? p[6 + ch * csize] : 0;
            }
            nfu++;
        }
        p += len;
    }
    for (int i = 0; i < not_; i++)
    {
        if ((ot[i].type & 0xFF00) != 0x0300)
            continue;
        for (int k = 0; k < nfu; k++)
        {
            if (fu[k].id == ot[i].src && ac_intf >= 0)
            {
                out->id = fu[k].id;
                out->ac_intf = ac_intf;
                memcpy(out->ctrl, fu[k].ctrl, 3);
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Streaming (runs in the client task, from the transfer callbacks)
// ---------------------------------------------------------------------------------------------

static inline uint32_t take_frame(void)
{
    uint32_t fill = u.wr - u.rd;
    if (u.buffering)
    {
        // start when PREFILL is queued, or after ~100 ms of waiting with anything queued (a slow emulator
        // would otherwise leave a partial buffer stuck forever)
        if (fill < PREFILL && !(fill > 0 && ++u.wait_frames > 4800))
            return 0;
        u.buffering = false;
        u.wait_frames = 0;
    }
    if (fill == 0)
    {
        u.buffering = true;
        u.underruns++;
        return 0;
    }
    uint32_t f = u.ring[u.rd & RING_MASK];
    __sync_synchronize();
    u.rd++;
    return f;
}

static void fill_transfer(usb_transfer_t *x)
{
    uint32_t *dst = (uint32_t *)x->data_buffer;
    int total = 0;
    for (int i = 0; i < XFER_PACKETS; i++)
    {
        u.acc += u.dst_rate; // frames per millisecond, fractional (44.1 kHz -> 44/45)
        uint32_t n = u.acc / 1000;
        u.acc -= n * 1000;
        for (uint32_t j = 0; j < n; j++)
        {
            const uint32_t f = take_frame();
            int a = (int16_t)f;
            a = a < 0 ? -a : a;
            if (a > u.peak_out)
                u.peak_out = a;
            *dst++ = f;
        }
        x->isoc_packet_desc[i].num_bytes = n * 4;
        total += n * 4;
    }
    x->num_bytes = total;
}

static void iso_cb(usb_transfer_t *x)
{
    if (x->status == USB_TRANSFER_STATUS_COMPLETED)
    {
        u.iso_ok++;
        for (int i = 0; i < XFER_PACKETS; i++)
            if (x->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED)
                u.iso_bad_packets++;
    }
    else if (x->status != USB_TRANSFER_STATUS_NO_DEVICE && x->status != USB_TRANSFER_STATUS_CANCELED)
    {
        u.iso_err++;
    }
    bool resubmit = !u.stop && x->status != USB_TRANSFER_STATUS_NO_DEVICE && x->status != USB_TRANSFER_STATUS_CANCELED;
    if (resubmit)
    {
        fill_transfer(x);
        if (usb_host_transfer_submit(x) == ESP_OK)
        {
            xSemaphoreGive(u.space_sem);
            return;
        }
    }
    u.inflight--;
}

// ---------------------------------------------------------------------------------------------
// Device handling (setup task)
// ---------------------------------------------------------------------------------------------

static void ctl_cb(usb_transfer_t *x)
{
    xSemaphoreGive((SemaphoreHandle_t)x->context);
}

static void teardown(void)
{
    u.streaming = false;
    u.stop = true;
    for (int i = 0; i < 100 && u.inflight > 0; i++) // transfers come back with NO_DEVICE / CANCELED
        vTaskDelay(pdMS_TO_TICKS(10));
    if (u.inflight > 0)
        RG_LOGW("[" TAG_USB "] %d transfers did not come back", u.inflight);
    for (int i = 0; i < XFER_COUNT; i++)
    {
        if (u.xfers[i] && u.inflight == 0)
            usb_host_transfer_free(u.xfers[i]);
        u.xfers[i] = NULL;
    }
    if (u.dev)
    {
        if (u.intf_claimed)
            usb_host_interface_release(u.client, u.dev, u.fmt.intf);
        usb_host_device_close(u.client, u.dev);
    }
    u.intf_claimed = false;
    u.dev = NULL;
    u.inflight = 0;
    RG_LOGI("[" TAG_USB "] device removed");
}

// One control transfer on endpoint 0 (class requests to the audio interfaces / endpoint). IN data is limited to 64 bytes.
static bool uac_ctrl(uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    const bool in = (reqtype & 0x80) != 0;
    const size_t dlen = in ? 64 : len; // IN transfers must be a multiple of the endpoint 0 packet size
    usb_transfer_t *t = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + dlen, 0, &t) != ESP_OK)
        return false;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = reqtype;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = len;
    if (!in && len)
        memcpy(t->data_buffer + sizeof(*s), data, len);
    t->num_bytes = sizeof(*s) + dlen;
    t->device_handle = u.dev;
    t->bEndpointAddress = 0;
    t->callback = ctl_cb;
    t->context = u.ctl_sem;
    bool ok = false, done = true;
    u.ctl_status = -1;
    if (usb_host_transfer_submit_control(u.client, t) == ESP_OK)
    {
        if (xSemaphoreTake(u.ctl_sem, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            u.ctl_status = t->status;
            ok = t->status == USB_TRANSFER_STATUS_COMPLETED;
            if (ok && in && len && t->actual_num_bytes >= (int)(sizeof(*s) + len))
                memcpy(data, t->data_buffer + sizeof(*s), len);
            else if (ok && in)
                ok = false;
        }
        else
            done = false; // still owned by the USB stack: leak it rather than free it under the controller
    }
    if (done)
        usb_host_transfer_free(t);
    return ok;
}

static void fu_init(const fu_info_t *f)
{
    const uint16_t idx = (uint16_t)(f->id << 8) | f->ac_intf;
    uint8_t b[2];
    RG_LOGI("[" TAG_USB "] playback feature unit %d (audio control interface %d), controls master/L/R: %02x %02x %02x",
            f->id, f->ac_intf, f->ctrl[0], f->ctrl[1], f->ctrl[2]);
    if (f->ctrl[0] & 0x01) // mute
    {
        bool had = uac_ctrl(0xA1, 0x81, 0x0100, idx, b, 1);
        const int was = had ? b[0] : -1;
        b[0] = 0;
        bool ok = uac_ctrl(0x21, 0x01, 0x0100, idx, b, 1);
        RG_LOGI("[" TAG_USB "] mute was %d, unmute request %s (status %d)", was, ok ? "ok" : "FAILED", u.ctl_status);
    }
    for (int ch = 0; ch < 3; ch++)
    {
        if (!(f->ctrl[ch] & 0x02)) // volume
            continue;
        const uint16_t val = 0x0200 | ch;
        bool okc = uac_ctrl(0xA1, 0x81, val, idx, b, 2);
        const int cur = okc ? (int16_t)(b[0] | b[1] << 8) : 0;
        bool okx = uac_ctrl(0xA1, 0x83, val, idx, b, 2);
        const int max = okx ? (int16_t)(b[0] | b[1] << 8) : 0;
        bool oks = false;
        if (okx)
            oks = uac_ctrl(0x21, 0x01, val, idx, b, 2); // b still holds max
        RG_LOGI("[" TAG_USB "] volume ch%d: current %s%d max %s%d (1/256 dB) -> set to max %s", ch, okc ? "" : "?", cur,
                okx ? "" : "?", max, oks ? "ok" : "skipped/FAILED");
    }
}

static void setup_device(uint8_t addr)
{
    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;

    if (u.dev)
        return; // one dongle at a time
    if (usb_host_device_open(u.client, addr, &u.dev) != ESP_OK)
    {
        u.dev = NULL;
        return;
    }
    usb_host_get_device_descriptor(u.dev, &dd);
    if (usb_host_get_active_config_descriptor(u.dev, &cd) != ESP_OK || !cd)
        goto fail;
    RG_LOGI("[" TAG_USB "] device %04x:%04x connected", dd ? dd->idVendor : 0, dd ? dd->idProduct : 0);

    if (!parse_config(cd, &u.fmt))
    {
        dump_config(cd);
        RG_LOGW("[" TAG_USB "] cannot use this device: %s", u.error);
        goto fail;
    }
    RG_LOGI("[" TAG_USB "] using interface %d alt %d, endpoint 0x%02x, packet %d, %u Hz stereo 16-bit",
            u.fmt.intf, u.fmt.alt, u.fmt.ep, u.fmt.mps, (unsigned)u.fmt.rate);

    if (usb_host_interface_claim(u.client, u.dev, u.fmt.intf, u.fmt.alt) != ESP_OK)
    {
        u.error = "cannot claim the audio interface";
        RG_LOGW("[" TAG_USB "] %s", u.error);
        goto fail;
    }
    u.intf_claimed = true;

    // SET_CUR sampling frequency on the endpoint (class request). A device with a single fixed rate may STALL: harmless.
    {
        uint8_t rate3[3] = {u.fmt.rate & 0xFF, (u.fmt.rate >> 8) & 0xFF, (u.fmt.rate >> 16) & 0xFF};
        if (!uac_ctrl(0x22, 0x01, 0x0100, u.fmt.ep, rate3, 3))
            RG_LOGI("[" TAG_USB "] sample rate request answered with status %d (ok for fixed-rate devices)", u.ctl_status);
    }

    // unmute and set the volume to maximum: dongles often start quiet (Windows does this from its mixer)
    {
        fu_info_t fu;
        if (find_playback_fu(cd, &fu))
            fu_init(&fu);
        else
            RG_LOGI("[" TAG_USB "] no mute/volume unit found in the playback path");
    }

    // start streaming
    u.dst_rate = u.fmt.rate;
    u.step = (uint32_t)(((uint64_t)u.src_rate << 16) / u.dst_rate);
    u.pos = 65536;
    u.pl = u.pr = 0;
    u.wr = u.rd = 0;
    u.acc = 0;
    u.buffering = true;
    u.wait_frames = 0;
    u.stop = false;
    u.inflight = 0;

    const int pkt_max = ((u.fmt.rate + 999) / 1000) * 4;
    for (int i = 0; i < XFER_COUNT; i++)
    {
        if (usb_host_transfer_alloc(pkt_max * XFER_PACKETS, XFER_PACKETS, &u.xfers[i]) != ESP_OK)
        {
            u.error = "out of memory for the USB transfers";
            RG_LOGW("[" TAG_USB "] %s", u.error);
            goto fail;
        }
        usb_transfer_t *x = u.xfers[i];
        x->device_handle = u.dev;
        x->bEndpointAddress = u.fmt.ep;
        x->callback = iso_cb;
        x->context = NULL;
    }
    for (int i = 0; i < XFER_COUNT; i++)
    {
        fill_transfer(u.xfers[i]);
        if (usb_host_transfer_submit(u.xfers[i]) != ESP_OK)
        {
            u.error = "cannot submit the audio transfers";
            RG_LOGW("[" TAG_USB "] %s", u.error);
            u.stop = true;
            goto fail;
        }
        u.inflight++;
    }
    u.streaming = true;
    RG_LOGI("[" TAG_USB "] streaming");
    return;

fail:
    teardown();
}

static void print_status(void);

static void uac_task(void *arg)
{
    for (;;)
    {
        uint32_t ev = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(5000)) != pdTRUE)
        {
            print_status();
            continue;
        }
        if (ev & EV_GONE)
            teardown();
        if (ev & EV_NEW_DEV)
            setup_device(u.new_addr);
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        u.new_addr = msg->new_dev.address;
        xTaskNotify(u.uac_task, EV_NEW_DEV, eSetBits);
    }
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
    {
        if (u.dev && msg->dev_gone.dev_hdl == u.dev)
        {
            u.streaming = false;
            u.stop = true;
            xTaskNotify(u.uac_task, EV_GONE, eSetBits);
        }
    }
}

static void client_task(void *arg)
{
    for (;;)
        usb_host_client_handle_events(u.client, portMAX_DELAY);
}

static void daemon_task(void *arg)
{
    usb_host_config_t cfg = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1, // installed here: the USB interrupt runs on core 1
        .fifo_settings_custom = {.nptx_fifo_lines = FIFO_NPTX_LINES, .ptx_fifo_lines = FIFO_PTX_LINES, .rx_fifo_lines = FIFO_RX_LINES},
    };
    esp_err_t err = usb_host_install(&cfg);
    if (err == ESP_OK)
    {
        usb_host_client_config_t ccfg = {
            .is_synchronous = false,
            .max_num_event_msg = 8,
            .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
        };
        err = usb_host_client_register(&ccfg, &u.client);
        if (err == ESP_OK)
        {
            // uac_task first: the client callback notifies it as soon as the client task runs
            xTaskCreatePinnedToCore(uac_task, "usb_uac", 4096, NULL, 5, &u.uac_task, 1);
            xTaskCreatePinnedToCore(client_task, "usb_client", 4096, NULL, 7, NULL, 1);
            u.started = true;
        }
    }
    if (err != ESP_OK)
    {
        u.error = "USB host install failed";
        RG_LOGE("[" TAG_USB "] USB host install failed: %d", err);
    }
    xSemaphoreGive(u.ready_sem);
    if (err != ESP_OK)
        vTaskDelete(NULL);

    for (;;)
    {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

// ---------------------------------------------------------------------------------------------
// rg_audio driver interface
// ---------------------------------------------------------------------------------------------

static void update_gain(void)
{
    // squared curve: 100% = unity, 50% = -12 dB
    u.gain = u.muted ? 0 : (u.volume * u.volume * 256) / 10000;
}

static void set_rates(int sample_rate)
{
    u.src_rate = sample_rate;
    if (u.dst_rate)
        u.step = (uint32_t)(((uint64_t)u.src_rate << 16) / u.dst_rate);
}

static bool driver_init(int device, int sample_rate)
{
    u.enabled = true;
    u.volume = 50;
    set_rates(sample_rate);
    update_gain();

    if (u.started)
        return true;

    u.error = NULL;
    u.ctl_sem = xSemaphoreCreateBinary();
    u.space_sem = xSemaphoreCreateBinary();
    u.ready_sem = xSemaphoreCreateBinary();
    if (!u.ctl_sem || !u.space_sem || !u.ready_sem)
    {
        u.error = "out of memory";
        return false;
    }
    if (xTaskCreatePinnedToCore(daemon_task, "usb_daemon", 4096, NULL, 6, NULL, 1) != pdPASS)
    {
        u.error = "cannot create the USB task";
        return false;
    }
    xSemaphoreTake(u.ready_sem, pdMS_TO_TICKS(2000));
    if (u.started)
        RG_LOGI("[" TAG_USB "] USB host started, waiting for a dongle (plug it in any time)");
    if (!u.started)
    {
        if (!u.error)
            u.error = "USB host did not start";
        return false;
    }
    return true;
}

static bool driver_deinit(void)
{
    // The USB stack stays installed (it cannot be reinstalled cleanly while a dongle is attached);
    // submit() simply goes back to sleeping for the duration of the audio.
    u.enabled = false;
    return true;
}

// every 5 s from the usb_uac task (printf needs stack the emulator's audio task does not have)
static void print_status(void)
{
    static int idle_logs;
    if (!u.enabled)
        return;
    const int pin = u.peak_in, pout = u.peak_out;
    u.peak_in = u.peak_out = 0;
    if (u.streaming)
        RG_LOGD("[" TAG_USB "] streaming: buffer %u frames, underruns %u, overruns %u | submits %u (last %u samples) | peak in %d out %d (0 = silence) | gain %d/256%s | usb ok %u err %u bad packets %u",
                (unsigned)(u.wr - u.rd), (unsigned)u.underruns, (unsigned)u.overruns, (unsigned)u.submits, (unsigned)u.last_count,
                pin, pout, u.gain, u.muted ? " MUTED" : "", (unsigned)u.iso_ok, (unsigned)u.iso_err, (unsigned)u.iso_bad_packets);
    else if (idle_logs++ < 3)
        RG_LOGI("[" TAG_USB "] no audio device streaming (host %s, device %s, last error: %s)",
                u.started ? "running" : "NOT running", u.dev ? "open" : "none", u.error ? u.error : "none");
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    u.submits++;
    u.last_count = count;
    if (!u.enabled || !u.streaming)
    {
        rg_usleep((uint32_t)(((uint64_t)count * 1000000) / (u.src_rate ? u.src_rate : 32768)));
        return true;
    }

    // the dongle's clock is the master: wait until the ring has room (never more than 100 ms)
    const int64_t t0 = rg_system_timer();
    while ((uint32_t)(u.wr - u.rd) > HIGH_WATER && u.streaming)
    {
        xSemaphoreTake(u.space_sem, pdMS_TO_TICKS(10));
        if (rg_system_timer() - t0 > 100000)
            break;
    }

    uint32_t wr = u.wr;
    const uint32_t rd = u.rd;
    const uint32_t gain = u.gain, step = u.step;
    uint32_t pos = u.pos;
    int pl = u.pl, pr = u.pr;

    int peak = 0;
    for (size_t i = 0; i < count; i++)
    {
        const int l1 = frames[i].left, r1 = frames[i].right;
        const int m = (l1 < 0 ? -l1 : l1) | (r1 < 0 ? -r1 : r1);
        if (m > peak)
            peak = m;
        while (pos < 65536)
        {
            if (wr - rd >= RING_FRAMES)
            {
                u.overruns++;
                break;
            }
            const int f = (int)(pos >> 4);
            int l = pl + (((l1 - pl) * f) >> 12);
            int r = pr + (((r1 - pr) * f) >> 12);
            l = (l * (int)gain) >> 8;
            r = (r * (int)gain) >> 8;
            u.ring[wr & RING_MASK] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
            wr++;
            pos += step;
        }
        pos = pos >= 65536 ? pos - 65536 : 0;
        pl = l1;
        pr = r1;
    }
    if (peak > u.peak_in)
        u.peak_in = peak;
    u.pos = pos;
    u.pl = pl;
    u.pr = pr;
    __sync_synchronize();
    u.wr = wr;
    return true;
}

static bool driver_set_mute(bool mute)
{
    u.muted = mute;
    update_gain();
    return true;
}

static bool driver_set_volume(int percent)
{
    u.volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    update_gain();
    return true;
}

static bool driver_set_sample_rate(int sample_rate)
{
    set_rates(sample_rate);
    return true;
}

static const char *driver_get_error(void)
{
    return u.error ? u.error : "Unspecified Error";
}

const rg_audio_driver_t rg_audio_driver_usb = {
    .name = "usb",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
    .set_mute = driver_set_mute,
    .set_volume = driver_set_volume,
    .set_sample_rate = driver_set_sample_rate,
    .get_error = driver_get_error,
};

#endif // RG_AUDIO_USE_USB
