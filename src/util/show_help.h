/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * The "show help" subsystem (SHS) in PRTE is intended to help the
 * developer convey meaningful information to the user (read longer
 * than is convenient in a single printf), particularly when errors
 * occur.  The SHS allows the storage of arbitrary-length help
 * messages in text files which can be parameterized by text filename,
 * message name, POSIX locale, and printf()-style parameters (e.g.,
 * "%s", "%d", etc.).  Note that the primary purpose of the SHS is to
 * display help messages, but it can actually be used to display any
 * arbitrary text messages.
 *
 * The function prte_show_help() is used to find a help message and
 * display it.  Its important parameters are a filename, message name,
 * and printf()-style varargs parameters used to substitute into the
 * message.
 *
 * It was originally intended that this system would support a very
 * simple version of i18n-like support, but we got (strong) feedback
 * that i18n support was not desired.  So it never happened.
 *
 * As such, the file lookup is quite straightforward -- the caller
 * passes in the filename to find the help message, and the SHS looks
 * for that file in $pkgdatadir (typically $prefix/share/openmpi).
 *
 * Once the file is successfully opened, the SHS looks for the
 * appropriate help message to display.  It looks for the message name
 * in the file, reads in the message, and displays it.  printf()-like
 * substitutions are performed (e.g., %d, %s, etc.) --
 * prte_show_help() takes a variable legnth argument list that are
 * used for these substitutions.
 *
 * The format of the help file is simplistic:
 *
 * - Comments begin with #.  Any characters after a # on a line are
 *   ignored.  It is not possible to escape a #.
 * - Message names are on a line by themselves and marked with [].
 *   Names can be any ASCII string within the [] (excluding the
 *   characters newline, linefeed, [, ], and #).
 * - Messages are any characters between message names and/or the end
 *   of the file.
 *
 * Here's a sample helpfile:
 *
 * \verbatimbegin
 * # This is a comment.
 * [topic 1]
 * Here's the first message.  Let's substitute in an integer: %d.
 * The quick brown fox jumped over the lazy %s.
 * # This is another comment -- it's not displayed in the first message.
 * [another:topic:foo:foo:foo]
 * This is the second message.  Let's just keep rolling along to get
 * to the second line in the message for this example.
 * \verbatimend
 *
 * It is expected that help messages will be grouped by filename;
 * similar messages should be in a single file.  For example, an MCA
 * component may install its own helpfile in PRTE's $pkgdatadir,
 * and therefore the component can invoke prte_show_help() to display
 * its own help messages.
 *
 * Message files in $pkgdatadir have a naming convention: they
 * generally start with the prefix "help-" and are followed by a name
 * descriptive of what kind of messages they contain.  MCA components
 * should generally abide by the MCA prefix rule, with the exception
 * that they should start the filename with "help-", as mentioned
 * previously.
 */

#ifndef PRTE_SHOW_HELP_H
#define PRTE_SHOW_HELP_H

#include "prte_config.h"

#include "src/rml/rml_types.h"
#include "src/pmix/pmix-internal.h"
#include "types.h"
#include <stdarg.h>

BEGIN_C_DECLS

/**
 * \internal
 *
 * Initialization of show_help subsystem
 */
PRTE_EXPORT int prte_show_help_init(void);
PRTE_EXPORT void prte_show_help_finalize(void);

/**
 * Look up a text message in a text file and display it to the
 * stderr using printf()-like substitutions (%d, %s, etc.).
 *
 * @param filename File where the text messages are contained.
 * @param topic String index of which message to display from the
 * text file.
 * @param want_error_header Display error-bar line header and
 * footer with the message.
 * @param varargs Any additional parameters are substituted,
 * printf()-style into the help message that is displayed.
 *
 * This function looks for the filename in the $pkgdatadir
 * (typically $prefix/share/openmpi), and looks up the message
 * based on the topic, and displays it.  If want_error_header is
 * true, a header and footer of asterisks are also displayed.
 *
 * Note that the "want_error_header" argument is int instead of bool,
 * because passing a parameter that undergoes default argument
 * promotion to va_start() has undefined behavior (according to clang
 * warnings on MacOS High Sierra).
 */
PRTE_EXPORT int prte_show_help(const char *filename, const char *topic, int want_error_header, ...);

/**
 * This function does the same thing as prte_show_help(), but accepts
 * a va_list form of varargs.
 */
PRTE_EXPORT int prte_show_vhelp(const char *filename, const char *topic, int want_error_header,
                                va_list ap);

/**
 * This function does the same thing as prte_show_help(), but returns
 * its output in a string (that must be freed by the caller).
 */
PRTE_EXPORT char *prte_show_help_string(const char *filename, const char *topic,
                                        int want_error_header, ...);

/**
 * This function does the same thing as prte_show_help_string(), but
 * accepts a va_list form of varargs.
 */
PRTE_EXPORT char *prte_show_help_vstring(const char *filename, const char *topic,
                                         int want_error_header, va_list ap);

/**
 * Exactly the same as prte_show_help, but pass in a rendered string,
 * rather than a varargs list which must be rendered.
 */
PRTE_EXPORT int prte_show_help_norender(const char *filename, const char *topic,
                                        int want_error_header, const char *output);

/**
 * This function adds another search location for the files that
 * back show_help messages. Locations will be searched starting
 * with the prefix installation directory, then cycled through
 * any additional directories in the order they were added
 *
 * This interface allows libraries that use OMPI to take advantage
 * of the show_help functionality. OMPI defines the show_help directory
 * based on where OMPI was installed. However, if the library wants to
 * use show_help to provide error output specific to itself, then it
 * nees to tell show_help how to find its own show_help files - without
 * interfering with the linked PRTE libs when they need to do show_help.
 */
PRTE_EXPORT int prte_show_help_add_dir(const char *directory);

/**
 * Pretend that this message has already been shown.
 *
 * Sends a control message to the HNP that will effectively suppress
 * this message from being shown.  Primitive *-wildcarding is
 * possible.
 */
PRTE_EXPORT int prte_show_help_suppress(const char *filename, const char *topic);

PRTE_EXPORT void prte_show_help_recv(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                                     prte_rml_tag_t tag, void *cbdata);

END_C_DECLS

#endif
