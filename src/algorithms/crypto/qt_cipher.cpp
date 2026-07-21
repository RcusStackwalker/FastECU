#include "src/algorithms/crypto/qt_cipher.h"

#include "src/algorithms/protocol/qt_bytes.h"

QByteArray QtCipher::encrypt_aes128_ecb(const QByteArray& plaintext, const QByteArray& key)
{
    return bytes::toQByteArray(cipher_.encrypt_aes128_ecb(bytes::view(plaintext), bytes::view(key)));
}

QByteArray QtCipher::decrypt_aes128_ecb(const QByteArray& ciphertext, const QByteArray& key)
{
    return bytes::toQByteArray(cipher_.decrypt_aes128_ecb(bytes::view(ciphertext), bytes::view(key)));
}
