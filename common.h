#ifndef KAYLES_COMMON_H
#define KAYLES_COMMON_H

#include <stdint.h>
#include <sys/types.h>

uint16_t read_port(char const *string);
size_t read_size(char const *string);
struct sockaddr_in get_server_address(char const *host, uint16_t port);
void install_signal_handler(int signal, void (*handler)(int), int flags);

#endif //KAYLES_COMMON_H