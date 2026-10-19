/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  Spyhawk
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "signature.h"

#include <memory>

#include <openssl/evp.h>

#include <QByteArray>

bool Utils::Signature::verifyEd25519(const QByteArray &message, const QByteArray &signature, const QByteArray &publicKey)
{
    const int publicKeySize = 32;
    const int signatureSize = 64;
    if ((publicKey.size() != publicKeySize) || (signature.size() != signatureSize))
        return false;

    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key {EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr
        , reinterpret_cast<const unsigned char *>(publicKey.constData()), static_cast<size_t>(publicKey.size())), &EVP_PKEY_free};
    if (!key)
        return false;

    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context {EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!context)
        return false;

    // Ed25519 takes no separate digest, so the message goes to the one-shot call
    if (EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        return false;

    return (EVP_DigestVerify(context.get()
        , reinterpret_cast<const unsigned char *>(signature.constData()), static_cast<size_t>(signature.size())
        , reinterpret_cast<const unsigned char *>(message.constData()), static_cast<size_t>(message.size())) == 1);
}
