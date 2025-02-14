/*
Authors:
    Guenter Geiger <geiger@epy.co.at>
    Roman Haefeli <reduzent@gmail.com>
*/

/* this doesn't run on Windows (yet?) */
#ifndef _WIN32

#define _GNU_SOURCE

#include <m_pd.h>

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sched.h>

#define CLASSNAME "command"
#define LIBVERSION "0.5"
#define INBUFSIZE 65536

void sys_rmpollfn(int fd);
void sys_addpollfn(int fd, void* fn, void *ptr);

static t_class *command_class;

static void drop_priority(void)
{
#if (_POSIX_PRIORITY_SCHEDULING - 0) >=  200112L
    struct sched_param par;
    par.sched_priority = 0;
    sched_setscheduler(0,SCHED_OTHER,&par);
#endif
}

static const char*command_atom2string(const t_atom*ap, char*buffer, unsigned int buffersize) {
  switch(ap->a_type) {
  default:
    atom_string(ap, buffer, buffersize);
    return buffer;
  case A_SYMBOL:
    return strncat(buffer, atom_getsymbol(ap)->s_name, buffersize);
  }
  return 0;
}

typedef struct _command
{
    t_object x_obj;
    void* x_binbuf;
    int fd_stdout_pipe[2];
    int fd_stdin_pipe[2];
    int fd_stderr_pipe[2];
    int pid;
    int x_del;
    int x_bin;  // -b flag: binary output
    int x_sync; // -s flag: synchronous (blocking) mode of operation
    t_outlet* x_done;
    t_outlet* x_stdout;
    t_outlet* x_stderr;
    t_clock* x_clock;
    t_symbol *path;
} t_command;

void command_cleanup(t_command* x)
{
    sys_rmpollfn(x->fd_stdout_pipe[0]);
    sys_rmpollfn(x->fd_stderr_pipe[0]);

    if (x->fd_stdout_pipe[0]>0) close(x->fd_stdout_pipe[0]);
    if (x->fd_stdout_pipe[1]>0) close(x->fd_stdout_pipe[1]);
    if (x->fd_stdin_pipe[0]>0) close(x->fd_stdin_pipe[0]);
    if (x->fd_stdin_pipe[1]>0) close(x->fd_stdin_pipe[1]);
    if (x->fd_stderr_pipe[0]>0) close(x->fd_stderr_pipe[0]);
    if (x->fd_stderr_pipe[1]>0) close(x->fd_stderr_pipe[1]);

    x->fd_stdout_pipe[0] = -1;
    x->fd_stdout_pipe[1] = -1;
    x->fd_stdin_pipe[0] = -1;
    x->fd_stdin_pipe[1] = -1;
    x->fd_stderr_pipe[0] = -1;
    x->fd_stderr_pipe[1] = -1;
    clock_unset(x->x_clock);
}

// snippet from pd's x_misc.c:fudiparse_binbufout
static void command_doit(void *z, t_binbuf *b, t_outlet *outlet)
{
    t_command *x = (t_command *)z;
    int msg, natom = binbuf_getnatom(b);
    t_atom *at = binbuf_getvec(b);

    for (msg = 0; msg < natom;)
    {
        int emsg;
        for (emsg = msg; emsg < natom && at[emsg].a_type != A_COMMA
            && at[emsg].a_type != A_SEMI; emsg++);

        if (emsg > msg)
        {
            int i;
            for (i = msg; i < emsg; i++)
	    	if (at[i].a_type == A_DOLLAR || at[i].a_type == A_DOLLSYM)
            {
                pd_error(x, "[%s]: got dollar sign in message", CLASSNAME);
                goto nodice;
            }
            if (at[msg].a_type == A_FLOAT)
            {
                if (emsg > msg + 1)
                    outlet_list(outlet,  0, emsg-msg, at + msg);
                else outlet_float(outlet,  at[msg].a_w.w_float);
            }
	    else if (at[msg].a_type == A_SYMBOL)
                outlet_anything(outlet,  at[msg].a_w.w_symbol,
                    emsg-msg-1, at + msg + 1);
        }
    nodice:
        msg = emsg + 1;
    }
}

void command_read(t_command *x, int fd)
{
    // text output mode
    if (x->x_bin == 0) {
        char buf[INBUFSIZE];
        t_binbuf* bbuf = binbuf_new();
        int i;
        int ret;

        ret = read(fd, buf,INBUFSIZE-1);
        buf[ret] = '\0';

        for (i=0;i<ret;i++)
            if (buf[i] == '\n') buf[i] = ';';
        if (buf[i] == 'M') buf[i] = 'X';
        if (ret < 0)
        {
            pd_error(x, "[%s]: pipe read error", CLASSNAME);
            sys_rmpollfn(fd);
            x->fd_stdout_pipe[0] = -1;
            close(fd);
            return;
        }
        else if (ret == 0)
        {
            pd_error(x, "[%s]: EOF on socket %d\n", CLASSNAME, fd);
            sys_rmpollfn(fd);
            x->fd_stdout_pipe[0] = -1;
            close(fd);
            return;
        }
        else
        {
            binbuf_text(bbuf, buf, strlen(buf));
            if (fd == x->fd_stdout_pipe[0])
            {
                command_doit(x,bbuf,x->x_stdout);
            }
            else if (fd == x->fd_stderr_pipe[0])
            {
                command_doit(x,bbuf,x->x_stderr);
            }
        }
        binbuf_free(bbuf);
    }
    // binary output mode
    else {
        char buf[INBUFSIZE];
        int outc, n;
        outc = read(fd, buf,INBUFSIZE-1);
        t_atom *outv = (t_atom *)getbytes(outc * sizeof(t_atom));
        for (n = 0; n < outc; n++)
            SETFLOAT(outv + n, (unsigned char)buf[n]);
        if (fd == x->fd_stdout_pipe[0])
            outlet_list(x->x_stdout, &s_list, outc, outv);
        if (fd == x->fd_stderr_pipe[0])
            outlet_list(x->x_stderr, &s_list, outc, outv);
        freebytes(outv, outc * sizeof(t_atom));
    }
}

void command_check(t_command* x)
{
    int ret;
    int status;
    int retval = 0;
    ret = waitpid(x->pid,&status,WNOHANG);
    if (ret == x->pid) {
        if(poll(&(struct pollfd){ .fd = x->fd_stdout_pipe[0], .events = POLLIN }, 1, 0)==1)  {
            command_read(x, x->fd_stdout_pipe[0]);
        }
        if(poll(&(struct pollfd){ .fd = x->fd_stderr_pipe[0], .events = POLLIN }, 1, 0)==1)  {
            command_read(x, x->fd_stderr_pipe[0]);
        }
        if (WIFEXITED(status))
            retval = WEXITSTATUS(status);
        command_cleanup(x);
        outlet_float(x->x_done, retval);
    }
    else {
        if (x->x_del < 100) x->x_del+=2; /* increment poll times */
        clock_delay(x->x_clock,x->x_del);
    }
}

static void command_send(t_command *x, t_symbol *s,int ac, t_atom *at)
{
    int i;
    char tmp[MAXPDSTRING];
    int size = 0;
    (void)s;    //suppress warning

    if (x->fd_stdin_pipe[0] == -1) return; /* nothing to send to */

    for (i=0;i<ac;i++) {
        tmp[size]=0;
        command_atom2string(at, tmp+size, MAXPDSTRING-size);
        at++;
        size=strlen(tmp);
        tmp[size++] = ' ';
    }
    tmp[size-1] = 0;
    if (write(x->fd_stdin_pipe[1],tmp,strlen(tmp)) == -1)
    {
        pd_error(x, "[%s]: writing to stdin of command failed", CLASSNAME);
    }
}

static void command_env(t_command *x, t_symbol *var, t_symbol *val)
{
    if(setenv(var->s_name, val->s_name, 1) < 0)
    {
        pd_error(x, "[%s]: setting environment variable failed. errno: %d", CLASSNAME, errno);
    } else {
        logpost(x, 3, "[%s]: setting %s=%s", CLASSNAME, var->s_name, val->s_name);
    }
}

static void command_exec(t_command *x, t_symbol *s, int ac, t_atom *at)
{
    int i;
    (void)s; // suppress warning

    if (x->fd_stdout_pipe[0] != -1) {
        pd_error(x, "[%s]: old process still running", CLASSNAME);
        return;
    }

    if (pipe(x->fd_stdin_pipe) == -1) {
        pd_error(x, "[%s]: unable to create stdin pipe", CLASSNAME);
        return;
    }

    if (pipe(x->fd_stdout_pipe) == -1) {
	pd_error(x, "[%s]: unable to create stdout pipe", CLASSNAME);
        return;
    }

    if (pipe(x->fd_stderr_pipe) == -1) {
	pd_error(x, "[%s]: unable to create stderr pipe", CLASSNAME);
        return;
    }

    sys_addpollfn(x->fd_stdout_pipe[0],command_read,x);
    sys_addpollfn(x->fd_stderr_pipe[0],command_read,x);
    x->pid = fork();

    if (x->pid == 0) {
        char**argv = getbytes((ac + 1) * sizeof(char*));
        /* reassign stdout */
        dup2(x->fd_stdin_pipe[0],  STDIN_FILENO);
        dup2(x->fd_stdout_pipe[1], STDOUT_FILENO);
        dup2(x->fd_stderr_pipe[1], STDERR_FILENO);
        close(x->fd_stdin_pipe[0]);
        close(x->fd_stdout_pipe[1]);
        close(x->fd_stderr_pipe[1]);

        /* drop privileges */
        drop_priority();
        /* lose setuid priveliges */
        if (seteuid(getuid()) == -1)
        {
            pd_error(x, "[%s]: seteuid failed", CLASSNAME);
            return;
        }
        for (i=0;i<ac;i++) {
	    argv[i] = getbytes(MAXPDSTRING);
	    command_atom2string(at, argv[i], MAXPDSTRING);
	    at++;
	}
	argv[i] = 0;
        if (chdir(x->path->s_name) == -1) {
            pd_error(x, "changing directory failed");
        }

	if (execvp(argv[0], argv) == -1) {
            pd_error(x, "execution failed");
        };

        for (i=0;i<ac;i++) {
            freebytes(argv[i], MAXPDSTRING);
        }
        freebytes(argv, (ac+1) * sizeof(char*));

	exit(errno);
    }
    // should we wait for command to return (blocking mode / -s) or
    // should we schedule a check for later (non-blocking mode)?
    if (x->x_sync)
    {
        int status;
        int retval = -1;
        waitpid(x->pid, &status, 0);
        if(poll(&(struct pollfd){ .fd = x->fd_stdout_pipe[0], .events = POLLIN }, 1, 0)==1)  {
            command_read(x, x->fd_stdout_pipe[0]);
        }
        if(poll(&(struct pollfd){ .fd = x->fd_stderr_pipe[0], .events = POLLIN }, 1, 0)==1)  {
            command_read(x, x->fd_stderr_pipe[0]);
        }
        if (WIFEXITED(status))
            retval = WEXITSTATUS(status);
        command_cleanup(x);
        outlet_float(x->x_done, retval);
    } else {
        x->x_del = 4;
        clock_delay(x->x_clock,x->x_del);
    }
}

void command_free(t_command* x)
{
    if (x->fd_stdout_pipe[0] != -1)
    {
        if (kill(x->pid, SIGINT) < -1)
        {
            pd_error(x, "[%s]: killing command failed", CLASSNAME);
        }
        command_cleanup(x);
    }
    binbuf_free(x->x_binbuf);
}

void command_kill(t_command *x)
{
    if (x->fd_stdin_pipe[0] == -1) return;
    if (kill(x->pid, SIGINT) < -1)
    {
        pd_error(x, "[%s]: killing command failed", CLASSNAME);
    }
}

static void *command_new(t_symbol *s, int argc, t_atom *argv)
{
    t_command *x = (t_command *)pd_new(command_class);
    (void)s->s_name;

    x->fd_stdout_pipe[0] = -1; // read end
    x->fd_stdout_pipe[1] = -1; // write end
    x->fd_stdin_pipe[0] = -1;  // read end
    x->fd_stdin_pipe[1] = -1;  // write end

    x->x_binbuf = binbuf_new();

    x->x_done = outlet_new(&x->x_obj, &s_float);
    x->x_stdout = outlet_new(&x->x_obj, 0);
    x->x_stderr = outlet_new(&x->x_obj, 0);
    x->x_clock = clock_new(x, (t_method) command_check);
    x->path = canvas_getdir(canvas_getcurrent());

    // check for -b flag (binary output)
    // and for -s flag (synchron mode)
    // snippet from Pd's x_net.c
    if (argc && argv->a_type == A_FLOAT)
    {
        x->x_bin = 0;
        x->x_sync = 0;
        argc = 0;
    }
    else while (argc && argv->a_type == A_SYMBOL &&
        *argv->a_w.w_symbol->s_name == '-')
    {
        if (!strcmp(argv->a_w.w_symbol->s_name, "-b"))
        {
            x->x_bin = 1;
        } else if (!strcmp(argv->a_w.w_symbol->s_name, "-s"))
        {
            x->x_sync = 1;
        }
        else
        {
            pd_error(x, "[%s]: unknown flag", CLASSNAME);
            postatom(argc, argv); endpost();
        }
        argc--; argv++;
    }
    if (argc)
    {
        pd_error(x, "[%s]: extra arguments ignored", CLASSNAME);
        postatom(argc, argv); endpost();
    }
    return (x);
}

void command_setup(void)
{
    command_class = class_new(gensym("command"), (t_newmethod)command_new,
                        (t_method)command_free,sizeof(t_command), 0, A_GIMME, 0);
    class_addmethod(command_class, (t_method)command_env, gensym("env"),
        A_SYMBOL, A_SYMBOL, 0);
    class_addmethod(command_class, (t_method)command_exec, gensym("exec"),
        A_GIMME, 0);
    class_addmethod(command_class, (t_method)command_kill, gensym("kill"), 0);
    class_addmethod(command_class, (t_method)command_send, gensym("send"),
        A_GIMME, 0);
    logpost(NULL, 2, "[%s]: version %s", CLASSNAME, LIBVERSION);
}

#endif /* _WIN32 */
