#ifdef THREAD_SAFE
#include "tls.h"
#else
#define TLS_VARS
#define GLOBAL(a) a
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "winutil.h"

#ifndef THREAD_SAFE
static char Win_Error_msg[256];
#endif

char *php3_win_err(void)
{
	TLS_VARS;

	FormatMessage(
					 FORMAT_MESSAGE_FROM_SYSTEM,
					 NULL,
					 GetLastError(),
					 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),		// Default language
					  (LPTSTR) GLOBAL(Win_Error_msg),
					 256,
					 NULL);

	return GLOBAL(Win_Error_msg);
}
