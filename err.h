#ifndef KAYLES_ERR_H
#define KAYLES_ERR_H

#include <stdnoreturn.h>

// Print information about a system error and quits.
noreturn void syserr(const char* fmt, ...);

// Print information about an error and quits.
noreturn void fatal(const char* fmt, ...);

// Print information about an error and return.
void error(const char* fmt, ...);

// Print the correct command-line usage of the program and quits.
void usage(const char *prog_name);

#endif