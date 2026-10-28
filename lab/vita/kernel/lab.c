/* Vita Lab additions to the kernel module: reading what is on screen,
 * counting frames, and setting clocks.
 *
 * Screen: the framebuffer the display shows now belongs to some process (the
 * game, or SceShell on the home screen). Its pixels are copied out of that
 * process a row at a time, scaled down by `step`, as RGB565, into a buffer of
 * the calling (user) module.
 *
 * Frame rate: a thread waits for each vertical blank and counts how often the
 * displayed framebuffer changes: a game that draws 30 new frames a second
 * flips 30 times, whatever the display's 60 Hz. */
#include "lab.h"

#include <psp2kern/display.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/power.h>
#include <psp2kern/kernel/sysclib.h>
#include <stdint.h>

#define LAB_MAX_WIDTH 1920

static SceUID lab_mutex = -1;
static SceUID lab_thread = -1;
static volatile int lab_running;
static volatile uint32_t lab_fps, lab_flips, lab_fb_pid, lab_gpu_set;
static uint32_t lab_row_in[LAB_MAX_WIDTH];
static uint16_t lab_row_out[LAB_MAX_WIDTH];

static int current_fb(SceDisplayFrameBufInfo *info)
{
    int head = ksceDisplayGetPrimaryHead();
    int ret;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    ret = ksceDisplayGetProcFrameBufInternal(-1, head, 0, info);
    if (ret < 0 || info->paddr == 0)
    {
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        ret = ksceDisplayGetProcFrameBufInternal(-1, head, 1, info);
    }
    if (ret >= 0 && info->paddr == 0)
        ret = -1;
    return ret;
}

static int lab_thread_main(SceSize args, void *argp)
{
    uintptr_t last = 0;
    uint32_t flips = 0;
    SceInt64 since = ksceKernelGetSystemTimeWide();

    (void)args;
    (void)argp;
    while (lab_running)
    {
        SceDisplayFrameBufInfo info;

        ksceDisplayWaitVblankStart();
        if (current_fb(&info) >= 0)
        {
            if (info.paddr != last)
            {
                ++flips;
                ++lab_flips;
                last = info.paddr;
            }
            lab_fb_pid = (uint32_t)info.pid;
        }
        SceInt64 now = ksceKernelGetSystemTimeWide();
        if (now - since >= 1000000)
        {
            lab_fps = (uint32_t)((flips * 1000000ULL + (uint64_t)(now - since) / 2) / (uint64_t)(now - since));
            flips = 0;
            since = now;
        }
    }
    return 0;
}

int lab_start(void)
{
    lab_mutex = ksceKernelCreateMutex("vitalab_screen", 0, 0, NULL);
    if (lab_mutex < 0)
        return lab_mutex;
    lab_running = 1;
    lab_thread = ksceKernelCreateThread("vitalab_fps", lab_thread_main, 0x80, 0x1000, 0, 0, NULL);
    if (lab_thread < 0)
    {
        lab_running = 0;
        return lab_thread;
    }
    ksceKernelStartThread(lab_thread, 0, NULL);
    return 0;
}

void lab_stop(void)
{
    lab_running = 0;
    if (lab_thread >= 0)
    {
        ksceKernelWaitThreadEnd(lab_thread, NULL, NULL);
        ksceKernelDeleteThread(lab_thread);
        lab_thread = -1;
    }
    if (lab_mutex >= 0)
    {
        ksceKernelDeleteMutex(lab_mutex);
        lab_mutex = -1;
    }
}

static uint16_t to565(uint32_t abgr)
{
    uint32_t r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Copies part of the screen: output rows from `row` on, as many as fit in
 * dst_size bytes, scaled down by `step`, as RGB565. step_row packs both
 * (step in the low 8 bits, row above them) because a syscall takes at most
 * four arguments. Returns the number of rows written, or an error:
 *   -102 the module didn't start   -103 nothing on screen (no framebuffer)
 *   -105 couldn't read the app's framebuffer   -106 couldn't write the buffer
 *   or the display driver's own error code.
 * info_user gets 8 words: out width, out height, source width, source height,
 * owning pid, pixel format, frames per second, total flips. */
int vitaLabKernelGetFrame(int step_row, void *dst_user, unsigned int dst_size, uint32_t *info_user)
{
    uint32_t state;
    SceDisplayFrameBufInfo fb;
    uint32_t info[8] = {0};
    int ret = 0, step = step_row & 0xFF;
    uint32_t row = (uint32_t)step_row >> 8;

    ENTER_SYSCALL(state);
    if (step < 1)
        step = 1;
    if (step > 8)
        step = 8;
    if (lab_mutex < 0)
    {
        EXIT_SYSCALL(state);
        return -102;
    }
    int fr = current_fb(&fb);
    if (fr < 0)
    {
        EXIT_SYSCALL(state);
        return fr == -1 ? -103 : fr;
    }
    ksceKernelLockMutex(lab_mutex, 1, NULL);
    uint32_t w = fb.framebuf.width, h = fb.framebuf.height, pitch = fb.framebuf.pitch;
    uint32_t fmt = fb.framebuf.pixelformat;
    /* both display formats are 32 bits: A8B8G8R8, or A2B10G10R10 (10 bits a channel) */
    int bpp = 4, ten = (fmt & 0xFFFF0000u) == SCE_DISPLAY_PIXELFORMAT_A2B10G10R10;
    if (w > LAB_MAX_WIDTH)
        w = LAB_MAX_WIDTH;
    uint32_t ow = w / (uint32_t)step, oh = h / (uint32_t)step;
    uint32_t rows = 0;
    for (uint32_t oy = row; oy < oh && (rows + 1) * ow * 2 <= dst_size; ++oy, ++rows)
    {
        const uint8_t *src = (const uint8_t *)fb.framebuf.base + (size_t)oy * step * pitch * bpp;
        if (ksceKernelCopyFromUserProc(fb.pid, lab_row_in, src, w * bpp) < 0)
        {
            ret = -105;
            goto out;
        }
        for (uint32_t ox = 0; ox < ow; ++ox)
        {
            uint32_t p = lab_row_in[ox * step];
            if (ten)
                p = ((p >> 2) & 0xFF) | (((p >> 12) & 0xFF) << 8) | (((p >> 22) & 0xFF) << 16);
            lab_row_out[ox] = to565(p);
        }
        if (ksceKernelCopyToUser((uint8_t *)dst_user + (size_t)rows * ow * 2, lab_row_out, ow * 2) < 0)
        {
            ret = -106;
            goto out;
        }
    }
    ret = (int)rows;
    info[0] = ow;
    info[1] = oh;
    info[2] = w;
    info[3] = h;
    info[4] = (uint32_t)fb.pid;
    info[5] = fmt;
    info[6] = lab_fps;
    info[7] = lab_flips;
    if (info_user)
        ksceKernelCopyToUser(info_user, info, sizeof(info));
out:
    ksceKernelUnlockMutex(lab_mutex, 1);
    EXIT_SYSCALL(state);
    return ret;
}

/* out_user gets 8 words: fps, total flips, framebuffer pid, ARM, bus, GPU,
 * GPU crossbar clocks (MHz), 0. */
int vitaLabKernelGetPerf(uint32_t *out_user)
{
    uint32_t state;
    uint32_t out[8];

    ENTER_SYSCALL(state);
    out[0] = lab_fps;
    out[1] = lab_flips;
    out[2] = lab_fb_pid;
    out[3] = (uint32_t)kscePowerGetArmClockFrequency();
    out[4] = (uint32_t)kscePowerGetBusClockFrequency();
    out[5] = lab_gpu_set; /* the GPU clock can't be read on every SDK: report what Lab last set (0 = untouched) */
    out[6] = (uint32_t)kscePowerGetGpuXbarClockFrequency();
    out[7] = 0;
    int ret = ksceKernelCopyToUser(out_user, out, sizeof(out));
    EXIT_SYSCALL(state);
    return ret;
}

/* Sets clocks in MHz; 0 leaves one as it is. Games may set their own again. */
int vitaLabKernelSetClocks(int arm, int bus, int gpu, int xbar)
{
    uint32_t state;
    int ret = 0;

    ENTER_SYSCALL(state);
    if (arm > 0 && arm <= 444)
        ret |= kscePowerSetArmClockFrequency(arm);
    if (bus > 0 && bus <= 222)
        ret |= kscePowerSetBusClockFrequency(bus);
    if (gpu > 0 && gpu <= 222)
    {
        ret |= kscePowerSetGpuClockFrequency(gpu);
        lab_gpu_set = (uint32_t)gpu;
    }
    if (xbar > 0 && xbar <= 166)
        ret |= kscePowerSetGpuXbarClockFrequency(xbar);
    EXIT_SYSCALL(state);
    return ret;
}
