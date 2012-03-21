#include <string.h>
#include <unistd.h>
#include "mc_ssl.h"
CipherContext *init_ssl_byname(char* cipher_name, char* password, int en) {
	unsigned char key[EVP_MAX_KEY_LENGTH];
	unsigned char iv[EVP_MAX_IV_LENGTH];
	Cipher *cipher = cipher_by_name(cipher_name);
	CipherContext *cc = (CipherContext*) malloc(sizeof(CipherContext));

	if (cipher == NULL) {
		return NULL;
	}

	EVP_BytesToKey(cipher->evptype(), EVP_md5(), NULL, (unsigned char*)password, strlen(password), 1, key, iv);
	if (en > 0) {
		cipher_init(cc, cipher, key, cipher->key_len, iv, cipher->block_size, CIPHER_ENCRYPT);
	} else {
		cipher_init(cc, cipher, key, cipher->key_len, iv, cipher->block_size, CIPHER_DECRYPT);
	}

	return cc;
}

int ssl_encrypt(struct ssl_wrap *ssl, char *data, size_t size) {
	Cipher *cipher = ssl->cc->cipher;
	char *buf = NULL;
	int is_tem = 0;
	if (size < cipher->block_size) {
		buf = (char*)calloc(cipher->block_size, 1);
		memcpy(buf, data, size);
		size = cipher->block_size;
		is_tem = 1;
	} else if ( (size % cipher->block_size) != 0 ){ // Not mod block size
		int tem_size = ((size / cipher->block_size) + 1) * cipher->block_size;
		fprintf(stderr, "size is %lu\tnew_size is %d\n", size, tem_size);
		buf = (char*)malloc(tem_size);
		fprintf(stderr, "after malloc\n");
		memcpy(buf, data, size);
		size = tem_size;
		is_tem = 1;
	} else {
		buf = data;
		if (ssl->ssl_buf_len < size) { // Resize ssl_buf
			ssl->ssl_buf_len = size;
			ssl->ssl_buf = (char*) realloc(ssl->ssl_buf, size);
		}
	}

	cipher_crypt(ssl->cc, (unsigned char*)ssl->ssl_buf, (unsigned char*)buf, size);

	if (is_tem) {
		free(buf);
	}
	return size;
}

/* Calculate a new size 
 * Resize ssl_buf if necessary */
int read_size_adjust(struct ssl_wrap *ssl, size_t size)
{
	Cipher *cipher = ssl->cc->cipher;
	if (size < cipher->block_size) {
		return cipher->block_size;
	} else if ( (size % cipher->block_size) != 0 ) {
		return ((size / cipher->block_size) + 1) * cipher->block_size;
	} 

	if (ssl->ssl_buf_len < size) {
		ssl->ssl_buf_len = size;
		ssl->ssl_buf = (char*)realloc(ssl->ssl_buf, size);
	}
	return size;
}

int ssl_decrypt(struct ssl_wrap *ssl, char *data, size_t new_size, size_t ori_size) 
{
	Cipher *cipher = ssl->cc->cipher;
	int is_tem = 0;
	char *buf = NULL;
	if (new_size == cipher->block_size) {
		buf = (char*)calloc(cipher->block_size, 1);
		is_tem = 1;
	} else if (ori_size != new_size){
		buf = (char*)calloc(1, new_size);
		is_tem = 1;
	} else {
		buf = data;
	}

	cipher_crypt(ssl->cc, (unsigned char*)buf, (unsigned char*)ssl->ssl_buf, new_size);

	if (is_tem) {
		memcpy(data, buf, ori_size);
		free(buf);
	}
	return ori_size;
}

int ssl_read(struct ssl_wrap *ssl, int fd, void* data, size_t size)
{
	int new_size;
	if ((new_size = read_size_adjust(ssl, size)) < 0) {
		return -1;
	}
	read(fd, ssl->ssl_buf, new_size);
	return ssl_decrypt(ssl, data, new_size, size);
}

int ssl_write(struct ssl_wrap *ssl, int fd, void* data, size_t size)
{
	if ((size = ssl_encrypt(ssl, data, size)) < 0) {
		return -1;
	}
	write(fd, ssl->ssl_buf, size);
	return 0;
}
