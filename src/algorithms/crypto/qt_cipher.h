#ifndef QT_CIPHER_H
#define QT_CIPHER_H

#include "src/algorithms/crypto/cipher.h"

#include <QByteArray>

// TRANSITIONAL Qt shim. Wraps the portable Cipher's byte-view API in
// QByteArray-taking methods for callers that haven't migrated off Qt
// containers yet.
class QtCipher
{
  public:
    QByteArray encrypt_aes128_ecb(const QByteArray& plaintext, const QByteArray& key);
    QByteArray decrypt_aes128_ecb(const QByteArray& ciphertext, const QByteArray& key);

  private:
    Cipher cipher_;
};

#endif // QT_CIPHER_H
