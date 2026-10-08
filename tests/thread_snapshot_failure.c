#include <windows.h>

BOOL WINAPI cmfm_enumerate_process_threads(HANDLE heap, LPDWORD *ids, UINT *count)
{
    (void)heap;
    *ids = NULL;
    *count = 0;
    return FALSE;
}
