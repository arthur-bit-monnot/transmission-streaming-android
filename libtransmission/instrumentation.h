/******************************************************************************
 * $Id: instrumentation.h 
 *
 * Copyright (c) 2010 Arthur Bit-Monnot
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#ifndef TR_INSTRUMENTATION_H
#define TR_INSTRUMENTATION_H 1


struct tr_session;


/*
 * Opens a log file where the instrumentation output will
 * be writen if isInstruEnabled is set to TRUE.
 * Does nothing otherwise.
 * It is attached to a session (passed as argument)
 */
void
tr_instruInit( struct tr_session * session );


/* 
 * Closes the log file attached to the session and
 * set isInstruEnabled to FALSE.
 */
void
tr_instruUninit( struct tr_session * session );


/* 
 * Print a message to the log file attached to session.
 * This add the current time at the beginning of the line.
 */
void tr_instruMsg( struct tr_session * session, const char * fmt, ... );





#endif /* TR_INSTRUMENTATION_H */

