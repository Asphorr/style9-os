/*
 * libedit_stub.c -- clean-room /usr/lib/libedit.3.dylib (style9 S5).
 *
 * dash names Apple's libedit for its interactive line editor and history,
 * but only reaches into it when the shell decides it is interactive (stdin
 * AND stderr are ttys).  Its own guards make NULL a complete answer:
 *
 *	el = el_init(...);  if (el != NULL) ... el_gets(el, ...)
 *	hist = history_init();  if (hist != NULL) ... history(hist, ...)
 *
 * so a stub whose el_init/history_init return NULL satisfies the bind and
 * permanently selects dash's non-editline input path -- which is the only
 * path a serial console can drive anyway.  No Apple code, no state, no
 * imports: every function returns its "absent" value.
 *
 * This is the THIRD dylib in the kernel registry (after our libSystem and
 * the real Homebrew libgmp) and the second clean-room one.  It must stay
 * self-contained (zero imports) so the dyld closure terminates at it.
 *
 * The opaque types are libedit's EditLine / History / HistEvent; callers
 * only ever pass the pointers back to us, so void * is the whole ABI.
 */

#define	UNUSED	__attribute__((unused))

void *
el_init(const char *prog UNUSED, void *fin UNUSED, void *fout UNUSED,
    void *ferr UNUSED)
{

	return (0);			/* no editor -> dash reads raw */
}

void
el_end(void *el UNUSED)
{
}

int
el_set(void *el UNUSED, int op UNUSED, ...)
{

	return (-1);
}

const char *
el_gets(void *el UNUSED, int *count)
{

	if (count != 0)
		*count = 0;
	return (0);
}

int
el_source(void *el UNUSED, const char *file UNUSED)
{

	return (-1);
}

void *
history_init(void)
{

	return (0);			/* no history -> dash skips it */
}

void
history_end(void *hist UNUSED)
{
}

int
history(void *hist UNUSED, void *ev UNUSED, int op UNUSED, ...)
{

	return (-1);
}
