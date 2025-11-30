/*-------------------------------------------------------------------------
 *
 * dump.h
 *    Unified index dump functionality for pg_textsearch
 *
 * IDENTIFICATION
 *    src/dump.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DUMP_H
#define DUMP_H

#include <postgres.h>

#include <lib/stringinfo.h>

/*
 * DumpOutput - abstraction for writing dump output to either
 * a StringInfo (for SQL return) or a FILE (for file output)
 */
typedef struct DumpOutput
{
	StringInfo str;		  /* StringInfo for SQL return, NULL if file mode */
	FILE	  *fp;		  /* FILE for file output, NULL if string mode */
	bool	   full_dump; /* If true, no truncation (file mode) */
} DumpOutput;

/* Initialize for string output (SQL return) */
static inline void
dump_init_string(DumpOutput *out, StringInfo str)
{
	out->str	   = str;
	out->fp		   = NULL;
	out->full_dump = false;
}

/* Initialize for file output */
static inline void
dump_init_file(DumpOutput *out, FILE *fp)
{
	out->str	   = NULL;
	out->fp		   = fp;
	out->full_dump = true;
}

/* Printf-style output */
static inline void dump_printf(DumpOutput *out, const char *fmt, ...)
		pg_attribute_printf(2, 3);

static inline void
dump_printf(DumpOutput *out, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	if (out->fp)
	{
		vfprintf(out->fp, fmt, args);
	}
	else if (out->str)
	{
		/* Use appendStringInfoVA for StringInfo */
		for (;;)
		{
			va_list args_copy;
			int		needed;

			va_copy(args_copy, args);
			needed = appendStringInfoVA(out->str, fmt, args_copy);
			va_end(args_copy);

			if (needed == 0)
				break;
			enlargeStringInfo(out->str, needed);
		}
	}

	va_end(args);
}

/* Check if we should truncate output (only in string mode) */
static inline bool
dump_should_truncate(DumpOutput *out, size_t limit)
{
	if (out->full_dump)
		return false;
	if (out->str && out->str->len > (int)limit)
		return true;
	return false;
}

/* Dump index function - declared in dump.c */
extern void tp_dump_index_to_output(const char *index_name, DumpOutput *out);

/* Summarize index function - declared in dump.c */
extern void
tp_summarize_index_to_output(const char *index_name, DumpOutput *out);

#endif /* DUMP_H */
