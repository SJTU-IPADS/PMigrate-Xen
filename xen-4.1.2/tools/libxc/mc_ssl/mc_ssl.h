#ifndef __MC_SSL_H__
#define __MC_SSL_H__

#include "cipher.h"
struct ssl_wrap {
	CipherContext *cc;
	unsigned long ssl_buf_len;
	char *ssl_buf;
};

CipherContext *init_ssl_byname(char* cipher_name, char* password, int en);
int ssl_encrypt(struct ssl_wrap *ssl, char *data, size_t size);
int read_size_adjust(struct ssl_wrap *ssl, size_t size);
int ssl_decrypt(struct ssl_wrap *ssl, char *data, size_t new_size, size_t ori_size);
int ssl_read(struct ssl_wrap *ssl, int fd, void* data, size_t size);
int ssl_write(struct ssl_wrap *ssl, int fd, void* data, size_t size);
#endif
