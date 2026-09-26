/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Intrusive linked-list and queue primitives, modeled on FreeBSD's
 * sys/queue.h.  Macros only -- no runtime cost, type-generic via the
 * caller's own struct type.
 */

#ifndef _SYS_QUEUE_H_
#define	_SYS_QUEUE_H_

/*
 * The containing struct of a member pointer.  STAILQ_LAST uses it to get
 * from stqh_last (the last element's stqe_next field) to the element.
 */
#ifndef __containerof
#define	__containerof(ptr, type, member)				\
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif

/*
 * Four list flavours, all intrusive: the link fields live inside the
 * caller's struct (a *_ENTRY field), so there is no per-node allocation
 * and lists hand back whole objects.
 *
 *	SLIST	singly-linked list		head-only insert/remove
 *	LIST	doubly-linked list		O(1) remove anywhere
 *	STAILQ	singly-linked tail queue	O(1) head and tail insert
 *	TAILQ	doubly-linked tail queue	O(1) every common op
 *
 * Pick the smallest flavour that covers the call sites; every element
 * pays for its link fields.
 *
 *                       SLIST    LIST     STAILQ   TAILQ
 *	head size           1 ptr   1 ptr    2 ptr    2 ptr
 *	entry size          1 ptr   2 ptr    1 ptr    2 ptr
 *	insert head         O(1)    O(1)     O(1)     O(1)
 *	insert tail         O(N)    O(N)     O(1)     O(1)
 *	insert after        O(1)    O(1)     O(1)     O(1)
 *	insert before        --     O(1)      --      O(1)
 *	remove head         O(1)    O(1)     O(1)     O(1)
 *	remove arbitrary    O(N)    O(1)     O(N)     O(1)
 *	reverse traversal    --      --       --      O(1) per step
 *
 * Insert-head and forward iteration only: SLIST.  Also removal from the
 * middle: LIST.  FIFO: STAILQ, or TAILQ if removal from the middle or
 * reverse traversal is needed.
 *
 * Common macro parameters:
 *	head	address of the *_HEAD structure
 *	elm	pointer to a containing element
 *	field	the name of the *_ENTRY field embedded in the element's type
 *	type	the C type name of the containing struct (used to walk a
 *		double-link via a back pointer, where macro pasting alone
 *		cannot recover the type)
 *	var	loop variable in *_FOREACH
 *	tvar	temporary used by *_FOREACH_SAFE to remember the next
 *		element before the body runs (so the body may remove var)
 */

/*
 * ----------------------------------------------------------------------
 * SLIST -- singly-linked list, head-only insert/remove fast path.
 * ----------------------------------------------------------------------
 */

#define	SLIST_HEAD(name, type)						\
struct name {								\
	struct type	*slh_first;	/* first element */		\
}

#define	SLIST_HEAD_INITIALIZER(head)					\
	{ NULL }

#define	SLIST_ENTRY(type)						\
struct {								\
	struct type	*sle_next;	/* next element */		\
}

#define	SLIST_INIT(head)						\
	do { (head)->slh_first = NULL; } while (0)

#define	SLIST_EMPTY(head)	((head)->slh_first == NULL)
#define	SLIST_FIRST(head)	((head)->slh_first)
#define	SLIST_NEXT(elm, field)	((elm)->field.sle_next)

#define	SLIST_INSERT_HEAD(head, elm, field) do {			\
	(elm)->field.sle_next = (head)->slh_first;			\
	(head)->slh_first = (elm);					\
} while (0)

#define	SLIST_INSERT_AFTER(slistelm, elm, field) do {			\
	(elm)->field.sle_next = (slistelm)->field.sle_next;		\
	(slistelm)->field.sle_next = (elm);				\
} while (0)

#define	SLIST_REMOVE_HEAD(head, field) do {				\
	(head)->slh_first = (head)->slh_first->field.sle_next;		\
} while (0)

#define	SLIST_REMOVE_AFTER(elm, field) do {				\
	(elm)->field.sle_next = (elm)->field.sle_next->field.sle_next;	\
} while (0)

/*
 * SLIST_REMOVE: O(N) walk from the head to the predecessor of `elm'.
 * `type' is the containing struct's type, for the walker.
 */
#define	SLIST_REMOVE(head, elm, type, field) do {			\
	if ((head)->slh_first == (elm)) {				\
		SLIST_REMOVE_HEAD((head), field);			\
	} else {							\
		struct type *_curelm = (head)->slh_first;		\
		while (_curelm->field.sle_next != (elm))		\
			_curelm = _curelm->field.sle_next;		\
		_curelm->field.sle_next =				\
		    _curelm->field.sle_next->field.sle_next;		\
	}								\
} while (0)

#define	SLIST_FOREACH(var, head, field)					\
	for ((var) = SLIST_FIRST((head));				\
	    (var) != NULL;						\
	    (var) = SLIST_NEXT((var), field))

#define	SLIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = SLIST_FIRST((head));				\
	    (var) && ((tvar) = SLIST_NEXT((var), field), 1);		\
	    (var) = (tvar))

/* SLIST_SWAP: O(1); take the whole list under a lock, walk it unlocked. */
#define	SLIST_SWAP(head1, head2, type) do {				\
	struct type *_swap_first = (head1)->slh_first;			\
	(head1)->slh_first = (head2)->slh_first;			\
	(head2)->slh_first = _swap_first;				\
} while (0)

/*
 * ----------------------------------------------------------------------
 * LIST -- doubly-linked list, O(1) remove from anywhere, no tail pointer.
 *
 * The back link is a pointer to the previous next field (le_prev), so
 * REMOVE need not special-case the head.
 * ----------------------------------------------------------------------
 */

#define	LIST_HEAD(name, type)						\
struct name {								\
	struct type	*lh_first;	/* first element */		\
}

#define	LIST_HEAD_INITIALIZER(head)					\
	{ NULL }

#define	LIST_ENTRY(type)						\
struct {								\
	struct type	 *le_next;	/* next element */		\
	struct type	**le_prev;	/* address of previous next */	\
}

#define	LIST_INIT(head)							\
	do { (head)->lh_first = NULL; } while (0)

#define	LIST_EMPTY(head)	((head)->lh_first == NULL)
#define	LIST_FIRST(head)	((head)->lh_first)
#define	LIST_NEXT(elm, field)	((elm)->field.le_next)

#define	LIST_INSERT_HEAD(head, elm, field) do {				\
	if (((elm)->field.le_next = (head)->lh_first) != NULL)		\
		(head)->lh_first->field.le_prev = &(elm)->field.le_next;\
	(head)->lh_first = (elm);					\
	(elm)->field.le_prev = &(head)->lh_first;			\
} while (0)

#define	LIST_INSERT_AFTER(listelm, elm, field) do {			\
	if (((elm)->field.le_next =					\
	    (listelm)->field.le_next) != NULL)				\
		(listelm)->field.le_next->field.le_prev =		\
		    &(elm)->field.le_next;				\
	(listelm)->field.le_next = (elm);				\
	(elm)->field.le_prev = &(listelm)->field.le_next;		\
} while (0)

#define	LIST_INSERT_BEFORE(listelm, elm, field) do {			\
	(elm)->field.le_prev = (listelm)->field.le_prev;		\
	(elm)->field.le_next = (listelm);				\
	*(listelm)->field.le_prev = (elm);				\
	(listelm)->field.le_prev = &(elm)->field.le_next;		\
} while (0)

#define	LIST_REMOVE(elm, field) do {					\
	if ((elm)->field.le_next != NULL)				\
		(elm)->field.le_next->field.le_prev =			\
		    (elm)->field.le_prev;				\
	*(elm)->field.le_prev = (elm)->field.le_next;			\
} while (0)

#define	LIST_FOREACH(var, head, field)					\
	for ((var) = LIST_FIRST((head));				\
	    (var) != NULL;						\
	    (var) = LIST_NEXT((var), field))

#define	LIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = LIST_FIRST((head));				\
	    (var) && ((tvar) = LIST_NEXT((var), field), 1);		\
	    (var) = (tvar))

#define	LIST_SWAP(head1, head2, type, field) do {			\
	struct type *_swap_tmp = (head1)->lh_first;			\
	(head1)->lh_first = (head2)->lh_first;				\
	(head2)->lh_first = _swap_tmp;					\
	if ((head1)->lh_first != NULL)					\
		(head1)->lh_first->field.le_prev = &(head1)->lh_first;	\
	if ((head2)->lh_first != NULL)					\
		(head2)->lh_first->field.le_prev = &(head2)->lh_first;	\
} while (0)

/*
 * ----------------------------------------------------------------------
 * STAILQ -- singly-linked tail queue.  O(1) head and tail insert.
 *
 * stqh_last always points at the last element's stqe_next slot, so an
 * append is one splice.  Removal from the middle is O(N).
 * ----------------------------------------------------------------------
 */

#define	STAILQ_HEAD(name, type)						\
struct name {								\
	struct type	 *stqh_first;	/* first element */		\
	struct type	**stqh_last;	/* address of last next */	\
}

#define	STAILQ_HEAD_INITIALIZER(head)					\
	{ NULL, &(head).stqh_first }

#define	STAILQ_ENTRY(type)						\
struct {								\
	struct type	*stqe_next;	/* next element */		\
}

#define	STAILQ_INIT(head) do {						\
	(head)->stqh_first = NULL;					\
	(head)->stqh_last = &(head)->stqh_first;			\
} while (0)

#define	STAILQ_EMPTY(head)	((head)->stqh_first == NULL)
#define	STAILQ_FIRST(head)	((head)->stqh_first)
#define	STAILQ_NEXT(elm, field)	((elm)->field.stqe_next)

#define	STAILQ_LAST(head, type, field)					\
	(STAILQ_EMPTY((head)) ? NULL :					\
	    __containerof((head)->stqh_last,				\
	        struct type, field.stqe_next))

#define	STAILQ_INSERT_HEAD(head, elm, field) do {			\
	if (((elm)->field.stqe_next = (head)->stqh_first) == NULL)	\
		(head)->stqh_last = &(elm)->field.stqe_next;		\
	(head)->stqh_first = (elm);					\
} while (0)

#define	STAILQ_INSERT_TAIL(head, elm, field) do {			\
	(elm)->field.stqe_next = NULL;					\
	*(head)->stqh_last = (elm);					\
	(head)->stqh_last = &(elm)->field.stqe_next;			\
} while (0)

#define	STAILQ_INSERT_AFTER(head, listelm, elm, field) do {		\
	if (((elm)->field.stqe_next =					\
	    (listelm)->field.stqe_next) == NULL)			\
		(head)->stqh_last = &(elm)->field.stqe_next;		\
	(listelm)->field.stqe_next = (elm);				\
} while (0)

#define	STAILQ_REMOVE_HEAD(head, field) do {				\
	if (((head)->stqh_first =					\
	    (head)->stqh_first->field.stqe_next) == NULL)		\
		(head)->stqh_last = &(head)->stqh_first;		\
} while (0)

#define	STAILQ_REMOVE_AFTER(head, elm, field) do {			\
	if (((elm)->field.stqe_next =					\
	    (elm)->field.stqe_next->field.stqe_next) == NULL)		\
		(head)->stqh_last = &(elm)->field.stqe_next;		\
} while (0)

#define	STAILQ_REMOVE(head, elm, type, field) do {			\
	if ((head)->stqh_first == (elm)) {				\
		STAILQ_REMOVE_HEAD((head), field);			\
	} else {							\
		struct type *_curelm = (head)->stqh_first;		\
		while (_curelm->field.stqe_next != (elm))		\
			_curelm = _curelm->field.stqe_next;		\
		STAILQ_REMOVE_AFTER((head), _curelm, field);		\
	}								\
} while (0)

#define	STAILQ_FOREACH(var, head, field)				\
	for ((var) = STAILQ_FIRST((head));				\
	    (var) != NULL;						\
	    (var) = STAILQ_NEXT((var), field))

#define	STAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = STAILQ_FIRST((head));				\
	    (var) && ((tvar) = STAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))

#define	STAILQ_CONCAT(head1, head2) do {				\
	if (!STAILQ_EMPTY((head2))) {					\
		*(head1)->stqh_last = (head2)->stqh_first;		\
		(head1)->stqh_last = (head2)->stqh_last;		\
		STAILQ_INIT((head2));					\
	}								\
} while (0)

/*
 * ----------------------------------------------------------------------
 * TAILQ -- doubly-linked tail queue.  Everything O(1), including reverse
 * traversal, for two pointers per element and per head.
 * ----------------------------------------------------------------------
 */

#define	TAILQ_HEAD(name, type)						\
struct name {								\
	struct type	 *tqh_first;	/* first element */		\
	struct type	**tqh_last;	/* address of last next */	\
}

#define	TAILQ_HEAD_INITIALIZER(head)					\
	{ NULL, &(head).tqh_first }

#define	TAILQ_ENTRY(type)						\
struct {								\
	struct type	 *tqe_next;	/* next element */		\
	struct type	**tqe_prev;	/* address of previous next */	\
}

#define	TAILQ_INIT(head) do {						\
	(head)->tqh_first = NULL;					\
	(head)->tqh_last = &(head)->tqh_first;				\
} while (0)

#define	TAILQ_EMPTY(head)	((head)->tqh_first == NULL)
#define	TAILQ_FIRST(head)	((head)->tqh_first)
#define	TAILQ_NEXT(elm, field)	((elm)->field.tqe_next)

#define	TAILQ_LAST(head, headname)					\
	(*(((struct headname *)((head)->tqh_last))->tqh_last))

#define	TAILQ_PREV(elm, headname, field)				\
	(*(((struct headname *)((elm)->field.tqe_prev))->tqh_last))

#define	TAILQ_INSERT_HEAD(head, elm, field) do {			\
	if (((elm)->field.tqe_next = (head)->tqh_first) != NULL)	\
		(head)->tqh_first->field.tqe_prev =			\
		    &(elm)->field.tqe_next;				\
	else								\
		(head)->tqh_last = &(elm)->field.tqe_next;		\
	(head)->tqh_first = (elm);					\
	(elm)->field.tqe_prev = &(head)->tqh_first;			\
} while (0)

#define	TAILQ_INSERT_TAIL(head, elm, field) do {			\
	(elm)->field.tqe_next = NULL;					\
	(elm)->field.tqe_prev = (head)->tqh_last;			\
	*(head)->tqh_last = (elm);					\
	(head)->tqh_last = &(elm)->field.tqe_next;			\
} while (0)

#define	TAILQ_INSERT_AFTER(head, listelm, elm, field) do {		\
	if (((elm)->field.tqe_next =					\
	    (listelm)->field.tqe_next) != NULL)				\
		(elm)->field.tqe_next->field.tqe_prev =			\
		    &(elm)->field.tqe_next;				\
	else								\
		(head)->tqh_last = &(elm)->field.tqe_next;		\
	(listelm)->field.tqe_next = (elm);				\
	(elm)->field.tqe_prev = &(listelm)->field.tqe_next;		\
} while (0)

#define	TAILQ_INSERT_BEFORE(listelm, elm, field) do {			\
	(elm)->field.tqe_prev = (listelm)->field.tqe_prev;		\
	(elm)->field.tqe_next = (listelm);				\
	*(listelm)->field.tqe_prev = (elm);				\
	(listelm)->field.tqe_prev = &(elm)->field.tqe_next;		\
} while (0)

#define	TAILQ_REMOVE(head, elm, field) do {				\
	if (((elm)->field.tqe_next) != NULL)				\
		(elm)->field.tqe_next->field.tqe_prev =			\
		    (elm)->field.tqe_prev;				\
	else								\
		(head)->tqh_last = (elm)->field.tqe_prev;		\
	*(elm)->field.tqe_prev = (elm)->field.tqe_next;			\
} while (0)

#define	TAILQ_FOREACH(var, head, field)					\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) != NULL;						\
	    (var) = TAILQ_NEXT((var), field))

#define	TAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) && ((tvar) = TAILQ_NEXT((var), field), 1);		\
	    (var) = (tvar))

#define	TAILQ_FOREACH_REVERSE(var, head, headname, field)		\
	for ((var) = TAILQ_LAST((head), headname);			\
	    (var) != NULL;						\
	    (var) = TAILQ_PREV((var), headname, field))

#define	TAILQ_FOREACH_REVERSE_SAFE(var, head, headname, field, tvar)	\
	for ((var) = TAILQ_LAST((head), headname);			\
	    (var) && ((tvar) = TAILQ_PREV((var), headname, field), 1);	\
	    (var) = (tvar))

#define	TAILQ_CONCAT(head1, head2, field) do {				\
	if (!TAILQ_EMPTY((head2))) {					\
		*(head1)->tqh_last = (head2)->tqh_first;		\
		(head2)->tqh_first->field.tqe_prev = (head1)->tqh_last;	\
		(head1)->tqh_last = (head2)->tqh_last;			\
		TAILQ_INIT((head2));					\
	}								\
} while (0)

#endif /* !_SYS_QUEUE_H_ */
