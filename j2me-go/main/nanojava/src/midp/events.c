/*
 * events.c - Zero-latency input event queue shared by the platform and the
 *            Java event thread (nanojava.Events).
 */
#include "midp.h"
#include "../vm/vm.h"
#include <string.h>

#define QUEUE_SIZE     128
#define KEY_TABLE_SIZE 512

static NjEvent  queue[QUEUE_SIZE];
static int      head, tail;
static bool     wakeup;
static uint32_t s_key_down_frame[KEY_TABLE_SIZE];

#if defined(ESP_PLATFORM)
uint32_t nj_stat_input_events, nj_stat_input_ms, nj_stat_input_max_ms;
uint32_t nj_stat_input_dropped;
#endif

static inline int key_index(int code)
{
    if (code >= -255 && code < 0) return -code - 1;
    if (code >= 0 && code < 256)  return code + 255;
    return -1;
}

static bool queue_put(const NjEvent *ev)
{
    int next = (tail + 1) % QUEUE_SIZE;
    if (next == head) {
#if defined(ESP_PLATFORM)
        nj_stat_input_dropped++;
#endif
        head = (head + 1) % QUEUE_SIZE;
    }
    queue[tail] = *ev;
    tail = next;
    return true;
}

static bool queue_get(NjEvent *ev)
{
    if (head == tail)
        return false;

    NjEvent *front = &queue[head];
    int idx = key_index(front->a);
    uint32_t cur_frame = pal_get_total_frames();

    /* Prevent a same-frame KEY_UP from erasing a tap before the game loop observes
     * the KEY_DOWN, without ever blocking subsequent events in the FIFO. */
    if (front->type == EV_KEY_UP && idx >= 0 && idx < KEY_TABLE_SIZE) {
        if (s_key_down_frame[idx] == cur_frame && ((head + 1) % QUEUE_SIZE) == tail) {
            return false;
        }
        s_key_down_frame[idx] = 0;
    }

    *ev = *front;
    head = (head + 1) % QUEUE_SIZE;

    if (ev->type == EV_KEY_DOWN && idx >= 0 && idx < KEY_TABLE_SIZE) {
        s_key_down_frame[idx] = cur_frame;
    }
    return true;
}

void midp_poll(void)
{
    NjEvent ev;
    bool    any = false;
    while (pal_poll_event(&ev)) {
        if (ev.type == EV_QUIT) {
            vm.exit_requested = true;
            return;
        }
        if (queue_put(&ev))
            any = true;
    }
    if (any)
        vm_wake_event_waiters();
}

static int Events_next(Thread *t, Slot *args, Slot *ret)
{
    Object *out = args[0].ref;
    jint    timeout = args[1].i;
    NjEvent ev;

    if (!out || ARRAY_LEN(out) < 2) {
        NPE(t);
        return NATIVE_OK;
    }
    if (queue_get(&ev)) {
        t->native_phase = 0;
#if defined(ESP_PLATFORM)
        uint32_t waited = (uint32_t)(pal_time_ms() - ev.t_ms);
        nj_stat_input_events++;
        nj_stat_input_ms += waited;
        if (waited > nj_stat_input_max_ms)
            nj_stat_input_max_ms = waited;
#endif
        ARRAY_DATA(out, jint)[0] = ev.a;
        ARRAY_DATA(out, jint)[1] = ev.b;
        ret->i = ev.type;
        return NATIVE_OK;
    }
    if (t->native_phase == 1 || timeout == 0 || wakeup) {
        t->native_phase = 0;
        wakeup = false;
        ret->i = 0;
        return NATIVE_OK;
    }
    t->native_phase = 1;
    t->state = TS_EVENT_WAIT;
    int64_t now = pal_time_ms();
    if (head != tail) {
        if (timeout < 0 || timeout > 4)
            timeout = 4;
    }
    t->wake_time = timeout > 0 ? now + timeout : 0;
    return NATIVE_BLOCK;
}

static int Events_wakeup(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)args; (void)ret;
    wakeup = true;
    vm_wake_event_waiters();
    return NATIVE_OK;
}

static const NativeEntry event_natives[] = {
    {"nanojava/Events", "next", NULL, Events_next},
    {"nanojava/Events", "wakeup", NULL, Events_wakeup},
    {NULL, NULL, NULL, NULL}
};

void midp_events_init(void)
{
    head = tail = 0;
    wakeup = false;
    memset(s_key_down_frame, 0, sizeof(s_key_down_frame));
    native_register(event_natives);
}
