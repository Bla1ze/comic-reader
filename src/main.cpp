#include "App.h"

#include <unistd.h>

#include <cstring>
#include <string>

// The app folder is the working directory the firmware starts us in (the ELF
// itself runs from a copy in /tmp). Arguments are ignored, as in Retro Launcher.
int main(int argc, char** argv)
{
    (void)argc;
    char cwd[1024] = {0};
    if (!getcwd(cwd, sizeof(cwd))) std::strcpy(cwd, ".");
    const int rc = runApp(cwd);
    if (rc == kRestart) {
        // After an update: run the new ELF from the stick, not the old /tmp
        // copy that /proc/self/exe points at. Same process, so the firmware's
        // launcher keeps waiting on us.
        const std::string elf = std::string(cwd) + "/comic-reader.elf";
        char* args[] = {const_cast<char*>(elf.c_str()), nullptr};
        ::execv(elf.c_str(), args);
        (void)argv;
    }
    return rc == kRestart ? 0 : rc;
}
