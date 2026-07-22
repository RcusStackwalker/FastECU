#ifndef CIPHER_H
#define CIPHER_H

#include "src/algorithms/protocol/bytes.h"

#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>

class Cipher
{
  public:
    Cipher();
    ~Cipher();

    int encrypt_aes128_ecb(unsigned char *plaintext, int plaintext_len, unsigned char *key, unsigned char *ciphertext);
    int decrypt_aes128_ecb(unsigned char *ciphertext, int ciphertext_len, unsigned char *key, unsigned char *plaintext);
    bytes::Bytes encrypt_aes128_ecb(bytes::ByteView plaintext, bytes::ByteView key);
    bytes::Bytes decrypt_aes128_ecb(bytes::ByteView ciphertext, bytes::ByteView key);

  private:
    EVP_CIPHER_CTX *ctx;
    void handleErrors(void);
};

#endif // CIPHER_H
