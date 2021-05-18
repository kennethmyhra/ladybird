/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Random.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/Code/EMSA_PSS.h>
#include <LibTLS/TLSv12.h>

namespace TLS {

bool TLSv12::expand_key()
{
    u8 key[192]; // soooooooo many constants
    auto key_buffer = Bytes { key, sizeof(key) };

    auto is_aead = this->is_aead();

    if (m_context.master_key.size() == 0) {
        dbgln("expand_key() with empty master key");
        return false;
    }

    auto key_size = key_length();
    auto mac_size = mac_length();
    auto iv_size = iv_length();

    pseudorandom_function(
        key_buffer,
        m_context.master_key,
        (const u8*)"key expansion", 13,
        ReadonlyBytes { m_context.remote_random, sizeof(m_context.remote_random) },
        ReadonlyBytes { m_context.local_random, sizeof(m_context.local_random) });

    size_t offset = 0;
    if (is_aead) {
        iv_size = 4; // Explicit IV size.
    } else {
        memcpy(m_context.crypto.local_mac, key + offset, mac_size);
        offset += mac_size;
        memcpy(m_context.crypto.remote_mac, key + offset, mac_size);
        offset += mac_size;
    }

    auto client_key = key + offset;
    offset += key_size;
    auto server_key = key + offset;
    offset += key_size;
    auto client_iv = key + offset;
    offset += iv_size;
    auto server_iv = key + offset;
    offset += iv_size;

    if constexpr (TLS_DEBUG) {
        dbgln("client key");
        print_buffer(client_key, key_size);
        dbgln("server key");
        print_buffer(server_key, key_size);
        dbgln("client iv");
        print_buffer(client_iv, iv_size);
        dbgln("server iv");
        print_buffer(server_iv, iv_size);
        if (!is_aead) {
            dbgln("client mac key");
            print_buffer(m_context.crypto.local_mac, mac_size);
            dbgln("server mac key");
            print_buffer(m_context.crypto.remote_mac, mac_size);
        }
    }

    if (is_aead) {
        memcpy(m_context.crypto.local_aead_iv, client_iv, iv_size);
        memcpy(m_context.crypto.remote_aead_iv, server_iv, iv_size);

        m_cipher_local = Crypto::Cipher::AESCipher::GCMMode(ReadonlyBytes { client_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Encryption, Crypto::Cipher::PaddingMode::RFC5246);
        m_cipher_remote = Crypto::Cipher::AESCipher::GCMMode(ReadonlyBytes { server_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Decryption, Crypto::Cipher::PaddingMode::RFC5246);
    } else {
        memcpy(m_context.crypto.local_iv, client_iv, iv_size);
        memcpy(m_context.crypto.remote_iv, server_iv, iv_size);

        m_cipher_local = Crypto::Cipher::AESCipher::CBCMode(ReadonlyBytes { client_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Encryption, Crypto::Cipher::PaddingMode::RFC5246);
        m_cipher_remote = Crypto::Cipher::AESCipher::CBCMode(ReadonlyBytes { server_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Decryption, Crypto::Cipher::PaddingMode::RFC5246);
    }

    m_context.crypto.created = 1;

    return true;
}

bool TLSv12::compute_master_secret(size_t length)
{
    if (m_context.premaster_key.size() == 0 || length < 48) {
        dbgln("there's no way I can make a master secret like this");
        dbgln("I'd like to talk to your manager about this length of {}", length);
        return false;
    }

    m_context.master_key.clear();
    m_context.master_key.grow(length);

    pseudorandom_function(
        m_context.master_key,
        m_context.premaster_key,
        (const u8*)"master secret", 13,
        ReadonlyBytes { m_context.local_random, sizeof(m_context.local_random) },
        ReadonlyBytes { m_context.remote_random, sizeof(m_context.remote_random) });

    m_context.premaster_key.clear();
    if constexpr (TLS_DEBUG) {
        dbgln("master key:");
        print_buffer(m_context.master_key);
    }
    expand_key();
    return true;
}

static bool wildcard_matches(const StringView& host, const StringView& subject)
{
    if (host.matches(subject))
        return true;

    if (subject.starts_with("*."))
        return wildcard_matches(host, subject.substring_view(2));

    return false;
}

Optional<size_t> TLSv12::verify_chain_and_get_matching_certificate(const StringView& host) const
{
    if (m_context.certificates.is_empty() || !m_context.verify_chain())
        return {};

    if (host.is_empty())
        return 0;

    for (size_t i = 0; i < m_context.certificates.size(); ++i) {
        auto& cert = m_context.certificates[i];
        if (wildcard_matches(host, cert.subject.subject))
            return i;
        for (auto& san : cert.SAN) {
            if (wildcard_matches(host, san))
                return i;
        }
    }

    return {};
}

void TLSv12::build_random(PacketBuilder& builder)
{
    u8 random_bytes[48];
    size_t bytes = 48;

    fill_with_random(random_bytes, bytes);

    // remove zeros from the random bytes
    for (size_t i = 0; i < bytes; ++i) {
        if (!random_bytes[i])
            random_bytes[i--] = get_random<u8>();
    }

    if (m_context.is_server) {
        dbgln("Server mode not supported");
        return;
    } else {
        *(u16*)random_bytes = AK::convert_between_host_and_network_endian((u16)Version::V12);
    }

    m_context.premaster_key = ByteBuffer::copy(random_bytes, bytes);

    const auto& certificate_option = verify_chain_and_get_matching_certificate(m_context.extensions.SNI); // if the SNI is empty, we'll make a special case and match *a* leaf certificate.
    if (!certificate_option.has_value()) {
        dbgln("certificate verification failed :(");
        alert(AlertLevel::Critical, AlertDescription::BadCertificate);
        return;
    }

    auto& certificate = m_context.certificates[certificate_option.value()];
    if constexpr (TLS_DEBUG) {
        dbgln("PreMaster secret");
        print_buffer(m_context.premaster_key);
    }

    Crypto::PK::RSA_PKCS1_EME rsa(certificate.public_key.modulus(), 0, certificate.public_key.public_exponent());

    Vector<u8, 32> out;
    out.resize(rsa.output_size());
    auto outbuf = out.span();
    rsa.encrypt(m_context.premaster_key, outbuf);

    if constexpr (TLS_DEBUG) {
        dbgln("Encrypted: ");
        print_buffer(outbuf);
    }

    if (!compute_master_secret(bytes)) {
        dbgln("oh noes we could not derive a master key :(");
        return;
    }

    builder.append_u24(outbuf.size() + 2);
    builder.append((u16)outbuf.size());
    builder.append(outbuf);
}

ByteBuffer TLSv12::build_certificate()
{
    PacketBuilder builder { MessageType::Handshake, m_context.options.version };

    Vector<const Certificate*> certificates;
    Vector<Certificate>* local_certificates = nullptr;

    if (m_context.is_server) {
        dbgln("Unsupported: Server mode");
        VERIFY_NOT_REACHED();
    } else {
        local_certificates = &m_context.client_certificates;
    }

    constexpr size_t der_length_delta = 3;
    constexpr size_t certificate_vector_header_size = 3;

    size_t total_certificate_size = 0;

    for (size_t i = 0; i < local_certificates->size(); ++i) {
        auto& certificate = local_certificates->at(i);
        if (!certificate.der.is_empty()) {
            total_certificate_size += certificate.der.size() + der_length_delta;

            // FIXME: Check for and respond with only the requested certificate types.
            if (true) {
                certificates.append(&certificate);
            }
        }
    }

    builder.append((u8)HandshakeType::CertificateMessage);

    if (!total_certificate_size) {
        dbgln_if(TLS_DEBUG, "No certificates, sending empty certificate message");
        builder.append_u24(certificate_vector_header_size);
        builder.append_u24(total_certificate_size);
    } else {
        builder.append_u24(total_certificate_size + certificate_vector_header_size); // 3 bytes for header
        builder.append_u24(total_certificate_size);

        for (auto& certificate : certificates) {
            if (!certificate->der.is_empty()) {
                builder.append_u24(certificate->der.size());
                builder.append(certificate->der.bytes());
            }
        }
    }
    auto packet = builder.build();
    update_packet(packet);
    return packet;
}

ByteBuffer TLSv12::build_client_key_exchange()
{
    PacketBuilder builder { MessageType::Handshake, m_context.options.version };
    builder.append((u8)HandshakeType::ClientKeyExchange);
    build_random(builder);

    m_context.connection_status = ConnectionStatus::KeyExchange;

    auto packet = builder.build();

    update_packet(packet);

    return packet;
}

}
