#pragma once
/**
 * @file MainStack.h
 * @brief The main thread's stack on Linux: the same 64 MB the programs are linked for elsewhere.
 *
 * Several Engines stand on the stack at once in the tests and the tools (an Engine is a few hundred kilobytes, and
 * since the oversamplers of 25.09.2026 somewhat more). Windows (`/STACK`) and macOS (`-stack_size`) take the main
 * thread's stack from the executable (Tests/CMakeLists.txt, Tools/render/CMakeLists.txt); Linux takes it from the
 * stack limit when the program starts -- `ulimit -s`, 8 MB by default -- and the self test died there with a
 * segmentation fault (02.10.2026, the first Linux build).
 *
 * ensureMainStack() is therefore the first line of those programs' main(): on Linux, when the limit is below what is
 * asked and the hard limit allows more, it raises the limit and starts the program once more with the same arguments
 * (execv of /proc/self/exe), which then has the stack. An environment variable marks the second start, so it never
 * loops; anywhere else, and whenever a step fails, it returns and the program runs with the stack it has.
 */
#if defined(__linux__) && !defined(__ANDROID__)
  #include <cstdlib>
  #include <sys/resource.h>
  #include <unistd.h>
#endif

namespace ambient {

/**
 * @brief Gives the main thread @p bytes of stack on Linux by starting the program once more (see the file comment).
 * @param argv  main()'s arguments, or nullptr for a program that takes none
 * @param bytes the stack the program needs
 */
inline void ensureMainStack(char** argv, unsigned long long bytes)
{
#if defined(__linux__) && !defined(__ANDROID__)
    rlimit rl{};
    if (getrlimit(RLIMIT_STACK, &rl) != 0) return;
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= bytes) return;
    if (std::getenv("AMBIENT_MAIN_STACK") != nullptr) return;          // the second start: take what there is
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < bytes) return;   // not allowed to raise it
    rl.rlim_cur = static_cast<rlim_t>(bytes);
    if (setrlimit(RLIMIT_STACK, &rl) != 0) return;
    setenv("AMBIENT_MAIN_STACK", "1", 1);
    char self[] = "/proc/self/exe";
    char* none[] = { self, nullptr };
    execv(self, argv != nullptr ? argv : none);
    // execv returned: the program goes on with the stack it started with.
#else
    (void)argv;
    (void)bytes;
#endif
}

} // namespace ambient
