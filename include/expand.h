#ifndef MYSH_EXPAND_H
#define MYSH_EXPAND_H

#include <stddef.h>

/*
 * Expand a single word string, performing in order:
 *   1. Tilde expansion          (~, ~/path, ~user)
 *   2. Parameter expansion      $VAR  ${VAR}  $?  $$  $#
 *   3. Command substitution     $(...)
 *   4. Pathname expansion       * ? [...]  (glob)
 *
 * Returns a NULL-terminated array of heap-allocated strings.
 * A non-glob word always returns exactly one string.
 * A glob that matches nothing returns the original pattern (POSIX).
 *
 * The caller must free every string in the array AND the array itself
 * using expand_free().
 *
 * Returns NULL on allocation failure.
 */
char **expand_word(const char *word, int in_double_quote);

/*
 * Expand every word in a NULL-terminated argv array in-place.
 * Replaces argv with a newly allocated array (caller must free with
 * expand_free_argv).  Returns the new argc, or -1 on error.
 */
int expand_argv(char ***argv);

/* Free a result from expand_word() */
void expand_free(char **words);

/* Free a result from expand_argv() */
void expand_free_argv(char **argv);

#endif /* MYSH_EXPAND_H */