/* (C) Guenter Geiger <geiger@epy.co.at> */

/* this doesn't run on Windows (yet?) */
#ifndef _WIN32

#include <m_pd.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sched.h>

void sys_rmpollfn(int fd);
void sys_addpollfn(int fd, void* fn, void *ptr);

/* ------------------------ command ----------------------------- */

#define INBUFSIZE 65536

static t_class *command_class;


static void drop_priority(void)
{
#if (_POSIX_PRIORITY_SCHEDULING - 0) >=  200112L
    struct sched_param par;
    par.sched_priority = 0;
    sched_setscheduler(0,SCHED_OTHER,&par);
#endif
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

void command_check(t_command* x)
{
    int ret;
    int status;
    ret = waitpid(x->pid,&status,WNOHANG);
    if (ret == x->pid) {
        command_cleanup(x);
        if (WIFEXITED(status)) {
            outlet_float(x->x_done,WEXITSTATUS(status));
        }
        else outlet_float(x->x_done,0);
    }
    else {
        if (x->x_del < 100) x->x_del+=2; /* increment poll times */
        clock_delay(x->x_clock,x->x_del);
    }
}

/* snippet from pd's code */
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
                pd_error(x, "got dollar sign in message");
                goto nodice;
            }
            if (at[msg].a_type == A_FLOAT)
            {
                if (emsg > msg + 1)
                    outlet_list(x->x_obj.ob_outlet,  0, emsg-msg, at + msg);
                else outlet_float(x->x_obj.ob_outlet,  at[msg].a_w.w_float);
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
        pd_error(x, "pipe read error");
        sys_rmpollfn(fd);
        x->fd_stdout_pipe[0] = -1;
        close(fd);
        return;
    }
    else if (ret == 0)
    {
        post("EOF on socket %d\n", fd);
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

static void command_send(t_command *x, t_symbol *s,int ac, t_atom *at)
{
    int i;
    char tmp[MAXPDSTRING];
    int size = 0;
    (void)s;    //suppress warning

    if (x->fd_stdin_pipe[0] == -1) return; /* nothing to send to */

    for (i=0;i<ac;i++) {
        atom_string(at,tmp+size,MAXPDSTRING - size);
        at++;
        size=strlen(tmp);
        tmp[size++] = ' ';
    }
    tmp[size-1] = 0;
    if (write(x->fd_stdin_pipe[1],tmp,strlen(tmp)) == -1)
    {
        pd_error(x, "writing to stdin of command failed");
    }
}

static void command_exec(t_command *x, t_symbol *s, int ac, t_atom *at)
{
    int i;
    char* argv[255];
    (void)s; // suppress warning

    if (x->fd_stdout_pipe[0] != -1) {
        post("old process still running");
        return;
    }

    if (pipe(x->fd_stdin_pipe) == -1) {
        pd_error(x, "unable to create stdin pipe");
        return;
    }

    if (pipe(x->fd_stdout_pipe) == -1) {
	pd_error(x, "unable to create stdout pipe");
        return;
    }

    if (pipe(x->fd_stderr_pipe) == -1) {
	pd_error(x, "unable to create stderr pipe");
        return;
    }

    sys_addpollfn(x->fd_stdout_pipe[0],command_read,x);
    sys_addpollfn(x->fd_stderr_pipe[0],command_read,x);
    x->pid = fork();

    if (x->pid == 0) {
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
            pd_error(x, "seteuid failed");
            return;
        }
        for (i=0;i<ac;i++) {
	    argv[i] = getbytes(255);
	    atom_string(at,argv[i],255);
	    at++;
	}
	argv[i] = 0;
        if (chdir(x->path->s_name) == -1) {
            pd_error(x, "changing directory failed");
        }

	if (execvp(argv[0], argv) == -1) {
            pd_error(x, "execution failed");
        };
	exit(0);
    }
    x->x_del = 4;
    clock_delay(x->x_clock,x->x_del);
}

void command_free(t_command* x)
{
    if (x->fd_stdout_pipe[0] != -1)
    {
        if (kill(x->pid, SIGINT) < -1)
        {
            pd_error(x, "killing command failed");
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
        pd_error(x, "killing command failed");
    }
}

static void *command_new(void)
{
    t_command *x = (t_command *)pd_new(command_class);

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
    return (x);
}

void command_setup(void)
{
    command_class = class_new(gensym("command"), (t_newmethod)command_new,
                        (t_method)command_free,sizeof(t_command), 0,0);
    class_addmethod(command_class, (t_method)command_kill, gensym("kill"), 0);
    class_addmethod(command_class, (t_method)command_exec, gensym("exec"),
        A_GIMME, 0);
    class_addmethod(command_class, (t_method)command_send, gensym("send"),
        A_GIMME, 0);
}


#endif /* _WIN32 */


