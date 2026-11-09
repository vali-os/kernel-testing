// Backend service callbacks referenced by ValiFS startup are outside this
// reader fixture. Any call into storage or service dispatch must fail the test.
#include <stdlib.h>

void
usched_mtx_lock(void)
{
    abort();
}

void
usched_mtx_unlock(void)
{
    abort();
}

void
GetFileService(void)
{
    abort();
}

void
GetGrachtClient(void)
{
    abort();
}

void
OSProcessCurrentID(void)
{
    abort();
}

void
OSTransferFileAt(void)
{
    abort();
}

void
gracht_client_await(void)
{
    abort();
}

void
sys_file_transfer_absolute_result(void)
{
    abort();
}

void
SHMBuffer(void)
{
    abort();
}

void
FSStorageRead(void)
{
    abort();
}

void
OsErrToErrNo(void)
{
    abort();
}

void
sys_file_fstat(void)
{
    abort();
}

void
sys_file_fstat_result(void)
{
    abort();
}
