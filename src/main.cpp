#include "App.h"

#include <unistd.h>

#include <cstring>
#include <string>

// The app folder is the working directory the firmware starts us in (the ELF
// itself runs from a copy in /tmp). Arguments are ignored, as in Retro Launcher.
int main()
{
    char cwd[1024] = {0};
    if (!getcwd(cwd, sizeof(cwd))) std::strcpy(cwd, ".");
    return runApp(cwd);
}
