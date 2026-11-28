#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int launch_error(const char *message)
{
    fprintf(stderr, "ED-UP: %s\n", message);
    execlp("zenity", "zenity", "--error", "--no-markup", "--title", "ED-UP Launcher", "--text", message,
           (char *)NULL);
    execlp("kdialog", "kdialog", "--title", "ED-UP Launcher", "--error", message, (char *)NULL);
    execlp("notify-send", "notify-send", "--urgency=critical", "ED-UP Launcher", message, (char *)NULL);
    return 1;
}

int main(int argc, char *argv[])
{
    char path[PATH_MAX];
    const char wrapper[] = "ed-up-launcher-linux.py";
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length < 0 || (size_t)length >= sizeof(path) - 1) {
        return launch_error("Could not find the ED-UP Launcher folder.");
    }
    path[length] = '\0';
    char *name = strrchr(path, '/');
    if (!name || (size_t)(name + 1 - path) + sizeof(wrapper) > sizeof(path)) {
        return launch_error("Could not find the ED-UP Launcher folder.");
    }
    memcpy(name + 1, wrapper, sizeof(wrapper));
    if (access(path, R_OK) != 0) {
        return launch_error("Extract the whole ED-UP Linux ZIP and keep its files together.");
    }

    char **command = calloc((size_t)argc + 3, sizeof(*command));
    if (!command) {
        return launch_error("Could not start ED-UP Launcher.");
    }
    command[0] = "python3";
    command[1] = path;
    command[2] = "--desktop";
    for (int i = 1; i < argc; ++i) {
        command[i + 2] = argv[i];
    }
    execvp(command[0], command);
    free(command);
    return launch_error("Could not start Python 3. Install Python 3 and open ED-UP Launcher again.");
}
