#include "exec.h"
#include "common.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

static void child_reset_signals(void)
{
    signal(SIGPIPE, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

/* 在子进程内应用重定向；失败时打印并返回 false */
static bool apply_redirs(const sh_command_t *cmd)
{
    for (const sh_redir_t *r = cmd->redirs; r; r = r->next) {
        if (r->kind == SH_RD_DUP) {
            if (dup2(r->dup_fd, r->fd) < 0) {
                dprintf(STDERR_FILENO, "shsh: %d>&%d: %s\n", r->fd, r->dup_fd,
                        strerror(errno));
                return false;
            }
            continue;
        }
        int flags = 0;
        if (r->kind == SH_RD_OUT)
            flags = O_WRONLY | O_CREAT | O_TRUNC;
        else if (r->kind == SH_RD_APPEND)
            flags = O_WRONLY | O_CREAT | O_APPEND;
        else
            flags = O_RDONLY;
        int nfd = open(r->target, flags, 0666);
        if (nfd < 0) {
            dprintf(STDERR_FILENO, "shsh: %s: %s\n", r->target,
                    strerror(errno));
            return false;
        }
        if (dup2(nfd, r->fd) < 0) {
            dprintf(STDERR_FILENO, "shsh: 重定向失败: %s\n", strerror(errno));
            close(nfd);
            return false;
        }
        if (nfd > STDERR_FILENO)
            close(nfd);
    }
    return true;
}

pid_t exec_spawn(const sh_command_t *cmd, pid_t pgroup, const char *cwd,
                 int in_fd, int out_fd, int err_fd, const int *close_fds,
                 size_t nclose, sh_child_fn child_fn, void *child_ctx)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setpgid(0, pgroup); /* pgroup=0 即自成一组；父进程也会再设一次 */

        if (!apply_redirs(cmd))
            _exit(1);
        if (in_fd >= 0 && dup2(in_fd, STDIN_FILENO) < 0)
            _exit(1);
        if (out_fd >= 0 && dup2(out_fd, STDOUT_FILENO) < 0)
            _exit(1);
        if (err_fd >= 0 && dup2(err_fd, STDERR_FILENO) < 0)
            _exit(1);
        /* 接管完 0/1/2 后再关闭继承来的管道 fd（顺序不能颠倒） */
        for (size_t i = 0; i < nclose; i++)
            if (close_fds[i] > STDERR_FILENO)
                close(close_fds[i]);
        if (in_fd > STDERR_FILENO)
            close(in_fd);
        if (out_fd > STDERR_FILENO)
            close(out_fd);
        if (err_fd > STDERR_FILENO)
            close(err_fd);

        child_reset_signals();
        if (cwd && *cwd && chdir(cwd) != 0)
            dprintf(STDERR_FILENO, "shsh: 无法进入目录 %s: %s\n", cwd,
                    strerror(errno));

        if (child_fn)
            _exit(child_fn(child_ctx, cmd));
        if (cmd->argc == 0)
            _exit(0); /* 纯重定向 */
        execvp(cmd->argv[0], cmd->argv);
        dprintf(STDERR_FILENO, "shsh: %s: 无法执行: %s\n", cmd->argv[0],
                strerror(errno));
        _exit(127);
    }
    if (setpgid(pid, pgroup > 0 ? pgroup : pid) != 0 && errno != EACCES &&
        errno != ESRCH)
        log_error("setpgid(%d) 失败: %s", (int)pid, strerror(errno));
    return pid;
}