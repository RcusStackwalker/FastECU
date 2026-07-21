#include "src/algorithms/crypto/cipher.h"

#include <gtest/gtest.h>

TEST(CipherPortable, EncryptDecryptRoundTrips)
{
    // AES-128-ECB with padding disabled (see Cipher::encrypt_aes128_ecb /
    // decrypt_aes128_ecb: EVP_CIPHER_CTX_set_padding(ctx, 0)) requires the
    // plaintext to already be a multiple of the 16-byte block size, so this
    // uses exactly one block rather than the brief's illustrative 8 bytes.
    const bytes::Bytes plain{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                             0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const bytes::Bytes key(16, 0xAB);

    Cipher cipher;
    const bytes::Bytes encrypted = cipher.encrypt_aes128_ecb(bytes::ByteView(plain), bytes::ByteView(key));
    EXPECT_NE(encrypted, plain);

    const bytes::Bytes decrypted = cipher.decrypt_aes128_ecb(bytes::ByteView(encrypted), bytes::ByteView(key));
    EXPECT_EQ(decrypted, plain);
}
