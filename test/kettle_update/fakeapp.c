/* Stands in for a broken new qbittorrent.exe in the install tests (it is compiled by the tests).

   KETTLE_FAKE_MODE   "exit": ends at once with exit code 3. "crash:N": runs N seconds, then ends with exit code 1.
                      "hang": never ends and never reports anything.
   KETTLE_FAKE_DATA   a folder of the test profile; the fake writes changed.txt there and overwrites marker.txt,
                      to show that undoing an update puts the profile back as it was. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void writeText(const char *folder, const char *name, const char *text)
{
    char path[MAX_PATH + 32];
    FILE *file;
    snprintf(path, sizeof(path), "%s\\%s", folder, name);
    file = fopen(path, "w");
    if (file)
    {
        fputs(text, file);
        fclose(file);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    char mode[64] = "";
    char data[MAX_PATH] = "";
    (void)instance; (void)previous; (void)command; (void)show;

    GetEnvironmentVariableA("KETTLE_FAKE_MODE", mode, sizeof(mode));
    if (GetEnvironmentVariableA("KETTLE_FAKE_DATA", data, sizeof(data)) > 0)
    {
        writeText(data, "changed.txt", "written by the new version");
        writeText(data, "marker.txt", "changed by the new version");
    }

    if (strcmp(mode, "exit") == 0)
        return 3;
    if (strncmp(mode, "crash:", 6) == 0)
    {
        Sleep((DWORD)atoi(mode + 6) * 1000);
        return 1;
    }
    for (;;)
        Sleep(1000);
}
