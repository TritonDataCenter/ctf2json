/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2011, Joyent, Inc. All rights reserved.
 * Copyright (c) 2011, Robert Mustacchi, Inc. All rights reserved.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <libctf.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <libgen.h>

/*
 * <sys/avl.h> is a private header. It's a little naughty to go in and expose
 * it. However, in all honesty better this than using some other avl
 * implementation.
 */
#include <sys/avl.h>

/*
 * <sys/list.h> is historically only for kernel drivers. We might as well use it
 * for the userland as well. We've graciously used the CDDL list.c from
 * usr/src/uts/common/list/list.c
 */
#include <sys/list.h>

#define	CTF_TYPE_NAMELEN	256		/* 2xDT_TYPE_NAMELEN */

#define	JSON_FORMAT_VERSION	"1.0"
#define	LIBCTF_VERSION		CTF_VERSION_2

typedef struct visit {
	avl_node_t	v_link;			/* list link */
	ctf_id_t	v_id;			/* id of this node */
	ctf_id_t	v_tdn;			/* id of what we point to */
} visit_t;

typedef struct psm_cb {
	ctf_file_t	*psm_fp;
	FILE		*psm_out;
	size_t		psm_size;
} psm_cb_t;

typedef struct arg {
	list_node_t	a_link;
	const char	*a_arg;
} arg_t;

static avl_tree_t g_visited;
static list_t g_types;
static const char *g_file;
static const char *g_prog;

static void walk_type(ctf_file_t *, ctf_id_t);

static int
visited_compare(const void *arg0, const void *arg1)
{
	const visit_t *l = arg0;
	const visit_t *r = arg1;

	if (l->v_id > r->v_id)
		return (1);

	if (l->v_id < r->v_id)
		return (-1);

	return (0);
}

static void
vwarn(const char *format, va_list alist)
{
	(void) fprintf(stderr, "%s: ", g_prog);
	(void) vfprintf(stderr, format, alist);
}

/*PRINTFLIKE1*/
static void
die(const char *format, ...)
{
	va_list alist;

	va_start(alist, format);
	vwarn(format, alist);
	va_end(alist);
	exit(1);
}

/*
 * Make sure that we understand the type the array contains
 */
static void
walk_array(ctf_file_t *fp, ctf_id_t id)
{
	ctf_arinfo_t arp;
	if (ctf_array_info(fp, id, &arp) != 0)
		die("failed to read array information\n");
	walk_type(fp, arp.ctr_contents);
}

/*ARGSUSED*/
static int
walk_struct_member(const char *name, ctf_id_t id, ulong_t offset, void *arg)
{
	ctf_file_t *fp = arg;
	walk_type(fp, id);
	return (0);
}

static void
walk_struct(ctf_file_t *fp, ctf_id_t id)
{
	(void) ctf_member_iter(fp, id, walk_struct_member, fp);
}

/*
 * Always attempt to resolve the type.
 */
static void
walk_type(ctf_file_t *fp, ctf_id_t oid)
{
	int kind;
	ctf_id_t id;
	visit_t search, *found;

	search.v_id = oid;
	found = avl_find(&g_visited, &search, NULL);
	if (found != NULL)
		return;

	id = ctf_type_resolve(fp, oid);

	search.v_id = id;
	found = avl_find(&g_visited, &search, NULL);
	if (found != NULL) {
		found = malloc(sizeof (visit_t));
		if (found == NULL)
			die("Failed to malloc\n");
		found->v_id = oid;
		found->v_tdn = id;
		avl_add(&g_visited, found);
		return;
	}

	kind = ctf_type_kind(fp, id);

	/*
	 * There are three different classes of types that we need to concern
	 * ourselves with here.
	 *
	 *  - Basic types with no aditional resolution. (ints, floats, etc.)
	 *  - Types that we never should dwal with (forward, typdef, etc.)
	 *  - Types that we need to look further at (arrays and structs.)
	 */
	switch (kind) {
	case CTF_K_ARRAY:
		walk_array(fp, id);
		break;
	case CTF_K_STRUCT:
		walk_struct(fp, id);
		break;
	case CTF_K_INTEGER:
	case CTF_K_FLOAT:
	case CTF_K_POINTER:
	case CTF_K_UNION:
	case CTF_K_ENUM:
		break;
	case CTF_K_UNKNOWN:
	case CTF_K_FORWARD:
	case CTF_K_TYPEDEF:
	case CTF_K_VOLATILE:
	case CTF_K_CONST:
	case CTF_K_RESTRICT:
	case CTF_K_FUNCTION:
	default:
		die("unknown or unresolved CTF kind for id %ld: %d\n", id,
		    kind);
	}

	/* Mark node as visited and don't forget where we came from */
	found = malloc(sizeof (visit_t));
	if (found == NULL)
		die("Failed to malloc\n");
	found->v_id = id;
	found->v_tdn = -1;
	avl_add(&g_visited, found);

	/* Already done, no need to add parent */
	if (oid == id)
		return;

	found = malloc(sizeof (visit_t));
	if (found == NULL)
		die("Failed to malloc\n");
	found->v_id = oid;
	found->v_tdn = id;
	avl_add(&g_visited, found);
}

static void
print_int(FILE *out, ctf_file_t *fp, ctf_id_t id)
{
	char name[CTF_TYPE_NAMELEN];
	ctf_encoding_t ep;

	if (ctf_type_encoding(fp, id, &ep) != 0)
		die("failed to read integer type encoding\n");
	if (ctf_type_name(fp, id, name, sizeof (name)) == NULL)
		die("failed to get name of type %ld\n", id);

	(void) fprintf(out, "\t\t{ \"name\": \"%s\", \"integer\": { "
	    "\"length\": %d, \"signed\": %s } }", name, ep.cte_bits / 8,
	    ep.cte_format & CTF_INT_SIGNED ? "true" : "false");
}

static void
print_float(FILE *out, ctf_file_t *fp, ctf_id_t id)
{
	char name[CTF_TYPE_NAMELEN];
	ctf_encoding_t ep;

	if (ctf_type_encoding(fp, id, &ep) != 0)
		die("failed to read integer type encoding\n");
	if (ctf_type_name(fp, id, name, sizeof (name)) == NULL)
		die("failed to get name of type %ld\n", id);

	(void) fprintf(out, "\t\t{ \"name\": \"%s\", \"float\": { "
	    "\"length\": %d } }", name, ep.cte_bits / 8);
}

static int
print_struct_member(const char *name, ctf_id_t id, ulong_t off, void *arg)
{
	ssize_t size;
	psm_cb_t *cb = arg;
	char type[CTF_TYPE_NAMELEN];

	if (ctf_type_name(cb->psm_fp, id, type, sizeof (type)) == NULL)
		die("failed to get name of type %ld\n", id);
	(void) fprintf(cb->psm_out, "\t\t\t{ \"name\": \"%s\", \"type\": "
	    "\"%s\" }",
	    name, type);
	size = ctf_type_size(cb->psm_fp, id);
	if (size + off / 8 != cb->psm_size)
		(void) fprintf(cb->psm_out, ",");
	(void) fprintf(cb->psm_out, "\n");
	return (0);
}

static void
print_struct(FILE *out, ctf_file_t *fp, ctf_id_t id)
{
	char name[CTF_TYPE_NAMELEN];
	psm_cb_t cb;

	if (ctf_type_name(fp, id, name, sizeof (name)) == NULL)
		die("failed to get name of type %ld\n", id);

	cb.psm_fp = fp;
	cb.psm_out = out;
	cb.psm_size = ctf_type_size(fp, id);

	(void) fprintf(out, "\t\t{ \"name\": \"%s\", \"struct\": [\n", name);
	(void) ctf_member_iter(fp, id, print_struct_member, &cb);
	(void) fprintf(out, "\t\t] }");
}

static void
print_typedef(FILE *out, ctf_file_t *fp, ctf_id_t idf, ctf_id_t idt)
{
	char from[CTF_TYPE_NAMELEN], to[CTF_TYPE_NAMELEN];

	if (ctf_type_name(fp, idf, from, sizeof (from)) == NULL)
		die("failed to get name of type %ld\n", idf);
	if (ctf_type_name(fp, idt, to, sizeof (to)) == NULL)
		die("failed to get name of type %ld\n", idt);

	(void) fprintf(out, "\t\t{ \"name\": \"%s\", \"typedef\": \"%s\" }",
	    from, to);
}

static void
print_tree(ctf_file_t *fp, avl_tree_t *avl)
{
	FILE *out = stdout;
	visit_t *cur, *last;
	int kind;

	cur = avl_first(avl);
	last = avl_last(avl);
	(void) fprintf(out, "\t[\n");
	for (; cur != NULL; cur = AVL_NEXT(avl, cur)) {
		if (cur->v_tdn != -1) {
			print_typedef(out, fp, cur->v_id, cur->v_tdn);
		} else {

			kind = ctf_type_kind(fp, cur->v_id);
			assert(kind != CTF_ERR);

			switch (kind) {
			case CTF_K_INTEGER:
				print_int(out, fp, cur->v_id);
				break;
			case CTF_K_FLOAT:
				print_float(out, fp, cur->v_id);
				break;
			case CTF_K_ARRAY:
				continue;
			case CTF_K_STRUCT:
				print_struct(out, fp, cur->v_id);
				break;
			default:
				die("Unimplemented kind. kind/id:  %d %ld\n",
				    kind, cur->v_id);
				break;
			}
		}

		if (cur != last)
			(void) fprintf(out, ",\n");
	}
	(void) fprintf(out, "\n\t]");
}

static void
print_metadata(FILE *out)
{
	time_t now;
	arg_t *arg, *last;

	now = time(NULL);
	(void) fprintf(out, "\t{\n\t\t\"ctf2json_version\": \"%s\",\n\t\t\""
	    "created_at\": "
	    "%ld,\n\t\t\"derived_from\": \"%s\",\n\t\t\"ctf_version\": %d,"
	    "\n\t\t\"requested_types\": [ ", JSON_FORMAT_VERSION, now, g_file,
	    LIBCTF_VERSION);

	last = list_tail(&g_types);
	for (arg = list_head(&g_types); arg != NULL;
	    arg = list_next(&g_types, arg)) {
		(void) fprintf(out, "\"%s\" ", arg->a_arg);
		if (arg != last)
			(void) fprintf(out, ", ");
	}

	(void) fprintf(out, "]\n\t}");
}

static void
add_list_arg(list_t *list, const char *arg)
{
	arg_t *a;

	a = malloc(sizeof (arg_t));
	if (a == NULL)
		die("failed to allocate memory\n");

	list_link_init(&a->a_link);
	a->a_arg = arg;
	list_insert_tail(list, a);
}

static void
build_tree(ctf_file_t *fp)
{
	ctf_id_t id;
	arg_t *arg;

	for (arg = list_head(&g_types); arg != NULL;
	    arg = list_next(&g_types, arg)) {
		id = ctf_lookup_by_name(fp, arg->a_arg);
		if (id < 0)
			die("type not present in binary: %s\n", arg->a_arg);

		walk_type(fp, id);
	}
}

static void
usage(void)
{
	(void) fprintf(stderr, "Usage: %s -f file -t type "
	    "[-t type ...]\n\n", g_prog);
	(void) fprintf(stderr, "\t-f  use file for CTF data\n");
	(void) fprintf(stderr, "\t-t  dump CTF data for type\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	int errp, c;
	ctf_file_t *ctfp;

	g_prog = basename(argv[0]);
	avl_create(&g_visited, visited_compare, sizeof (visit_t), 0);
	list_create(&g_types, sizeof (arg_t), 0);

	if (argc == 1)
		usage();

	while ((c = getopt(argc, argv, "t:f:")) != EOF) {
		switch (c) {
		case 'f':
			if (g_file != NULL)
				die("-f can only be specified once\n");
			g_file = optarg;
			break;
		case 't':
			add_list_arg(&g_types, optarg);
			break;
		case ':':
		case '?':
		default:
			usage();
			break;
		}
	}

	if (g_file == NULL)
		die("missing required -f option\n");

	if (list_is_empty(&g_types))
		die("missing required -t option\n");

	(void) ctf_version(LIBCTF_VERSION);
	ctfp = ctf_open(g_file, &errp);
	if (ctfp == NULL)
		die("failed to ctf_open file: %s\n", g_file);

	build_tree(ctfp);

	(void) fprintf(stdout, "{ \"metadata\":\n");
	print_metadata(stdout);
	(void) fprintf(stdout, ",\n\"data\":\n");
	print_tree(ctfp, &g_visited);
	(void) fprintf(stdout, "\n}\n");

	exit(0);
}
