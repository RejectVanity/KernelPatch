#include <kputils.h>
#include <stdarg.h>
#include <sucompat.h>
#include <linux/string.h>
#include <linux/syscall.h>
#include <ktypes.h>
#include <stdbool.h>
#include <uapi/scdefs.h>
#include <syscall.h>
#include <predata.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <accctl.h>
#include <linux/slab.h>
#include <module.h>

static char *__user supercmd_str_to_user_sp(const char *data, uintptr_t *sp)
{
    int len = strlen(data) + 1;
    *sp -= len;
    *sp &= 0xFFFFFFFFFFFFFFF8;
    int cplen = compat_copy_to_user((void *)*sp, data, len);
    if (cplen > 0) return (char *__user) * sp;
    return 0;
}

static void supercmd_exec(char **__user u_filename_p, const char *cmd, uintptr_t *sp)
{
    int cplen = 0;
#if 1
    cplen = compat_copy_to_user(*u_filename_p, cmd, strlen(cmd) + 1);
#endif
    if (cplen <= 0) *u_filename_p = supercmd_str_to_user_sp(cmd, sp);
}

static void supercmd_echo(char **__user u_filename_p, char **__user uargv, uintptr_t *sp, const char *fmt, ...)
{
    supercmd_exec(u_filename_p, ANDROID_ECHO_PATH, sp);

    char buffer[4096];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);

    const char *__user cmd = supercmd_str_to_user_sp(ANDROID_ECHO_PATH, sp);
    const char *__user argv1 = supercmd_str_to_user_sp(buffer, sp);

    set_user_arg_ptr(0, *uargv, 0, (uintptr_t)cmd);
    set_user_arg_ptr(0, *uargv, 1, (uintptr_t)argv1);
    set_user_arg_ptr(0, *uargv, 2, 0);
}

static const char supercmd_help[] =
    ""
    "KernelPatch supercmd:\n"
    "Usage: truncate <superkey|su> [-uZc] [Command [[SubCommand]...]]\n"
    "superkey:                      Authentication. If current uid is su allowed, 'su' can be used as superkey.\n"
    "Options:\n"
    "  -u <UID>                     Change user id to UID.\n"
    "  -Z <SCONTEXT>                Change security context to SCONTEXT.\n"
    "Command:\n"
    "  help:                        Print this help message.\n"
    "  version:                     Print Kernel version and KernelPatch version,\n "
    "    eg: 50a0a,a06 means kernel version 5.10.10, KernelPatch version 0.10.6.\n"
    "  -c <COMMAND> [...]:          Pass a single COMMAND to the default shell.\n"
    "  exec <PATH> [...]:           Execute command with full PATH.\n"
    "  sumgr <SubCommand> [...]:    SU permission manager\n"
    "    The default command obtain a shell with the specified TO_UID and SCONTEXT is 'kp',\n"
    "    whose full PATH is '/system/bin/kp'. This can avoid conflicts with the existing 'su' command.\n"
    "    If you wish to modify this PATH, you can use the 'reset' command.\n"
    "    SubCommand:\n"
    "      grant <UID> [TO_UID] [SCONTEXT]  Grant su permission to UID.\n"
    "      revoke                           Revoke su permission to UID.\n"
    "      num                              Get the number of uids with the aforementioned permissions.\n"
    "      list                             List aforementioned uids.\n"
    "      profile <UID>                    Get the profile of the uid configuration.\n"
    "      reset <PATH>                     Reset '/system/bin/kp' to PATH. The length of PATH must be between 1-127.\n"
    "      path                             Get current su PATH.\n"
    "  module <SubCommand> [...]:   KernelPatch Module manager\n"
    "    SubCommand:\n"
    "      load <KPM_PATH> [KPM_ARGS]       Load module with KPM_PATH and KPM_ARGS.\n"
    "      ctl0 <KPM_NAME> <CTL_ARGS>       Control module named KPM_PATH with CTL_ARGS.\n"
    "      unload <KPM_NAME>                Unload module named KPM_NAME.\n"
    "      num                              Get the number of modules that have been loaded.\n"
    "      list                             List names of all loaded modules.\n"
    "      info <KPM_NAME>                  Get detailed information about module named KPM_NAME.\n"
    "  key <SubCommand> [...]:      Superkey manager\n"
    "    SubCommand:\n"
    "      get:                             Print current superkey\n"
    "      set <SUPERKEY>:                  Set current superkey\n"
    "      hash [enable|disable]:           Whether to use hash to verify the root superkey.\n"
    "";

void handle_supercmd(char **__user u_filename_p, char **__user uargv)
{
    // allow root-allowed user use supercmd
    static int allow_root_supercmd = 1;

    // key
    const char __user *p1 = get_user_arg_ptr(0, *uargv, 1);
    if (!p1 || IS_ERR(p1)) return;

    struct su_profile profile = { .to_uid = 0, .scontext = "" };

    // auth key
    char arg1[SUPER_KEY_LEN];
    if (compat_strncpy_from_user(arg1, p1, sizeof(arg1)) <= 0) return;

    if (!auth_superkey(arg1)) {
    } else if (allow_root_supercmd && !strcmp("su", arg1)) {
        uid_t uid = current_uid();
        if (!is_su_allow_uid(uid)) return;
        profile = profile_su_allow_uid(uid);
    } else {
        return;
    }

#define SUPERCMD_ARGS_NO 16

    // args
    const char *parr[SUPERCMD_ARGS_NO + 4] = { 0 };
    for (int i = 2; i < SUPERCMD_ARGS_NO; i++) {
        const char __user *ua = get_user_arg_ptr(0, *uargv, i);
        if (!ua || IS_ERR(ua)) break;
        const char *a = strndup_user(ua, 512);
        if (IS_ERR(a)) break;
        if (a[0] == '-' && a[1] == 'c') break;
        if (!strcmp("exec", a)) break;
        parr[i] = a;
    }

    uint64_t sp = current_user_stack_pointer();

    // if no any more
    if (!parr[2]) {
        supercmd_exec(u_filename_p, sh_path, &sp);
        *uargv += 2 * 8;
        commit_su(profile.to_uid, profile.scontext);
        return;
    }

    int pi = 2;

    // options, contiguous
    while (pi < SUPERCMD_ARGS_NO) {
        const char *arg = parr[pi];
        if (!arg || arg[0] != '-') break;
        // ignore -c
        if (arg[0] == '-' && arg[1] == 'c') break;
        char o = arg[1];
        pi++;
        switch (o) {
        case 'u':
            if (parr[pi]) {
                unsigned long long to_uid = profile.to_uid;
                kstrtoull(parr[pi++], 10, &to_uid);
                profile.to_uid = to_uid;
            } else {
                supercmd_echo(u_filename_p, uargv, &sp, "supercmd error: invalid to_uid");
                goto free;
            }
            break;
        case 'Z':
            if (parr[pi]) {
                strncpy(profile.scontext, parr[pi++], sizeof(profile.scontext) - 1);
                profile.scontext[sizeof(profile.scontext) - 1] = '\0';
            } else {
                supercmd_echo(u_filename_p, uargv, &sp, "supercmd error: invalid scontext");
                goto free;
            }
            break;
        default:
            goto out_opt;
        }
    }

out_opt:

    commit_su(profile.to_uid, profile.scontext);

    int rc = 0;
    const char *msg = 0;
    const char *err_msg = 0;
    char buffer[4096];

    // command
    const char **carr = parr + pi;
    const char *cmd = 0;
    if (pi < SUPERCMD_ARGS_NO - 1) cmd = carr[0];
    if (!cmd) {
        supercmd_exec(u_filename_p, sh_path, &sp);
        *uargv += pi * 8;
        goto free;
    }

    if (!strcmp("help", cmd)) {
        msg = supercmd_help;
    } else if (!strcmp("-c", cmd)) {
        supercmd_exec(u_filename_p, sh_path, &sp);
        *uargv += (carr - parr - 1) * 8;
    } else if (!strcmp("exec", cmd)) {
        if (!carr[1]) {
            err_msg = "invalid commmand path";
            goto echo;
        }
        supercmd_exec(u_filename_p, carr[1], &sp);
        *uargv += 3 * 8;
    } else if (!strcmp("version", cmd)) {
        supercmd_echo(u_filename_p, uargv, &sp, "%x,%x", kver, kpver);
    } else if (!strcmp("key", cmd)) {
        const char *sub_cmd = carr[1];
        if (!sub_cmd) sub_cmd = "";
        if (!strcmp("get", sub_cmd)) {
            msg = get_superkey();
        } else if (!strcmp("set", sub_cmd)) {
            const char *key = carr[2];
            if (!key) {
                err_msg = "invalid new key";
                goto echo;
            }
            msg = key;
            reset_superkey(key);
        } else if (!strcmp("hash", sub_cmd)) {
            const char *able = carr[2];
            if (!strcmp("enable", able) || !strcmp("disable", able)) {
                msg = able;
                enable_auth_root_key(1);
            } else if (!strcmp("disable", able)) {
                msg = able;
                enable_auth_root_key(0);
            } else {
                err_msg = "enable or disable";
            }
        } else {
            err_msg = "invalid subcommand";
        }
    } else if (!strcmp("module", cmd)) {
        const char *sub_cmd = carr[1];
        if (!sub_cmd) sub_cmd = "";
        if (!strcmp("num", sub_cmd)) {
            int num = get_module_nums();
            supercmd_echo(u_filename_p, uargv, &sp, "%d", num);
        } else if (!strcmp("list", sub_cmd)) {
            buffer[0] = '\0';
            list_modules(buffer, sizeof(buffer));
            msg = buffer;
        } else if (!strcmp("load", sub_cmd)) {
            const char *path = carr[2];
            if (!path) {
                err_msg = "invalid module path";
                goto echo;
            }
            rc = load_module_path(path, carr[3], 0);
            if (!rc) msg = path;
        } else if (!strcmp("ctl0", sub_cmd)) {
            const char *name = carr[2];
            if (!name) {
                err_msg = "invalid module name";
                goto echo;
            }
            const char *mod_args = carr[3];
            if (!mod_args) {
                err_msg = "invalid control arguments";
                goto echo;
            }
            buffer[0] = '\0';
            rc = module_control0(name, mod_args, buffer, sizeof(buffer));
            msg = buffer;
        } else if (!strcmp("ctl1", sub_cmd)) {
            err_msg = "not implement";
        } else if (!strcmp("unload", sub_cmd)) {
            const char *name = carr[2];
            if (!name) {
                err_msg = "invalid module name";
                goto echo;
            }
            rc = unload_module(name, 0);
            if (!rc) msg = name;
        } else if (!strcmp("info", sub_cmd)) {
            const char *name = carr[2];
            if (!name) {
                err_msg = "invalid module name";
                goto echo;
            }
            buffer[0] = '\0';
            int sz = get_module_info(name, buffer, sizeof(buffer));
            if (sz <= 0) rc = sz;
            msg = buffer;
        } else {
            err_msg = "invalid subcommand";
        }
    } else if (!strcmp("sumgr", cmd)) {
        const char *sub_cmd = carr[1];
        if (!sub_cmd) sub_cmd = "";
        if (!strcmp(sub_cmd, "grant")) {
            unsigned long long uid = 0, to_uid = 0;
            const char *scontext = "";
            if (!carr[2] || kstrtoull(carr[2], 10, &uid)) {
                supercmd_echo(u_filename_p, uargv, &sp, "supercmd error: illegal uid");
                goto free;
            }
            if (carr[3]) kstrtoull(carr[3], 10, &to_uid);
            if (carr[4]) scontext = carr[4];
            su_add_allow_uid(uid, to_uid, scontext, 1);
            supercmd_echo(u_filename_p, uargv, &sp, "supercmd: grant %d, %d, %s", uid, to_uid, scontext);
        } else if (!strcmp(sub_cmd, "revoke")) {
            const char *suid = carr[2];
            unsigned long long uid;
            if (!suid || kstrtoull(suid, 10, &uid)) {
                supercmd_echo(u_filename_p, uargv, &sp, "supercmd error: illegal uid");
                goto free;
            }
            su_remove_allow_uid(uid, 1);
            msg = suid;
        } else if (!strcmp(sub_cmd, "num")) {
            int num = su_allow_uid_nums();
            supercmd_echo(u_filename_p, uargv, &sp, "%d", num);
        } else if (!strcmp(sub_cmd, "list")) {
            int num = su_allow_uid_nums();
            uid_t *uids = (uid_t *)buffer;
            int offset = 0;
            su_allow_uids(0, uids, num);

            char *msg_buf = buffer + num * sizeof(uid_t);
            msg_buf[0] = '\0';
            for (int i = 0; i < num; i++) {
                offset += sprintf(msg_buf + offset, "%d\n", uids[i]);
            };
            if (offset > 0) msg_buf[offset - 1] = '\0';
            msg = msg_buf;
        } else if (!strcmp(sub_cmd, "profile")) {
            unsigned long long uid;
            if (!carr[2] || kstrtoull(carr[2], 10, &uid)) {
                err_msg = "invalid uid";
                goto echo;
            }
            struct su_profile *profile = (struct su_profile *)buffer;
            rc = su_allow_uid_profile(0, uid, profile);
            char *msg = buffer + sizeof(struct su_profile);
            msg[0] = '\0';
            if (!rc)
                sprintf(msg, "uid: %d, to_uid: %d, scontext: %s", profile->uid, profile->to_uid, profile->scontext);
        } else if (!strcmp(sub_cmd, "reset")) {
            rc = su_reset_path(carr[2]);
        } else if (!strcmp(sub_cmd, "path")) {
            msg = su_get_path();
        } else {
            err_msg = "invalid subcommand";
        }
    } else if (!strcmp("bootlog", cmd)) {
        msg = get_boot_log();
    } else if (!strcmp("test", cmd)) {
        msg = "test done...";
    } else {
        err_msg = "invalid command";
    }

echo:
    if (msg) supercmd_echo(u_filename_p, uargv, &sp, msg);
    if (rc) supercmd_echo(u_filename_p, uargv, &sp, "supercmd error code: %d", rc);
    if (err_msg) supercmd_echo(u_filename_p, uargv, &sp, "supercmd error message: %s", err_msg);

free:
    // free args
    for (int i = 2; i < sizeof(parr) / sizeof(parr[0]); i++) {
        const char *a = parr[i];
        if (!a) break;
        kfree(a);
    }
}