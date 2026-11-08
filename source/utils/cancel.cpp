#include "cancel.h"

static bool (*s_check)() = 0;

void setTransferCancelCheck(bool (*check)())
{
    s_check = check;
}

bool transferCancelled()
{
    return s_check != 0 && s_check();
}
