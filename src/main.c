#include <stdio.h>
#include <stdlib.h>
#include "mysh.h"

int main(int argc, char **argv)
{
    /* Print startup banner */
    printf("%s v%s\n", MYSH_NAME, MYSH_VERSION);

    /* Run the shell */
    return shell_run(argc, argv);
}
