/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.
 */
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <string.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
#include "../posix_spawn/spawn.h"

static char *supported_jobs[] = {"kill", "fg", "bg", "jobs", "stop", "exit",
                                 "cust1", "cust2",
							     "sleep", "ls", "ps"};

static int num_supported_jobs = 11;

static void handle_child_status(pid_t pid, int status);

//Help for built-in commands it support
static void
usage(char *progname)
{
	if (strcmp(progname, "cush") == 0) {
		printf("Usage: %s -h\n"
		       "cush help\n",
			   progname);
			   
		exit(EXIT_SUCCESS);
	}
	else {
		printf("Usage: %s -h\n"
		       "help\n",
			   progname);
	}
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

static struct job * pid2job[MAXJOBS];

static struct ast_command_line * jid2cline[MAXJOBS];

/* Return job corresponding to jid */

//Receive jid, which is job's id (process id),
//and return job(process) of that id.
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

	//waitpid() : suspends execution of calling process
	//until child process, specified by pid, has changed states.
	//wait(&status) equivalent to waitpid(-1, &status, 0)
    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, not that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

static struct job *jobs2remove[MAXJOBS];
int num_jobs2remove = 0;

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */
	
	if (status == 0) {
		struct job *job = pid2job[(int)pid];
		job->num_processes_alive--;
		
		if (job->num_processes_alive == 0) {
			if (job->status == BACKGROUND) {
				printf("[%d]	Done\n", job->jid);
			}
			jobs2remove[num_jobs2remove] = job;
			num_jobs2remove++;
			pid2job[(int)pid] = NULL;
		}
	}
}

void remove_job(struct job *job);

void
remove_job(struct job *job)
{
	struct list_elem *e = &(job->elem);
	struct ast_command_line * cline = jid2cline[job->jid];
	jid2cline[job->jid] = NULL;
	
	list_remove(e);
	delete_job(job);
	
	//ast_command_line_free(cline);
	free(cline);
}

void remove_jobs(void);

void
remove_jobs(void) 
{
	for (int i = 0; i < num_jobs2remove; i++) {
		struct job *job = jobs2remove[i];
		
		remove_job(job);
		
		jobs2remove[i] = NULL;
	}
	
	num_jobs2remove = 0;
}

void built_in_jobs(void);

void
built_in_jobs(void)
{
	for (struct list_elem * e = list_begin(&job_list); 
         e != list_prev(list_end(&job_list)); 
         e = list_next(e)) {
		
		struct job *job = list_entry(e, struct job, elem);
		
		print_job(job);
	}
}

void built_in_fg(int jid);

void 
built_in_fg(int jid)
{
	struct job *job = jid2job[jid];
	job->status = FOREGROUND;
	wait_for_job(job);
}

//extern char **environ;

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
	//If executed custom shell with -h option,
	//print help about commands it provides.
	//It does not start the custom command.
	//ex) cush -h
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

	//Initialize all initial conditions
    list_init(&job_list); //initialize job list for custom shell
    signal_set_handler(SIGCHLD, sigchld_handler); //
    termstate_init(); //

    /* Read/eval loop. */
	//Receive commands for custom shell
	//If go out from this loop, user go out from this custom shell
    for (;;) {
		signal_block(SIGCHLD);

        /* Do not output a prompt unless shell's stdin is a terminal */
		//Receive command from terminal
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

		//If readline(prompt) received EOF, it returns NULL.
		//If recieved ctrl+d (EOF), go out custom shell.
        if (cmdline == NULL)  /* User typed EOF */
            break;

        struct ast_command_line * cline = ast_parse_command_line(cmdline);
        free (cmdline);
		
        if (cline == NULL) {                 /* Error in command line */
			continue;
		}

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
			sigchld_handler(SIGCHLD, NULL, NULL);
			remove_jobs();
            continue;
        }
		
		for (struct list_elem * e = list_begin (&cline->pipes); 
             e != list_end (&cline->pipes); 
             e = list_next (e)) {
				 
			//printf("pipe\n");
				 
			struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
			
			struct job *job = add_job(pipe);
			
			jid2cline[job->jid] = cline;
			
			if (!(pipe->bg_job)) {
				job->status = FOREGROUND;
			}
			else {
				job->status = BACKGROUND;
			}
			
			for (struct list_elem * e = list_begin(&pipe->commands); 
			     e != list_end(&pipe->commands); 
			     e = list_next(e)) {
					 
				//printf("cmd\n");
				
				struct ast_command *cmd = list_entry(e, struct ast_command, elem);
				
				char **argv = cmd->argv;
				char *to_do = argv[0];
				
				bool do_job = false;
				for (int i = 0; i < num_supported_jobs; i++) {
					char *supported_job = supported_jobs[i];
					if (strcmp(supported_job, to_do) == 0) {
						do_job = true;
						break;
					}
				}
				
				if (!do_job) {
					printf("not supported\n");
					continue;
				}
				
				if (argv[1] != NULL) {
					if (strcmp(argv[1], "-h") == 0) {
						usage(to_do);
						continue;
					}
				}
				
				if (strcmp(to_do, "jobs") == 0) {
					built_in_jobs();
					continue;
				}
				
				else if (strcmp(to_do, "fg") == 0){
					built_in_fg(atoi(argv[1]));
					continue;
				}
				
				pid_t pid;
				int status;
				status = posix_spawnp(&pid, to_do, NULL, NULL, argv, NULL);
				
				if (status != 0) {
					printf("posix status error\n");
				}
				
				pid2job[(int)pid] = job;
				job->num_processes_alive++;
				
			}
			
			if (job->num_processes_alive == 0) {
				remove_job(job);
			}
			else if (!(pipe->bg_job)) {
				wait_for_job(job);
			}
			else {
				print_job(job);
			}
		}
		
		sigchld_handler(SIGCHLD, NULL, NULL);
		remove_jobs();
		
		//ast_command_line_free(cline);
		
    }
	
    return 0;
}
