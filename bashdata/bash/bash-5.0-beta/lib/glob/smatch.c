/* strmatch.c -- ksh-like extended pattern matching for the shell and filename
		globbing. */

/* Copyright (C) 1991-2017 Free Software Foundation, Inc.

   This file is part of GNU Bash, the Bourne Again SHell.
   
   Bash is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Bash is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Bash.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#include <stdio.h>	/* for debugging */
				
#include "strmatch.h"
#include <chartypes.h>

#include "bashansi.h"
#include "shmbutil.h"
#include "xmalloc.h"

/* First, compile `sm_loop.c' for single-byte characters. */
#define CHAR	unsigned char
#define U_CHAR	unsigned char
#define XCHAR	char
#define INT	int
#define L(CS)	CS
#define INVALID	-1

#undef STREQ
#undef STREQN
#define STREQ(a, b) ((a)[0] == (b)[0] && strcmp(a, b) == 0)
#define STREQN(a, b, n) ((a)[0] == (b)[0] && strncmp(a, b, n) == 0)

#ifndef GLOBASCII_DEFAULT
#  define GLOBASCII_DEFAULT 0
#endif

int glob_asciirange = GLOBASCII_DEFAULT;

/* We use strcoll(3) for range comparisons in bracket expressions,
   even though it can have unwanted side effects in locales
   other than POSIX or US.  For instance, in the de locale, [A-Z] matches
   all characters.  If GLOB_ASCIIRANGE is non-zero, and we're not forcing
   the use of strcoll (e.g., for explicit collating symbols), we use
   straight ordering as if in the C locale. */

#if defined (HAVE_STRCOLL)
/* Helper function for collating symbol equivalence. */
static int
rangecmp (c1, c2, forcecoll)
     int c1, c2;
     int forcecoll;
{
  static char s1[2] = { ' ', '\0' };
  static char s2[2] = { ' ', '\0' };
  int ret;

  /* Eight bits only.  Period. */
  c1 &= 0xFF;
  c2 &= 0xFF;

  if (c1 == c2)
    return (0);

  if (forcecoll == 0 && glob_asciirange)
    return (c1 - c2);

  s1[0] = c1;
  s2[0] = c2;

  if ((ret = strcoll (s1, s2)) != 0)
    return ret;
  return (c1 - c2);
}
#else /* !HAVE_STRCOLL */
#  define rangecmp(c1, c2, f)	((int)(c1) - (int)(c2))
#endif /* !HAVE_STRCOLL */

#if defined (HAVE_STRCOLL)
static int
collequiv (c1, c2)
     int c1, c2;
{
  return (rangecmp (c1, c2, 1) == 0);
}
#else
#  define collequiv(c1, c2)	((c1) == (c2))
#endif

#define _COLLSYM	_collsym
#define __COLLSYM	__collsym
#define POSIXCOLL	posix_collsyms
#include "collsyms.h"

static int
collsym (s, len)
     CHAR *s;
     int len;
{
  register struct _collsym *csp;
  char *x;

  x = (char *)s;
  for (csp = posix_collsyms; csp->name; csp++)
    {
      if (STREQN(csp->name, x, len) && csp->name[len] == '\0')
	return (csp->code);
    }
  if (len == 1)
    return s[0];
  return INVALID;
}

/* unibyte character classification */
#if !defined (isascii) && !defined (HAVE_ISASCII)
#  define isascii(c)	((unsigned int)(c) <= 0177)
#endif

enum char_class
  {
    CC_NO_CLASS = 0,
    CC_ASCII, CC_ALNUM, CC_ALPHA, CC_BLANK, CC_CNTRL, CC_DIGIT, CC_GRAPH,
    CC_LOWER, CC_PRINT, CC_PUNCT, CC_SPACE, CC_UPPER, CC_WORD, CC_XDIGIT
  };

static char const *const cclass_name[] =
  {
    "",
    "ascii", "alnum", "alpha", "blank", "cntrl", "digit", "graph",
    "lower", "print", "punct", "space", "upper", "word", "xdigit"
  };

#define N_CHAR_CLASS (sizeof(cclass_name) / sizeof (cclass_name[0]))

static enum char_class
is_valid_cclass (name)
     const char *name;
{
  enum char_class ret;
  int i;

  ret = CC_NO_CLASS;

  for (i = 1; i < N_CHAR_CLASS; i++)
    {
      if (STREQ (name, cclass_name[i]))
	{
	  ret = (enum char_class)i;
	  break;
	}
    }

  return ret;
}

static int
cclass_test (c, char_class)
     int c;
     enum char_class char_class;
{
  int result;

  switch (char_class)
    {
      case CC_ASCII:
	result = isascii (c);
	break;
      case CC_ALNUM:
	result = ISALNUM (c);
	break;
      case CC_ALPHA:
	result = ISALPHA (c);
	break;
      case CC_BLANK:  
	result = ISBLANK (c);
	break;
      case CC_CNTRL:
	result = ISCNTRL (c);
	break;
      case CC_DIGIT:
	result = ISDIGIT (c);
	break;
      case CC_GRAPH:
	result = ISGRAPH (c);
	break;
      case CC_LOWER:
	result = ISLOWER (c);
	break;
      case CC_PRINT: 
	result = ISPRINT (c);
	break;
      case CC_PUNCT:
	result = ISPUNCT (c);
	break;
      case CC_SPACE:
	result = ISSPACE (c);
	break;
      case CC_UPPER:
	result = ISUPPER (c);
	break;
      case CC_WORD:
        result = (ISALNUM (c) || c == '_');
	break;
      case CC_XDIGIT:
	result = ISXDIGIT (c);
	break;
      default:
	result = -1;
	break;
    }

  return result;  
}
	
static int
is_cclass (c, name)
     int c;
     const char *name;
{
  enum char_class char_class;
  int result;

  char_class = is_valid_cclass (name);
  if (char_class == CC_NO_CLASS)
    return -1;

  result = cclass_test (c, char_class);
  return (result);
}

/* Now include `sm_loop.c' for single-byte characters. */
/* The result of FOLD is an `unsigned char' */
# define FOLD(c) ((flags & FNM_CASEFOLD) \
	? TOLOWER ((unsigned char)c) \
	: ((unsigned char)c))

#define FCT			internal_strmatch
#define GMATCH			gmatch
#define COLLSYM			collsym
#define PARSE_COLLSYM		parse_collsym
#define BRACKMATCH		brackmatch
#define PATSCAN			glob_patscan
#define STRCOMPARE		strcompare
#define EXTMATCH		extmatch
#define STRUCT			smat_struct
#define STRCHR(S, C)		strchr((S), (C))
#define MEMCHR(S, C, N)		memchr((S), (C), (N))
#define STRCOLL(S1, S2)		strcoll((S1), (S2))
#define STRLEN(S)		strlen(S)
#define STRCMP(S1, S2)		strcmp((S1), (S2))
#define RANGECMP(C1, C2, F)	rangecmp((C1), (C2), (F))
#define COLLEQUIV(C1, C2)	collequiv((C1), (C2))
#define CTYPE_T			enum char_class
#define IS_CCLASS(C, S)		is_cclass((C), (S))
#include "sm_loop.c"

#if HANDLE_MULTIBYTE

#  define CHAR		wchar_t
#  define U_CHAR	wint_t
#  define XCHAR		wchar_t
#  define INT		wint_t
#  define L(CS)		L##CS
#  define INVALID	WEOF

#  undef STREQ
#  undef STREQN
#  define STREQ(s1, s2) ((wcscmp (s1, s2) == 0))
#  define STREQN(a, b, n) ((a)[0] == (b)[0] && wcsncmp(a, b, n) == 0)

extern char *mbsmbchar __P((const char *));

static int
rangecmp_wc (c1, c2, forcecoll)
     wint_t c1, c2;
     int forcecoll;
{
  static wchar_t s1[2] = { L' ', L'\0' };
  static wchar_t s2[2] = { L' ', L'\0' };

  if (c1 == c2)
    return 0;

  if (forcecoll == 0 && glob_asciirange && c1 <= UCHAR_MAX && c2 <= UCHAR_MAX)
    return ((int)(c1 - c2));

  s1[0] = c1;
  s2[0] = c2;

  return (wcscoll (s1, s2));
}

static int
collequiv_wc (c, equiv)
     wint_t c, equiv;
{
  return (c == equiv);
}

/* Helper function for collating symbol. */
#  define _COLLSYM	_collwcsym
#  define __COLLSYM	__collwcsym
#  define POSIXCOLL	posix_collwcsyms
#  include "collsyms.h"

static wint_t
collwcsym (s, len)
     wchar_t *s;
     int len;
{
  register struct _collwcsym *csp;

  for (csp = posix_collwcsyms; csp->name; csp++)
    {
      if (STREQN(csp->name, s, len) && csp->name[len] == L'\0')
	return (csp->code);
    }
  if (len == 1)
    return s[0];
  return INVALID;
}

static int
is_wcclass (wc, name)
     wint_t wc;
     wchar_t *name;
{
  char *mbs;
  mbstate_t state;
  size_t mbslength;
  wctype_t desc;
  int want_word;

  if ((wctype ("ascii") == (wctype_t)0) && (wcscmp (name, L"ascii") == 0))
    {
      int c;

      if ((c = wctob (wc)) == EOF)
	return 0;
      else
        return (c <= 0x7F);
    }

  want_word = (wcscmp (name, L"word") == 0);
  if (want_word)
    name = L"alnum";

  memset (&state, '\0', sizeof (mbstate_t));
  mbs = (char *) malloc (wcslen(name) * MB_CUR_MAX + 1);
  if (mbs == 0)
    return -1;
  mbslength = wcsrtombs (mbs, (const wchar_t **)&name, (wcslen(name) * MB_CUR_MAX + 1), &state);

  if (mbslength == (size_t)-1 || mbslength == (size_t)-2)
    {
      free (mbs);
      return -1;
    }
  desc = wctype (mbs);
  free (mbs);

  if (desc == (wctype_t)0)
    return -1;

  if (want_word)
    return (iswctype (wc, desc) || wc == L'_');
  else
    return (iswctype (wc, desc));
}

/* Return 1 if there are no char class [:class:] expressions (degenerate case)
   or only posix-specified (C locale supported) char class expressions in
   PATTERN.  These are the ones where it's safe to punt to the single-byte
   code, since wide character support allows locale-defined char classes.
   This only uses single-byte code, but is only needed to support multibyte
   locales. */
static int
posix_cclass_only (pattern)
     char *pattern;
{
  char *p, *p1;
  char cc[16];		/* sufficient for all valid posix char class names */
  enum char_class valid;

  p = pattern;
  while (p = strchr (p, '['))
    {
      if (p[1] != ':')
	{
	  p++;
	  continue;
        }
      p += 2;		/* skip past "[:" */
      /* Find end of char class expression */
      for (p1 = p; *p1;  p1++)
	if (*p1 == ':' && p1[1] == ']')
	  break;
      if (*p1 == 0)	/* no char class expression found */
	break;
      /* Find char class name and validate it against posix char classes */
      if ((p1 - p) >= sizeof (cc))
	return 0;
      bcopy (p, cc, p1 - p);
      cc[p1 - p] = '\0';
      valid = is_valid_cclass (cc);
      if (valid == CC_NO_CLASS)
	return 0;		/* found unrecognized char class name */

      p = p1 + 2;		/* found posix char class name */
    }
    
  return 1;			/* no char class names or only posix */
}      

/* Now include `sm_loop.c' for multibyte characters. */
#define FOLD(c) ((flags & FNM_CASEFOLD) && iswupper (c) ? towlower (c) : (c))
#define FCT			internal_wstrmatch
#define GMATCH			gmatch_wc
#define COLLSYM			collwcsym
#define PARSE_COLLSYM		parse_collwcsym
#define BRACKMATCH		brackmatch_wc
#define PATSCAN			glob_patscan_wc
#define STRCOMPARE		wscompare
#define EXTMATCH		extmatch_wc
#define STRUCT			wcsmat_struct
#define STRCHR(S, C)		wcschr((S), (C))
#define MEMCHR(S, C, N)		wmemchr((S), (C), (N))
#define STRCOLL(S1, S2)		wcscoll((S1), (S2))
#define STRLEN(S)		wcslen(S)
#define STRCMP(S1, S2)		wcscmp((S1), (S2))
#define RANGECMP(C1, C2, F)	rangecmp_wc((C1), (C2), (F))
#define COLLEQUIV(C1, C2)	collequiv_wc((C1), (C2))
#define CTYPE_T			enum char_class
#define IS_CCLASS(C, S)		is_wcclass((C), (S))
#include "sm_loop.c"

#endif /* HAVE_MULTIBYTE */

int
xstrmatch (pattern, string, flags)
     char *pattern;
     char *string;
     int flags;
{
#if HANDLE_MULTIBYTE
  int ret;
  size_t n;
  wchar_t *wpattern, *wstring;
  size_t plen, slen, mplen, mslen;

  if (MB_CUR_MAX == 1)
    return (internal_strmatch ((unsigned char *)pattern, (unsigned char *)string, flags));

  if (mbsmbchar (string) == 0 && mbsmbchar (pattern) == 0 && posix_cclass_only (pattern) )
    return (internal_strmatch ((unsigned char *)pattern, (unsigned char *)string, flags));

  n = xdupmbstowcs (&wpattern, NULL, pattern);
  if (n == (size_t)-1 || n == (size_t)-2)
    return (internal_strmatch ((unsigned char *)pattern, (unsigned char *)string, flags));

  n = xdupmbstowcs (&wstring, NULL, string);
  if (n == (size_t)-1 || n == (size_t)-2)
    {
      free (wpattern);
      return (internal_strmatch ((unsigned char *)pattern, (unsigned char *)string, flags));
    }

  ret = internal_wstrmatch (wpattern, wstring, flags);

  free (wpattern);
  free (wstring);

  return ret;
#else
  return (internal_strmatch ((unsigned char *)pattern, (unsigned char *)string, flags));
#endif /* !HANDLE_MULTIBYTE */
}
