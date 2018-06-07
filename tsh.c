/* 
 * tsh - A tiny shell program with job control
 * 
 * <HingOn Miu>
 * <hmiu>
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* Masks for access permission */
#define DEF_MODE       S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH
#define DEF_OUTFLAG    O_CREAT | O_TRUNC | O_APPEND | O_WRONLY
#define DEF_INFLAG     O_RDONLY

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */


/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    enum builtins_t {       /* Indicates if argv[0] is a builtin command */
        BUILTIN_NONE,
        BUILTIN_QUIT,
        BUILTIN_JOBS,
        BUILTIN_BG,
        BUILTIN_FG} builtins;
};
/* End global variables */


/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list); 
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid); 
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);



/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(job_list);


    /* Execute the shell's read/eval loop */
    while (1) {

        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { 
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }
        
        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';
        
        /* Evaluate the command line */
        eval(cmdline);
        
        fflush(stdout);
        fflush(stdout);
    } 
    
    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */

/*** eval function reference from textbook CS:APP chapter 8 and lecture pdf ***/

void eval(char *cmdline) 
{
    int bg;              /* should the job run in bg or fg? */
    struct cmdline_tokens tok;
    pid_t pid;           /* process pid */
    int jid;             /* job jid */
    struct job_t *job;      /* initiatized pointer for job */
    sigset_t mask;          /* for blocking and unblocking signal */
    int input_fd = STDIN_FILENO;    /* standard input */
    int output_fd = STDOUT_FILENO;  /* standard output */

    sigemptyset(&mask);             /* initialize mask for signal */
    sigaddset(&mask, SIGCHLD);          /* for blocking SIGCHLD */
    sigaddset(&mask, SIGINT);          /* for blocking SIGINT */
    sigaddset(&mask, SIGTSTP);          /* for blocking SIGTSTP */

    /* Parse command line */
    bg = parseline(cmdline, &tok);
    
    if(tok.infile != NULL){    /* check if I/O redirection is needed */
        input_fd = open(tok.infile, DEF_INFLAG, DEF_MODE);
    } /* opens the file and get the file descriptor for input file */
    
    if(tok.outfile != NULL){    /* check if I/O redirection is needed */
        output_fd = open(tok.outfile, DEF_OUTFLAG, DEF_MODE);
    } /* opens the file and get the file descriptor for output file */

    if (bg == -1) return;               /* parsing error */

    if (tok.argv[0] == NULL) return;   /* ignore empty lines */
    
    if (tok.builtins == BUILTIN_QUIT) { /* quit the shell */
        exit(0);
    }
    
    if (tok.builtins == BUILTIN_JOBS) { /* prints out the job list */
        listjobs(job_list, output_fd);
        return;
    }
    
    if (tok.builtins == BUILTIN_FG || tok.builtins == BUILTIN_BG) {
        /* FG: update any state of background job to running foreground job */
        /* BG: update a stopped background job to a running background job */
        if ((char)(tok.argv[1][0]) != '%') { /* check if it provides pid */
            pid = atoi(tok.argv[1]);
            job = getjobpid(job_list, pid); /* get the job pointer from jid */
            jid = job->jid;              /* get jid */
        }
        else { /* it provides jid */
            jid = atoi(&(tok.argv[1][1]));
            job = getjobjid(job_list, jid); /* get the job pointer from pid */
            pid = job->pid;              /* get pid */
        }
        if (tok.builtins == BUILTIN_FG) {
            job->state = FG;        /* update job state to foreground */
        }
        else { /* tok.builtins == BUILTIN_BG */
            printf("[%d] (%d) %s", jid, pid, job->cmdline);
            job->state = BG;        /* update job state to background */
        }
        kill(pid, SIGCONT);         /* restarts job by sending SIGCONT */
        return;
    }
    
    if (tok.builtins == BUILTIN_NONE) { /* if not builtin commands */
        sigprocmask(SIG_BLOCK, &mask, NULL);    /* block signals */
        if ((pid = fork()) == 0) {      /* initialize new child process */
            if(input_fd != STDIN_FILENO){
                dup2(input_fd, STDIN_FILENO);
                /* copies the descriptor table entry of output_fd to
                  descriptor table entry of STDIN_FILENO, and overwrites
                  the previous contents of descriptor table entry of
                  STDIN_FILENO */
            }
            
            if(output_fd != STDOUT_FILENO){
                dup2(output_fd, STDOUT_FILENO);
                /* copies the descriptor table entry of output_fd to
                  descriptor table entry of STDOUT_FILENO, and overwrites
                  the previous contents of descriptor table entry of
                  STDOUT_FILENO */
            }
            
            setpgid(0, 0);
            /* puts the child in a new process group whose group ID is
              identical to the child's pid, so that it ensures that there
              will be only one process, this shell itself, in the actual
              foreground process group of the shell running this shell */
            
            sigprocmask(SIG_UNBLOCK, &mask, NULL);   /* unblock signals */
            if (execve(tok.argv[0], tok.argv, environ) < 0) {
                /* execve loads tok.argv[0], and runs the programn with the
                  argument list tok.argv, and set the environmental varibale
                  in environ */
                printf("%s: Command not found. \n", tok.argv[0]);
                exit(0);
            }
        }
        
        if (!bg) {        /* foreground job */
            addjob(job_list, pid, FG, cmdline);    /* add foreground job */
            sigprocmask(SIG_UNBLOCK, &mask, NULL); /* unblock signals */
            int FGpid;      /* initialize foreground pid */
            int status;
            /* check if foreground job is running to reap zombie */
            while ((FGpid = fgpid(job_list))) {
                /* if no foreground job is running, fgpid(job_list) returns 0 */
                if ((FGpid = waitpid(FGpid, &status, WNOHANG|WUNTRACED)) > 0) {
                    /* reap zombie of the terminated FG job */
                    if(WIFEXITED(status)) {
                        /* terminates normally */
                        deletejob(job_list, FGpid);
                    }
                    if(WIFSTOPPED(status)){ /* stopped */
                        int sig = WSTOPSIG(status);
                        if (sig == SIGTSTP) {
                            /* stopped by SIGTSTP */
                            getjobpid(job_list, FGpid)->state = ST;
                        }
                    }
                    if(WIFSIGNALED(status)){ /* terminated by signals */
                        int sig = WTERMSIG(status);
                        if (sig == SIGINT) {
                            /* terminated by SIGINT */
                            deletejob(job_list, FGpid);
                        }
                    }
                }
            }
        }
        
        else {           /* background job */
            addjob(job_list, pid, BG, cmdline);    /* add background job */
            sigprocmask(SIG_UNBLOCK, &mask, NULL);   /* unblock SIGCHLD */
            int jid = pid2jid(pid);             /* job ID for background job */
            printf("[%d] (%d) %s\n", jid, pid, cmdline);
        }
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters 
 *             enclosed in single or double quotes are treated as a single
 *             argument. 
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job  
 *  -1:        if cmdline is incorrectly formatted
 * 
 * Note:       The string elements of tok (e.g., argv[], infile, outfile) 
 *             are statically allocated inside parseline() and will be 
 *             overwritten the next time this function is invoked.
 */
int parseline(const char *cmdline, struct cmdline_tokens *tok) 
{

    static char array[MAXLINE];          /* holds local copy of command line */
    const char delims[10] = " \t\r\n";   /* argument delimiters (white-space) */
    char *buf = array;                   /* ptr that traverses command line */
    char *next;                          /* ptr to the end of the current arg */
    char *endbuf;                        /* ptr to the end of the cmdline string */
    int is_bg;                           /* background job? */

    int parsing_state;                   /* indicates if the next token is the
                                            input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }
        
        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the input/output file */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr, "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
        tok->builtins = BUILTIN_FG;
    } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The 
 *     handler reaps all available zombie children, but doesn't wait 
 *     for any other currently running children to terminate.  
 */
void sigchld_handler(int sig) {
    int status;
    pid_t pid; 
    while ((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0){
        /* reap zombie of the terminated or stopped jobs */
        if(WIFEXITED(status)){ /* child terminated normally */
            deletejob(job_list, pid);
        }
        if(WIFSTOPPED(status)){ /* child got stopped */
            int sig = WSTOPSIG(status);  /* get signal type from status */
            if (sig == SIGTSTP) { /* if signal stopped is SIGTSTP */
                int jid = pid2jid(pid);
                getjobpid(job_list, pid)->state = ST;
                /* change job state to stopped */
                printf("Job [%d] (%d) stopped by signal 20\n", jid, pid);
            }
            else { /* if signal is SIGSTOP, SIGTTIN or SIGTTOU */
                int jid = pid2jid(pid);
                getjobpid(job_list, pid)->state = ST;
                /* change job state to stopped */
                printf("Job [%d] (%d) stopped by signal %d\n", jid, pid, sig);
            }
        }
        if(WIFSIGNALED(status)){/* child terminated cuz of uncaught signal */
            int sig = WTERMSIG(status);  /* get signal type from status */
            if (sig == SIGINT) { /* if sig is SIGINT */
                int jid = pid2jid(pid);
                deletejob(job_list, pid);
                printf("Job [%d] (%d) terminated by signal 2\n", jid, pid);
            }
            else { /* if signal is SIGHUP, SIGILL, SIGBUS, SIGALARM... */
                int jid = pid2jid(pid);
                printf("Job [%d] (%d) terminated by signal %d\n",jid, pid, sig);
                deletejob(job_list, pid);
            }
        }
    }
    if (errno == EINTR){ /* waitpid got interrupted */
        unix_error("waitpid function interrupted by signal");
    }
    if (errno != ECHILD){ /* there is still unreaped zombies left */
        unix_error("waitpid error");
    }
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    int FGpid = fgpid(job_list); /* get pid of a foreground job */
    kill(-FGpid, SIGINT); /* catch SIGINT and send it to foreground job */
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    int FGpid = fgpid(job_list); /* get pid of a foreground job */
    kill(-FGpid, SIGTSTP); /* catch SIGTSTP and send it to foreground job */
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *job_list) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", job_list[i].jid, job_list[i].pid, job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *job_list, pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *job_list, int output_fd) 
{
    int i;
    char buf[MAXLINE];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE);
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
    if(output_fd != STDOUT_FILENO)
        close(output_fd);
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

