#include "App.h"

#include <unistd.h>

#include <cstring>
#include <string>

// The app folder is the working directory the firmware starts us in (the
// ELF itself runs from a copy in /tmp).
int main(int argc, char** argv)
{
    char cwd[1024] = {0};
    if (argc >= 2 && argv[1][0] == '/') std::strncpy(cwd, argv[1], sizeof(cwd) - 1);
    else if (!getcwd(cwd, sizeof(cwd))) std::strcpy(cwd, ".");
    return runApp(cwd);
}
