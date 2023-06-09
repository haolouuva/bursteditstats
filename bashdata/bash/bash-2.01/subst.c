/* subst.c -- The part of the shell that does parameter, command, and
   globbing substitutions. */

/* Copyright (C) 1987,1989 Free Software Foundation, Inc.

   This file is part of GNU Bash, the Bourne Again SHell.

   Bash is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2, or (at your option) any later
   version.

   Bash is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License along
   with Bash; see the file COPYING.  If not, write to the Free Software
   Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. */

#include "config.h"

#include "bashtypes.h"
#include <stdio.h>
#include <pwd.h>
#include <signal.h>
#include <errno.h>

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif

#include "bashansi.h"
#include "posixstat.h"

#include "shell.h"
#include "flags.h"
#include "jobs.h"
#include "execute_cmd.h"
#include "filecntl.h"
#include "trap.h"
#include "pathexp.h"
#include "mailcheck.h"

#if !defined (HAVE_RESTARTABLE_SYSCALLS)	/* for getc_with_restart */
#include "input.h"
#endif

#include "builtins/getopt.h"
#include "builtins/common.h"

#if defined (READLINE)
#  include "bashline.h"
#  include <readline/readline.h>
#else
#  include <tilde/tilde.h>
#endif

#if defined (HISTORY)
#  include "bashhist.h"
#  include <readline/history.h>
#endif

#include <glob/fnmatch.h>

#if !defined (errno)
extern int errno;
#endif /* !errno */

/* The size that strings change by. */
#define DEFAULT_INITIAL_ARRAY_SIZE 112
#define DEFAULT_ARRAY_SIZE 128

/* Variable types. */
#define VT_VARIABLE	0
#define VT_POSPARMS	1
#define VT_ARRAYVAR	2
#define VT_ARRAYMEMBER	3

/* Flags for quoted_strchr */
#define ST_BACKSL	0x01
#define ST_CTLESC	0x02

/* How to quote character C. */
static char *make_quoted_char ();

/* Process ID of the last command executed within command substitution. */
pid_t last_command_subst_pid = NO_PID;

/* Extern functions and variables from different files. */
extern int last_command_exit_value, interactive, interactive_shell;
extern int subshell_environment, startup_state;
extern int dollar_dollar_pid;
extern int posixly_correct;
extern int eof_encountered, eof_encountered_limit, ignoreeof;
extern char *this_command_name;
extern struct fd_bitmap *current_fds_to_close;
#if defined (READLINE)
extern int no_line_editing;
extern int hostname_list_initialized;
#endif

extern void getopts_reset ();

/* Non-zero means to allow unmatched globbed filenames to expand to
   a null file. */
int allow_null_glob_expansion;

/* Variables to keep track of which words in an expanded word list (the
   output of expand_word_list_internal) are the result of globbing
   expansions.  GLOB_ARGV_FLAGS is used by execute_cmd.c. */
char *glob_argv_flags;
static int glob_argv_flags_size;

static WORD_LIST expand_word_error, expand_word_fatal;
static char expand_param_error, expand_param_fatal;

static WORD_LIST *expand_string_internal ();
static WORD_LIST *expand_word_internal (), *expand_word_list_internal ();
static WORD_LIST *expand_string_leave_quoted ();
static WORD_LIST *expand_string_for_rhs ();
static WORD_LIST *word_list_split ();
static WORD_LIST *quote_list (), *dequote_list ();
static int unquoted_substring (), unquoted_member ();
static int do_assignment_internal ();
static char *string_extract_verbatim (), *string_extract ();
static char *string_extract_double_quoted (), *string_extract_single_quoted ();
static int skip_single_quoted (), skip_double_quoted ();
static char *extract_delimited_string ();
static char *extract_dollar_brace_string ();

/* **************************************************************** */
/*								    */
/*			Utility Functions			    */
/*								    */
/* **************************************************************** */

/* Cons a new string from STRING starting at START and ending at END,
   not including END. */
char *
substring (string, start, end)
     char *string;
     int start, end;
{
  register int len;
  register char *result;

  len = end - start;
  result = xmalloc (len + 1);
  strncpy (result, string + start, len);
  result[len] = '\0';
  return (result);
}

static char *
quoted_substring (string, start, end)
     char *string;
     int start, end;
{
  register int len, l;
  register char *result, *s, *r;

  len = end - start;

  /* Move to string[start], skipping quoted characters. */
  for (s = string, l = 0; *s && l < start; )
    {
      if (*s == CTLESC)
	{
          s++;
          continue;
	}
      l++;
      if (*s == 0)
        break;
    }

  r = result = xmalloc (2*len + 1);      /* save room for quotes */

  /* Copy LEN characters, including quote characters. */
  s = string + l;
  for (l = 0; l < len; s++)
    {
      if (*s == CTLESC)
        *r++ = *s++;
      *r++ = *s;
      l++;
      if (*s == 0)
        break;
    }
  *r = '\0';
  return result;
}

/* Find the first occurrence of character C in string S, obeying shell
   quoting rules.  If (FLAGS & ST_BACKSL) is non-zero, backslash-escaped
   characters are skipped.  If (FLAGS & ST_CTLESC) is non-zero, characters
   escaped with CTLESC are skipped. */
static inline char *
quoted_strchr (s, c, flags)
     char *s;
     int c, flags;
{
  register char *p;

  for (p = s; *p; p++)
    {
      if (((flags & ST_BACKSL) && *p == '\\')
	    || ((flags & ST_CTLESC) && *p == CTLESC))
	{
	  p++;
	  if (*p == '\0')
	    return ((char *)NULL);
	  continue;
	}
      else if (*p == c)
	return p;
    }
  return ((char *)NULL);
}

/* Conventions:

     A string with s[0] == CTLNUL && s[1] == 0 is a quoted null string.
     The parser passes CTLNUL as CTLESC CTLNUL. */

/* The parser passes us CTLESC as CTLESC CTLESC and CTLNUL as CTLESC CTLNUL.
   This is necessary to make unquoted CTLESC and CTLNUL characters in the
   data stream pass through properly.
   Here we remove doubled CTLESC characters inside quoted strings before
   quoting the entire string, so we do not double the number of CTLESC
   characters. */
static char *
remove_quoted_escapes (string)
     char *string;
{
  register char *s;
  int docopy;
  char *t, *t1;

  if (string == NULL)
    return (string);

  t1 = t = xmalloc (strlen (string) + 1);
  for (docopy = 0, s = string; *s; s++, t1++)
    {
      if (*s == CTLESC && (s[1] == CTLESC || s[1] == CTLNUL))
	{
	  s++;
	  docopy = 1;
	}
      *t1 = *s;
    }
  *t1 = '\0';
  if (docopy)
    strcpy (string, t);
  free (t);
  return (string);
}

/* Quote escape characters in string s, but no other characters.  This is
   used to protect CTLESC and CTLNUL in variable values from the rest of
   the word expansion process after the variable is expanded. */
static char *
quote_escapes (string)
     char *string;
{
  register char *s, *t;
  char *result;

  result = xmalloc ((strlen (string) * 2) + 1);
  for (s = string, t = result; *s; )
    {
      if (*s == CTLESC || *s == CTLNUL)
	*t++ = CTLESC;
      *t++ = *s++;
    }
  *t = '\0';
  return (result);
}

#ifdef INCLUDE_UNUSED
static char *
dequote_escapes (string)
     char *string;
{
  register char *s, *t;
  char *result;

  result = xmalloc (strlen (string) + 1);
  for (s = string, t = result; *s; )
    {
      if (*s == CTLESC && (s[1] == CTLESC || s[1] == CTLNUL))
	{
	  s++;
	  if (*s == '\0')
	    break;
	}
      *t++ = *s++;
    }
  *t = '\0';
  return result;
}
#endif

/* Extract a substring from STRING, starting at SINDEX and ending with
   one of the characters in CHARLIST.  Don't make the ending character
   part of the string.  Leave SINDEX pointing at the ending character.
   Understand about backslashes in the string.  If VARNAME is non-zero,
   and array variables have been compiled into the shell, everything
   between a `[' and a corresponding `]' is skipped over. */
static char *
string_extract (string, sindex, charlist, varname)
     char *string, *charlist;
     int *sindex, varname;
{
  register int c, i;
  char *temp;

  for (i = *sindex; c = string[i]; i++)
    {
      if (c == '\\')
	if (string[i + 1])
	  i++;
	else
	  break;
#if defined (ARRAY_VARS)
      else if (varname && c == '[')
	{
	  int ni;
	  /* If this is an array subscript, skip over it and continue. */
	  ni = skipsubscript (string, i);
	  if (string[ni] == ']')
	    i = ni;
	}
#endif
      else if (MEMBER (c, charlist))
	  break;
    }
  c = i - *sindex;
  temp = xmalloc (1 + c);
  strncpy (temp, string + *sindex, c);
  temp[c] = '\0';
  *sindex = i;
  return (temp);
}

/* Extract the contents of STRING as if it is enclosed in double quotes.
   SINDEX, when passed in, is the offset of the character immediately
   following the opening double quote; on exit, SINDEX is left pointing after
   the closing double quote.  If STRIPDQ is non-zero, unquoted double
   quotes are stripped and the string is terminated by a null byte.
   Backslashes between the embedded double quotes are processed.  If STRIPDQ
   is zero, an unquoted `"' terminates the string. */
static inline char *
string_extract_double_quoted (string, sindex, stripdq)
     char *string;
     int *sindex, stripdq;
{
  int c, j, i, t;
  char *temp, *ret;		/* The new string we return. */
  int pass_next, backquote, si;	/* State variables for the machine. */
  int dquote;

  pass_next = backquote = dquote = 0;
  temp = xmalloc (1 + strlen (string) - *sindex);

  for (j = 0, i = *sindex; c = string[i]; i++)
    {
      /* Process a character that was quoted by a backslash. */
      if (pass_next)
	{
	  /* Posix.2 sez:

	     ``The backslash shall retain its special meaning as an escape
	     character only when followed by one of the characters:
	     	$	`	"	\	<newline>''.

	     If STRIPDQ is zero, we handle the double quotes here and let
	     expand_word_internal handle the rest.  If STRIPDQ is non-zero,
	     we have already been through one round of backslash stripping,
	     and want to strip these backslashes only if DQUOTE is non-zero,
	     indicating that we are inside an embedded double-quoted string. */

	     /* If we are in an embedded quoted string, then don't strip
		backslashes before characters for which the backslash
		retains its special meaning, but remove backslashes in
		front of other characters.  If we are not in an
		embedded quoted string, don't strip backslashes at all.
		This mess is necessary because the string was already
		surrounded by double quotes (and sh has some really weird
		quoting rules).
		The returned string will be run through expansion as if
		it were double-quoted. */
	  if ((stripdq == 0 && c != '"') ||
	      (stripdq && ((dquote && strchr (slashify_in_quotes, c)) || dquote == 0)))
	    temp[j++] = '\\';
	  temp[j++] = c;
	  pass_next = 0;
	  continue;
	}

      /* A backslash protects the next character.  The code just above
	 handles preserving the backslash in front of any character but
	 a double quote. */
      if (c == '\\')
	{
	  pass_next++;
	  continue;
	}

      /* Inside backquotes, ``the portion of the quoted string from the
	 initial backquote and the characters up to the next backquote
	 that is not preceded by a backslash, having escape characters
	 removed, defines that command''. */
      if (backquote)
	{
	  if (c == '`')
	    backquote = 0;
	  temp[j++] = c;
	  continue;
	}

      if (c == '`')
	{
	  temp[j++] = c;
	  backquote++;
	  continue;
	}

      /* Pass everything between `$(' and the matching `)' or a quoted
	 ${ ... } pair through according to the Posix.2 specification. */
      if (c == '$' && ((string[i + 1] == '(') || (string[i + 1] == '{')))
	{
	  si = i + 2;
	  if (string[i + 1] == '(')
	    ret = extract_delimited_string (string, &si, "$(", "(", ")");
	  else
	    ret = extract_dollar_brace_string (string, &si, 1);

	  temp[j++] = '$';
	  temp[j++] = string[i + 1];

	  for (t = 0; ret[t]; t++, j++)
	    temp[j] = ret[t];
	  temp[j++] = string[si];

	  i = si;
	  free (ret);
	  continue;
	}

      /* Add any character but a double quote to the quoted string we're
         accumulating. */
      if (c != '"')
	{
	  temp[j++] = c;
	  continue;
	}

      /* c == '"' */
      if (stripdq)
	{
	  dquote ^= 1;
	  continue;
	}

      break;
    }
  temp[j] = '\0';

  /* Point to after the closing quote. */
  if (c)
    i++;
  *sindex = i;

  return (temp);
}

/* This should really be another option to string_extract_double_quoted. */
static inline int
skip_double_quoted (string, sind)
     char *string;
     int sind;
{
  int c, j, i;
  char *ret;
  int pass_next, backquote, si;

  pass_next = backquote = 0;

  for (j = 0, i = sind; c = string[i]; i++)
    {
      if (pass_next)
	{
	  pass_next = 0;
	  continue;
	}
      else if (c == '\\')
	{
	  pass_next++;
	  continue;
	}
      else if (backquote)
	{
	  if (c == '`')
	    backquote = 0;
	  continue;
	}
      else if (c == '`')
	{
	  backquote++;
	  continue;
	}
      else if (c == '$' && ((string[i + 1] == '(') || (string[i + 1] == '{')))
	{
	  si = i + 2;
	  if (string[i + 1] == '(')
	    ret = extract_delimited_string (string, &si, "$(", "(", ")");
	  else
	    ret = extract_dollar_brace_string (string, &si, 0);

	  i = si;
	  free (ret);
	  continue;
	}
      else if (c != '"')
	continue;
      else
	break;
    }

  if (c)
    i++;

  return (i);
}

/* Extract the contents of STRING as if it is enclosed in single quotes.
   SINDEX, when passed in, is the offset of the character immediately
   following the opening single quote; on exit, SINDEX is left pointing after
   the closing single quote. */
static inline char *
string_extract_single_quoted (string, sindex)
     char *string;
     int *sindex;
{
  register int i, j;
  char *t;

  for (i = *sindex; string[i] && string[i] != '\''; i++)
    ;

  j = i - *sindex;
  t = xmalloc (1 + j);
  strncpy (t, string + *sindex, j);
  t[j] = '\0';

  if (string[i])
    i++;
  *sindex = i;

  return (t);
}

static inline int
skip_single_quoted (string, sind)
     char *string;
     int sind;
{
  register int i;

  for (i = sind; string[i] && string[i] != '\''; i++)
    ;
  if (string[i])
    i++;
  return i;
}

/* Just like string_extract, but doesn't hack backslashes or any of
   that other stuff.  Obeys quoting.  Used to do splitting on $IFS. */
static char *
string_extract_verbatim (string, sindex, charlist)
     char *string, *charlist;
     int *sindex;
{
  register int i = *sindex;
  int c;
  char *temp;

  if (charlist[0] == '\'' && charlist[1] == '\0')
    {
      temp = string_extract_single_quoted (string, sindex);
      --*sindex;	/* leave *sindex at separator character */
      return temp;
    }

  for (i = *sindex; c = string[i]; i++)
    {
      if (c == CTLESC)
	{
	  i++;
	  continue;
	}

      if (MEMBER (c, charlist))
	break;
    }

  c = i - *sindex;
  temp = xmalloc (1 + c);
  strncpy (temp, string + *sindex, c);
  temp[c] = '\0';
  *sindex = i;

  return (temp);
}

/* Extract the $( construct in STRING, and return a new string.
   Start extracting at (SINDEX) as if we had just seen "$(".
   Make (SINDEX) get the position of the matching ")". */
char *
extract_command_subst (string, sindex)
     char *string;
     int *sindex;
{
  return (extract_delimited_string (string, sindex, "$(", "(", ")"));
}

/* Extract the $[ construct in STRING, and return a new string.
   Start extracting at (SINDEX) as if we had just seen "$[".
   Make (SINDEX) get the position of the matching "]". */
char *
extract_arithmetic_subst (string, sindex)
     char *string;
     int *sindex;
{
  return (extract_delimited_string (string, sindex, "$[", "[", "]"));
}

#if defined (PROCESS_SUBSTITUTION)
/* Extract the <( or >( construct in STRING, and return a new string.
   Start extracting at (SINDEX) as if we had just seen "<(".
   Make (SINDEX) get the position of the matching ")". */
char *
extract_process_subst (string, starter, sindex)
     char *string;
     char *starter;
     int *sindex;
{
  return (extract_delimited_string (string, sindex, starter, "(", ")"));
}
#endif /* PROCESS_SUBSTITUTION */

#if defined (ARRAY_VARS)
char *
extract_array_assignment_list (string, sindex)
     char *string;
     int *sindex;
{
  return (extract_delimited_string (string, sindex, "(", (char *)NULL, ")"));
}
#endif

/* Extract and create a new string from the contents of STRING, a
   character string delimited with OPENER and CLOSER.  SINDEX is
   the address of an int describing the current offset in STRING;
   it should point to just after the first OPENER found.  On exit,
   SINDEX gets the position of the last character of the matching CLOSER.
   If OPENER is more than a single character, ALT_OPENER, if non-null,
   contains a character string that can also match CLOSER and thus
   needs to be skipped. */
static char *
extract_delimited_string (string, sindex, opener, alt_opener, closer)
     char *string;
     int *sindex;
     char *opener, *alt_opener, *closer;
{
  int i, c, si;
  char *t, *result;
  int pass_character, nesting_level;
  int len_closer, len_opener, len_alt_opener;

  len_opener = STRLEN (opener);
  len_alt_opener = STRLEN (alt_opener);
  len_closer = STRLEN (closer);

  pass_character = 0;

  nesting_level = 1;
  i = *sindex;

  while (nesting_level)
    {
      c = string[i];

      if (c == 0)
        break;

      if (pass_character)	/* previous char was backslash */
	{
	  pass_character = 0;
	  i++;
	  continue;
	}

      if (c == CTLESC)
	{
	  pass_character++;
	  i++;
	  continue;
	}

#if 0
      if (c == '\\' && delimiter == '"' &&
	      (member (string[i], slashify_in_quotes)))
#else
      if (c == '\\')
#endif
	{
	  pass_character++;
	  i++;
	  continue;
	}

      /* Process a nested OPENER. */
      if (STREQN (string + i, opener, len_opener))
	{
	  si = i + len_opener;
	  t = extract_delimited_string (string, &si, opener, alt_opener, closer);
	  i = si + 1;
	  FREE (t);
	  continue;
	}

      /* Process a nested ALT_OPENER */
      if (len_alt_opener && STREQN (string + i, alt_opener, len_alt_opener))
	{
	  si = i + len_alt_opener;
	  t = extract_delimited_string (string, &si, alt_opener, alt_opener, closer);
	  i = si + 1;
	  FREE (t);
	  continue;
	}

      /* If the current substring terminates the delimited string, decrement
	 the nesting level. */
      if (STREQN (string + i, closer, len_closer))
	{
	  i += len_closer - 1;	/* move to last char of the closer */
	  nesting_level--;
	  if (nesting_level == 0)
	    break;
	}

      /* Pass old-style command substitution through verbatim. */
      if (c == '`')
        {
          si = i + 1;
          t = string_extract (string, &si, "`", 0);
          i = si + 1;
          FREE (t);
          continue;
        }

      /* Pass single-quoted strings through verbatim. */
      if (c == '\'')
        {
          si = i + 1;
          i = skip_single_quoted (string, si);
          continue;
        }

      /* Pass embedded double-quoted strings through verbatim as well. */
      if (c == '"')
        {
          si = i + 1;
          i = skip_double_quoted (string, si);
          continue;
        }

      i++;	/* move past this character, which was not special. */
    }

  si = i - *sindex - len_closer + 1;
  result = xmalloc (1 + si);
  strncpy (result, string + *sindex, si);
  result[si] = '\0';
  *sindex = i;

  if (c == 0 && nesting_level)
    {
      report_error ("bad substitution: no `%s' in %s", closer, string);
      free (result);
      jump_to_top_level (DISCARD);
    }

  return (result);
}

/* Extract a parameter expansion expression within ${ and } from STRING.
   Obey the Posix.2 rules for finding the ending `}': count braces while
   skipping over enclosed quoted strings and command substitutions.
   SINDEX is the address of an int describing the current offset in STRING;
   it should point to just after the first `{' found.  On exit, SINDEX
   gets the position of the matching `}'.  QUOTED is non-zero if this
   occurs inside double quotes. */
/* XXX -- this is very similar to extract_delimited_string -- XXX */
static char *
extract_dollar_brace_string (string, sindex, quoted)
     char *string;
     int *sindex, quoted;
{
  register int i, c, l;
  int pass_character, nesting_level, si;
  char *result, *t;

  pass_character = 0;

  nesting_level = 1;

  for (i = *sindex; (c = string[i]); i++)
    {
      if (pass_character)
	{
	  pass_character = 0;
	  continue;
	}

      if (c == CTLESC)
	{
	  pass_character++;
	  continue;
	}

      /* Backslashes quote the next character. */
      if (c == '\\')
	{
	  pass_character++;
	  continue;
	}

      if (string[i] == '$' && string[i+1] == '{')
	{
	  nesting_level++;
	  i++;
	  continue;
	}

      if (c == '}')
	{
	  nesting_level--;
	  if (nesting_level == 0)
	    break;
	  continue;
	}

      /* Pass the contents of old-style command substitutions through
	 verbatim. */
      if (c == '`')
	{
	  si = i + 1;
	  t = string_extract (string, &si, "`", 0);
	  i = si;
	  free (t);
	  continue;
	}

      /* Pass the contents of new-style command substitutions through
	 verbatim. */
      if (string[i] == '$' && string[i+1] == '(')
	{
	  si = i + 2;
	  t = extract_delimited_string (string, &si, "$(", "(", ")");
	  i = si;
	  free (t);
	  continue;
	}

      /* Pass the contents of single-quoted strings through verbatim. */
      if (c == '\'')
	{
	  si = i + 1;
	  i = skip_single_quoted (string, si);
	  /* skip_single_quoted leaves index one past close quote */
	  i--;
	  continue;
	}

      /* Pass embedded double-quoted strings through verbatim as well. */
      if (c == '"')
	{
	  si = i + 1;
	  /* skip_double_quoted leaves index one past close quote */
	  i = skip_double_quoted (string, si);
	  i--;
	  continue;
	}
    }

  l = i - *sindex;
  result = xmalloc (1 + l);
  strncpy (result, string + *sindex, l);
  result[l] = '\0';
  *sindex = i;

  if (c == 0 && nesting_level)
    {
      report_error ("bad substitution: no ending `}' in %s", string);
      free (result);
      jump_to_top_level (DISCARD);
    }

  return (result);
}

/* Remove backslashes which are quoting backquotes from STRING.  Modifies
   STRING, and returns a pointer to it. */
char *
de_backslash (string)
     char *string;
{
  register int i, l;

  for (i = 0, l = strlen (string); i < l; i++)
    if (string[i] == '\\' && (string[i + 1] == '`' || string[i + 1] == '\\' ||
			      string[i + 1] == '$'))
      strcpy (string + i, string + i + 1);	/* XXX - should be memmove */
  return (string);
}

#if 0
/* Replace instances of \! in a string with !. */
void
unquote_bang (string)
     char *string;
{
  register int i, j;
  register char *temp;

  temp = xmalloc (1 + strlen (string));

  for (i = 0, j = 0; (temp[j] = string[i]); i++, j++)
    {
      if (string[i] == '\\' && string[i + 1] == '!')
	{
	  temp[j] = '!';
	  i++;
	}
    }
  strcpy (string, temp);
  free (temp);
}
#endif

#if defined (READLINE)
/* Return 1 if the portion of STRING ending at EINDEX is quoted (there is
   an unclosed quoted string), or if the character at EINDEX is quoted
   by a backslash. */
int
char_is_quoted (string, eindex)
     char *string;
     int eindex;
{
  int i, pass_next, quoted;

  for (i = pass_next = quoted = 0; i <= eindex; i++)
    {
      if (pass_next)
	{
	  pass_next = 0;
	  if (i >= eindex)	/* XXX was if (i >= eindex - 1) */
	    return 1;
	  continue;
	}
      else if (string[i] == '\'' || string[i] == '"')
	{
	  i = (string[i] == '\'') ? skip_single_quoted (string, ++i)
				  : skip_double_quoted (string, ++i);
	  if (i > eindex)
	    return 1;
	  i--;	/* the skip functions increment past the closing quote. */
	}
      else if (string[i] == '\\')
	{
	  pass_next = 1;
	  continue;
	}
    }
  return (0);
}

int
unclosed_pair (string, eindex, openstr)
     char *string;
     int eindex;
     char *openstr;
{
  int i, pass_next, openc, olen;

  olen = strlen (openstr);
  for (i = pass_next = openc = 0; i <= eindex; i++)
    {
      if (pass_next)
	{
	  pass_next = 0;
	  if (i >= eindex)	/* XXX was if (i >= eindex - 1) */
	    return 0;
	  continue;
	}
      else if (STREQN (string + i, openstr, olen))
	{
	  openc = 1 - openc;
	  i += olen - 1;
	}
      else if (string[i] == '\'' || string[i] == '"')
	{
	  i = (string[i] == '\'') ? skip_single_quoted (string, i)
				  : skip_double_quoted (string, i);
	  if (i > eindex)
	    return 0;
	}
      else if (string[i] == '\\')
	{
	  pass_next = 1;
	  continue;
	}
    }
  return (openc);
}
#endif /* READLINE */

#if 0
/* UNUSED */
/* Extract the name of the variable to bind to from the assignment string. */
char *
assignment_name (string)
     char *string;
{
  int offset;
  char *temp;

  offset = assignment (string);
  if (offset == 0)
    return (char *)NULL;
  temp = xmalloc (offset + 1);
  strncpy (temp, string, offset);
  temp[offset] = '\0';
  return (temp);
}
#endif

/* Return a single string of all the words in LIST.  SEP is the separator
   to put between individual elements of LIST in the output string. */
static char *
string_list_internal (list, sep)
     WORD_LIST *list;
     char *sep;
{
  register WORD_LIST *t;
  char *result, *r;
  int word_len, sep_len, result_size;

  if (list == 0)
    return ((char *)NULL);

  /* This is nearly always called with either sep[0] == 0 or sep[1] == 0. */
  sep_len = STRLEN (sep);
  result_size = 0;

  for (t = list; t; t = t->next)
    {
      if (t != list)
	result_size += sep_len;
      result_size += strlen (t->word->word);
    }

  r = result = xmalloc (result_size + 1);

  for (t = list; t; t = t->next)
    {
      if (t != list && sep_len)
	{
	  if (sep_len > 1)
	    {
	      FASTCOPY (sep, r, sep_len);
	      r += sep_len;
	    }
	  else
	    *r++ = sep[0];
	}

      word_len = strlen (t->word->word);
      FASTCOPY (t->word->word, r, word_len);
      r += word_len;
    }

  *r = '\0';
  return (result);
}

/* Return a single string of all the words present in LIST, separating
   each word with a space. */
char *
string_list (list)
     WORD_LIST *list;
{
  return (string_list_internal (list, " "));
}

/* Return a single string of all the words present in LIST, obeying the
   quoting rules for "$*", to wit: (P1003.2, draft 11, 3.5.2) "If the
   expansion [of $*] appears within a double quoted string, it expands
   to a single field with the value of each parameter separated by the
   first character of the IFS variable, or by a <space> if IFS is unset." */
char *
string_list_dollar_star (list)
     WORD_LIST *list;
{
  char *ifs, sep[2];

  ifs = get_string_value ("IFS");
  if (ifs == 0)
    sep[0] = ' ';
  else if (*ifs == '\0')
    sep[0] = '\0';
  else
    sep[0] = *ifs;

  sep[1] = '\0';

  return (string_list_internal (list, sep));
}

/* Return the list of words present in STRING.  Separate the string into
   words at any of the characters found in SEPARATORS.  If QUOTED is
   non-zero then word in the list will have its quoted flag set, otherwise
   the quoted flag is left as make_word () deemed fit.

   This obeys the P1003.2 word splitting semantics.  If `separators' is
   exactly <space><tab><newline>, then the splitting algorithm is that of
   the Bourne shell, which treats any sequence of characters from `separators'
   as a delimiter.  If IFS is unset, which results in `separators' being set
   to "", no splitting occurs.  If separators has some other value, the
   following rules are applied (`IFS white space' means zero or more
   occurrences of <space>, <tab>, or <newline>, as long as those characters
   are in `separators'):

	1) IFS white space is ignored at the start and the end of the
	   string.
	2) Each occurrence of a character in `separators' that is not
	   IFS white space, along with any adjacent occurrences of
	   IFS white space delimits a field.
	3) Any nonzero-length sequence of IFS white space delimits a field.
   */

/* BEWARE!  list_string strips null arguments.  Don't call it twice and
   expect to have "" preserved! */

/* Perform quoted null character removal on STRING.  We don't allow any
   quoted null characters in the middle or at the ends of strings because
   of how expand_word_internal works.  remove_quoted_nulls () turns
   STRING into an empty string iff it only consists of a quoted null,
   and removes all unquoted CTLNUL characters. */
/*
#define remove_quoted_nulls(string) \
  do { if (QUOTED_NULL (string)) string[0] ='\0'; } while (0)
*/
static void
remove_quoted_nulls (string)
     char *string;
{
  char *nstr, *s, *p;

  nstr = savestring (string);
  nstr[0] = '\0';
  for (p = nstr, s = string; *s; s++)
    {
      if (*s == CTLESC)
	{
	  *p++ = *s++;	/* CTLESC */
	  if (*s == 0)
	    break;
	  *p++ = *s;	/* quoted char */
	  continue;
	}
      if (*s == CTLNUL)
        continue;
      *p++ = *s;
    }
  *p = '\0';
  strcpy (string, nstr);
  free (nstr);
}

/* Perform quoted null character removal on each element of LIST.
   This modifies LIST. */
void
word_list_remove_quoted_nulls (list)
     WORD_LIST *list;
{
  register WORD_LIST *t;

  for (t = list; t; t = t->next)
    remove_quoted_nulls (t->word->word);
}

/* This performs word splitting and quoted null character removal on
   STRING. */
#define issep(c)	(member ((c), separators))

WORD_LIST *
list_string (string, separators, quoted)
     register char *string, *separators;
     int quoted;
{
  WORD_LIST *result;
  WORD_DESC *t;
  char *current_word, *s;
  int sindex, sh_style_split;

  if (!string || !*string)
    return ((WORD_LIST *)NULL);

  sh_style_split =
    separators && *separators && (STREQ (separators, " \t\n"));

  /* Remove sequences of whitespace at the beginning of STRING, as
     long as those characters appear in IFS.  Do not do this if
     STRING is quoted or if there are no separator characters. */
  if (!quoted || !separators || !*separators)
    {
      for (s = string; *s && spctabnl (*s) && issep (*s); s++);

      if (!*s)
	return ((WORD_LIST *)NULL);

      string = s;
    }

  /* OK, now STRING points to a word that does not begin with white space.
     The splitting algorithm is:
     	extract a word, stopping at a separator
     	skip sequences of spc, tab, or nl as long as they are separators
     This obeys the field splitting rules in Posix.2. */
  for (result = (WORD_LIST *)NULL, sindex = 0; string[sindex]; )
    {
      current_word = string_extract_verbatim (string, &sindex, separators);
      if (current_word == 0)
	break;

      /* If we have a quoted empty string, add a quoted null argument.  We
	 want to preserve the quoted null character iff this is a quoted
	 empty string; otherwise the quoted null characters are removed
	 below. */
      if (QUOTED_NULL (current_word))
	{
	  t = make_bare_word ("");
	  t->flags |= W_QUOTED;
	  free (t->word);
	  t->word = make_quoted_char ('\0');
	  result = make_word_list (t, result);
	}
      else if (current_word[0] != '\0')
	{
	  /* If we have something, then add it regardless.  However,
	     perform quoted null character removal on the current word. */
	  remove_quoted_nulls (current_word);
	  result = make_word_list (make_word (current_word), result);
	  if (quoted & (Q_DOUBLE_QUOTES|Q_HERE_DOCUMENT))
	    result->word->flags |= W_QUOTED;
	}

      /* If we're not doing sequences of separators in the traditional
	 Bourne shell style, then add a quoted null argument. */
      else if (!sh_style_split && !spctabnl (string[sindex]))
	{
	  t = make_bare_word ("");
	  t->flags |= W_QUOTED;
	  free (t->word);
	  t->word = make_quoted_char ('\0');
	  result = make_word_list (t, result);
	}

      free (current_word);

      /* Move past the current separator character. */
      if (string[sindex])
	sindex++;

      /* Now skip sequences of space, tab, or newline characters if they are
	 in the list of separators. */
      while (string[sindex] && spctabnl (string[sindex]) && issep (string[sindex]))
	sindex++;
    }
  return (REVERSE_LIST (result, WORD_LIST *));
}

/* Parse a single word from STRING, using SEPARATORS to separate fields.
   ENDPTR is set to the first character after the word.  This is used by
   the `read' builtin.
   XXX - this function is very similar to list_string; they should be
	 combined - XXX */
char *
get_word_from_string (stringp, separators, endptr)
     char **stringp, *separators, **endptr;
{
  register char *s;
  char *current_word;
  int sindex, sh_style_split;

  if (!stringp || !*stringp || !**stringp)
    return ((char *)NULL);

  s = *stringp;

  sh_style_split =
    separators && *separators && (STREQ (separators, " \t\n"));

  /* Remove sequences of whitespace at the beginning of STRING, as
     long as those characters appear in IFS. */
  if (sh_style_split || !separators || !*separators)
    {
      for (; *s && spctabnl (*s) && issep (*s); s++);

      /* If the string is nothing but whitespace, update it and return. */
      if (!*s)
	{
	  *stringp = s;
	  if (endptr)
	    *endptr = s;
	  return ((char *)NULL);
	}
    }

  /* OK, S points to a word that does not begin with white space.
     Now extract a word, stopping at a separator, save a pointer to
     the first character after the word, then skip sequences of spc,
     tab, or nl as long as they are separators.

     This obeys the field splitting rules in Posix.2. */
  sindex = 0;
  current_word = string_extract_verbatim (s, &sindex, separators);

  /* Set ENDPTR to the first character after the end of the word. */
  if (endptr)
    *endptr = s + sindex;

  /* Move past the current separator character. */
  if (s[sindex])
    sindex++;

  /* Now skip sequences of space, tab, or newline characters if they are
     in the list of separators. */
  while (s[sindex] && spctabnl (s[sindex]) && issep (s[sindex]))
    sindex++;

  /* Update STRING to point to the next field. */
  *stringp = s + sindex;
  return (current_word);
}

/* Remove IFS white space at the end of STRING.  Start at the end
   of the string and walk backwards until the beginning of the string
   or we find a character that's not IFS white space and not CTLESC.
   Only let CTLESC escape a white space character if SAW_ESCAPE is
   non-zero.  */
char *
strip_trailing_ifs_whitespace (string, separators, saw_escape)
     char *string, *separators;
     int saw_escape;
{
  char *s;

  s = string + STRLEN (string) - 1;
  while (s > string && ((spctabnl (*s) && issep (*s)) ||
			(saw_escape && *s == CTLESC && spctabnl (s[1]))))
    s--;
  *++s = '\0';
  return string;
}

#if 0
#if defined (ARRAY_VARS)
WORD_LIST *
list_string_with_quotes (string)
     char *string;
{
  WORD_LIST *list;
  char *token, *s;
  int c, i, tokstart, len;

  for (s = string; s && *s && spctabnl (*s); s++)
    ;
  if (s == 0 || *s == 0)
    return ((WORD_LIST *)NULL);

  tokstart = i = 0;
  list = (WORD_LIST *)NULL;
  while (1)
    {
      c = s[i];
      if (c == '\\')
	{
	  i++;
	  if (s[i])
	    i++;
	}
      else if (c == '\'')
        i = skip_single_quoted (s, ++i);
      else if (c == '"')
	i = skip_double_quoted (s, ++i);
      else if (c == 0 || spctabnl (c))
	{
	  /* We have found the end of a token.  Make a word out of it and
	     add it to the word list. */
	  len = i - tokstart;
	  token = xmalloc (len + 1);
	  strncpy (token, s + tokstart, len);
	  token[len] = '\0';
	  list = make_word_list (make_word (token), list);
	  free (token);
	  while (spctabnl (s[i]))
	    i++;
	  if (s[i])
	    tokstart = i;
	  else
	    break;
	}
      else
	i++;	/* normal character */
    }
  return (REVERSE_LIST (list, WORD_LIST *));
}
#endif /* ARRAY_VARS */
#endif /* 0 */

#if defined (PROCESS_SUBSTITUTION)
#define EXP_CHAR(s) (s == '$' || s == '`' || s == '<' || s == '>' || s == CTLESC)
#else
#define EXP_CHAR(s) (s == '$' || s == '`' || s == CTLESC)
#endif

/* If there are any characters in STRING that require full expansion,
   then call FUNC to expand STRING; otherwise just perform quote
   removal if necessary.  This returns a new string. */
static char *
maybe_expand_string (string, quoted, func)
     char *string;
     int quoted;
     WORD_LIST *(*func)();
{
  WORD_LIST *list;
  int i, saw_quote;
  char *ret;

  for (i = saw_quote = 0; string[i]; i++)
    {
      if (EXP_CHAR (string[i]))
	break;
      else if (string[i] == '\'' || string[i] == '\\' || string[i] == '"')
	saw_quote = 1;
    }

  if (string[i])
    {
      list = (*func) (string, quoted);
      if (list)
	{
	  ret = string_list (list);
	  dispose_words (list);
	}
      else
	ret = (char *)NULL;
    }
  else if (saw_quote && ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) == 0))
    ret = string_quote_removal (string, quoted);
  else
    ret = savestring (string);
  return ret;
}

static inline char *
expand_string_to_string (string, quoted, func)
     char *string;
     int quoted;
     WORD_LIST *(*func)();
{
  WORD_LIST *list;
  char *ret;

  if (string == 0 || *string == '\0')
    return ((char *)NULL);

  list = (*func) (string, quoted);
  if (list)
    {
      ret = string_list (list);
      dispose_words (list);
    }
  else
    ret = (char *)NULL;

  return (ret);
}

#if defined (ARRAY_VARS)
SHELL_VAR *
do_array_element_assignment (name, value)
     char *name, *value;
{
  char *t;
  int ind, ni;
  SHELL_VAR *entry;

  t = strchr (name, '[');
  if (t == 0)
    return ((SHELL_VAR *)NULL);
  ind = t - name;
  ni = skipsubscript (name, ind);
  if ((ALL_ELEMENT_SUB (t[1]) && t[2] == ']') || (ni <= ind + 1))
    {
      report_error ("%s: bad array subscript", name);
      return ((SHELL_VAR *)NULL);
    }
  *t++ = '\0';
  ind = array_expand_index (t, ni - ind);
  if (ind < 0)
    {
      t[-1] = '[';		/* restore original name */
      report_error ("%s: bad array subscript", name);
      return ((SHELL_VAR *)NULL);
    }
  entry = bind_array_variable (name, ind, value);
  t[-1] = '[';		/* restore original name */
  return (entry);
}
#endif /* ARRAY_VARS */

/* Given STRING, an assignment string, get the value of the right side
   of the `=', and bind it to the left side.  If EXPAND is true, then
   perform parameter expansion, command substitution, and arithmetic
   expansion on the right-hand side.  Perform tilde expansion in any
   case.  Do not perform word splitting on the result of expansion. */
static int
do_assignment_internal (string, expand)
     char *string;
     int expand;
{
  int offset;
  char *name, *value;
  SHELL_VAR *entry;
#if defined (ARRAY_VARS)
  char *t;
  int ni, assign_list = 0;
#endif

  offset = assignment (string);
  name = savestring (string);
  value = (char *)NULL;

  if (name[offset] == '=')
    {
      char *temp;

      name[offset] = 0;
      temp = name + offset + 1;

#if defined (ARRAY_VARS)
      if (expand && temp[0] == '(' && strchr (temp, ')'))
	{
	  assign_list = ni = 1;
	  value = extract_delimited_string (temp, &ni, "(", (char *)NULL, ")");
	}
      else
#endif

      /* Perform tilde expansion. */
      if (expand && temp[0])
        {
	  temp = (strchr (temp, '~') && unquoted_member ('~', temp))
			? bash_tilde_expand (temp)
			: savestring (temp);

	  value = maybe_expand_string (temp, 0, expand_string_unsplit);
	  free (temp);
	}
      else
	value = savestring (temp);
    }

  if (value == 0)
    {
      value = xmalloc (1);
      value[0] = '\0';
    }

  if (echo_command_at_execute)
#if defined (ARRAY_VARS)
    if (assign_list)
      fprintf (stderr, "%s%s=(%s)\n", indirection_level_string (), name, value);
    else
#endif
    fprintf (stderr, "%s%s=%s\n", indirection_level_string (), name, value);

#define ASSIGN_RETURN(r)	do { FREE (value); free (name); return (r); } while (0)

#if defined (ARRAY_VARS)
  if (t = strchr (name, '['))
    {
      if (assign_list)
	{
	  report_error ("%s: cannot assign list to array member", name);
	  ASSIGN_RETURN (0);
	}
      entry = do_array_element_assignment (name, value);
      if (entry == 0)
        ASSIGN_RETURN (0);
    }
  else if (assign_list)
    entry = assign_array_from_string (name, value);
  else
#endif /* ARRAY_VARS */
  entry = bind_variable (name, value);

  stupidly_hack_special_variables (name);

  if (entry)
    entry->attributes &= ~att_invisible;

  /* Return 1 if the assignment seems to have been performed correctly. */
  ASSIGN_RETURN (entry ? ((entry->attributes & att_readonly) == 0) : 0);
}

/* Perform the assignment statement in STRING, and expand the
   right side by doing command and parameter expansion. */
int
do_assignment (string)
     char *string;
{
  return do_assignment_internal (string, 1);
}

/* Given STRING, an assignment string, get the value of the right side
   of the `=', and bind it to the left side.  Do not do command and
   parameter substitution on the right hand side. */
int
do_assignment_no_expand (string)
     char *string;
{
  return do_assignment_internal (string, 0);
}

/* Most of the substitutions must be done in parallel.  In order
   to avoid using tons of unclear goto's, I have some functions
   for manipulating malloc'ed strings.  They all take INDX, a
   pointer to an integer which is the offset into the string
   where manipulation is taking place.  They also take SIZE, a
   pointer to an integer which is the current length of the
   character array for this string. */

/* Append SOURCE to TARGET at INDEX.  SIZE is the current amount
   of space allocated to TARGET.  SOURCE can be NULL, in which
   case nothing happens.  Gets rid of SOURCE by freeing it.
   Returns TARGET in case the location has changed. */
inline char *
sub_append_string (source, target, indx, size)
     char *source, *target;
     int *indx, *size;
{
  if (source)
    {
      int srclen, n;

      srclen = STRLEN (source);
      if (srclen >= (int)(*size - *indx))
	{
	  n = srclen + *indx;
	  n = (n + DEFAULT_ARRAY_SIZE) - (n % DEFAULT_ARRAY_SIZE);
	  target = xrealloc (target, (*size = n));
	}

      FASTCOPY (source, target + *indx, srclen);
      *indx += srclen;
      target[*indx] = '\0';

      free (source);
    }
  return (target);
}

#if 0
/* UNUSED */
/* Append the textual representation of NUMBER to TARGET.
   INDX and SIZE are as in SUB_APPEND_STRING. */
char *
sub_append_number (number, target, indx, size)
     int number, *indx, *size;
     char *target;
{
  char *temp;

  temp = itos (number);
  return (sub_append_string (temp, target, indx, size));
}
#endif

/* Return the word list that corresponds to `$*'. */
WORD_LIST *
list_rest_of_args ()
{
  register WORD_LIST *list, *args;
  int i;

  /* Break out of the loop as soon as one of the dollar variables is null. */
  for (i = 1, list = (WORD_LIST *)NULL; i < 10 && dollar_vars[i]; i++)
    list = make_word_list (make_bare_word (dollar_vars[i]), list);

  for (args = rest_of_args; args; args = args->next)
    list = make_word_list (make_bare_word (args->word->word), list);

  return (REVERSE_LIST (list, WORD_LIST *));
}

int
number_of_args ()
{
  register WORD_LIST *list;
  int n;

  for (n = 0; n < 9 && dollar_vars[n+1]; n++)
    ;
  for (list = rest_of_args; list; list = list->next)
    n++;
  return n;
}

/* Make a single large string out of the dollar digit variables,
   and the rest_of_args.  If DOLLAR_STAR is 1, then obey the special
   case of "$*" with respect to IFS. */
char *
string_rest_of_args (dollar_star)
     int dollar_star;
{
  register WORD_LIST *list;
  char *string;

  list = list_rest_of_args ();
  string = dollar_star ? string_list_dollar_star (list) : string_list (list);
  dispose_words (list);
  return (string);
}

/***************************************************
 *						   *
 *	   Functions to Expand a String		   *
 *						   *
 ***************************************************/
/* Call expand_word_internal to expand W and handle error returns.
   A convenience function for functions that don't want to handle
   any errors or free any memory before aborting. */
static WORD_LIST *
call_expand_word_internal (w, q, c, e)
     WORD_DESC *w;
     int q, *c, *e;
{
  WORD_LIST *result;

  result = expand_word_internal (w, q, c, e);
  if (result == &expand_word_error)
    {
      /* By convention, each time this error is returned, w->word has
	 already been freed. */
      w->word = (char *)NULL;
      jump_to_top_level (DISCARD);
      /* NOTREACHED */
    }
  else if (result == &expand_word_fatal)
    jump_to_top_level (FORCE_EOF);
    /* NOTREACHED */
  else
    return (result);
}

/* Perform parameter expansion, command substitution, and arithmetic
   expansion on STRING, as if it were a word.  Leave the result quoted. */
static WORD_LIST *
expand_string_internal (string, quoted)
     char *string;
     int quoted;
{
  WORD_DESC td;
  WORD_LIST *tresult;

  if (string == 0 || *string == 0)
    return ((WORD_LIST *)NULL);

  bzero ((char *)&td, sizeof (td));
  td.word = string;
  tresult = call_expand_word_internal (&td, quoted, (int *)NULL, (int *)NULL);
  return (tresult);
}

/* Expand STRING by performing parameter expansion, command substitution,
   and arithmetic expansion.  Dequote the resulting WORD_LIST before
   returning it, but do not perform word splitting.  The call to
   remove_quoted_nulls () is in here because word splitting normally
   takes care of quote removal. */
WORD_LIST *
expand_string_unsplit (string, quoted)
     char *string;
     int quoted;
{
  WORD_LIST *value;

  if (!string || !*string)
    return ((WORD_LIST *)NULL);

  value = expand_string_internal (string, quoted);
  if (value)
    {
      if (value->word)
	remove_quoted_nulls (value->word->word);
      dequote_list (value);
    }
  return (value);
}

/* Expand STRING just as if you were expanding a word, but do not dequote
   the resultant WORD_LIST.  This is called only from within this file,
   and is used to correctly preserve quoted characters when expanding
   things like ${1+"$@"}.  This does parameter expansion, command
   subsitution, arithmetic expansion, and word splitting. */
static WORD_LIST *
expand_string_leave_quoted (string, quoted)
     char *string;
     int quoted;
{
  WORD_LIST *tlist;
  WORD_LIST *tresult;

  if (string == 0 || *string == '\0')
    return ((WORD_LIST *)NULL);

  tlist = expand_string_internal (string, quoted);

  if (tlist)
    {
      tresult = word_list_split (tlist);
      dispose_words (tlist);
      return (tresult);
    }
  return ((WORD_LIST *)NULL);
}

/* This does not perform word splitting or dequote the WORD_LIST
   it returns. */
static WORD_LIST *
expand_string_for_rhs (string, quoted, dollar_at_p, has_dollar_at)
     char *string;
     int quoted, *dollar_at_p, *has_dollar_at;
{
  WORD_DESC td;
  WORD_LIST *tresult;

  if (string == 0 || *string == '\0')
    return (WORD_LIST *)NULL;

  bzero ((char *)&td, sizeof (td));
  td.word = string;
  tresult = call_expand_word_internal (&td, quoted, dollar_at_p, has_dollar_at);
  return (tresult);
}

/* Expand STRING just as if you were expanding a word.  This also returns
   a list of words.  Note that filename globbing is *NOT* done for word
   or string expansion, just when the shell is expanding a command.  This
   does parameter expansion, command substitution, arithmetic expansion,
   and word splitting.  Dequote the resultant WORD_LIST before returning. */
WORD_LIST *
expand_string (string, quoted)
     char *string;
     int quoted;
{
  WORD_LIST *result;

  if (!string || !*string)
    return ((WORD_LIST *)NULL);

  result = expand_string_leave_quoted (string, quoted);
  return (result ? dequote_list (result) : result);
}

/***************************************************
 *						   *
 *	Functions to handle quoting chars	   *
 *						   *
 ***************************************************/

static WORD_LIST *
dequote_list (list)
     WORD_LIST *list;
{
  register char *s;
  register WORD_LIST *tlist;

  for (tlist = list; tlist; tlist = tlist->next)
    {
      s = dequote_string (tlist->word->word);
      free (tlist->word->word);
      tlist->word->word = s;
    }
  return list;
}

static char *
make_quoted_char (c)
     int c;
{
  char *temp;

  temp = xmalloc (3);
  if (c == 0)
    {
      temp[0] = CTLNUL;
      temp[1] = '\0';
    }
  else
    {
      temp[0] = CTLESC;
      temp[1] = c;
      temp[2] = '\0';
    }
  return (temp);
}

/* Quote STRING.  Return a new string. */
char *
quote_string (string)
     char *string;
{
  register char *t;
  char *result;

  if (*string == 0)
    {
      result = xmalloc (2);
      result[0] = CTLNUL;
      result[1] = '\0';
    }
  else
    {
      result = xmalloc ((strlen (string) * 2) + 1);

      for (t = result; *string; )
	{
	  *t++ = CTLESC;
	  *t++ = *string++;
	}
      *t = '\0';
    }
  return (result);
}

/* De-quoted quoted characters in STRING. */
char *
dequote_string (string)
     char *string;
{
  register char *t;
  char *result;

  result = xmalloc (strlen (string) + 1);

  if (QUOTED_NULL (string))
    {
      result[0] = '\0';
      return (result);
    }

  /* If no character in the string can be quoted, don't bother examining
     each character.  Just return a copy of the string passed to us. */
  if (strchr (string, CTLESC) == NULL)		/* XXX */
    {						/* XXX */
      strcpy (result, string);			/* XXX */
      return (result);				/* XXX */
    }

  for (t = result; *string; string++, t++)
    {
      if (*string == CTLESC)
	{
	  string++;

	  if (!*string)
	    break;
	}

      *t = *string;
    }

  *t = '\0';
  return (result);
}

/* Quote the entire WORD_LIST list. */
static WORD_LIST *
quote_list (list)
     WORD_LIST *list;
{
  register WORD_LIST *w;
  char *t;

  for (w = list; w; w = w->next)
    {
      t = w->word->word;
      w->word->word = quote_string (t);
      free (t);
      w->word->flags |= W_QUOTED;
    }
  return list;
}

/* **************************************************************** */
/*								    */
/*		    Functions for Removing Patterns		    */
/*								    */
/* **************************************************************** */

/* Remove the portion of PARAM matched by PATTERN according to OP, where OP
   can have one of 4 values:
	RP_LONG_LEFT	remove longest matching portion at start of PARAM
	RP_SHORT_LEFT	remove shortest matching portion at start of PARAM
	RP_LONG_RIGHT	remove longest matching portion at end of PARAM
	RP_SHORT_RIGHT	remove shortest matching portion at end of PARAM
*/

#define RP_LONG_LEFT	1
#define RP_SHORT_LEFT	2
#define RP_LONG_RIGHT	3
#define RP_SHORT_RIGHT	4

static char *
remove_pattern (param, pattern, op)
     char *param, *pattern;
     int op;
{
  register int len;
  register char *end;
  register char *p, *ret, c;

  if (param == NULL || *param == '\0')
    return (param);
  if (pattern == NULL || *pattern == '\0')	/* minor optimization */
    return (savestring (param));

  len = STRLEN (param);
  end = param + len;

  switch (op)
    {
      case RP_LONG_LEFT:	/* remove longest match at start */
	for (p = end; p >= param; p--)
	  {
	    c = *p; *p = '\0';
	    if (fnmatch (pattern, param, 0) != FNM_NOMATCH)
	      {
		*p = c;
		return (savestring (p));
	      }
	    *p = c;
	  }
	break;

      case RP_SHORT_LEFT:	/* remove shortest match at start */
	for (p = param; p <= end; p++)
	  {
	    c = *p; *p = '\0';
	    if (fnmatch (pattern, param, 0) != FNM_NOMATCH)
	      {
		*p = c;
		return (savestring (p));
	      }
	    *p = c;
	  }
	break;

      case RP_LONG_RIGHT:	/* remove longest match at end */
	for (p = param; p <= end; p++)
	  {
	    if (fnmatch (pattern, p, 0) != FNM_NOMATCH)
	      {
		c = *p; *p = '\0';
		ret = savestring (param);
		*p = c;
		return (ret);
	      }
	  }
	break;

      case RP_SHORT_RIGHT:	/* remove shortest match at end */
	for (p = end; p >= param; p--)
	  {
	    if (fnmatch (pattern, p, 0) != FNM_NOMATCH)
	      {
		c = *p; *p = '\0';
		ret = savestring (param);
		*p = c;
		return (ret);
	      }
	  }
	break;
    }
  return (savestring (param));	/* no match, return original string */
}

/* Return 1 of the first character of STRING could match the first
   character of pattern PAT.  Used to avoid n2 calls to fnmatch(). */
static int
match_pattern_char (pat, string)
     char *pat, *string;
{
#if 0
  register char *np;
  int neg;
  char c1;
#endif
  char c;

  if (*string == 0)
    return (0);

  switch (c = *pat++)
    {
    default:
      return (*string == c);
    case '\\':
      return (*string == *pat);
    case '?':
      return (*string != '\0');
    case '*':
      return (1);
    case '[':
#if 0
      for (np = pat; *np != ']'; np++);
      if (*np == 0)
        return (*string == '[');
      if (neg = (*pat == '!' || *pat == '^'))
	pat++;
      for ( ; (c1 = *pat++) != ']'; )
	{
	  if (c1 == '\\')
	    c1 = *pat++;
	  if (c1 == 0)
	    return (0);
	  if (*pat != '-' || pat[1] == '\0' || pat[1] == ']')
	    return (neg ? *string != c1 : *string == c1);
	  if (c1 <= *string && *string <= pat[1])
	    return (1);
	  pat += 2;
	}
#else
      return (*string != '\0');
#endif
    }
}

/* Match PAT anywhere in STRING and return the match boundaries.
   This returns 1 in case of a successful match, 0 otherwise.  SP
   and EP are pointers into the string where the match begins and
   ends, respectively.  MTYPE controls what kind of match is attempted.
   MATCH_BEG and MATCH_END anchor the match at the beginning and end
   of the string, respectively.  The longest match is returned. */
static int
match_pattern (string, pat, mtype, sp, ep)
     char *string, *pat;
     int mtype;
     char **sp, **ep;
{
  int c;
  register char *p, *p1;
  char *end;

  if (string == 0 || *string == 0 || pat == 0 || *pat == 0)
    return (0);

  end = string + STRLEN (string);

  switch (mtype)
    {
    case MATCH_ANY:
      for (p = string; p <= end; p++)
	{
	  if (match_pattern_char (pat, p))
	    {
	      for (p1 = end; p1 >= p; p1--)
		{
		  c = *p1; *p1 = '\0';
		  if (fnmatch (pat, p, 0) == 0)
		    {
		      *p1 = c;
		      *sp = p;
		      *ep = p1;
		      return 1;
		    }
		  *p1 = c;
		}
	    }
	}
      return (0);

    case MATCH_BEG:
      if (match_pattern_char (pat, string) == 0)
        return (0);
      for (p = end; p >= string; p--)
	{
	  c = *p; *p = '\0';
	  if (fnmatch (pat, string, 0) == 0)
	    {
	      *p = c;
	      *sp = string;
	      *ep = p;
	      return 1;
	    }
	  *p = c;
	}
      return (0);

    case MATCH_END:
      for (p = string; p <= end; p++)
	if (fnmatch (pat, p, 0) == 0)
	  {
	    *sp = p;
	    *ep = end;
	    return 1;
	  }
      return (0);
    }

  return (0);
}

/*******************************************
 *					   *
 *	Functions to expand WORD_DESCs	   *
 *					   *
 *******************************************/

/* Expand WORD, performing word splitting on the result.  This does
   parameter expansion, command substitution, arithmetic expansion,
   word splitting, and quote removal. */

WORD_LIST *
expand_word (word, quoted)
     WORD_DESC *word;
     int quoted;
{
  WORD_LIST *result, *tresult;

  tresult = call_expand_word_internal (word, quoted, (int *)NULL, (int *)NULL);
  result = word_list_split (tresult);
  dispose_words (tresult);
  return (result ? dequote_list (result) : result);
}

/* Expand WORD, but do not perform word splitting on the result.  This
   does parameter expansion, command substitution, arithmetic expansion,
   and quote removal. */
WORD_LIST *
expand_word_no_split (word, quoted)
     WORD_DESC *word;
     int quoted;
{
  WORD_LIST *result;

  result = call_expand_word_internal (word, quoted, (int *)NULL, (int *)NULL);
  return (result ? dequote_list (result) : result);
}

/* Perform shell expansions on WORD, but do not perform word splitting or
   quote removal on the result. */
WORD_LIST *
expand_word_leave_quoted (word, quoted)
     WORD_DESC *word;
     int quoted;
{
  return (call_expand_word_internal (word, quoted, (int *)NULL, (int *)NULL));
}

/* Return the value of a positional parameter.  This handles values > 10. */
char *
get_dollar_var_value (ind)
     int ind;
{
  char *temp;
  WORD_LIST *p;

  if (ind < 10)
    temp = dollar_vars[ind] ? savestring (dollar_vars[ind]) : (char *)NULL;
  else	/* We want something like ${11} */
    {
      ind -= 10;
      for (p = rest_of_args; p && ind--; p = p->next)
        ;
      temp = p ? savestring (p->word->word) : (char *)NULL;
    }
  return (temp);
}

#if defined (PROCESS_SUBSTITUTION)

/* **************************************************************** */
/*								  */
/*		    Hacking Process Substitution		    */
/*								  */
/* **************************************************************** */

#if !defined (HAVE_DEV_FD)
/* Named pipes must be removed explicitly with `unlink'.  This keeps a list
   of FIFOs the shell has open.  unlink_fifo_list will walk the list and
   unlink all of them. add_fifo_list adds the name of an open FIFO to the
   list.  NFIFO is a count of the number of FIFOs in the list. */
#define FIFO_INCR 20
extern char *mktemp ();

static char **fifo_list = (char **)NULL;
static int nfifo;
static int fifo_list_size;

static void
add_fifo_list (pathname)
     char *pathname;
{
  if (nfifo >= fifo_list_size - 1)
    {
      fifo_list_size += FIFO_INCR;
      fifo_list = (char **)xrealloc (fifo_list,
				     fifo_list_size * sizeof (char *));
    }

  fifo_list[nfifo++] = savestring (pathname);
}

void
unlink_fifo_list ()
{
  if (nfifo == 0)
    return;

  while (nfifo--)
    {
      unlink (fifo_list[nfifo]);
      free (fifo_list[nfifo]);
      fifo_list[nfifo] = (char *)NULL;
    }
  nfifo = 0;
}

static char *
make_named_pipe ()
{
  char *tname;

  tname = mktemp (savestring ("/tmp/sh-np-XXXXXX"));
  if (mkfifo (tname, 0600) < 0)
    {
      free (tname);
      return ((char *)NULL);
    }

  add_fifo_list (tname);
  return (tname);
}

#else /* HAVE_DEV_FD */

/* DEV_FD_LIST is a bitmap of file descriptors attached to pipes the shell
   has open to children.  NFDS is a count of the number of bits currently
   set in DEV_FD_LIST.  TOTFDS is a count of the highest possible number
   of open files. */
static char *dev_fd_list = (char *)NULL;
static int nfds;
static int totfds;	/* The highest possible number of open files. */

static void
add_fifo_list (fd)
     int fd;
{
  if (!dev_fd_list || fd >= totfds)
    {
      int ofds;

      ofds = totfds;
      totfds = getdtablesize ();
      if (totfds < 0 || totfds > 256)
	totfds = 256;
      if (fd > totfds)
	totfds = fd + 2;

      dev_fd_list = xrealloc (dev_fd_list, totfds);
      bzero (dev_fd_list + ofds, totfds - ofds);
    }

  dev_fd_list[fd] = 1;
  nfds++;
}

void
unlink_fifo_list ()
{
  register int i;

  if (nfds == 0)
    return;

  for (i = 0; nfds && i < totfds; i++)
    if (dev_fd_list[i])
      {
	close (i);
	dev_fd_list[i] = 0;
	nfds--;
      }

  nfds = 0;
}

#if defined (NOTDEF)
print_dev_fd_list ()
{
  register int i;

  fprintf (stderr, "pid %d: dev_fd_list:", getpid ());
  fflush (stderr);

  for (i = 0; i < totfds; i++)
    {
      if (dev_fd_list[i])
	fprintf (stderr, " %d", i);
    }
  fprintf (stderr, "\n");
}
#endif /* NOTDEF */

static char *
make_dev_fd_filename (fd)
     int fd;
{
  char *ret;

  ret = xmalloc (sizeof (DEV_FD_PREFIX) + 4);
  sprintf (ret, "%s%d", DEV_FD_PREFIX, fd);
  add_fifo_list (fd);
  return (ret);
}

#endif /* HAVE_DEV_FD */

/* Return a filename that will open a connection to the process defined by
   executing STRING.  HAVE_DEV_FD, if defined, means open a pipe and return
   a filename in /dev/fd corresponding to a descriptor that is one of the
   ends of the pipe.  If not defined, we use named pipes on systems that have
   them.  Systems without /dev/fd and named pipes are out of luck.

   OPEN_FOR_READ_IN_CHILD, if 1, means open the named pipe for reading or
   use the read end of the pipe and dup that file descriptor to fd 0 in
   the child.  If OPEN_FOR_READ_IN_CHILD is 0, we open the named pipe for
   writing or use the write end of the pipe in the child, and dup that
   file descriptor to fd 1 in the child.  The parent does the opposite. */

static char *
process_substitute (string, open_for_read_in_child)
     char *string;
     int open_for_read_in_child;
{
  char *pathname;
  int fd, result;
  pid_t old_pid, pid;
#if defined (HAVE_DEV_FD)
  int parent_pipe_fd, child_pipe_fd;
  int fildes[2];
#endif /* HAVE_DEV_FD */
#if defined (JOB_CONTROL)
  pid_t old_pipeline_pgrp;
#endif

  if (!string || !*string)
    return ((char *)NULL);

#if !defined (HAVE_DEV_FD)
  pathname = make_named_pipe ();
#else /* HAVE_DEV_FD */
  if (pipe (fildes) < 0)
    {
      sys_error ("cannot make pipe for process substitution");
      return ((char *)NULL);
    }
  /* If OPEN_FOR_READ_IN_CHILD == 1, we want to use the write end of
     the pipe in the parent, otherwise the read end. */
  parent_pipe_fd = fildes[open_for_read_in_child];
  child_pipe_fd = fildes[1 - open_for_read_in_child];
  /* Move the parent end of the pipe to some high file descriptor, to
     avoid clashes with FDs used by the script. */
  parent_pipe_fd = move_to_high_fd (parent_pipe_fd, 1, 64);

  pathname = make_dev_fd_filename (parent_pipe_fd);
#endif /* HAVE_DEV_FD */

  if (!pathname)
    {
      sys_error ("cannot make pipe for process substitution");
      return ((char *)NULL);
    }

  old_pid = last_made_pid;

#if defined (JOB_CONTROL)
  old_pipeline_pgrp = pipeline_pgrp;
  pipeline_pgrp = shell_pgrp;
#if 0
  cleanup_the_pipeline ();
#else
  save_pipeline (1);
#endif
#endif /* JOB_CONTROL */

  pid = make_child ((char *)NULL, 1);
  if (pid == 0)
    {
      reset_terminating_signals ();	/* XXX */
      /* Cancel traps, in trap.c. */
      restore_original_signals ();
      setup_async_signals ();
      subshell_environment = SUBSHELL_COMSUB;
    }

#if defined (JOB_CONTROL)
  set_sigchld_handler ();
  stop_making_children ();
  pipeline_pgrp = old_pipeline_pgrp;
#endif /* JOB_CONTROL */

  if (pid < 0)
    {
      sys_error ("cannot make child for process substitution");
      free (pathname);
#if defined (HAVE_DEV_FD)
      close (parent_pipe_fd);
      close (child_pipe_fd);
#endif /* HAVE_DEV_FD */
      return ((char *)NULL);
    }

  if (pid > 0)
    {
#if defined (JOB_CONTROL)
      restore_pipeline (1);
#endif

      last_made_pid = old_pid;

#if defined (JOB_CONTROL) && defined (PGRP_PIPE)
      close_pgrp_pipe ();
#endif /* JOB_CONTROL && PGRP_PIPE */

#if defined (HAVE_DEV_FD)
      close (child_pipe_fd);
#endif /* HAVE_DEV_FD */

      return (pathname);
    }

  set_sigint_handler ();

#if defined (JOB_CONTROL)
  set_job_control (0);
#endif /* JOB_CONTROL */

#if !defined (HAVE_DEV_FD)
  /* Open the named pipe in the child. */
  fd = open (pathname, open_for_read_in_child ? O_RDONLY|O_NONBLOCK : O_WRONLY);
  if (fd < 0)
    {
      sys_error ("cannot open named pipe %s for %s", pathname,
	open_for_read_in_child ? "reading" : "writing");
      exit (127);
    }
#else /* HAVE_DEV_FD */
  fd = child_pipe_fd;
#endif /* HAVE_DEV_FD */

  if (dup2 (fd, open_for_read_in_child ? 0 : 1) < 0)
    {
      sys_error ("cannot duplicate named pipe %s as fd %d", pathname,
	open_for_read_in_child ? 0 : 1);
      exit (127);
    }

  close (fd);

  /* Need to close any files that this process has open to pipes inherited
     from its parent. */
  if (current_fds_to_close)
    {
      close_fd_bitmap (current_fds_to_close);
      current_fds_to_close = (struct fd_bitmap *)NULL;
    }

#if defined (HAVE_DEV_FD)
  /* Make sure we close the parent's end of the pipe and clear the slot
     in the fd list so it is not closed later, if reallocated by, for
     instance, pipe(2). */
  close (parent_pipe_fd);
  dev_fd_list[parent_pipe_fd] = 0;
#endif /* HAVE_DEV_FD */

  result = parse_and_execute (string, "process substitution", (SEVAL_NONINT|SEVAL_NOHIST));

#if !defined (HAVE_DEV_FD)
  /* Make sure we close the named pipe in the child before we exit. */
  close (open_for_read_in_child ? 0 : 1);
#endif /* !HAVE_DEV_FD */

  exit (result);
  /*NOTREACHED*/
}
#endif /* PROCESS_SUBSTITUTION */

static char *
read_comsub (fd, quoted)
     int fd, quoted;
{
  char *istring, buf[128], *bufp;
  int bufn, istring_index, istring_size, c;

  istring = (char *)NULL;
  istring_index = istring_size = bufn = 0;

  /* Read the output of the command through the pipe. */
  while (1)
    {
      if (fd < 0)
        break;
      if (--bufn <= 0)
	{
	  while ((bufn = read (fd, buf, sizeof(buf))) < 0 && errno == EINTR)
	    ;
	  if (bufn <= 0) 
	    break;
	  bufp = buf;
	}
      c = *bufp++;

      /* Add the character to ISTRING, possibly after resizing it. */
      RESIZE_MALLOCED_BUFFER (istring, istring_index, 2, istring_size, DEFAULT_ARRAY_SIZE);

      if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) || c == CTLESC || c == CTLNUL)
	istring[istring_index++] = CTLESC;

      istring[istring_index++] = c;
    }

  if (istring)
    istring[istring_index] = '\0';

  /* If we read no output, just return now and save ourselves some
     trouble. */
  if (istring_index == 0)
    {
      FREE (istring);
      return (char *)NULL;
    }

  /* Strip trailing newlines from the output of the command. */
  if (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
    {
      while (istring_index > 0)
	{
	  if (istring[istring_index - 1] == '\n')
	    {
	      --istring_index;

	      /* If the newline was quoted, remove the quoting char. */
	      if (istring[istring_index - 1] == CTLESC)
		--istring_index;
	    }
	  else
	    break;
	}
      istring[istring_index] = '\0';
    }
  else
    strip_trailing (istring, istring_index - 1, 1);

  return istring;
}

/* Perform command substitution on STRING.  This returns a string,
   possibly quoted. */
static char *
command_substitute (string, quoted)
     char *string;
     int quoted;
{
  pid_t pid, old_pid, old_pipeline_pgrp;
  char *istring;
  int result, fildes[2];

  istring = (char *)NULL;

  /* Don't fork () if there is no need to.  In the case of no command to
     run, just return NULL. */
  if (!string || !*string || (string[0] == '\n' && !string[1]))
    return ((char *)NULL);

  /* Pipe the output of executing STRING into the current shell. */
  if (pipe (fildes) < 0)
    {
      sys_error ("cannot make pipes for command substitution");
      goto error_exit;
    }

  old_pid = last_made_pid;
#if defined (JOB_CONTROL)
  old_pipeline_pgrp = pipeline_pgrp;
  pipeline_pgrp = shell_pgrp;
  cleanup_the_pipeline ();
#endif

  pid = make_child ((char *)NULL, 0);
  if (pid == 0)
    /* Reset the signal handlers in the child, but don't free the
       trap strings. */
    reset_signal_handlers ();

#if defined (JOB_CONTROL)
  set_sigchld_handler ();
  stop_making_children ();
  pipeline_pgrp = old_pipeline_pgrp;
#endif /* JOB_CONTROL */

  if (pid < 0)
    {
      sys_error ("cannot make child for command substitution");
    error_exit:

      FREE (istring);
      close (fildes[0]);
      close (fildes[1]);
      return ((char *)NULL);
    }

  if (pid == 0)
    {
      set_sigint_handler ();	/* XXX */
#if defined (JOB_CONTROL)
      set_job_control (0);
#endif
      if (dup2 (fildes[1], 1) < 0)
	{
	  sys_error ("command_substitute: cannot duplicate pipe as fd 1");
	  exit (EXECUTION_FAILURE);
	}

      /* If standard output is closed in the parent shell
	 (such as after `exec >&-'), file descriptor 1 will be
	 the lowest available file descriptor, and end up in
	 fildes[0].  This can happen for stdin and stderr as well,
	 but stdout is more important -- it will cause no output
	 to be generated from this command. */
      if ((fildes[1] != fileno (stdin)) &&
	  (fildes[1] != fileno (stdout)) &&
	  (fildes[1] != fileno (stderr)))
	close (fildes[1]);

      if ((fildes[0] != fileno (stdin)) &&
	  (fildes[0] != fileno (stdout)) &&
	  (fildes[0] != fileno (stderr)))
	close (fildes[0]);

      /* The currently executing shell is not interactive. */
      interactive = 0;

      /* This is a subshell environment. */
      subshell_environment = SUBSHELL_COMSUB;

      /* Command substitution does not inherit the -e flag. */
      exit_immediately_on_error = 0;

      remove_quoted_escapes (string);

      startup_state = 2;	/* see if we can avoid a fork */
      /* Give command substitution a place to jump back to on failure,
	 so we don't go back up to main (). */
      result = setjmp (top_level);

      if (result == EXITPROG)
	exit (last_command_exit_value);
      else if (result)
	exit (EXECUTION_FAILURE);
      else
	exit (parse_and_execute (string, "command substitution", SEVAL_NOHIST));
    }
  else
    {
#if defined (JOB_CONTROL) && defined (PGRP_PIPE)
      close_pgrp_pipe ();
#endif /* JOB_CONTROL && PGRP_PIPE */

      close (fildes[1]);

      istring = read_comsub (fildes[0], quoted);

      close (fildes[0]);

      last_command_exit_value = wait_for (pid);
      last_command_subst_pid = pid;
      last_made_pid = old_pid;

#if defined (JOB_CONTROL)
      /* If last_command_exit_value > 128, then the substituted command
	 was terminated by a signal.  If that signal was SIGINT, then send
	 SIGINT to ourselves.  This will break out of loops, for instance. */
      if (last_command_exit_value == (128 + SIGINT))
	kill (getpid (), SIGINT);

      /* wait_for gives the terminal back to shell_pgrp.  If some other
	 process group should have it, give it away to that group here. */
      if (interactive && pipeline_pgrp != (pid_t)0)
	give_terminal_to (pipeline_pgrp);
#endif /* JOB_CONTROL */

      return (istring);
    }
}

/********************************************************
 *							*
 *	Utility functions for parameter expansion	*
 *							*
 ********************************************************/

static int
getpatspec (c, value)
     int c;
     char *value;
{
  if (c == '#')
    return ((*value == '#') ? RP_LONG_LEFT : RP_SHORT_LEFT);
  else	/* c == '%' */
    return ((*value == '%') ? RP_LONG_RIGHT : RP_SHORT_RIGHT);
}

/* Posix.2 says that the WORD should be run through tilde expansion,
   parameter expansion, command substitution and arithmetic expansion.
   This leaves the result quoted, so quote_string_for_globbing () has
   to be called to fix it up for fnmatch ().  If QUOTED is non-zero,
   it means that the entire expression was enclosed in double quotes.
   This means that quoting characters in the pattern do not make any
   special pattern characters quoted.  For example, the `*' in the
   following retains its special meaning: "${foo#'*'}". */
static char *
getpattern (value, quoted, expandpat)
     char *value;
     int quoted, expandpat;
{
  char *pat, *tword;
  WORD_LIST *l;
  int i;

  tword = strchr (value, '~') ? bash_tilde_expand (value) : savestring (value);

  /* expand_string_internal () leaves WORD quoted and does not perform
     word splitting. */
  if (expandpat && (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && *tword)
    {
      i = 0;
      pat = string_extract_double_quoted (tword, &i, 1);
      free (tword);
      tword = pat;
    }

  /* There is a problem here:  how to handle single or double quotes in the
     pattern string when the whole expression is between double quotes? */
#if 0
  l = *tword ? expand_string_for_rhs (tword, quoted, (int *)NULL, (int *)NULL)
#else
  l = *tword ? expand_string_for_rhs (tword,
				      (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) ? Q_NOQUOTE : quoted,
				      (int *)NULL, (int *)NULL)
#endif
	     : (WORD_LIST *)0;
  free (tword);
  pat = string_list (l);
  dispose_words (l);
  if (pat)
    {
      tword = quote_string_for_globbing (pat, 1);
      free (pat);
      pat = tword;
    }
  return (pat);
}

/* Handle removing a pattern from a string as a result of ${name%[%]value}
   or ${name#[#]value}. */
static char *
parameter_brace_remove_pattern (value, temp, c, quoted)
     char *value, *temp;
     int c, quoted;
{
  int patspec;
  char *pattern, *tword;

  patspec = getpatspec (c, value);
  if (patspec == RP_LONG_LEFT || patspec == RP_LONG_RIGHT)
    value++;

  pattern = getpattern (value, quoted, 1);

  tword = remove_pattern (temp, pattern, patspec);

  FREE (pattern);
  return (tword);
}

static char *
list_remove_pattern (list, pattern, patspec, type, quoted)
     WORD_LIST *list;
     char *pattern;
     int patspec, type, quoted;
{
  WORD_LIST *new, *l;
  WORD_DESC *w;
  char *tword;

  for (new = (WORD_LIST *)NULL, l = list; l; l = l->next)
    {
      tword = remove_pattern (l->word->word, pattern, patspec);
      w = make_bare_word (tword);
      free (tword);
      new = make_word_list (w, new);
    }

  l = REVERSE_LIST (new, WORD_LIST *);
  if (type == '*')
    tword = (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) ? string_list_dollar_star (l) : string_list (l);
  else
    tword = string_list ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) ? quote_list (l) : l);

  dispose_words (l);
  return (tword);
}

static char *
parameter_list_remove_pattern (value, type, c, quoted)
     char *value;
     int type, c, quoted;
{
  int patspec;
  char *pattern, *ret;
  WORD_LIST *list;

  patspec = getpatspec (c, value);
  if (patspec == RP_LONG_LEFT || patspec == RP_LONG_RIGHT)
    value++;

  pattern = getpattern (value, quoted, 1);

  list = list_rest_of_args ();
  ret = list_remove_pattern (list, pattern, patspec, type, quoted);
  dispose_words (list);
  FREE (pattern);
  return (ret);
}

#if defined (ARRAY_VARS)
static char *
array_remove_pattern (value, aspec, aval, c, quoted)
     char *value, *aspec, *aval;	/* AVAL == evaluated ASPEC */
     int c, quoted;
{
  SHELL_VAR *var;
  int len, patspec;
#if 0
  int ind;
#endif
  char *ret, *t, *pattern;
  WORD_LIST *l;

  var = array_variable_part (aspec, &t, &len);
  if (var == 0)
    return ((char *)NULL);

  patspec = getpatspec (c, value);
  if (patspec == RP_LONG_LEFT || patspec == RP_LONG_RIGHT)
    value++;

  pattern = getpattern (value, quoted, 1);

  if (ALL_ELEMENT_SUB (t[0]) && t[1] == ']')
    {
      if (array_p (var) == 0)
        {
          report_error ("%s: bad array subscript", aspec);
          FREE (pattern);
          return ((char *)NULL);
        }
      l = array_to_word_list (array_cell (var));
      if (l == 0)
        return ((char *)NULL);
      ret = list_remove_pattern (l, pattern, patspec, t[0], quoted);
      dispose_words (l);
    }
  else
    {
#if 0
      ind = array_expand_index (t, len);
      if (ind < 0)
	{
	  report_error ("%s: bad array subscript", aspec);
	  FREE (pattern);
	  return ((char *)NULL);
	}
      if (array_p (var) == 0 && ind != 0)
	return ((char *)NULL);

      t = array_p (var) ? array_reference (array_cell (var), ind) : value_cell (var);
      ret = remove_pattern (t, pattern, patspec);
#else
      ret = remove_pattern (aval, pattern, patspec);
#endif
      if (ret)
	{
	  t = quote_escapes (ret);
	  free (ret);
	  ret = t;
	}
    }

  FREE (pattern);
  return ret;
}

int
valid_array_reference (name)
     char *name;
{
  char *t;
  int r, len;

  t = strchr (name, '[');
  if (t)
    {
      *t = '\0';
      r = legal_identifier (name);
      *t = '[';
      if (r == 0)
	return 0;
      /* Check for a properly-terminated non-blank subscript. */
      len = skipsubscript (t, 0);
      if (t[len] != ']' || len == 1)
	return 0;
      for (r = 1; r < len; r++)
	if (whitespace (t[r]) == 0)
	  return 1;
      return 0;
    }
  return 0;
}

/* Expand the array index beginning at S and extending LEN characters. */
int
array_expand_index (s, len)
     char *s;
     int len;
{
  char *exp, *t;
  int val, expok;

  exp = xmalloc (len);
  strncpy (exp, s, len - 1);
  exp[len - 1] = '\0';
  t = maybe_expand_string (exp, 0, expand_string);
  this_command_name = (char *)NULL;
  val = evalexp (t, &expok);
  free (t);
  free (exp);
  if (expok == 0)
    jump_to_top_level (DISCARD);
  return val;
}

/* Return the variable specified by S without any subscript.  If non-null,
   return the index of the start of the subscript in *SUBP.  If non-null,
   the length of the subscript is returned in *LENP. */
SHELL_VAR *
array_variable_part (s, subp, lenp)
     char *s, **subp;
     int *lenp;
{
  char *t;
  int ind, ni;
  SHELL_VAR *var;

  t = strchr (s, '[');
  ind = t - s;
  ni = skipsubscript (s, ind);
  if (ni <= ind + 1 || s[ni] != ']')
    {
      report_error ("%s: bad array subscript", s);
      return ((SHELL_VAR *)NULL);
    }

  *t = '\0';
  var = find_variable (s);
  *t++ = '[';

  if (subp)
    *subp = t;
  if (lenp)
    *lenp = ni - ind;
  return var;
}

static char *
array_value_internal (s, quoted, allow_all)
     char *s;
     int quoted, allow_all;
{
  int len, ind;
  char *retval, *t;
  WORD_LIST *l;
  SHELL_VAR *var;

  var = array_variable_part (s, &t, &len);

  if (var == 0)
    return (char *)NULL;

  if (ALL_ELEMENT_SUB (t[0]) && t[1] == ']')
    {
      if (array_p (var) == 0 || allow_all == 0)
        {
          report_error ("%s: bad array subscript", s);
          return ((char *)NULL);
        }
      l = array_to_word_list (array_cell (var));
      if (l == (WORD_LIST *)NULL)
	return ((char *) NULL);

      if (t[0] == '*')		/* ${name[*]} */
	retval = (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) ? string_list_dollar_star (l) : string_list (l);
      else			/* ${name[@]} */
	retval = string_list ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) ? quote_list (l) : l);

      dispose_words (l);
    }
  else
    {
      ind = array_expand_index (t, len);
      if (ind < 0)
	{
	  report_error ("%s: bad array subscript", var->name);
	  return ((char *)NULL);
	}
      if (array_p (var) == 0)
        return (ind == 0 ? savestring (value_cell (var)) : (char *)NULL);
      retval = array_reference (array_cell (var), ind);
      if (retval)
	retval = quote_escapes (retval);
    }

  return retval;
}

static char *
array_value (s, quoted)
     char *s;
     int quoted;
{
  return (array_value_internal (s, quoted, 1));
}

/* Return the value of the array indexing expression S as a single string.
   If ALLOW_ALL is 0, do not allow `@' and `*' subscripts.  This is used
   by other parts of the shell such as the arithmetic expression evaluator
   in expr.c. */
char *
get_array_value (s, allow_all)
     char *s;
     int allow_all;
{
  return (array_value_internal (s, 0, allow_all));
}

static int
array_length_reference (s)
     char *s;
{
  int ind, len;
  char *t;
  ARRAY *array;
  SHELL_VAR *var;

  var = array_variable_part (s, &t, &len);

  /* If unbound variables should generate an error, report one and return
     failure. */
  if ((var == 0 || array_p (var) == 0) && unbound_vars_is_error)
    {
      ind = *--t;
      *t = '\0';
      report_error ("%s: unbound variable", s);
      *t++ = (char)ind;
      return (-1);
    }
  else if (var == 0)
    return 0;
  else if (array_p (var) == 0)
    return 1;

  array = array_cell (var);

  if (ALL_ELEMENT_SUB (t[0]) && t[1] == ']')
    return (array_num_elements (array));

  ind = array_expand_index (t, len);
  if (ind < 0)
    {
      report_error ("%s: bad array subscript", t);
      return (-1);
    }
  t = array_reference (array, ind);
  len = STRLEN (t);

  return (len);
}
#endif /* ARRAY_VARS */

static int
valid_brace_expansion_word (name, var_is_special)
     char *name;
     int var_is_special;
{
  if (digit (*name) && all_digits (name))
    return 1;
  else if (var_is_special)
    return 1;
#if defined (ARRAY_VARS)
  else if (valid_array_reference (name))
    return 1;
#endif /* ARRAY_VARS */
  else if (legal_identifier (name))
    return 1;
  else
    return 0;
}

/* Parameter expand NAME, and return a new string which is the expansion,
   or NULL if there was no expansion.
   VAR_IS_SPECIAL is non-zero if NAME is one of the special variables in
   the shell, e.g., "@", "$", "*", etc.  QUOTED, if non-zero, means that
   NAME was found inside of a double-quoted expression. */
static char *
parameter_brace_expand_word (name, var_is_special, quoted)
     char *name;
     int var_is_special, quoted;
{
  char *temp, *tt;
  int arg_index;
  SHELL_VAR *var;
  WORD_LIST *l;

  /* Handle multiple digit arguments, as in ${11}. */
  if (digit (*name))
    {
      arg_index = atoi (name);
      temp = get_dollar_var_value (arg_index);
    }
  else if (var_is_special)      /* ${@} */
    {
      tt = xmalloc (2 + strlen (name));
      tt[0] = '$';
      strcpy (tt + 1, name);
      l = expand_string_leave_quoted (tt, quoted);
      free (tt);
      temp = string_list (l);
      dispose_words (l);
    }
#if defined (ARRAY_VARS)
  else if (valid_array_reference (name))
    {
      temp = array_value (name, quoted);
    }
#endif
  else if (var = find_variable (name))
    {
      if (var && invisible_p (var) == 0)
        {
#if defined (ARRAY_VARS)
	  temp = array_p (var) ? array_reference (array_cell (var), 0) : value_cell (var);
#else
	  temp = value_cell (var);
#endif

	  if (temp)
	    temp = quote_escapes (temp);
        }
      else
	temp = (char *)NULL;
    }
  else
    temp = (char *)NULL;

  return (temp);
}

/* Expand an indirect reference to a variable: ${!NAME} expands to the
   value of the variable whose name is the value of NAME. */
static char *
parameter_brace_expand_indir (name, var_is_special, quoted)
     char *name;
     int var_is_special, quoted;
{
  char *temp, *t;

  t = parameter_brace_expand_word (name, var_is_special, quoted);
  if (t == 0)
    return (t);
  temp = parameter_brace_expand_word (t, t[0] == '@' && t[1] == '\0', quoted);
  free (t);
  return temp;
}

/* Expand the right side of a parameter expansion of the form ${NAMEcVALUE},
   depending on the value of C, the separating character.  C can be one of
   "-", "+", or "=".  QUOTED is true if the entire brace expression occurs
   between double quotes. */
static char *
parameter_brace_expand_rhs (name, value, c, quoted, qdollaratp, hasdollarat)
     char *name, *value;
     int c, quoted, *qdollaratp, *hasdollarat;
{
  WORD_LIST *l;
  char *t, *t1, *temp;
  int hasdol;

  temp = (*value == '~' || (strchr (value, '~') && unquoted_substring ("=~", value)))
        ? bash_tilde_expand (value)
	: savestring (value);

  /* If the entire expression is between double quotes, we want to treat
     the value as a double-quoted string, with the exception that we strip
     embedded unescaped double quotes. */
  if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && *temp)
    {
      hasdol = 0;
      t = string_extract_double_quoted (temp, &hasdol, 1);
      free (temp);
      temp = t;
    }

  hasdol = 0;
  /* XXX was 0 not quoted */
  l = *temp ? expand_string_for_rhs (temp, quoted, &hasdol, (int *)NULL)
	    : (WORD_LIST *)0;
  if (hasdollarat)
    *hasdollarat = hasdol || (l && l->next);
  free (temp);
  if (l)
    {
      /* The expansion of TEMP returned something.  We need to treat things
	  slightly differently if HASDOL is non-zero. */
      temp = string_list (l);
      /* If l->next is not null, we know that TEMP contained "$@", since that
	 is the only expansion that creates more than one word. */
      if ((hasdol && quoted) || l->next)
	*qdollaratp = 1;
      dispose_words (l);
    }
  else if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && hasdol)
    {
      /* The brace expansion occurred between double quotes and there was
	 a $@ in TEMP.  It does not matter if the $@ is quoted, as long as
 	 it does not expand to anything.  In this case, we want to return
 	 a quoted empty string. */
      temp = xmalloc (2);
      temp[0] = CTLNUL;
      temp[1] = '\0';
    }
  else
    temp = (char *)NULL;

  if (c == '-' || c == '+')
    return (temp);

  /* c == '=' */
  t = temp ? savestring (temp) : savestring ("");
  t1 = dequote_string (t);
  free (t);
  bind_variable (name, t1);
  free (t1);
  return (temp);
}

/* Deal with the right hand side of a ${name:?value} expansion in the case
   that NAME is null or not set.  If VALUE is non-null it is expanded and
   used as the error message to print, otherwise a standard message is
   printed. */
static void
parameter_brace_expand_error (name, value)
     char *name, *value;
{
  WORD_LIST *l;
  char *temp;

  if (value && *value)
    {
      l = expand_string (value, 0);
      temp =  string_list (l);
      report_error ("%s: %s", name, temp ? temp : "");	/* XXX was value not "" */
      FREE (temp);
      dispose_words (l);
    }
  else
    report_error ("%s: parameter null or not set", name);

  /* Free the data we have allocated during this expansion, since we
     are about to longjmp out. */
  free (name);
  FREE (value);
}

/* Return 1 if NAME is something for which parameter_brace_expand_length is
   OK to do. */
static int
valid_length_expression (name)
     char *name;
{
  return (!name[1] ||						/* ${#} */
	  ((name[1] == '@' || name[1] == '*') && !name[2]) ||	/* ${#@}, ${#*} */
	  (digit (name[1]) && all_digits (name + 1)) ||		/* ${#11} */
#if defined (ARRAY_VARS)
	  valid_array_reference (name + 1) ||			/* ${#a[7]} */
#endif
	  legal_identifier (name + 1));				/* ${#PS1} */
}

/* Handle the parameter brace expansion that requires us to return the
   length of a parameter. */
static int
parameter_brace_expand_length (name)
     char *name;
{
  char *t, *newname;
  int number;
  WORD_LIST *list;
#if defined (ARRAY_VARS)
  SHELL_VAR *var;
#endif

  if (name[1] == '\0')			/* ${#} */
    number = number_of_args ();
#if defined (ARRAY_VARS)
  else if (valid_array_reference (name + 1))
    number = array_length_reference (name + 1);
#endif /* ARRAY_VARS */
  else if (name[1] != '*' && name[1] != '@')
    {
      number = 0;

      if (digit (name[1]))		/* ${#1} */
	{
	  t = get_dollar_var_value (atoi (name + 1));
	  number = STRLEN (t);
	  FREE (t);
	}
#if defined (ARRAY_VARS)
      else if ((var = find_variable (name + 1)) && array_p (var))
	{
	  t = array_reference (array_cell (var), 0);
	  number = STRLEN (t);
	}
#endif
      else				/* ${#PS1} */
	{
	  newname = savestring (name);
	  newname[0] = '$';
	  list = expand_string (newname, Q_DOUBLE_QUOTES);
	  t = list ? string_list (list) : (char *)NULL;
	  free (newname);
	  if (list)
	    dispose_words (list);

	  number = STRLEN (t);
	  FREE (t);
	}
    }
  else					/* ${#@} and ${#*} */
    number = number_of_args ();

  return (number);
}

/* Verify and limit the start and end of the desired substring.  If
   VTYPE == 0, a regular shell variable is being used; if it is 1,
   then the positional paramters are being used; if it is 2, then
   VALUE is really a pointer to an array variable that should be used. */
static int
verify_substring_values (value, substr, vtype, e1p, e2p)
     char *value, *substr;
     int vtype, *e1p, *e2p;
{
  char *t, *temp1;
  int len, expok;
#if defined (ARRAY_VARS)
 ARRAY *a;
#endif

  t = strchr (substr, ':');
  if (t)
    *t = '\0';
  temp1 = maybe_expand_string (substr, Q_DOUBLE_QUOTES, expand_string);
  *e1p = evalexp (temp1, &expok);
  free (temp1);
  if (expok == 0)
    return (0);

  switch (vtype)
    {
    case VT_VARIABLE:
    case VT_ARRAYMEMBER:
      len = strlen (value);
      break;
    case VT_POSPARMS:
      len = number_of_args () + 1;
      break;
#if defined (ARRAY_VARS)
    case VT_ARRAYVAR:
      a = (ARRAY *)value;
      len = array_num_elements (a) + 1;
      break;
#endif
    }

  if (*e1p < 0)		/* negative offsets count from end */
    *e1p += len;

  if (*e1p >= len || *e1p < 0)
    return (0);

  if (t)
    {
      t++;
      temp1 = maybe_expand_string (t, Q_DOUBLE_QUOTES, expand_string);
      t[-1] = ':';
      *e2p = evalexp (temp1, &expok);
      free (temp1);
      if (expok == 0)
        return (0);
      if (*e2p < 0)
        {
          internal_error ("%s: substring expression < 0", t);
	  return (0);
        }
      *e2p += *e1p;		/* want E2 chars starting at E1 */
      if (*e2p > len)
        *e2p = len;
    }
  else
    *e2p = len;

  return (1);
}

/* Return a string containing the positional parameters from START to
   END, inclusive.  If STRING[0] == '*', we obey the rules for $*,
   which only makes a difference if QUOTED is non-zero. */
static char *
pos_params (string, start, end, quoted)
     char *string;
     int start, end, quoted;
{
  WORD_LIST *save, *params, *h, *t;
  char *ret;
  int i;

  save = params = list_rest_of_args ();
  if (save == 0)
    return ((char *)NULL);

  for (i = 1; params && i < start; i++)
    params = params->next;
  if (params == 0)
    return ((char *)NULL);
  for (h = t = params; params && i < end; i++)
    {
      t = params;
      params = params->next;
    }

  t->next = (WORD_LIST *)NULL;
  if (string[0] == '*')
    ret = (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) ? string_list_dollar_star (h) : string_list (h);
  else
    ret = string_list ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) ? quote_list (h) : h);
  t->next = params;

  dispose_words (save);
  return (ret);
}

/* Return the type of variable specified by VARNAME (simple variable,
   positional param, or array variable.  Also return the value specified
   by VARNAME (value of a variable or a reference to an array element). */
static int
get_var_and_type (varname, value, varp, valp)
     char *varname, *value;
     SHELL_VAR **varp;
     char **valp;
{
  int vtype;
  char *temp;
#if defined (ARRAY_VARS)
  SHELL_VAR *v;
#endif

  vtype = (varname[0] == '@' || varname[0] == '*') && varname[1] == '\0';	/* VT_POSPARMS */
  *varp = (SHELL_VAR *)NULL;

#if defined (ARRAY_VARS)
  if (valid_array_reference (varname))
    {
      v = array_variable_part (varname, &temp, (int *)0);
      if (v && array_p (v))
	{
	  if ((temp[0] == '@' || temp[0] == '*') && temp[1] == ']')
	    {
	      vtype = VT_ARRAYVAR;
	      *valp = (char *)array_cell (v);
	    }
	  else
	    {
	      vtype = VT_ARRAYMEMBER;
	      *valp = array_value (varname, 1);
	    }
	  *varp = v;
	}
      else
	return -1;
    }
  else if ((v = find_variable (varname)) && array_p (v))
    {
      vtype = VT_VARIABLE;
      *varp = v;
      *valp = array_reference (array_cell (v), 0);
    }
  else
#endif
  *valp = value;

  return vtype;
}

/* Process a variable substring expansion: ${name:e1[:e2]}.  If VARNAME
   is `@', use the positional parameters; otherwise, use the value of
   VARNAME.  If VARNAME is an array variable, use the array elements. */

static char *
parameter_brace_substring (varname, value, substr, quoted)
     char *varname, *value, *substr;
     int quoted;
{
  int e1, e2, vtype;
  char *temp, *val;
  SHELL_VAR *v;

  if (value == 0)
    return ((char *)NULL);

  this_command_name = varname;

  vtype = get_var_and_type (varname, value, &v, &val);
  if (vtype == -1)
    return ((char *)NULL);

  if (verify_substring_values (val, substr, vtype, &e1, &e2) == 0)
    {
      if (val && vtype == VT_ARRAYMEMBER)
	free (val);
      return (&expand_param_error);
    }

  switch (vtype)
    {
    case VT_VARIABLE:
    case VT_ARRAYMEMBER:
      temp = quoted ? quoted_substring (value, e1, e2) : substring (value, e1, e2);
      break;
    case VT_POSPARMS:
      temp = pos_params (varname, e1, e2, quoted);
      break;
#if defined (ARRAY_VARS)
    case VT_ARRAYVAR:
      temp = array_subrange (array_cell (v), e1, e2, quoted);
      break;
#endif
    }

  return temp;
}

char *
pat_subst (string, pat, rep, mflags)
     char *string, *pat, *rep;
     int mflags;
{
  char *ret, *s, *e, *str;
  int rsize, rptr, l, replen, mtype;

  ret = xmalloc (rsize = 64);
  ret[0] = '\0';

  mtype = mflags & MATCH_TYPEMASK;

  for (replen = STRLEN (rep), rptr = 0, str = string;;)
    {
      if (match_pattern (str, pat, mtype, &s, &e) == 0)
	break;
      l = s - str;
      RESIZE_MALLOCED_BUFFER (ret, rptr, (l + replen), rsize, 64);

      /* OK, now copy the leading unmatched portion of the string (from
	 str to s) to ret starting at rptr (the current offset).  Then copy
         the replacement string at ret + rptr + (s - str).  Increment
         rptr (if necessary) and str and go on. */
      if (l)
	{
	  strncpy (ret + rptr, str, l);
	  rptr += l;
	}
      if (replen)
	{
	  strncpy (ret + rptr, rep, replen);
	  rptr += replen;
	}
      str = e;		/* e == end of match */
      if (((mflags & MATCH_GLOBREP) == 0) || mtype != MATCH_ANY)
        break;
    }

  /* Now copy the unmatched portion of the input string */
  if (*str)
    {
      RESIZE_MALLOCED_BUFFER (ret, rptr, STRLEN(str) + 1, rsize, 64);
      strcpy (ret + rptr, str);
    }
  else
    ret[rptr] = '\0';

  return ret;
}

/* Do pattern match and replacement on the positional parameters. */
static char *
pos_params_pat_subst (string, pat, rep, mflags)
     char *string, *pat, *rep;
     int mflags;
{
  WORD_LIST *save, *params;
  WORD_DESC *w;
  char *ret;

  save = params = list_rest_of_args ();
  if (save == 0)
    return ((char *)NULL);

  for ( ; params; params = params->next)
    {
      ret = pat_subst (params->word->word, pat, rep, mflags);
      w = make_bare_word (ret);
      dispose_word (params->word);
      params->word = w;
      FREE (ret);
    }

  ret = string_list ((mflags & MATCH_QUOTED) ? quote_list (save) : save);
  dispose_words (save);

  return (ret);
}

static char *
parameter_brace_patsub (varname, value, patsub, quoted)
     char *varname, *value, *patsub;
     int quoted;
{
  int vtype, mflags;
  char *val, *temp, *pat, *rep, *p;
  SHELL_VAR *v;

  if (value == 0)
    return ((char *)NULL);

  this_command_name = varname;

  vtype = get_var_and_type (varname, value, &v, &val);
  if (vtype == -1)
    return ((char *)NULL);

  mflags = 0;
  if (*patsub == '/')
    {
      mflags |= MATCH_GLOBREP;
      patsub++;
    }

  if (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
    mflags |= MATCH_QUOTED;

  if (rep = quoted_strchr (patsub, '/', ST_BACKSL))
    *rep++ = '\0';
  else
    rep = (char *)NULL;

  if (rep && *rep == '\0')
    rep = (char *)NULL;

  /* Expand PAT and REP for command, variable and parameter, arithmetic,
     and process substitution.  Also perform quote removal.  Do not
     perform word splitting or filename generation. */
  pat = maybe_expand_string (patsub, quoted, expand_string_unsplit);
  if (rep)
    {
      if ((mflags & MATCH_QUOTED) == 0)
	rep = maybe_expand_string (rep, quoted, expand_string_unsplit);
      else
        rep = expand_string_to_string (rep, quoted, expand_string_unsplit);
    }

  p = pat;
  if (pat && pat[0] == '#')
    {
      mflags |= MATCH_BEG;
      p++;
    }
  else if (pat && pat[0] == '%')
    {
      mflags |= MATCH_END;
      p++;
    }
  else
    mflags |= MATCH_ANY;

  /* OK, we now want to substitute REP for PAT in VAL.  If GLOBAL is 1,
     the substitution is done everywhere, otherwise only the first
     occurrence of PAT is replaced. */
  switch (vtype)
    {
    case VT_VARIABLE:
    case VT_ARRAYMEMBER:
      temp = pat_subst (val, p, rep, mflags);
      break;
    case VT_POSPARMS:
      temp = pos_params_pat_subst (val, p, rep, mflags);
      break;
#if defined (ARRAY_VARS)
    case VT_ARRAYVAR:
      temp = array_pat_subst (array_cell (v), p, rep, mflags);
      break;
#endif
    }

  if (val && v && array_p (v) && vtype == VT_ARRAYMEMBER)
    free (val);

  FREE (pat);
  FREE (rep);

  return temp;
}

/* ${[#][!]name[[:]#[#]%[%]-=?+[word][:e1[:e2]]]} */
static char *
parameter_brace_expand (string, indexp, quoted, quoted_dollar_atp, contains_dollar_at)
     char *string;
     int *indexp, quoted, *quoted_dollar_atp, *contains_dollar_at;
{
  int check_nullness, var_is_set, var_is_null, var_is_special;
  int want_substring, want_indir, want_patsub;
  char *name, *value, *temp, *temp1;
  int t_index, sindex, c, number;

  sindex = *indexp;
  t_index = ++sindex;
  name = string_extract (string, &t_index, "#%:-=?+/}", 1);
  value = (char *)NULL;
  var_is_set = var_is_null = var_is_special = check_nullness = 0;
  want_substring = want_indir = want_patsub = 0;

  /* If the name really consists of a special variable, then
     make sure that we have the entire name.  Handle indirect
     references to special variables here, too. */
  if ((sindex == t_index ||
      ((sindex == t_index - 1) && string[sindex] == '!')) &&
	(string[t_index] == '-' ||
	 string[t_index] == '?' ||
	 string[t_index] == '#'))
    {
      t_index++;
      free (name);
      temp1 = string_extract (string, &t_index, "#%:-=?+/}", 0);
      name = xmalloc (3 + (strlen (temp1)));
      *name = string[sindex];
      if (string[sindex] == '!')
	{
	  /* indirect ref. of special variable */
	  name[1] = string[sindex + 1];
	  strcpy (name + 2, temp1);
	}
      else
	strcpy (name + 1, temp1);
      free (temp1);
    }
  sindex = t_index;

  /* Find out what character ended the variable name.  Then
     do the appropriate thing. */
  if (c = string[sindex])
    sindex++;

  /* If c is followed by one of the valid parameter expansion
     characters, move past it as normal.  If not, assume that
     a substring specification is being given, and do not move
     past it. */
  if (c == ':' && member (string[sindex], "-=?+"))
    {
      check_nullness++;
      if (c = string[sindex])
	sindex++;
    }
  else if (c == ':')
    want_substring = 1;
  else if (c == '/')
    want_patsub = 1;

  want_indir = *name == '!';

  /* Determine the value of this variable. */

  /* Check for special variables, directly and indirectly
     referenced. */
  if ((digit (*name) && all_digits (name)) ||
      (name[1] == '\0' && member (*name, "#-?$!@*")) ||
      (want_indir && name[2] == '\0' && member (name[1], "#-?$!@*")))
    var_is_special++;

  /* Check for special expansion things. */
  if (*name == '#')	/* length of a parameter */
    {
      /* Take the lengths of some of the shell's special
         parameters. */
      if (string[sindex] == '}' && name[1] == '\0' &&
	  check_nullness == 0 && member (c, "-?$!#"))
	{
	  free (name);
	  switch (c)
	    {
	    case '-':
	      temp1 = which_set_flags ();
	      break;
	    case '?':
	      temp1 = itos (last_command_exit_value);
	      break;
	    case '$':
	      temp1 = itos (dollar_dollar_pid);
	      break;
	    case '!':
	      if (last_asynchronous_pid == NO_PID)
		temp1 = (char *)NULL;
	      else
		temp1 = itos ((int)last_asynchronous_pid);
	      break;
	    case '#':
	      temp1 = itos (number_of_args ());
	      break;
	    }
	  number = STRLEN (temp1);
	  FREE (temp1);
	  *indexp = ++sindex;	/* { string[sindex] == '}' */
	  return (itos (number));
	}

      /* Don't allow things like ${#:-foo} to go by; they are
         errors.  If we are not pointing at the character just
         after the closing brace, then we haven't gotten all of
         the name.  Since it begins with a special character,
         this is a bad substitution.  Explicitly check for ${#:},
         which the rules do not catch.  Also check NAME for
         validity before trying to go on. */
      if (string[sindex - 1] != '}' ||
	  member (c, "?-=+") ||
	  (name[1] == '\0' && c == '}' && check_nullness) ||
	  (valid_length_expression (name) == 0))
	{
	  temp = (char *)NULL;
	  goto bad_substitution;
	}

      number = parameter_brace_expand_length (name);
      free (name);

      *indexp = sindex;
      return ((number < 0) ? &expand_param_error : itos (number));
    }

  /* ${@} is identical to $@. */
  if (name[0] == '@' && name[1] == '\0')
    {
      if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && quoted_dollar_atp)
	*quoted_dollar_atp = 1;

      if (contains_dollar_at)
	*contains_dollar_at = 1;
    }

  /* Make sure that NAME is valid before trying to go on. */
  if (valid_brace_expansion_word (want_indir ? name + 1 : name,
					var_is_special) == 0)
    {
      temp = (char *)NULL;
      goto bad_substitution;
    }

  if (want_indir)
    temp = parameter_brace_expand_indir (name + 1, var_is_special, quoted);
  else
    temp = parameter_brace_expand_word (name, var_is_special, quoted);

#if defined (ARRAY_VARS)
  if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && valid_array_reference (name))
    {
      temp1 = strchr (name, '[');
      if (temp1 && temp1[1] == '@' && temp1[2] == ']')
        {
  	  if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && quoted_dollar_atp)
	    *quoted_dollar_atp = 1;
	  if (contains_dollar_at)
	    *contains_dollar_at = 1;
	}
    }
#endif

  var_is_set = temp != (char *)0;
  var_is_null = check_nullness && (var_is_set == 0 || *temp == 0);

  /* Get the rest of the stuff inside the braces. */
  if (c && c != '}')
    {
      /* Extract the contents of the ${ ... } expansion
         according to the Posix.2 rules. */
      value = extract_dollar_brace_string (string, &sindex, quoted);
      /*{*/
      if (string[sindex] == '}')
        sindex++;
      else
	goto bad_substitution;
    }
  else
    value = (char *)NULL;

  *indexp = sindex;

  /* If this is a substring spec, process it and add the result. */
  if (want_substring)
    {
      temp1 = parameter_brace_substring (name, temp, value, quoted);
      FREE (name);
      FREE (value);
      FREE (temp);
      return (temp1);
    }
  else if (want_patsub)
    {
      temp1 = parameter_brace_patsub (name, temp, value, quoted);
      FREE (name);
      FREE (value);
      FREE (temp);
      return (temp1);
    }

  /* Do the right thing based on which character ended the variable name. */
  switch (c)
    {
    default:
    case '\0':
    bad_substitution:
      report_error ("%s: bad substitution", string ? string : "??");
      FREE (value);
      FREE (temp);
      free (name);
      return &expand_param_error;

    /*{*/
    case '}':
      if (var_is_set == 0 && unbound_vars_is_error)
        {
	  report_error ("%s: unbound variable", name);
	  FREE (value);
	  FREE (temp);
	  free (name);
	  last_command_exit_value = EXECUTION_FAILURE;
	  return (interactive_shell ? &expand_param_error : &expand_param_fatal);
	}
      break;

    case '#':	/* ${param#[#]pattern} */
    case '%':	/* ${param%[%]pattern} */
      if (value == 0 || *value == '\0' || temp == 0 || *temp == '\0')
        {
          FREE (value);
	  break;
        }
      if ((name[0] == '@' || name[0] == '*') && name[1] == '\0')
	temp1 = parameter_list_remove_pattern (value, name[0], c, quoted);
#if defined (ARRAY_VARS)
      else if (valid_array_reference (name))
	temp1 = array_remove_pattern (value, name, temp, c, quoted);
#endif
      else
	temp1 = parameter_brace_remove_pattern (value, temp, c, quoted);
      free (temp);
      free (value);
      temp = temp1;
      break;

    case '-':
    case '=':
    case '?':
    case '+':
      if (var_is_set && var_is_null == 0)
        {
	  /* We don't want the value of the named variable for
	     anything, just the value of the right hand side. */
	  if (c == '+')
	    {
	      FREE (temp);
	      if (value)
	        {
		  temp = parameter_brace_expand_rhs (name, value, c,
						     quoted,
						     quoted_dollar_atp,
						     contains_dollar_at);
		  free (value);
		}
	      else
	        temp = (char *)NULL;
	    }
	  else
	    {
	      FREE (value);
	    }
	  /* Otherwise do nothing; just use the value in TEMP. */
	}
      else	/* VAR not set or VAR is NULL. */
        {
	  FREE (temp);
	  temp = (char *)NULL;
	  if (c == '=' && var_is_special)
	    {
	      report_error ("$%s: cannot assign in this way", name);
	      free (name);
	      free (value);
	      return &expand_param_error;
	    }
	  else if (c == '?')
	    {
	      parameter_brace_expand_error (name, value);
	      return (interactive ? &expand_param_error : &expand_param_fatal);
	    }
	  else if (c != '+')
	    temp = parameter_brace_expand_rhs (name, value, c, quoted,
					       quoted_dollar_atp,
					       contains_dollar_at);
	  free (value);
	}
      break;
    }
  free (name);
  return (temp);
}

/* Make a word list which is the parameter and variable expansion,
   command substitution, arithmetic substitution, and quote removed
   expansion of WORD.  Return a pointer to a WORD_LIST which is the
   result of the expansion.  If WORD contains a null word, the word
   list returned is also null.

   QUOTED contains flag values defined in shell.h.

   CONTAINS_DOLLAR_AT and EXPANDED_SOMETHING are return values; when non-null
   they point to an integer value which receives information about expansion.
   CONTAINS_DOLLAR_AT gets non-zero if WORD contained "$@", else zero.
   EXPANDED_SOMETHING get non-zero if WORD contained any parameter expansions,
   else zero.

   This only does word splitting in the case of $@ expansion.  In that
   case, we split on ' '. */

/* Values for the local variable quoted_state. */
#define UNQUOTED	 0
#define PARTIALLY_QUOTED 1
#define WHOLLY_QUOTED    2

static WORD_LIST *
expand_word_internal (word, quoted, contains_dollar_at, expanded_something)
     WORD_DESC *word;
     int quoted;
     int *contains_dollar_at;
     int *expanded_something;
{
  WORD_LIST *list;
  WORD_DESC *tword;
  SHELL_VAR *var;

  /* The intermediate string that we build while expanding. */
  char *istring;

  /* The current size of the above object. */
  int istring_size;

  /* Index into ISTRING. */
  int istring_index;

  /* Temporary string storage. */
  char *temp, *temp1;

  /* The text of WORD. */
  register char *string;

  /* The index into STRING. */
  int sindex;

  /* This gets 1 if we see a $@ while quoted. */
  int quoted_dollar_at;

  /* One of UNQUOTED, PARTIALLY_QUOTED, or WHOLLY_QUOTED, depending on
     whether WORD contains no quoting characters, a partially quoted
     string (e.g., "xx"ab), or is fully quoted (e.g., "xxab"). */
  int quoted_state;

  int had_quoted_null;

  int expok;

  register int c;		/* Current character. */
  int number;			/* Temporary number value. */
  int t_index;			/* For calls to string_extract_xxx. */

  istring = xmalloc (istring_size = DEFAULT_INITIAL_ARRAY_SIZE);
  istring[istring_index = 0] = '\0';

  quoted_dollar_at = had_quoted_null = 0;
  quoted_state = UNQUOTED;

  string = word->word;
  if (string == 0)
    goto finished_with_string;

  if (contains_dollar_at)
    *contains_dollar_at = 0;

  /* Begin the expansion. */

  for (sindex = 0; ;)
    {
      c = string[sindex];

      /* Case on toplevel character. */
      switch (c)
	{
	case '\0':
	  goto finished_with_string;

	case CTLESC:
	  temp = xmalloc (3);
	  temp[0] = CTLESC;
	  temp[1] = c = string[++sindex];
	  temp[2] = '\0';

	  if (string[sindex])
	    sindex++;

	  goto add_string;

#if defined (PROCESS_SUBSTITUTION)
	  /* Process substitution. */
	case '<':
	case '>':
	  {
	    if (string[++sindex] != '(' || (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) || posixly_correct)
	      {
		sindex--;
		goto add_character;
	      }
	    else
	      t_index = sindex + 1; /* skip past both '<' and '(' */

	    temp1 = extract_process_subst (string, (c == '<') ? "<(" : ">(", &t_index);
	    sindex = t_index;

	    /* If the process substitution specification is `<()', we want to
	       open the pipe for writing in the child and produce output; if
	       it is `>()', we want to open the pipe for reading in the child
	       and consume input. */
	    temp = temp1 ? process_substitute (temp1, (c == '>')) : (char *)0;

	    FREE (temp1);

	    goto dollar_add_string;
	  }
#endif /* PROCESS_SUBSTITUTION */

	/* See about breaking this into a separate function:
	    char *
	    param_expand (string, sindex, quoted, expanded_something,
			  contains_dollar_at, quoted_dollar_at)
	    char *string;
	    int *sindex, quoted, *expanded_something, *contains_dollar_at;
	    int *quoted_dollar_at;
	*/
	case '$':

	  if (expanded_something)
	    *expanded_something = 1;

	  c = string[++sindex];

	  /* Do simple cases first. Switch on what follows '$'. */
	  switch (c)
	    {
	      /* $0 .. $9? */
	    case '0':
	    case '1':
	    case '2':
	    case '3':
	    case '4':
	    case '5':
	    case '6':
	    case '7':
	    case '8':
	    case '9':
	      temp1 = dollar_vars[digit_value (c)];
	      if (unbound_vars_is_error && temp1 == (char *)NULL)
		{
		  report_error ("$%c: unbound variable", c);
		  free (string);
		  free (istring);
		  last_command_exit_value = EXECUTION_FAILURE;
		  return (interactive_shell ? &expand_word_error : &expand_word_fatal);
		}
	      temp = temp1 ? savestring (temp1) : (char *)NULL;
	      goto dollar_add_string;

	      /* $$ -- pid of the invoking shell. */
	    case '$':
	      number = dollar_dollar_pid;

	    add_number:
	      temp = itos (number);
	    dollar_add_string:
	      if (string[sindex]) sindex++;

	      /* Add TEMP to ISTRING. */
	    add_string:
	      if (temp)
		{
		  istring = sub_append_string
		    (temp, istring, &istring_index, &istring_size);
		  temp = (char *)0;
		}

	      break;

	      /* $# -- number of positional parameters. */
	    case '#':
	      number = number_of_args ();
	      goto add_number;

	      /* $? -- return value of the last synchronous command. */
	    case '?':
	      number = last_command_exit_value;
	      goto add_number;

	      /* $- -- flags supplied to the shell on invocation or
		 by `set'. */
	    case '-':
	      temp = which_set_flags ();
	      goto dollar_add_string;

	      /* $! -- Pid of the last asynchronous command. */
	    case '!':
	      number = (int)last_asynchronous_pid;

	      /* If no asynchronous pids have been created, expand
		 to nothing. */
	      if (number == (int)NO_PID)
		{
		  if (string[sindex])
		    sindex++;
		  if (expanded_something)
		    *expanded_something = 0;
		  break;
		}
	      goto add_number;

	      /* The only difference between this and $@ is when the
		 arg is quoted. */
	    case '*':		/* `$*' */
	      temp = string_rest_of_args (quoted);

	      /* If there are no command-line arguments, this should just
		 disappear if there are other characters in the expansion,
		 even if it's quoted. */
	      if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && temp && *temp == '\0')
		{
		  free (temp);
		  temp = (char *)NULL;
		}
	      /* In the case of a quoted string, quote the entire arg-list.
		 "$1 $2 $3".  Otherwise quote the special escape characters. */
	      if (temp)
		{
		  temp1 = temp;
		  temp = (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
				? quote_string (temp)
				: quote_escapes (temp);
		  free (temp1);
		}
	      goto dollar_add_string;

	      /* When we have "$@" what we want is "$1" "$2" "$3" ... This
		 means that we have to turn quoting off after we split into
		 the individually quoted arguments so that the final split
		 on the first character of $IFS is still done.  */
	    case '@':		/* `$@' */
	      list = list_rest_of_args ();

	      /* We want to flag the fact that we saw this.  We can't turn
		 off quoting entirely, because other characters in the
		 string might need it (consider "\"$@\""), but we need some
		 way to signal that the final split on the first character
		 of $IFS should be done, even though QUOTED is 1. */
	      if (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
		quoted_dollar_at = 1;
	      if (contains_dollar_at)
		*contains_dollar_at = 1;
	      temp = string_list (((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && list) ? quote_list (list) : list);
	      /* If the expansion is not quoted, protect any special escape
		 characters in the expansion by quoting them. */
	      if (temp && quoted == 0)
		{
		  temp1 = temp;
		  temp = quote_escapes (temp);
		  free (temp1);
		}
	      dispose_words (list);
	      goto dollar_add_string;

	    case '{':	/*}*/
	      temp = parameter_brace_expand (string, &sindex, quoted,
					     &quoted_dollar_at,
					     contains_dollar_at);
	      if (temp == &expand_param_error || temp == &expand_param_fatal)
		{
		  free (string);
		  free (istring);
		  return (temp == &expand_param_error) ? &expand_word_error
						       : &expand_word_fatal;
		}
	      /* XXX */
	      /* quoted nulls should be removed if there is anything else
		 in the string. */
	      /* Note that we saw the quoted null so we can add one back at
		 the end of this function if there are no other characters
		 in the string, discard TEMP, and go on. */
	      if (temp && QUOTED_NULL (temp))
		{
		  had_quoted_null = 1;
		  free (temp);
		  break;
		}

	      goto add_string;
	      /* break; */

	      /* Do command or arithmetic substitution. */
	    case '(':	/*)*/
	      /* We have to extract the contents of this paren substitution. */
	      t_index = sindex + 1;
	      temp = extract_command_subst (string, &t_index);
	      sindex = t_index;

	      /* For Posix.2-style `$(( ))' arithmetic substitution,
	         extract the expression and pass it to the evaluator. */
	      if (temp && *temp == '(')
		{
		  temp1 = temp + 1;
		  t_index = strlen (temp1) - 1;

		  if (temp1[t_index] != ')')
		    {
#if 0
		      report_error ("%s: bad arithmetic substitution", temp);
		      free (temp);
		      free (string);
		      free (istring);
		      return (&expand_word_error);
#else
		      goto comsub;
#endif
		    }

		  /* Cut off ending `)' */
		  temp1[t_index] = '\0';

		  /* Expand variables found inside the expression. */
		  temp1 = maybe_expand_string (temp1, Q_DOUBLE_QUOTES, expand_string);

		  /* No error messages. */
		  this_command_name = (char *)NULL;
		  number = evalexp (temp1, &expok);
		  free (temp);
		  free (temp1);
		  if (expok == 0)
		    {
		      free (string);
		      free (istring);
		      return (&expand_word_error);
		    }
		  goto add_number;
		}

	comsub:
	      temp1 = command_substitute (temp, quoted);
	      FREE (temp);
	      temp = temp1;
	      goto dollar_add_string;

	      /* Do straight arithmetic substitution. */
	    case '[':
	      /* We have to extract the contents of this
		 arithmetic substitution. */
	      t_index = sindex + 1;
	      temp = extract_arithmetic_subst (string, &t_index);
	      sindex = t_index;

	       /* Do initial variable expansion. */
	      temp1 = maybe_expand_string (temp, Q_DOUBLE_QUOTES, expand_string);

	      /* No error messages. */
	      this_command_name = (char *)NULL;
	      number = evalexp (temp1, &expok);
	      free (temp1);
	      free (temp);
	      if (expok == 0)
		{
		  free (string);
		  free (istring);
		  return (&expand_word_error);
		}
	      goto add_number;

	    default:
	      /* Find the variable in VARIABLE_LIST. */
	      temp = (char *)NULL;

	      for (t_index = sindex;
		   (c = string[sindex]) && legal_variable_char (c);
		   sindex++);
	      temp1 = substring (string, t_index, sindex);

	      /* If this isn't a variable name, then just output the `$'. */
	      if (temp1 == 0 || *temp1 == '\0')
		{
		  FREE (temp1);
		  temp = xmalloc (2);
		  temp[0] = '$';
		  temp[1] = '\0';
		  if (expanded_something)
		    *expanded_something = 0;
		  goto add_string;
		}

	      /* If the variable exists, return its value cell. */
	      var = find_variable (temp1);

	      if (var && invisible_p (var) == 0 && value_cell (var))
		{
#if defined (ARRAY_VARS)
		  if (array_p (var))
		    {
		      temp = array_reference (array_cell (var), 0);
		      if (temp)
			temp = quote_escapes (temp);
		    }
		  else
#endif
		  temp = quote_escapes (value_cell (var));
		  free (temp1);
		  goto add_string;
		}

	      temp = (char *)NULL;

	      if (unbound_vars_is_error)
		report_error ("%s: unbound variable", temp1);
	      else
		{
		  free (temp1);
		  goto add_string;
		}

	      free (temp1);
	      free (string);
	      last_command_exit_value = EXECUTION_FAILURE;
	      free (istring);
	      return ((unbound_vars_is_error && interactive_shell == 0)
			? &expand_word_fatal
			: &expand_word_error);
	    }
	  break;		/* End case '$': */

	case '`':		/* Backquoted command substitution. */
	  {
	    sindex++;

	    if (expanded_something)
	      *expanded_something = 1;

	    temp = string_extract (string, &sindex, "`", 0);
	    de_backslash (temp);
	    temp1 = command_substitute (temp, quoted);
	    FREE (temp);
	    temp = temp1;
	    goto dollar_add_string;
	  }

	case '\\':
	  if (string[sindex + 1] == '\n')
	    {
	      sindex += 2;
	      continue;
	    }

	  c = string[++sindex];

	  if (quoted & Q_HERE_DOCUMENT)
	    temp1 = slashify_in_here_document;
	  else if (quoted & Q_DOUBLE_QUOTES)
	    temp1 = slashify_in_quotes;
	  else
	    temp1 = "";

	  if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) && member (c, temp1) == 0)
	    {
	      temp = xmalloc (3);
	      temp[0] = '\\'; temp[1] = c; temp[2] = '\0';
	    }
	  else
	    /* This character is quoted, so add it in quoted mode. */
	    temp = make_quoted_char (c);

	  if (c)
	    sindex++;
	  goto add_string;

	case '"':
	  if (quoted & (Q_DOUBLE_QUOTES|Q_HERE_DOCUMENT|Q_NOQUOTE))
	    goto add_character;

	  t_index = ++sindex;
	  temp = string_extract_double_quoted (string, &sindex, 0);

	  /* If the quotes surrounded the entire string, then the
	     whole word was quoted. */
	  quoted_state = (t_index == 1 && string[sindex] == '\0')
			    ? WHOLLY_QUOTED
	  		    : PARTIALLY_QUOTED;

	  if (temp && *temp)
	    {
	      int dollar_at_flag;

	      tword = make_word (temp);		/* XXX */
	      free (temp);
	      temp = (char *)NULL;

	      list = expand_word_internal (tword, Q_DOUBLE_QUOTES, &dollar_at_flag, (int *)NULL);

	      if (list == &expand_word_error || list == &expand_word_fatal)
		{
		  free (istring);
		  free (string);
		  /* expand_word_internal has already freed temp_word->word
		     for us because of the way it prints error messages. */
		  tword->word = (char *)NULL;
		  dispose_word (tword);
		  return list;
		}

	      dispose_word (tword);

	      /* "$@" (a double-quoted dollar-at) expands into nothing,
		 not even a NULL word, when there are no positional
		 parameters. */
	      if (list == 0 && dollar_at_flag)
		{
		  quoted_dollar_at++;
		  break;
		}

	      /* If we get "$@", we know we have expanded something, so we
		 need to remember it for the final split on $IFS.  This is
		 a special case; it's the only case where a quoted string
		 can expand into more than one word.  It's going to come back
		 from the above call to expand_word_internal as a list with
		 a single word, in which all characters are quoted and
		 separated by blanks.  What we want to do is to turn it back
		 into a list for the next piece of code. */
	      if (list)
		dequote_list (list);

	      if (dollar_at_flag)
		{
		  quoted_dollar_at++;
		  if (contains_dollar_at)
		    *contains_dollar_at = 1;
		  if (expanded_something)
		    *expanded_something = 1;
		}
	    }
	  else
	    {
	      /* What we have is "".  This is a minor optimization. */
	      free (temp);
	      list = (WORD_LIST *)NULL;
	    }

	  /* The code above *might* return a list (consider the case of "$@",
	     where it returns "$1", "$2", etc.).  We can't throw away the
	     rest of the list, and we have to make sure each word gets added
	     as quoted.  We test on tresult->next:  if it is non-NULL, we
	     quote the whole list, save it to a string with string_list, and
	     add that string. We don't need to quote the results of this
	     (and it would be wrong, since that would quote the separators
	     as well), so we go directly to add_string. */
	  if (list)
	    {
	      if (list->next)
		{
		  temp = string_list (quote_list (list));
		  dispose_words (list);
		  goto add_string;
		}
	      else
		{
		  temp = savestring (list->word->word);
		  dispose_words (list);
		}
	    }
	  else
	    temp = (char *)NULL;

	  /* We do not want to add quoted nulls to strings that are only
	     partially quoted; we can throw them away. */
	  if (temp == 0 && quoted_state == PARTIALLY_QUOTED)
	    {
	      FREE (temp);
	      continue;
	    }

	add_quoted_string:

	  if (temp)
	    {
	      temp1 = temp;
	      temp = quote_string (temp);
	      free (temp1);
	    }
	  else
	    {
	      /* Add NULL arg. */
	      temp = xmalloc (2);
	      temp[0] = CTLNUL;
	      temp[1] = '\0';
	    }
	  goto add_string;
	  /* break; */

	case '\'':
	  if (quoted & (Q_DOUBLE_QUOTES|Q_HERE_DOCUMENT|Q_NOQUOTE))
	    goto add_character;

	  t_index = ++sindex;
	  temp = string_extract_single_quoted (string, &sindex);

	  /* If the entire STRING was surrounded by single quotes,
	     then the string is wholly quoted. */
	  quoted_state = (t_index == 1 && string[sindex] == '\0')
			    ? WHOLLY_QUOTED
	  		    : PARTIALLY_QUOTED;

	  /* If all we had was '', it is a null expansion. */
	  if (*temp == '\0')
	    {
	      free (temp);
	      temp = (char *)NULL;
	    }
	  else
	    remove_quoted_escapes (temp);

	  /* We do not want to add quoted nulls to strings that are only
	     partially quoted; such nulls are discarded. */
	  if (temp == 0 && (quoted_state == PARTIALLY_QUOTED))
	    continue;

	  goto add_quoted_string;
	  /* break; */

	default:
	  /* This is the fix for " $@ " */
	  if (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
	    {
	      temp = make_quoted_char (c);
	      goto dollar_add_string;
	    }

	add_character:
	  RESIZE_MALLOCED_BUFFER (istring, istring_index, 1, istring_size,
				  DEFAULT_ARRAY_SIZE);
	  istring[istring_index++] = c;
	  istring[istring_index] = '\0';

	  /* Next character. */
	  sindex++;
	}
    }

finished_with_string:
  /* OK, we're ready to return.  If we have a quoted string, and
     quoted_dollar_at is not set, we do no splitting at all; otherwise
     we split on ' '.  The routines that call this will handle what to
     do if nothing has been expanded. */

  /* Partially and wholly quoted strings which expand to the empty
     string are retained as an empty arguments.  Unquoted strings
     which expand to the empty string are discarded.  The single
     exception is the case of expanding "$@" when there are no
     positional parameters.  In that case, we discard the expansion. */

  /* Because of how the code that handles "" and '' in partially
     quoted strings works, we need to make ISTRING into a QUOTED_NULL
     if we saw quoting characters, but the expansion was empty.
     "" and '' are tossed away before we get to this point when
     processing partially quoted strings.  This makes "" and $xxx""
     equivalent when xxx is unset.  We also look to see whether we
     saw a quoted null from a ${} expansion and add one back if we
     need to. */

  /* If we expand to nothing and there were no single or double quotes
     in the word, we throw it away.  Otherwise, we return a NULL word.
     The single exception is for $@ surrounded by double quotes when
     there are no positional parameters.  In that case, we also throw
     the word away. */

  if (*istring == '\0')
    {
      if (quoted_dollar_at == 0 && (had_quoted_null || quoted_state == PARTIALLY_QUOTED))
	{
	  istring[0] = CTLNUL;
	  istring[1] = '\0';
	  tword = make_bare_word (istring);
	  list = make_word_list (tword, (WORD_LIST *)NULL);
	  if (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
	    tword->flags |= W_QUOTED;
	}
      /* According to sh, ksh, and Posix.2, if a word expands into nothing
	 and a double-quoted "$@" appears anywhere in it, then the entire
	 word is removed. */
      else  if (quoted_state == UNQUOTED || quoted_dollar_at)
	list = (WORD_LIST *)NULL;
#if 0
      else
	{
	  tword = make_bare_word (istring);
	  list = make_word_list (tword, (WORD_LIST *)NULL);
	  if (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
	    tword->flags |= W_QUOTED;
	}
#endif
    }
  else if (word->flags & W_NOSPLIT)
    {
      tword = make_bare_word (istring);
      list = make_word_list (tword, (WORD_LIST *)NULL);
      if (word->flags & W_ASSIGNMENT)
	tword->flags |= W_ASSIGNMENT;	/* XXX */
      if (quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES))
        tword->flags |= W_QUOTED;
    }
  else
    {
      char *ifs_chars;

      if (quoted_dollar_at)
	{
	  var = find_variable ("IFS");
	  ifs_chars = var ? value_cell (var) : " \t\n";
	}
      else
	ifs_chars = (char *)NULL;

      /* According to Posix.2, "$@" expands to a single word if
	 IFS="" and the positional parameters are not empty. */
      if (quoted_dollar_at && ifs_chars && *ifs_chars)
	{
	  list = list_string (istring, " ", 1);
	}
      else
	{
	  tword = make_bare_word (istring);
	  list = make_word_list (tword, (WORD_LIST *)NULL);
	  if ((quoted & (Q_DOUBLE_QUOTES|Q_HERE_DOCUMENT)) || (quoted_state == WHOLLY_QUOTED))
	    tword->flags |= W_QUOTED;
	  if (word->flags & W_ASSIGNMENT)
	    tword->flags |= W_ASSIGNMENT;
	}
    }

  free (istring);
  return (list);
}

/* **************************************************************** */
/*								    */
/*		   Functions for Quote Removal			    */
/*								    */
/* **************************************************************** */

/* Perform quote removal on STRING.  If QUOTED > 0, assume we are obeying the
   backslash quoting rules for within double quotes. */
char *
string_quote_removal (string, quoted)
     char *string;
     int quoted;
{
  char *r, *result_string, *temp;
  int sindex, tindex, c, dquote;

  /* The result can be no longer than the original string. */
  r = result_string = xmalloc (strlen (string) + 1);

  for (dquote = sindex = 0; c = string[sindex];)
    {
      switch (c)
	{
	case '\\':
	  c = string[++sindex];
	  if (((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) || dquote) && member (c, slashify_in_quotes) == 0)
	    *r++ = '\\';
	  /* FALLTHROUGH */

	default:
	  *r++ = c;
	  sindex++;
	  break;

	case '\'':
	  if ((quoted & (Q_HERE_DOCUMENT|Q_DOUBLE_QUOTES)) || dquote)
	    {
	      *r++ = c;
	      sindex++;
	      break;
	    }
	  tindex = sindex + 1;
	  temp = string_extract_single_quoted (string, &tindex);
	  if (temp)
	    {
	      strcpy (r, temp);
	      r += strlen (r);
	      free (temp);
	    }
	  sindex = tindex;
	  break;

	case '"':
	  dquote = 1 - dquote;
	  sindex++;
	  break;
	}
    }
    *r = '\0';
    return (result_string);
}

#if 0
/* UNUSED */
/* Perform quote removal on word WORD.  This allocates and returns a new
   WORD_DESC *. */
WORD_DESC *
word_quote_removal (word, quoted)
     WORD_DESC *word;
     int quoted;
{
  WORD_DESC *w;
  char *t;

  t = string_quote_removal (word->word, quoted);
  w = make_bare_word (t);
  free (t);
  return (w);
}

/* Perform quote removal on all words in LIST.  If QUOTED is non-zero,
   the members of the list are treated as if they are surrounded by
   double quotes.  Return a new list, or NULL if LIST is NULL. */
WORD_LIST *
word_list_quote_removal (list, quoted)
     WORD_LIST *list;
     int quoted;
{
  WORD_LIST *result, *t, *tresult;

  for (t = list, result = (WORD_LIST *)NULL; t; t = t->next)
    {
      tresult = (WORD_LIST *)xmalloc (sizeof (WORD_LIST));
      tresult->word = word_quote_removal (t->word, quoted);
      tresult->next = (WORD_LIST *)NULL;
      result = (WORD_LIST *) list_append (result, tresult);
    }
  return (result);
}
#endif

/* Return 1 if CHARACTER appears in an unquoted portion of
   STRING.  Return 0 otherwise. */
static int
unquoted_member (character, string)
     int character;
     char *string;
{
  int sindex, c;

  for (sindex = 0; c = string[sindex]; )
    {
      if (c == character)
	return (1);

      switch (c)
	{
	default:
	  sindex++;
	  break;

	case '\\':
	  sindex++;
	  if (string[sindex])
	    sindex++;
	  break;

	case '\'':
	  sindex = skip_single_quoted (string, ++sindex);
	  break;

	case '"':
	  sindex = skip_double_quoted (string, ++sindex);
	  break;
	}
    }
  return (0);
}

/* Return 1 if SUBSTR appears in an unquoted portion of STRING. */
static int
unquoted_substring (substr, string)
     char *substr, *string;
{
  int sindex, c, sublen;

  if (substr == 0 || *substr == '\0')
    return (0);

  sublen = strlen (substr);
  for (sindex = 0; c = string[sindex]; )
    {
      if (STREQN (string + sindex, substr, sublen))
	return (1);

      switch (c)
	{
	case '\\':
	  sindex++;

	  if (string[sindex])
	    sindex++;
	  break;

	case '\'':
	  sindex = skip_single_quoted (string, ++sindex);
	  break;

	case '"':
	  sindex = skip_double_quoted (string, ++sindex);
	  break;

	default:
	  sindex++;
	  break;
	}
    }
  return (0);
}

/*******************************************
 *					   *
 *    Functions to perform word splitting  *
 *					   *
 *******************************************/

/* This splits a single word into a WORD LIST on $IFS, but only if the word
   is not quoted.  list_string () performs quote removal for us, even if we
   don't do any splitting. */
WORD_LIST *
word_split (w)
     WORD_DESC *w;
{
  WORD_LIST *result;
  SHELL_VAR *ifs;
  char *ifs_chars;

  if (w)
    {
      ifs = find_variable ("IFS");
      /* If IFS is unset, it defaults to " \t\n". */
      ifs_chars = ifs ? value_cell (ifs) : " \t\n";

      if ((w->flags & W_QUOTED) || !ifs_chars)
	ifs_chars = "";

      result = list_string (w->word, ifs_chars, w->flags & W_QUOTED);
    }
  else
    result = (WORD_LIST *)NULL;

  return (result);
}

/* Perform word splitting on LIST and return the RESULT.  It is possible
   to return (WORD_LIST *)NULL. */
static WORD_LIST *
word_list_split (list)
     WORD_LIST *list;
{
  WORD_LIST *result, *t, *tresult;

  for (t = list, result = (WORD_LIST *)NULL; t; t = t->next)
    {
      tresult = word_split (t->word);
      result = (WORD_LIST *) list_append (result, tresult);
    }
  return (result);
}

/**************************************************
 * 						  *
 *	Functions to expand an entire WORD_LIST	  *
 *						  *
 **************************************************/

static WORD_LIST *varlist = (WORD_LIST *)NULL;

/* Separate out any initial variable assignments from TLIST.  If set -k has
   been executed, remove all assignment statements from TLIST.  Initial
   variable assignments and other environment assignments are placed
   on VARLIST. */
static WORD_LIST *
separate_out_assignments (tlist)
     WORD_LIST *tlist;
{
  register WORD_LIST *vp, *lp;

  if (!tlist)
    return ((WORD_LIST *)NULL);

  varlist = (WORD_LIST *)NULL;
  vp = lp = tlist;

  /* Separate out variable assignments at the start of the command.
     Loop invariant: vp->next == lp
     Loop postcondition:
     	lp = list of words left after assignment statements skipped
     	tlist = original list of words
  */
  while (lp && (lp->word->flags & W_ASSIGNMENT))
    {
      vp = lp;
      lp = lp->next;
    }

  /* If lp != tlist, we have some initial assignment statements. */
  /* We make VARLIST point to the list of assignment words and
     TLIST point to the remaining words.  */
  if (lp != tlist)
    {
      varlist = tlist;
      /* ASSERT(vp->next == lp); */
      vp->next = (WORD_LIST *)NULL;	/* terminate variable list */
      tlist = lp;			/* remainder of word list */
    }

  /* vp == end of variable list */
  /* tlist == remainder of original word list without variable assignments */
  if (!tlist)
    /* All the words in tlist were assignment statements */
    return ((WORD_LIST *)NULL);

  /* ASSERT(tlist != NULL); */
  /* ASSERT((tlist->word->flags & W_ASSIGNMENT) == 0); */

  /* If the -k option is in effect, we need to go through the remaining
     words, separate out the assignment words, and place them on VARLIST. */
  if (place_keywords_in_env)
    {
      WORD_LIST *tp;	/* tp == running pointer into tlist */

      tp = tlist;
      lp = tlist->next;

      /* Loop Invariant: tp->next == lp */
      /* Loop postcondition: tlist == word list without assignment statements */
      while (lp)
	{
	  if (lp->word->flags & W_ASSIGNMENT)
	    {
	      /* Found an assignment statement, add this word to end of
		 varlist (vp). */
	      if (!varlist)
		varlist = vp = lp;
	      else
		{
		  vp->next = lp;
		  vp = lp;
		}

	      /* Remove the word pointed to by LP from TLIST. */
	      tp->next = lp->next;
	      /* ASSERT(vp == lp); */
	      lp->next = (WORD_LIST *)NULL;
	      lp = tp->next;
	    }
	  else
	    {
	      tp = lp;
	      lp = lp->next;
	    }
	}
    }
  return (tlist);
}

/* Take the list of words in LIST and do the various substitutions.  Return
   a new list of words which is the expanded list, and without things like
   variable assignments. */

WORD_LIST *
expand_words (list)
     WORD_LIST *list;
{
  return (expand_word_list_internal (list, 1));
}

/* Same as expand_words (), but doesn't hack variable or environment
   variables. */
WORD_LIST *
expand_words_no_vars (list)
     WORD_LIST *list;
{
  return (expand_word_list_internal (list, 0));
}

/* The workhorse for expand_words () and expand_words_no_vars ().
   First arg is LIST, a WORD_LIST of words.
   Second arg DO_VARS is non-zero if you want to do environment and
   variable assignments, else zero.

   This does all of the substitutions: brace expansion, tilde expansion,
   parameter expansion, command substitution, arithmetic expansion,
   process substitution, word splitting, and pathname expansion.  Words
   with the W_QUOTED or W_NOSPLIT bits set, or for which no expansion
   is done, do not undergo word splitting.  Words with the W_ASSIGNMENT
   bit set do not undergo pathname expansion. */
static WORD_LIST *
expand_word_list_internal (list, do_vars)
     WORD_LIST *list;
     int do_vars;
{
  WORD_LIST *tlist, *new_list, *next, *temp_list, *orig_list, *disposables;
  char *temp_string;
  int tint;

  if (list == 0)
    return ((WORD_LIST *)NULL);

  tlist = copy_word_list (list);

  if (do_vars)
    {
      tlist = separate_out_assignments (tlist);
      if (tlist == 0)
	{
	  if (varlist)
	    {
	      /* All the words were variable assignments, so they are placed
		 into the shell's environment. */
	      for (new_list = varlist; new_list; new_list = new_list->next)
		{
		  this_command_name = (char *)NULL;	/* no arithmetic errors */
		  tint = do_assignment (new_list->word->word);
		  /* Variable assignment errors in non-interactive shells
		     running in Posix.2 mode cause the shell to exit. */
		  if (tint == 0 && interactive_shell == 0 && posixly_correct)
		    {
		      last_command_exit_value = EXECUTION_FAILURE;
		      jump_to_top_level (FORCE_EOF);
		    }
		}
	      dispose_words (varlist);
	      varlist = (WORD_LIST *)NULL;
	    }
	  return ((WORD_LIST *)NULL);
	}
    }

  /* Begin expanding the words that remain.  The expansions take place on
     things that aren't really variable assignments. */

#if defined (BRACE_EXPANSION)
  /* Do brace expansion on this word if there are any brace characters
     in the string. */
  if (brace_expansion && tlist)
    {
      register char **expansions;
      WORD_LIST *braces;
      WORD_DESC *w;
      int eindex;

      for (braces = disposables = (WORD_LIST *)NULL; tlist; tlist = next)
	{
	  next = tlist->next;

	  /* Only do brace expansion if the word has a brace character.  If
	     not, just add the word list element to BRACES and continue.  In
	     the common case, at least when running shell scripts, this will
	     degenerate to a bunch of calls to `strchr', and then what is
	     basically a reversal of TLIST into BRACES, which is corrected
	     by a call to reverse_list () on BRACES when the end of TLIST
	     is reached. */
	  if (strchr (tlist->word->word, '{'))
	    {
	      expansions = brace_expand (tlist->word->word);

	      for (eindex = 0; temp_string = expansions[eindex]; eindex++)
		{
		  w = make_word (temp_string);
		  /* If brace expansion didn't change the word, preserve
		     the flags.  We may want to preserve the flags
		     unconditionally someday -- XXX */
		  if (STREQ (temp_string, tlist->word->word))
		    w->flags = tlist->word->flags;
		  braces = make_word_list (w, braces);
		  free (expansions[eindex]);
		}
	      free (expansions);

	      /* Add TLIST to the list of words to be freed after brace
		 expansion has been performed. */
	      tlist->next = disposables;
	      disposables = tlist;
	    }
	  else
	    {
	      tlist->next = braces;
	      braces = tlist;
	    }
	}

      dispose_words (disposables);
      tlist = REVERSE_LIST (braces, WORD_LIST *);
    }
#endif /* BRACE_EXPANSION */

  /* We do tilde expansion all the time.  This is what 1003.2 says. */
  for (orig_list = tlist, new_list = (WORD_LIST *)NULL; tlist; tlist = next)
    {
      WORD_LIST *expanded;
      int expanded_something, has_dollar_at;

      temp_string = tlist->word->word;

      next = tlist->next;

      /* Posix.2 section 3.6.1 says that tildes following `=' in words
	 which are not assignment statements are not expanded.  We do
	 this only if POSIXLY_CORRECT is enabled.  Essentially, we do
	 tilde expansion on unquoted assignment statements (flags include
	 W_ASSIGNMENT but not W_QUOTED). */
      if (temp_string[0] == '~' ||
	    (((tlist->word->flags & (W_ASSIGNMENT|W_QUOTED)) == W_ASSIGNMENT) &&
	     posixly_correct == 0 &&
	     strchr (temp_string, '~') &&
	     (unquoted_substring ("=~", temp_string) || unquoted_substring (":~", temp_string))))
	{
	  tlist->word->word = bash_tilde_expand (temp_string);
	  free (temp_string);
	}

      expanded_something = 0;
      expanded = expand_word_internal
	(tlist->word, 0, &has_dollar_at, &expanded_something);

      if (expanded == &expand_word_error || expanded == &expand_word_fatal)
	{
	  /* By convention, each time this error is returned,
	     tlist->word->word has already been freed. */
	  tlist->word->word = (char *)NULL;

	  /* Dispose our copy of the original list. */
	  dispose_words (orig_list);
	  /* Dispose the new list we're building. */
	  dispose_words (new_list);

	  if (expanded == &expand_word_error)
	    jump_to_top_level (DISCARD);
	  else
	    jump_to_top_level (FORCE_EOF);
	}

      /* Don't split words marked W_NOSPLIT. */
      if (expanded_something && (tlist->word->flags & W_NOSPLIT) == 0)
	{
	  temp_list = word_list_split (expanded);
	  dispose_words (expanded);
	}
      else
	{
	  /* If no parameter expansion, command substitution, process
	     substitution, or arithmetic substitution took place, then
	     do not do word splitting.  We still have to remove quoted
	     null characters from the result. */
	  word_list_remove_quoted_nulls (expanded);
	  temp_list = expanded;
	}

      /* In the most common cases, t will be a list containing only one
	 element, so the call to reverse_list would be wasted. */
      expanded = REVERSE_LIST (temp_list, WORD_LIST *);
      new_list = (WORD_LIST *)list_append (expanded, new_list);
    }

  new_list = REVERSE_LIST (new_list, WORD_LIST *);

  dispose_words (orig_list);

  /* Okay, we're almost done.  Now let's just do some filename
     globbing. */
  if (new_list)
    {
      char **glob_array;
      register int glob_index;
      WORD_LIST *glob_list;
      WORD_DESC *tword;

      orig_list = disposables = (WORD_LIST *)NULL;
      tlist = new_list;

      /* orig_list == output list, despite the name. */
      if (disallow_filename_globbing == 0)
	{
	  glob_array = (char **)NULL;
	  while (tlist)
	    {
	      /* For each word, either globbing is attempted or the word is
		 added to orig_list.  If globbing succeeds, the results are
		 added to orig_list and the word (tlist) is added to the list
		 of disposable words.  If globbing fails and failed glob
		 expansions are left unchanged (the shell default), the
		 original word is added to orig_list.  If globbing fails and
		 failed glob expansions are removed, the original word is
		 added to the list of disposable words.  orig_list ends up
		 in reverse order and requires a call to reverse_list to
		 be set right.  After all words are examined, the disposable
		 words are freed. */
	      next = tlist->next;

	      /* If the word isn't an assignment and contains an unquoted
	         pattern matching character, then glob it. */
#if 0
	      if ((tlist->word->flags & (W_QUOTED|W_ASSIGNMENT)) == 0 &&
#else
	      if ((tlist->word->flags & W_ASSIGNMENT) == 0 &&
#endif
		  unquoted_glob_pattern_p (tlist->word->word))
		{
		  glob_array = shell_glob_filename (tlist->word->word);

		  /* Handle error cases.
		     I don't think we should report errors like "No such file
		     or directory".  However, I would like to report errors
		     like "Read failed". */

		  if (GLOB_FAILED (glob_array))
		    {
		      glob_array = (char **) xmalloc (sizeof (char *));
		      glob_array[0] = (char *)NULL;
		    }

		  /* Dequote the current word in case we have to use it. */
		  if (glob_array[0] == NULL)
		    {
		      temp_string = dequote_string (tlist->word->word);
		      free (tlist->word->word);
		      tlist->word->word = temp_string;
		    }

		  /* Make the array into a word list. */
		  glob_list = (WORD_LIST *)NULL;
		  for (glob_index = 0; glob_array[glob_index]; glob_index++)
		    {
		      tword = make_bare_word (glob_array[glob_index]);
		      tword->flags |= W_GLOBEXP;	/* XXX */
		      glob_list = make_word_list (tword, glob_list);
		    }

		  if (glob_list)
		    {
		      orig_list = (WORD_LIST *)list_append (glob_list, orig_list);
		      tlist->next = disposables;
		      disposables = tlist;
		    }
		  else if (allow_null_glob_expansion == 0)
		    {
		      /* Failed glob expressions are left unchanged. */
		      tlist->next = orig_list;
		      orig_list = tlist;
		    }
		  else
		    {
		      /* Failed glob expressions are removed. */
		      tlist->next = disposables;
		      disposables = tlist;
		    }
		}
	      else
		{
		  /* Dequote the string. */
		  temp_string = dequote_string (tlist->word->word);
		  free (tlist->word->word);
		  tlist->word->word = temp_string;
		  tlist->next = orig_list;
		  orig_list = tlist;
		}

	      free_array (glob_array);
	      glob_array = (char **)NULL;

	      tlist = next;
	    }

	  if (disposables)
	    dispose_words (disposables);

	  new_list = REVERSE_LIST (orig_list, WORD_LIST *);
	}
      else
	{
	  /* Dequote the words, because we're not performing globbing. */
	  for (temp_list = new_list; temp_list; temp_list = temp_list->next)
	    {
	      temp_string = dequote_string (temp_list->word->word);
	      free (temp_list->word->word);
	      temp_list->word->word = temp_string;
	    }
	}
    }

  if (do_vars)
    {
      Function *assign_func;

      /* If the remainder of the words expand to nothing, Posix.2 requires
	 that the variable and environment assignments affect the shell's
	 environment. */
      assign_func = new_list ? assign_in_env : do_assignment;

      for (temp_list = varlist; temp_list; temp_list = temp_list->next)
	{
	  this_command_name = (char *)NULL;
	  tint = (*assign_func) (temp_list->word->word);
	  /* Variable assignment errors in non-interactive shells running
	     in Posix.2 mode cause the shell to exit. */
	  if (tint == 0 && assign_func == do_assignment &&
		interactive_shell == 0 && posixly_correct)
	    {
	      last_command_exit_value = EXECUTION_FAILURE;
	      jump_to_top_level (FORCE_EOF);
	    }
	}

      dispose_words (varlist);
      varlist = (WORD_LIST *)NULL;
    }

  tint = list_length (new_list) + 1;
  RESIZE_MALLOCED_BUFFER (glob_argv_flags, 0, tint, glob_argv_flags_size, 16);
  for (tint = 0, tlist = new_list; tlist; tlist = tlist->next)
    glob_argv_flags[tint++] = (tlist->word->flags & W_GLOBEXP) ? '1' : '0';
  glob_argv_flags[tint] = '\0';

  return (new_list);
}

/*************************************************
 *						 *
 *	Functions to manage special variables	 *
 *						 *
 *************************************************/

/* An alist of name.function for each special variable.  Most of the
   functions don't do much, and in fact, this would be faster with a
   switch statement, but by the end of this file, I am sick of switch
   statements. */

#define SET_INT_VAR(name, intvar)  intvar = find_variable (name) != 0

struct name_and_function {
  char *name;
  VFunction *function;
} special_vars[] = {
  { "PATH", sv_path },
  { "MAIL", sv_mail },
  { "MAILPATH", sv_mail },
  { "MAILCHECK", sv_mail },

  { "POSIXLY_CORRECT", sv_strict_posix },
  { "GLOBIGNORE", sv_globignore },

  /* Variables which only do something special when READLINE is defined. */
#if defined (READLINE)
  { "TERM", sv_terminal },
  { "TERMCAP", sv_terminal },
  { "TERMINFO", sv_terminal },
  { "HOSTFILE", sv_hostfile },
#endif /* READLINE */

  /* Variables which only do something special when HISTORY is defined. */
#if defined (HISTORY)
  { "HISTIGNORE", sv_histignore },
  { "HISTSIZE", sv_histsize },
  { "HISTFILESIZE", sv_histsize },
  { "HISTCONTROL", sv_history_control },
#  if defined (BANG_HISTORY)
  { "histchars", sv_histchars },
#  endif /* BANG_HISTORY */
#endif /* HISTORY */

  { "IGNOREEOF", sv_ignoreeof },
  { "ignoreeof", sv_ignoreeof },

  { "OPTIND", sv_optind },
  { "OPTERR", sv_opterr },

  { "TEXTDOMAIN", sv_locale },
  { "TEXTDOMAINDIR", sv_locale },
  { "LC_ALL", sv_locale },
  { "LC_COLLATE", sv_locale },
  { "LC_CTYPE", sv_locale },
  { "LC_MESSAGES", sv_locale },
  { "LANG", sv_locale },

#if defined (HAVE_TZSET) && defined (PROMPT_STRING_DECODE)
  { "TZ", sv_tz },
#endif

  { (char *)0, (VFunction *)0 }
};

/* The variable in NAME has just had its state changed.  Check to see if it
   is one of the special ones where something special happens. */
void
stupidly_hack_special_variables (name)
     char *name;
{
  int i;

  for (i = 0; special_vars[i].name; i++)
    {
      if (STREQ (special_vars[i].name, name))
	{
	  (*(special_vars[i].function)) (name);
	  return;
	}
    }
}

/* What to do just after the PATH variable has changed. */
void
sv_path (name)
     char *name;
{
  /* hash -r */
  flush_hashed_filenames ();
}

/* What to do just after one of the MAILxxxx variables has changed.  NAME
   is the name of the variable.  This is called with NAME set to one of
   MAIL, MAILCHECK, or MAILPATH.  */
void
sv_mail (name)
     char *name;
{
  /* If the time interval for checking the files has changed, then
     reset the mail timer.  Otherwise, one of the pathname vars
     to the users mailbox has changed, so rebuild the array of
     filenames. */
  if (name[4] == 'C')  /* if (strcmp (name, "MAILCHECK") == 0) */
    reset_mail_timer ();
  else
    {
      free_mail_files ();
      remember_mail_dates ();
    }
}

/* What to do when GLOBIGNORE changes. */
void
sv_globignore (name)
     char *name;
{
  setup_glob_ignore (name);
}

#if defined (READLINE)
/* What to do just after one of the TERMxxx variables has changed.
   If we are an interactive shell, then try to reset the terminal
   information in readline. */
void
sv_terminal (name)
     char *name;
{
  if (interactive_shell && no_line_editing == 0)
    rl_reset_terminal (get_string_value ("TERM"));
}

void
sv_hostfile (name)
     char *name;
{
  hostname_list_initialized = 0;
}
#endif /* READLINE */

#if defined (HISTORY)
/* What to do after the HISTSIZE or HISTFILESIZE variables change.
   If there is a value for this HISTSIZE (and it is numeric), then stifle
   the history.  Otherwise, if there is NO value for this variable,
   unstifle the history.  If name is HISTFILESIZE, and its value is
   numeric, truncate the history file to hold no more than that many
   lines. */
void
sv_histsize (name)
     char *name;
{
  char *temp;
  long num;

  temp = get_string_value (name);

  if (temp && *temp)
    {
      if (legal_number (temp, &num))
        {
	  if (name[4] == 'S')
	    {
	      stifle_history (num);
	      num = where_history ();
	      if (history_lines_this_session > num)
		history_lines_this_session = num;
	    }
	  else
	    {
	      history_truncate_file (get_string_value ("HISTFILE"), (int)num);
	      if (num <= history_lines_in_file)
		history_lines_in_file = num;
	    }
	}
    }
  else if (name[4] == 'S')
    unstifle_history ();
}

/* What to do after the HISTIGNORE variable changes. */
void
sv_histignore (name)
     char *name;
{
  setup_history_ignore (name);
}

/* What to do after the HISTCONTROL variable changes. */
void
sv_history_control (name)
     char *name;
{
  char *temp;

  history_control = 0;
  temp = get_string_value (name);

  if (temp && *temp && STREQN (temp, "ignore", 6))
    {
      if (temp[6] == 's')	/* ignorespace */
	history_control = 1;
      else if (temp[6] == 'd')	/* ignoredups */
	history_control = 2;
      else if (temp[6] == 'b')	/* ignoreboth */
	history_control = 3;
    }
}

#if defined (BANG_HISTORY)
/* Setting/unsetting of the history expansion character. */
void
sv_histchars (name)
     char *name;
{
  char *temp;

  temp = get_string_value (name);
  if (temp)
    {
      history_expansion_char = *temp;
      if (temp[0] && temp[1])
	{
	  history_subst_char = temp[1];
	  if (temp[2])
	      history_comment_char = temp[2];
	}
    }
  else
    {
      history_expansion_char = '!';
      history_subst_char = '^';
      history_comment_char = '#';
    }
}
#endif /* BANG_HISTORY */
#endif /* HISTORY */

#if defined (HAVE_TZSET) && defined (PROMPT_STRING_DECODE)
void
sv_tz (name)
     char *name;
{
  tzset ();
}
#endif

/* If the variable exists, then the value of it can be the number
   of times we actually ignore the EOF.  The default is small,
   (smaller than csh, anyway). */
void
sv_ignoreeof (name)
     char *name;
{
  SHELL_VAR *tmp_var;
  char *temp;

  eof_encountered = 0;

  tmp_var = find_variable (name);
  ignoreeof = tmp_var != 0;
  temp = tmp_var ? value_cell (tmp_var) : (char *)NULL;
  if (temp)
    eof_encountered_limit = (*temp && all_digits (temp)) ? atoi (temp) : 10;
  set_shellopts ();	/* make sure `ignoreeof' is/is not in $SHELLOPTS */
}

void
sv_optind (name)
     char *name;
{
  char *tt;
  int s;

  tt = get_string_value ("OPTIND");
  if (tt && *tt)
    {
      s = atoi (tt);

      /* According to POSIX, setting OPTIND=1 resets the internal state
	 of getopt (). */
      if (s < 0 || s == 1)
	s = 0;
    }
  else
    s = 0;
  getopts_reset (s);
}

void
sv_opterr (name)
     char *name;
{
  char *tt;

  tt = get_string_value ("OPTERR");
  sh_opterr = (tt && *tt) ? atoi (tt) : 1;
}

void
sv_strict_posix (name)
     char *name;
{
  SET_INT_VAR (name, posixly_correct);
  posix_initialize (posixly_correct);
#if defined (READLINE)
  if (interactive_shell)
    posix_readline_initialize (posixly_correct);
#endif /* READLINE */
  set_shellopts ();	/* make sure `posix' is/is not in $SHELLOPTS */
}

void
sv_locale (name)
     char *name;
{
  char *v;

  v = get_string_value (name);
  if (name[0] == 'L' && name[1] == 'A')	/* LANG */
    set_lang (name, v);
  else
    set_locale_var (name, v);		/* LC_*, TEXTDOMAIN* */
}
