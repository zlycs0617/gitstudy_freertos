#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

uint32_t ulGetRunTimeCounterValue( void )
{
    return ( uint32_t ) clock();
}

void vAssertCalled( const char * pcFile, uint32_t ulLine )
{
    taskDISABLE_INTERRUPTS();

    printf( "[0][assert] configASSERT failed: %s:%lu\n",
            pcFile,
            ( unsigned long ) ulLine );
    fflush( stdout );

    for( ;; )
    {
    }
}
