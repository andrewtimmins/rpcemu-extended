/*
* Copyright (c) 2021, RiscOS Cloverleaf
* https://riscoscloverleaf.com/
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdarg.h>
#include <oslib/syslog.h>

int debug_report = 0;
#if defined(CONFIG_DEBUG) && defined(CONFIG_DEBUG_REPORTER)
void
dprintf (const char *msg, ...)
{
    if (!debug_report) {
        return;
    }
    char buf[256];
    va_list ap;

    va_start (ap, msg);
    vsnprintf (buf, sizeof (buf), msg, ap);
    va_end (ap);
    buf[sizeof (buf) - 1] = '\0'; /* Just to be sure.  */
//    if (!xsyslog_irq_mode(1)) {
//        xsyslog_log_message(Module_Title, buf, 64);
//        xsyslog_irq_mode(0);
//    }

  __asm__ volatile ("MOV\tr0, %[buf]\n\t"
		    "SWI\t%[SWI_XReport_Text0]\n\t"
		    :
		    : [buf] "r" (buf),
                      [SWI_XReport_Text0] "i" (0x54c80 | (1<<17))
		    : "r0", "r14", "cc");
}
#endif
