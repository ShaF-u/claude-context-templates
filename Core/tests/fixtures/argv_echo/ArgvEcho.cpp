// Test-only fixture: prints exactly what this process actually received
// as argv, so a test can verify RunProcess's command-line QUOTING
// (Core/src/Util/ProcessRunner.cpp's QuoteArg/BuildCommandLine) against
// adversarial argument content (embedded quotes, trailing backslashes,
// spaces) round-trips correctly through a REAL child process boundary,
// rather than trusting the quoting algorithm by inspection alone.
//
// Output format: "<argc-1>\x1f<arg1>\x1f<arg2>..." -- 0x1F (ASCII Unit
// Separator) as the delimiter, since it's vanishingly unlikely to appear
// in any test argument, unlike a space or comma.
#include <cstdio>

int main(int argc, char** argv) {
    std::printf("%d", argc - 1);
    for (int i = 1; i < argc; ++i) {
        std::fputc('\x1f', stdout);
        std::fputs(argv[i], stdout);
    }
    return 0;
}
