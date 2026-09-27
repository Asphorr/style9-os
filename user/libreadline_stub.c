/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * libreadline_stub.c -- clean-room libreadline.8.dylib, a line reader
 * without editing or history.
 *
 * sqlite3's shell reads through readline only when stdin is a terminal.
 * This readline prints the prompt and takes one line from fd 0 as the
 * terminal's own line discipline delivers it (erase and kill work,
 * arrows and history do not), returning it malloc'd without the newline,
 * or NULL at end of file.  The history calls keep nothing; completion is
 * never offered.
 *
 * It imports read, write, malloc, realloc and __error from libSystem,
 * which every program here has already mapped.
 */

typedef __SIZE_TYPE__	size_t;

#define	NULL		((void *)0)
#define	EINTR		4
#define	ENOTSUP		45

long	read(int, void *, unsigned long);
long	write(int, const void *, unsigned long);
void	*malloc(size_t);
void	*realloc(void *, size_t);
int	*__error(void);

/* Set by the caller, never consulted: no completion here. */
void	*rl_attempted_completion_function;
int	 rl_attempted_completion_over;

char *
readline(const char *prompt)
{
	size_t	 cap;
	size_t	 len;
	char	*line;
	char	*grown;
	char	 c;
	long	 n;

	if (prompt != NULL) {
		for (len = 0; prompt[len] != '\0'; len++)
			continue;
		(void)write(1, prompt, len);
	}
	cap = 128;
	len = 0;
	line = malloc(cap);
	if (line == NULL)
		return (NULL);
	for (;;) {
		n = read(0, &c, 1);
		if (n < 0 && *__error() == EINTR)
			continue;	/* the caller's handler has run */
		if (n <= 0) {
			if (len == 0)
				return (NULL);
			break;		/* a last line without a newline */
		}
		if (c == '\n')
			break;
		if (len + 1 == cap) {
			grown = realloc(line, cap * 2);
			if (grown == NULL)
				break;
			line = grown;
			cap *= 2;
		}
		line[len++] = c;
	}
	line[len] = '\0';
	return (line);
}

void
add_history(const char *line)
{

	(void)line;
}

/* No history file is kept: readline's errno-style ENOTSUP. */
int
read_history(const char *file)
{

	(void)file;
	return (ENOTSUP);
}

int
write_history(const char *file)
{

	(void)file;
	return (ENOTSUP);
}

void
stifle_history(int max)
{

	(void)max;
}

char **
rl_completion_matches(const char *text, void *generator)
{

	(void)text;
	(void)generator;
	return (NULL);
}
