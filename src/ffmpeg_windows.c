#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// NOTE: We can't include raylib.h in here cause it collides with windows.h. So we just copy pasted log related declarations.

// Function specifiers in case library is build/used as a shared library (Windows)
// NOTE: Microsoft specifiers to tell compiler that symbols are imported/exported from a .dll
#if defined(_WIN32)
    #if defined(BUILD_LIBTYPE_SHARED)
        #if defined(__TINYC__)
            #define __declspec(x) __attribute__((x))
        #endif
        #define RLAPI __declspec(dllexport)     // We are building the library as a Win32 shared library (.dll)
    #elif defined(USE_LIBTYPE_SHARED)
        #define RLAPI __declspec(dllimport)     // We are using the library as a Win32 shared library (.dll)
    #endif
#endif

#ifndef RLAPI
    #define RLAPI       // Functions defined as 'extern' by default (implicit specifiers)
#endif

// Trace log level
// NOTE: Organized by priority level
typedef enum {
    LOG_ALL = 0,        // Display all logs
    LOG_TRACE,          // Trace logging, intended for internal use only
    LOG_DEBUG,          // Debug logging, used for internal debugging, it should be disabled on release builds
    LOG_INFO,           // Info logging, used for program execution info
    LOG_WARNING,        // Warning logging, used on recoverable failures
    LOG_ERROR,          // Error logging, used on unrecoverable failures
    LOG_FATAL,          // Fatal logging, used to abort program: exit(EXIT_FAILURE)
    LOG_NONE            // Disable logging
} TraceLogLevel;

RLAPI void TraceLog(int logLevel, const char *text, ...);

typedef struct {
    HANDLE hProcess;
    HANDLE hPipeWrite;
} FFMPEG;

FFMPEG *ffmpeg_start_rendering(size_t width, size_t height, size_t fps, const char *sound_file_path)
{
    HANDLE pipe_read;
    HANDLE pipe_write;

    SECURITY_ATTRIBUTES saAttr = {0};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;

    if (!CreatePipe(&pipe_read, &pipe_write, &saAttr, 0)) {
        TraceLog(LOG_ERROR, "FFMPEG: Could not create pipe. System Error Code: %d", GetLastError());
        return NULL;
    }

    if (!SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0)) {
        TraceLog(LOG_ERROR, "FFMPEG: Could not mark write pipe as non-inheritable. System Error Code: %d", GetLastError());
        return NULL;
    }

    // https://docs.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output

    STARTUPINFO siStartInfo;
    ZeroMemory(&siStartInfo, sizeof(siStartInfo));
    siStartInfo.cb = sizeof(STARTUPINFO);
    // NOTE: theoretically setting NULL to std handles should not be a problem
    // https://docs.microsoft.com/en-us/windows/console/getstdhandle?redirectedfrom=MSDN#attachdetach-behavior
    siStartInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (siStartInfo.hStdError == INVALID_HANDLE_VALUE) {
        TraceLog(LOG_ERROR, "FFMPEG: Could get standard error handle for the child. System Error Code: %d", GetLastError());
        return NULL;
    }
    siStartInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    if (siStartInfo.hStdOutput == INVALID_HANDLE_VALUE) {
        TraceLog(LOG_ERROR, "FFMPEG: Could get standard output handle for the child. System Error Code: %d", GetLastError());
        return NULL;
    }
    siStartInfo.hStdInput = pipe_read;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    // TODO: use String_Builder in here
    // TODO: sanitize user input through sound_file_path
    char cmd_buffer[1024*2];
    snprintf(cmd_buffer, sizeof(cmd_buffer), "ffmpeg.exe -loglevel verbose -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - -i \"%s\" -c:v libx264 -vb 2500k -c:a aac -ab 200k -pix_fmt yuv420p output.mp4", (int)width, (int)height, (int)fps, sound_file_path);

    if (!CreateProcess(NULL, cmd_buffer, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo)) {
        TraceLog(LOG_ERROR, "FFMPEG: Could not create child process. System Error Code: %d", GetLastError());

        CloseHandle(pipe_write);
        CloseHandle(pipe_read);

        return NULL;
    }

    CloseHandle(pipe_read);
    CloseHandle(piProcInfo.hThread);

    FFMPEG *ffmpeg = malloc(sizeof(FFMPEG));
    assert(ffmpeg != NULL && "Buy MORE RAM lol!!");
    ffmpeg->hProcess = piProcInfo.hProcess;
    ffmpeg->hPipeWrite = pipe_write;
    return ffmpeg;
}

bool ffmpeg_send_frame_flipped(FFMPEG *ffmpeg, void *data, size_t width, size_t height)
{
    for (size_t y = height; y > 0; --y) {
        // TODO: handle ERROR_IO_PENDING
        if (!WriteFile(ffmpeg->hPipeWrite, (uint32_t*)data + (y - 1)*width, sizeof(uint32_t)*width, NULL, NULL)) {
            TraceLog(LOG_ERROR, "FFMPEG: failed to write into ffmpeg pipe. System Error Code: %d", GetLastError());
            return false;
        }
    }
    return true;
}

bool ffmpeg_end_rendering(FFMPEG *ffmpeg)
{
    HANDLE hPipeWrite = ffmpeg->hPipeWrite;
    HANDLE hProcess = ffmpeg->hProcess;
    free(ffmpeg);

    FlushFileBuffers(hPipeWrite);
    CloseHandle(hPipeWrite);

    if (WaitForSingleObject(hProcess, INFINITE) == WAIT_FAILED) {
        TraceLog(LOG_ERROR, "FFMPEG: could not wait on child process. System Error Code: %d", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    DWORD exit_status;
    if (GetExitCodeProcess(hProcess, &exit_status) == 0) {
        TraceLog(LOG_ERROR, "FFMPEG: could not get process exit code. System Error Code: %d", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    if (exit_status != 0) {
        TraceLog(LOG_ERROR, "FFMPEG: command exited with exit code %lu", exit_status);
        CloseHandle(hProcess);
        return false;
    }

    CloseHandle(hProcess);

    return true;
}

// TODO: where can we find this symbol for the Windows build?
void __imp__wassert() {}
