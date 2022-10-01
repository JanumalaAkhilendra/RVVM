/*
main.c - RVVM Entry point, API example
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    Mr0maks <mr.maks0443@gmail.com>
                    KotB <github.com/0xCatPKG>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "rvvmlib.h"
#include "utils.h"

#include "devices/clint.h"
#include "devices/plic.h"
#include "devices/ns16550a.h"
#include "devices/ata.h"
#include "devices/fb_window.h"
#include "devices/ps2-altera.h"
#include "devices/ps2-keyboard.h"
#include "devices/ps2-mouse.h"
#include "devices/syscon.h"
#include "devices/rtc-goldfish.h"
#include "devices/pci-bus.h"
#include "devices/nvme.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#ifdef _WIN32
// For unicode, console setup
#if     _WIN32_WINNT < 0x0500
#undef  _WIN32_WINNT
#define _WIN32_WINNT   0x0500
#endif
#include <windows.h>
#endif

#ifdef USE_NET
#include "devices/eth-oc.h"
#endif

#ifndef VERSION
#define VERSION "v0.4"
#endif

typedef struct {
    const char* bootrom;
    const char* kernel;
    const char* dtb;
    const char* dumpdtb;
    const char* image;
    size_t mem;
    uint32_t smp;
    uint32_t fb_x;
    uint32_t fb_y;
    bool rv64;
    bool sbi_align_fix;
    bool nogui;
} vm_args_t;

static size_t get_arg(const char** argv, const char** arg_name, const char** arg_val)
{
    if (argv[0][0] == '-') {
        size_t offset = (argv[0][1] == '-') ? 2 : 1;
        *arg_name = &argv[0][offset];
        for (size_t i=0; argv[0][offset + i] != 0; ++i) {
            if (argv[0][offset + i] == '=') {
                // Argument format -arg=val
                *arg_val = &argv[0][offset + i + 1];
                return 1;
            }
        }

        if (argv[1] == NULL || argv[1][0] == '-') {
            // Argument format -arg
            *arg_val = "";
            return 1;
        } else {
            // Argument format -arg val
            *arg_val = argv[1];
            return 2;
        }
    } else {
        *arg_name = "bootrom";
        *arg_val = argv[0];
        return 1;
    }
}

static inline bool cmp_arg(const char* arg, const char* name)
{
    for (size_t i=0; arg[i] != 0 && arg[i] != '='; ++i) {
        if (arg[i] != name[i]) return false;
    }
    return true;
}

static void print_help()
{
#if defined(_WIN32) && !defined(UNDER_CE)
    const wchar_t* help = L"\n"
#else
    printf("\n"
#endif
           "  ██▀███   ██▒   █▓ ██▒   █▓ ███▄ ▄███▓\n"
           " ▓██ ▒ ██▒▓██░   █▒▓██░   █▒▓██▒▀█▀ ██▒\n"
           " ▓██ ░▄█ ▒ ▓██  █▒░ ▓██  █▒░▓██    ▓██░\n"
           " ▒██▀▀█▄    ▒██ █░░  ▒██ █░░▒██    ▒██ \n"
           " ░██▓ ▒██▒   ▒▀█░     ▒▀█░  ▒██▒   ░██▒\n"
           " ░ ▒▓ ░▒▓░   ░ ▐░     ░ ▐░  ░ ▒░   ░  ░\n"
           "   ░▒ ░ ▒░   ░ ░░     ░ ░░  ░  ░      ░\n"
           "   ░░   ░      ░░       ░░  ░      ░   \n"
           "    ░           ░        ░         ░   \n"
           "               ░        ░              \n"
           "\n"
           "https://github.com/LekKit/RVVM ("VERSION")\n"
           "\n"
           "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n"
           "This is free software: you are free to change and redistribute it.\n"
           "There is NO WARRANTY, to the extent permitted by law.\n"
           "\n"
           "Usage: rvvm [-mem 256M] [-smp 1] [-kernel ...] ... [bootrom]\n"
           "\n"
           "    -mem <amount>    Memory amount, default: 256M\n"
           "    -smp <count>     Cores count, default: 1\n"
#ifdef USE_RV64
           "    -rv64            Enable 64-bit RISC-V, 32-bit by default\n"
#endif
           "    -kernel <file>   Load kernel Image as SBI payload\n"
           "    -image <file>    Attach hard drive with raw image\n"
#ifdef USE_FB
           "    -res 1280x720    Change framebuffer resoulution\n"
           "    -nogui           Disable framebuffer & mouse/keyboard\n"
#endif
           "    -dtb <file>      Pass custom DTB to the machine\n"
#ifdef USE_FDT
           "    -dumpdtb <file>  Dump autogenerated DTB to file\n"
#endif
#ifdef USE_JIT
           "    -nojit           Disable RVJIT\n"
           "    -jitcache 16M    Per-core JIT cache size\n"
#endif
           "    -verbose         Enable verbose logging\n"
           "    -help            Show this help message\n"
           "    [bootrom]        Machine bootrom (SBI, BBL, etc)\n"
#if defined(_WIN32) && !defined(UNDER_CE)
           "\n";
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), help, wcslen(help), NULL, NULL);
#else
           "\n");
#endif
}

static bool parse_args(int argc, const char** argv, vm_args_t* args)
{
    const char* arg_name = "";
    const char* arg_val = "";
    uint8_t argpair;

    // Default params: 1 core, 256M ram, 640x480 screen
    args->smp = 1;
    args->mem = 256 << 20;
    args->fb_x = 640;
    args->fb_y = 480;

    for (int i=1; i<argc;) {
        argpair = get_arg(argv + i, &arg_name, &arg_val);
        i += argpair;
        if (cmp_arg(arg_name, "dtb")) {
            args->dtb = arg_val;
        } else if (cmp_arg(arg_name, "image")) {
            args->image = arg_val;
        } else if (cmp_arg(arg_name, "bootrom")) {
            args->bootrom = arg_val;
        } else if (cmp_arg(arg_name, "kernel")) {
            args->kernel = arg_val;
        } else if (cmp_arg(arg_name, "mem")) {
            if (strlen(arg_val))
                args->mem = ((size_t)atoi(arg_val)) << mem_suffix_shift(arg_val[strlen(arg_val)-1]);
        } else if (cmp_arg(arg_name, "smp")) {
            args->smp = atoi(arg_val);
        } else if (cmp_arg(arg_name, "res")) {
            size_t i;
            for (i=0; arg_val[i] && arg_val[i] != 'x'; ++i);
            if (arg_val[i] != 'x') {
                rvvm_error("Invalid resoulution: %s, expects 640x480", arg_val);
                return false;
            }
            args->fb_x = atoi(arg_val);
            args->fb_y = atoi(arg_val + i + 1);
        } else if (cmp_arg(arg_name, "dumpdtb")) {
            args->dumpdtb = arg_val;
        } else if (cmp_arg(arg_name, "rv64")) {
            args->rv64 = true;
            if (argpair == 2) i--;
        } else if (cmp_arg(arg_name, "nogui")) {
            args->nogui = true;
            if (argpair == 2) i--;
        } else if (cmp_arg(arg_name, "help")
                 || cmp_arg(arg_name, "h")
                 || cmp_arg(arg_name, "H")) {
            print_help();
            return false;
        }
    }
    return true;
}

static void rvvm_run_with_args(vm_args_t args)
{
    rvvm_machine_t* machine = rvvm_create_machine(RVVM_DEFAULT_MEMBASE, args.mem, args.smp, args.rv64);
    if (machine == NULL) {
        rvvm_error("Failed to create VM");
        return;
    }

    if (!rvvm_load_bootrom(machine, args.bootrom)
     || !rvvm_load_kernel(machine, args.kernel)
     || !rvvm_load_dtb(machine, args.dtb)) {
        rvvm_error("Failed to initialize VM");
        return;
    }

    clint_init_auto(machine);
    plic_ctx_t* plic = plic_init_auto(machine);
    pci_bus_t* pci_bus = pci_bus_init_auto(machine, plic);

    ns16550a_init_auto(machine, plic);
    syscon_init_auto(machine);

    if (args.image) {
        rvvm_cmdline_append(machine, "root=/dev/nvme0n1 rootflags=discard rw");
        if (!nvme_init(pci_bus, args.image, true)) {
            rvvm_error("Unable to open image file %s", args.image);
            return;
        }
    }

#ifdef USE_FB
    if (!args.nogui) {
        static struct ps2_device ps2_mouse;
        ps2_mouse = ps2_mouse_create();
        altps2_init(machine, 0x20000000, plic, plic_alloc_irq(plic), &ps2_mouse);

        static struct ps2_device ps2_keyboard;
        ps2_keyboard = ps2_keyboard_create();
        altps2_init(machine, 0x20001000, plic, plic_alloc_irq(plic), &ps2_keyboard);

        init_fb(machine, 0x30000000, args.fb_x, args.fb_y, &ps2_mouse, &ps2_keyboard);
        rvvm_cmdline_append(machine, "console=tty0");
    }
#endif

#ifdef USE_NET
    ethoc_init_auto(machine, plic);
#endif
#ifdef USE_RTC
    rtc_goldfish_init_auto(machine, plic);
#endif

    if (args.dumpdtb) {
        rvvm_dump_dtb(machine, args.dumpdtb);
    }

    rvvm_enable_builtin_eventloop(false);
    rvvm_start_machine(machine);
    rvvm_run_eventloop(); // Returns on machine shutdown

    rvvm_free_machine(machine);
}

static int rvvm_main(int argc, const char** argv)
{
    vm_args_t args = {0};
    rvvm_set_args(argc, argv);

    if (!parse_args(argc, argv, &args)) return 0;
    if (args.bootrom == NULL) {
        printf("Usage: %s [-help] [-mem 256M] [-rv64] ... [bootrom]\n", argv[0]);
        return 0;
    }

    rvvm_run_with_args(args);
    return 0;
}

int main(int argc, char** argv)
{
#if defined(_WIN32) && !defined(UNDER_CE)
    HWND console = GetConsoleWindow();
    DWORD pid;
    GetWindowThreadProcessId(console, &pid);
    if (GetCurrentProcessId() == pid) {
        // If we don't have a parent terminal, destroy our console
        FreeConsole();
    }
    // Use UTF-8 arguments
    LPWSTR* argv_u16 = CommandLineToArgvW(GetCommandLineW(), &argc);
    argv = safe_calloc(sizeof(char*), argc);
    for (int i=0; i<argc; ++i) {
        size_t arg_len = WideCharToMultiByte(CP_UTF8, 0, argv_u16[i], -1, NULL, 0, NULL, NULL);
        argv[i] = safe_calloc(sizeof(char), arg_len);
        WideCharToMultiByte(CP_UTF8, 0, argv_u16[i], -1, argv[i], arg_len, NULL, NULL);
    }
#endif
    int ret = rvvm_main(argc, (const char**)argv);
#if defined(_WIN32) && !defined(UNDER_CE)
    for (int i=0; i<argc; ++i) free(argv[i]);
    free(argv);
#endif
    return ret;
}
