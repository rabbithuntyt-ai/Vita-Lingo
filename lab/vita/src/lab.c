/* Vita Lab additions to the user module:
 *
 *   commands  delete <TITLEID>, apps, stats, clock <arm> <bus> <gpu> <xbar> | clock default
 *   port 1339 the screen: send "shot <step>\n", get a 40-byte header and RGB565 pixels
 *   port 1340 (UDP) discovery: send "VITALAB?", get "VITALAB <version> <ftp> <cmd> <screen>"
 *
 * The PC side (lab/pc, vitalab.exe) uses all of these. */
#include "lab.h"

#include "promote.h"

#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <stdio.h>
#include <string.h>
#include <vitacompanion_kernel.h>

#define LAB_VERSION "1.1"
#define SCREEN_PORT 1339
#define DISCOVERY_PORT 1340

extern volatile int run;

static uint8_t shot_buf[32 * 1024]; /* 16 rows of 960 pixels */
static SceUID screen_thid = -1, disc_thid = -1;
static volatile int screen_sock = -1, disc_sock = -1;
static volatile int lab_up;

/* ---- commands ---- */

void cmd_delete(char **arg_list, size_t arg_count, char *res_msg)
{
    (void)arg_count;
    int r = promote_delete(arg_list[1]);
    if (r < 0)
        snprintf(res_msg, 256, "Error: could not uninstall %s (0x%08X).\n", arg_list[1], r);
    else
        snprintf(res_msg, 256, "Uninstalled %s.\n", arg_list[1]);
}

/* the title ID of a running app's process */
static int title_of(SceUID pid, char *out, size_t n)
{
    char buf[256];
    memset(buf, 0, sizeof(buf));
    if (sceAppMgrAppParamGetString(pid, 12, buf, sizeof(buf)) < 0)
        return -1;
    snprintf(out, n, "%s", buf);
    return 0;
}

void cmd_apps(char **arg_list, size_t arg_count, char *res_msg)
{
    (void)arg_list;
    (void)arg_count;
    SceInt32 ids[16];
    uint32_t perf[8] = {0};
    vitaLabKernelGetPerf(perf);
    int n = sceAppMgrGetRunningAppIdListForShell(ids, 16);
    char *p = res_msg;
    p += sprintf(p, "running:");
    for (int i = 0; i < n && i < 16; ++i)
    {
        SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(ids[i]);
        char title[16] = "?";
        title_of(pid, title, sizeof(title));
        p += sprintf(p, " %s%s", title, (uint32_t)pid == perf[2] ? "(on screen)" : "");
    }
    strcpy(p, "\n");
}

/* One line of numbers the PC side parses: frame rate, CPU load per core
 * (measured over 250 ms), clocks, and what is on screen. */
void cmd_stats(char **arg_list, size_t arg_count, char *res_msg)
{
    (void)arg_list;
    (void)arg_count;
    SceKernelSystemInfo a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.size = b.size = sizeof(a);
    SceInt64 t0 = sceKernelGetProcessTimeWide();
    sceKernelGetSystemInfo(&a);
    sceKernelDelayThread(250 * 1000);
    sceKernelGetSystemInfo(&b);
    SceInt64 dt = sceKernelGetProcessTimeWide() - t0;
    int load[4];
    for (int c = 0; c < 4; ++c)
    {
        SceInt64 idle = (SceInt64)(b.cpuInfo[c].idleClock - a.cpuInfo[c].idleClock);
        int l = dt > 0 ? (int)(100 - idle * 100 / dt) : 0;
        load[c] = l < 0 ? 0 : (l > 100 ? 100 : l);
    }
    uint32_t perf[8] = {0};
    int pr = vitaLabKernelGetPerf(perf);
    char title[16] = "SHELL";
    SceInt32 ids[16];
    int n = sceAppMgrGetRunningAppIdListForShell(ids, 16);
    for (int i = 0; i < n && i < 16; ++i)
    {
        if ((uint32_t)sceAppMgrGetProcessIdByAppIdForShell(ids[i]) == perf[2])
            title_of((SceUID)perf[2], title, sizeof(title));
    }
    snprintf(res_msg, 512,
             "fps=%u flips=%u cpu=%d,%d,%d,%d arm=%u bus=%u gpu=%u xbar=%u app=%s kernel=%s\n",
             perf[0], perf[1], load[0], load[1], load[2], load[3], perf[3], perf[4], perf[5], perf[6], title,
             pr < 0 ? "missing" : "ok");
}

void cmd_clock(char **arg_list, size_t arg_count, char *res_msg)
{
    int v[4] = {0, 0, 0, 0};
    /* arg_count includes the command's own name */
    if (arg_count == 2 && !strcmp(arg_list[1], "default"))
    {
        v[0] = 333;
        v[1] = 111;
        v[2] = 111;
        v[3] = 111;
    }
    else
    {
        for (size_t i = 1; i < arg_count && i <= 4; ++i)
            sscanf(arg_list[i], "%d", &v[i - 1]);
    }
    int r = vitaLabKernelSetClocks(v[0], v[1], v[2], v[3]);
    uint32_t perf[8] = {0};
    vitaLabKernelGetPerf(perf);
    snprintf(res_msg, 256, "%sclocks now arm=%u bus=%u gpu=%u xbar=%u\n", r < 0 ? "Some clocks refused. " : "", perf[3],
             perf[4], perf[5], perf[6]);
}

/* ---- the screen ---- */

static int send_all(int s, const void *data, unsigned int len)
{
    const char *p = data;
    while (len)
    {
        int r = sceNetSend(s, p, len, 0);
        if (r <= 0)
            return -1;
        p += r;
        len -= (unsigned int)r;
    }
    return 0;
}

static int screen_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (run && lab_up && screen_sock >= 0)
    {
        SceNetSockaddrIn addr;
        unsigned int alen = sizeof(addr);
        int c = sceNetAccept(screen_sock, (SceNetSockaddr *)&addr, &alen);
        if (c < 0)
        {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        int tmo = 3 * 1000 * 1000;
        sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
        sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
        char req[64];
        int got = 0;
        while (got < (int)sizeof(req) - 1)
        {
            int r = sceNetRecv(c, req + got, 1, 0);
            if (r <= 0 || req[got] == '\n')
                break;
            got += r;
        }
        req[got] = 0;
        int step = 1;
        sscanf(req, "shot %d", &step);
        if (step < 1 || step > 8)
            step = 1;
        /* the picture goes out a few rows at a time through a small buffer:
         * SceShell, which this plugin lives in, has little memory to spare */
        uint32_t info[8] = {0};
        int r = vitaLabKernelGetFrame(step, shot_buf, sizeof(shot_buf), info);
        uint32_t head[10];
        memcpy(head, "VLAB", 4);
        head[1] = r < 0 ? (uint32_t)r : 1; /* version, or the error */
        memcpy(&head[2], info, sizeof(info));
        if (r < 0)
            head[2] = head[3] = 0;
        if (send_all(c, head, sizeof(head)) == 0 && r > 0)
        {
            uint32_t ow = info[0], oh = info[1], row = 0;
            while (r > 0 && send_all(c, shot_buf, (unsigned int)r * ow * 2) == 0)
            {
                row += (uint32_t)r;
                if (row >= oh)
                    break;
                r = vitaLabKernelGetFrame(step | (int)(row << 8), shot_buf, sizeof(shot_buf), NULL);
            }
        }
        sceNetSocketClose(c);
    }
    return 0;
}

/* ---- being found on the network ---- */

static int disc_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (run && lab_up && disc_sock >= 0)
    {
        char buf[32];
        SceNetSockaddrIn from;
        unsigned int flen = sizeof(from);
        int r = sceNetRecvfrom(disc_sock, buf, sizeof(buf) - 1, 0, (SceNetSockaddr *)&from, &flen);
        if (r <= 0)
        {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        buf[r] = 0;
        if (!strncmp(buf, "VITALAB?", 8))
        {
            char reply[96];
            snprintf(reply, sizeof(reply), "VITALAB %s 1337 1338 %d", LAB_VERSION, SCREEN_PORT);
            sceNetSendto(disc_sock, reply, strlen(reply), 0, (SceNetSockaddr *)&from, flen);
        }
    }
    return 0;
}

static int listen_on(const char *name, int type, int port)
{
    int s = sceNetSocket(name, SCE_NET_AF_INET, type, 0);
    if (s < 0)
        return s;
    int on = 1;
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &on, sizeof(on));
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.sin_family = SCE_NET_AF_INET;
    a.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    a.sin_port = sceNetHtons((unsigned short)port);
    if (sceNetBind(s, (SceNetSockaddr *)&a, sizeof(a)) < 0 || (type == SCE_NET_SOCK_STREAM && sceNetListen(s, 4) < 0))
    {
        sceNetSocketClose(s);
        return -1;
    }
    return s;
}

void lab_net_up(void)
{
    if (lab_up)
        return;
    lab_up = 1;
    screen_sock = listen_on("vitalab_screen", SCE_NET_SOCK_STREAM, SCREEN_PORT);
    disc_sock = listen_on("vitalab_disc", SCE_NET_SOCK_DGRAM, DISCOVERY_PORT);
    if (screen_sock >= 0)
    {
        screen_thid = sceKernelCreateThread("vitalab_screen", screen_thread, 0x40, 0x4000, 0, 0, NULL);
        if (screen_thid >= 0)
            sceKernelStartThread(screen_thid, 0, NULL);
    }
    if (disc_sock >= 0)
    {
        disc_thid = sceKernelCreateThread("vitalab_disc", disc_thread, 0x40, 0x2000, 0, 0, NULL);
        if (disc_thid >= 0)
            sceKernelStartThread(disc_thid, 0, NULL);
    }
}

void lab_net_down(void)
{
    if (!lab_up)
        return;
    lab_up = 0;
    /* closing the sockets wakes the threads */
    if (screen_sock >= 0)
    {
        sceNetSocketClose(screen_sock);
        screen_sock = -1;
    }
    if (disc_sock >= 0)
    {
        sceNetSocketClose(disc_sock);
        disc_sock = -1;
    }
    if (screen_thid >= 0)
    {
        sceKernelWaitThreadEnd(screen_thid, NULL, NULL);
        sceKernelDeleteThread(screen_thid);
        screen_thid = -1;
    }
    if (disc_thid >= 0)
    {
        sceKernelWaitThreadEnd(disc_thid, NULL, NULL);
        sceKernelDeleteThread(disc_thid);
        disc_thid = -1;
    }
}

void lab_end(void)
{
    lab_net_down();
}
