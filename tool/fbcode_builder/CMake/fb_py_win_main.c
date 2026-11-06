// Copyright (c) Facebook, Inc. and its affiliates.

#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>

#define PATH_SIZE 32768

typedef int (*Py_Main)(int, wchar_t**);

int locate_py_main(int argc, wchar_t** argv) {
  /*
   * We have to dynamically locate Python3.dll because we may be loading a
   * Python native module while running. If that module is built with a
   * different Python version, we will end up a DLL import error. To resolve
   * this, we can either ship an embedded version of Python with us or
   * dynamically look up existing Python distribution installed on user's
   * machine. This way, we should be able to get a consistent version of
   * Python3.dll and .pyd modules.
   */
  HINSTANCE python_dll;
  Py_Main pymain;

  python_dll =
      LoadLibraryExW(L"python3.dll", NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

  int returncode = 0;
  if (python_dll != NULL) {
    pymain = (Py_Main)GetProcAddress(python_dll, "Py_Main");

    if (pymain != NULL) {
      returncode = (pymain)(argc, argv);
    } else {
      fprintf(stderr, "error: %d unable to load Py_Main\n", GetLastError());
    }

    FreeLibrary(python_dll);
  } else {
    fprintf(stderr, "error: %d unable to locate python3.dll\n", GetLastError());
    return 1;
  }
  return returncode;
}

int wmain() {
  /*
   * This executable will be prepended to the start of a Python ZIP archive.
   * Python will be able to directly execute the ZIP archive, so we simply
   * need to tell Py_Main() to run our own file.  Duplicate the argument list
   * and add our file name to the beginning to tell Python what file to invoke.
   */
  wchar_t** pyargv = malloc(sizeof(wchar_t*) * (__argc + 1));
  if (!pyargv) {
    fprintf(stderr, "error: failed to allocate argument vector\n");
    return 1;
  }

  /* Py_Main wants the wide character version of the argv so we pull those
   * values from the global __wargv array that has been prepared by MSVCRT.
   *
   * In order for the zipapp to run we need to insert an extra argument in
   * the front of the argument vector that points to ourselves.
   *
   * An additional complication is that, depending on who prepared the argument
   * string used to start our process, the computed __wargv[0] can be a simple
   * shell word like `watchman-wait` which is normally resolved together with
   * the PATH by the shell.
   * That unresolved path isn't sufficient to start the zipapp on windows;
   * we need the fully qualified path.
   *
   * Given:
   * __wargv == {"watchman-wait", "-h"}
   *
   * we want to pass the following to Py_Main:
   *
   * {
   *   "z:\build\watchman\python\watchman-wait.exe",
   *   "z:\build\watchman\python\watchman-wait.exe",
   *   "-h"
   * }
   */
  wchar_t full_path_to_argv0[PATH_SIZE];
  DWORD len = GetModuleFileNameW(NULL, full_path_to_argv0, PATH_SIZE);
  if (len == 0 ||
      len == PATH_SIZE && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
    fprintf(
        stderr,
        "error: %d while retrieving full path to this executable\n",
        GetLastError());
    return 1;
  }

  for (int n = 1; n < __argc; ++n) {
    pyargv[n + 1] = __wargv[n];
  }
  pyargv[0] = full_path_to_argv0;
  pyargv[1] = full_path_to_argv0;

  return locate_py_main(__argc + 1, pyargv);
}
