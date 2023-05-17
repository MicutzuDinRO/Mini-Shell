// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "cmd.h"
#include "utils.h"

#define READ		0
#define WRITE		1

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	/* Execute cd. */
	if (dir == NULL || dir->string == NULL || strcmp(dir->string, "~") == 0 || strcmp(dir->string, "") == 0)
		if (chdir(getenv("HOME")) == -1)
			return false;

	if (strcmp(dir->string, "-") == 0)
		if (chdir(getenv("OLDPWD")) == -1)
			return false;

	if (chdir(dir->string) == -1)
		return false;

	return true;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	/* Execute exit/quit. */
	exit(EXIT_SUCCESS);

	return -1;	/* Replace with actual exit status. */
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	/* Sanity checks. */
	if (s == NULL)
		return 0;

	if (s->verb == NULL)
		return 0;

	/* If builtin command, execute the command. */
	if (strcmp(s->verb->string, "cd") == 0) {
		int fd;

		if (s->in != NULL) {
			fd = open(s->in->string, O_RDONLY);
			close(fd);
		}

		if (s->out != NULL) {
			fd = open(s->out->string, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			close(fd);
		}

		if (s->err != NULL) {
			fd = open(s->err->string, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}

		return !shell_cd(s->params);
	}

	if (strcmp(s->verb->string, "exit") == 0 || strcmp(s->verb->string, "quit") == 0)
		return shell_exit();

	/* If variable assignment, execute the assignment and return
	 * the exit status.
	 */
	if (s->verb->next_part != NULL)
		if (strcmp(s->verb->next_part->string, "=") == 0)
			return setenv(s->verb->string, get_word(s->verb->next_part->next_part), 1);

	/* If external command:
	 *   1. Fork new process
	 *     2c. Perform redirections in child
	 *     3c. Load executable in child
	 *   2. Wait for child
	 *   3. Return exit status
	 */
	pid_t pid = fork();
	int rc, status, fd, size;

	switch (pid) {
	case -1:
		return -1;

	case 0:
		if (s->in != NULL) {
			fd = open(get_word(s->in), O_RDONLY);
			dup2(fd, STDIN_FILENO);
			close(fd);
		}

		if (s->out != NULL) {
			if (s->io_flags & IO_OUT_APPEND)
				fd = open(get_word(s->out), O_WRONLY | O_CREAT | O_APPEND, 0644);
			else
				fd = open(get_word(s->out), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(fd, STDOUT_FILENO);
			close(fd);
		}

		if (s->err != NULL) {
			if (s->io_flags & IO_ERR_APPEND)
				fd = open(get_word(s->err), O_WRONLY | O_CREAT | O_APPEND, 0644);
			else
				fd = open(get_word(s->err), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}

		if (s->out != NULL && s->err != NULL && strcmp(s->out->string, s->err->string) == 0) {
			fd = open(get_word(s->out), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}

		rc = execvp(s->verb->string, get_argv(s, &size));
		if (rc == -1) {
			fprintf(stderr, "Execution failed for '%s'\n", s->verb->string);
			exit(EXIT_FAILURE);
		}
		return rc;

	default:
		rc = waitpid(pid, &status, 0);
		if (rc == -1)
			return -1;
		if (WIFEXITED(status))
			return status;
		return 0;
	}

	return -1; /* Replace with actual exit status. */
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* Execute cmd1 and cmd2 simultaneously. */
	pid_t pid1, pid2;
	int rc, status1, status2;

	pid1 = fork();
	switch (pid1) {
	case -1:
		return false;

	case 0:
		rc = parse_command(cmd1, level + 1, father);
		exit(rc);

	default:
		pid2 = fork();
		switch (pid2) {
		case -1:
			return false;

		case 0:
			rc = parse_command(cmd2, level + 1, father);
			exit(rc);

		default:
			rc = waitpid(pid1, &status1, 0);
			if (rc == -1)
				return false;
			rc = waitpid(pid2, &status2, 0);
			if (rc == -1)
				return false;
			return status1 && status2;
		}
	}

	return false; /* Replace with actual exit status. */
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* Redirect the output of cmd1 to the input of cmd2. */
	int fd[2];
	int rc, status1, status2;
	pid_t pid1, pid2;

	rc = pipe(fd);
	if (rc == -1)
		return false;

	pid1 = fork();
	switch (pid1) {
	case -1:
		return false;

	case 0:
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		close(fd[1]);
		rc = parse_command(cmd1, level + 1, father);
		exit(rc);

	default:
		close(fd[1]);
		pid2 = fork();
		switch (pid2) {
		case -1:
			return false;

		case 0:
			dup2(fd[0], STDIN_FILENO);
			close(fd[0]);
			rc = parse_command(cmd2, level + 1, father);
			exit(rc);

		default:
			close(fd[0]);
			rc = waitpid(pid1, &status1, 0);
			if (rc == -1)
				return false;
			rc = waitpid(pid2, &status2, 0);
			if (rc == -1)
				return false;
			return status2;
		}
	}

	return false; /* Replace with actual exit status. */
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	/* sanity checks */
	if (c == NULL)
		return 0;

	if (c->op == OP_NONE) {
		/* Execute a simple command. */
		return parse_simple(c->scmd, level, father);
	}

	int rc;

	switch (c->op) {
	case OP_SEQUENTIAL:
		/* Execute the commands one after the other. */
		rc = parse_command(c->cmd1, level + 1, c);
		rc &= parse_command(c->cmd2, level + 1, c);
		return rc;

	case OP_PARALLEL:
		/* Execute the commands simultaneously. */
		return run_in_parallel(c->cmd1, c->cmd2, level, c);

	case OP_CONDITIONAL_NZERO:
		/* Execute the second command only if the first one
		 * returns non zero.
		 */
		rc = parse_command(c->cmd1, level + 1, c);
		if (rc != 0)
			return parse_command(c->cmd2, level + 1, c);
		return rc;

	case OP_CONDITIONAL_ZERO:
		/* Execute the second command only if the first one
		 * returns zero.
		 */
		rc = parse_command(c->cmd1, level + 1, c);
		if (rc == 0)
			return parse_command(c->cmd2, level + 1, c);
		return rc;

	case OP_PIPE:
		/* Redirect the output of the first command to the
		 * input of the second.
		 */
		return run_on_pipe(c->cmd1, c->cmd2, level, c);

	default:
		return SHELL_EXIT;
	}

	return -1; /* Replace with actual exit code of command. */
}
