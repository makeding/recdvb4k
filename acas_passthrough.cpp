/*
 * Derived from dantto4k ACAS/MMT-TLV code.
 *
 * Original project: https://github.com/nekohkr/dantto4k.git
 * Local source revision: 141fd49cf674c9c9e3f3c770a20b3291ff292969
 * Original source areas:
 *   - src/acasCard.{h,cpp}
 *   - src/smartCard.{h,cpp}
 *   - src/acasHandler.{h,cpp}
 *   - src/mmtTlvDemuxer.cpp
 *   - src/signalingMessage.{h,cpp}
 *   - src/fragmentAssembler.{h,cpp}
 *   - src/mmtp.{h,cpp}
 *   - src/tlv.{h,cpp}
 *
 * This adapter keeps the ACAS ECM/key and MMT/TLV descrambling path, but writes
 * the original TLV stream back out instead of using dantto4k's MPEG-2 TS remux.
 */
#include "acas_passthrough.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "config.h"

#if defined(HAVE_PCSC_WINSCARD_H) || defined(__APPLE__)
#include <PCSC/winscard.h>
#else
#include <winscard.h>
#endif

#include "aes.h"
#include "sha256.h"

namespace {

constexpr uint8_t kTlvSync = 0x7f;
constexpr uint8_t kTlvHeaderCompressedIpPacket = 0x03;
constexpr uint8_t kMmtpPayloadControlMessage = 0x02;
constexpr uint8_t kMmtTableEcm0 = 0x82;
constexpr uint8_t kMmtTableEcm1 = 0x83;
constexpr uint8_t kScrambleEven = 0x02;
constexpr uint8_t kScrambleOdd = 0x03;
constexpr size_t kMaxTlvPacketSize = 4096;

constexpr uint8_t kMasterKey[] = {
    0x4F, 0x4C, 0x7C, 0xEB, 0x34, 0xFE, 0xB0, 0xA3,
    0x1E, 0x41, 0x19, 0x51, 0xE1, 0x35, 0x15, 0x12,
    0x87, 0xD3, 0x3D, 0x33, 0xD4, 0x9B, 0x4F, 0x52,
    0x05, 0x77, 0xF9, 0xEF, 0xE5, 0x56, 0x1F, 0x32,
};

uint16_t be16(const uint8_t *p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool is_valid_tlv_packet_type(uint8_t packet_type)
{
    return packet_type <= 0x04 || packet_type >= 0xfd;
}

void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

std::vector<uint8_t> apdu_case4(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                                const std::vector<uint8_t>& data, uint8_t le)
{
    std::vector<uint8_t> apdu = { cla, ins, p1, p2, static_cast<uint8_t>(data.size()) };
    apdu.insert(apdu.end(), data.begin(), data.end());
    apdu.push_back(le);
    return apdu;
}

class PcscCard {
public:
    ~PcscCard()
    {
        if (hcard) {
            SCardDisconnect(hcard, SCARD_LEAVE_CARD);
        }
        if (hcontext) {
            SCardReleaseContext(hcontext);
        }
    }

    int32_t reconnect()
    {
        if (!hcard) {
            connect();
            return SCARD_S_SUCCESS;
        }
        return SCardReconnect(hcard, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1,
                              SCARD_RESET_CARD, &active_protocol);
    }

    std::vector<uint8_t> transmit(const std::vector<uint8_t>& message, int32_t *status)
    {
        connect();

        DWORD recv_length = 256;
        std::vector<uint8_t> recv(recv_length);
        int32_t result = SCardTransmit(hcard, SCARD_PCI_T1, message.data(),
                                    static_cast<uint32_t>(message.size()),
                                    nullptr, recv.data(), &recv_length);
        for (int retry = 0; result != SCARD_S_SUCCESS && retry < 5; ++retry) {
            if (result == SCARD_W_RESET_CARD || result == SCARD_E_NOT_TRANSACTED) {
                *status = SCARD_W_RESET_CARD;
                return {};
            }
            recv_length = 256;
            result = SCardTransmit(hcard, SCARD_PCI_T1, message.data(),
                                   static_cast<uint32_t>(message.size()),
                                   nullptr, recv.data(), &recv_length);
        }
        *status = result;
        if (result != SCARD_S_SUCCESS || recv_length < 2) {
            return {};
        }

        recv.resize(recv_length);
        return recv;
    }

    void begin()
    {
        connect();
        SCardBeginTransaction(hcard);
    }

    void end()
    {
        if (hcard) {
            SCardEndTransaction(hcard, SCARD_LEAVE_CARD);
        }
    }

private:
    void init()
    {
        if (hcontext) {
            return;
        }
        int32_t result = SCardEstablishContext(SCARD_SCOPE_USER, nullptr, nullptr, &hcontext);
        if (result != SCARD_S_SUCCESS) {
            throw std::runtime_error("SCardEstablishContext failed");
        }
    }

    bool is_acas_card(SCARDHANDLE card)
    {
        std::default_random_engine engine(std::random_device{}());
        std::uniform_int_distribution<int> distrib(0, 255);

        std::vector<uint8_t> data = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x8A, 0xF7 };
        std::vector<uint8_t> a0init(8);
        for (uint8_t& v : a0init) {
            v = static_cast<uint8_t>(distrib(engine));
        }
        data.insert(data.end(), a0init.begin(), a0init.end());

        std::vector<uint8_t> apdu = apdu_case4(0x90, 0xA0, 0x00, 0x01, data, 0x00);
        DWORD recv_length = 256;
        std::vector<uint8_t> recv(recv_length);
        LONG result = SCardTransmit(card, SCARD_PCI_T1, apdu.data(),
                                    static_cast<DWORD>(apdu.size()),
                                    nullptr, recv.data(), &recv_length);
        if (result != SCARD_S_SUCCESS || recv_length < 0x0e + 0x20 + 2) {
            return false;
        }
        recv.resize(recv_length);
        if (recv[recv.size() - 2] != 0x90 || recv[recv.size() - 1] != 0x00) {
            return false;
        }

        std::vector<uint8_t> a0data(recv.begin(), recv.end() - 2);
        std::vector<uint8_t> a0response(a0data.begin() + 0x06, a0data.begin() + 0x0e);
        std::vector<uint8_t> a0hash(a0data.begin() + 0x0e, a0data.begin() + 0x0e + 0x20);

        std::vector<uint8_t> plain_kcl;
        plain_kcl.insert(plain_kcl.end(), std::begin(kMasterKey), std::end(kMasterKey));
        plain_kcl.insert(plain_kcl.end(), a0init.begin(), a0init.end());
        plain_kcl.insert(plain_kcl.end(), a0response.begin(), a0response.end());
        sha256_t kcl = SHA256::hash(plain_kcl);

        std::vector<uint8_t> plain_data;
        plain_data.insert(plain_data.end(), kcl.begin(), kcl.end());
        plain_data.insert(plain_data.end(), a0init.begin(), a0init.end());
        sha256_t hash = SHA256::hash(plain_data);

        return std::equal(hash.begin(), hash.end(), a0hash.begin());
    }

    std::string find_acas_reader()
    {
        init();
        DWORD size = 0;
        int32_t result = SCardListReaders(hcontext, nullptr, nullptr, &size);
        if (result != SCARD_S_SUCCESS || size == 0) {
            throw std::runtime_error("No smart card readers are available");
        }

        std::vector<char> readers(size);
        result = SCardListReaders(hcontext, nullptr, readers.data(), &size);
        if (result != SCARD_S_SUCCESS || readers.empty() || readers[0] == '\0') {
            throw std::runtime_error("Failed to list smart card readers");
        }

        for (const char *reader = readers.data(); *reader; reader += std::strlen(reader) + 1) {
            SCARDHANDLE candidate = 0;
            DWORD active_protocol = 0;
            result = SCardConnect(hcontext, reader, SCARD_SHARE_SHARED,
                                  SCARD_PROTOCOL_T1, &candidate, &active_protocol);
            if (result != SCARD_S_SUCCESS) {
                continue;
            }

            bool matched = is_acas_card(candidate);
            SCardDisconnect(candidate, SCARD_LEAVE_CARD);
            if (matched) {
                return std::string(reader);
            }
        }

        throw std::runtime_error("No ACAS smart card was found");
    }

    void connect()
    {
        init();
        if (hcard) {
            return;
        }
        reader_name = find_acas_reader();
        int32_t result = SCardConnect(hcontext, reader_name.c_str(), SCARD_SHARE_SHARED,
                                   SCARD_PROTOCOL_T1, &hcard, &active_protocol);
        if (result != SCARD_S_SUCCESS) {
            throw std::runtime_error("Failed to connect to smart card: " + reader_name);
        }
    }

    SCARDCONTEXT hcontext = 0;
    SCARDHANDLE hcard = 0;
    DWORD active_protocol = 0;
    std::string reader_name;
};

class Transaction {
public:
    explicit Transaction(PcscCard& card) : card(card) { card.begin(); }
    ~Transaction() { card.end(); }

private:
    PcscCard& card;
};

class Acas {
public:
    bool on_ecm(const std::vector<uint8_t>& ecm)
    {
        if (ecm == last_ecm) {
            return true;
        }
        last_ecm = ecm;
        return update_key(ecm);
    }

    bool decrypt_payload(uint8_t scramble_flag,
                         uint16_t packet_id,
                         uint32_t packet_sequence_number,
                         uint8_t *payload,
                         size_t payload_size)
    {
        if (payload_size <= 8) {
            return false;
        }

        const std::array<uint8_t, 16> *key_ptr = nullptr;
        if (scramble_flag == kScrambleEven) {
            key_ptr = &even_key;
        } else if (scramble_flag == kScrambleOdd) {
            key_ptr = &odd_key;
        }
        if (!key_ptr || !has_key) {
            return false;
        }

        std::array<uint8_t, 16> iv{};
        put_be16(iv.data(), packet_id);
        put_be32(iv.data() + 2, packet_sequence_number);

        AES_ctx ctx;
        AES_init_ctx_iv(&ctx, key_ptr->data(), iv.data());
        AES_CTR_xcrypt_buffer(&ctx, payload + 8, payload_size - 8);
        return true;
    }

private:
    bool update_key(const std::vector<uint8_t>& ecm)
    {
        if (ecm.size() < 0x1b) {
            return false;
        }

        for (int retry = 0; retry < 5; ++retry) {
            try {
                Transaction transaction(card);
                sha256_t kcl;
                if (!get_a0_auth_kcl(kcl)) {
                    return false;
                }

                int32_t status = SCARD_S_SUCCESS;
                auto response = card.transmit(apdu_case4(0x90, 0x34, 0x00, 0x01, ecm, 0x00), &status);
                if (status == SCARD_W_RESET_CARD) {
                    card.reconnect();
                    continue;
                }
                if (status != SCARD_S_SUCCESS || response.size() < 0x06 + 32 + 2 ||
                    response[response.size() - 2] != 0x90 || response[response.size() - 1] != 0x00) {
                    return false;
                }

                std::vector<uint8_t> ecm_response(response.begin() + 0x06, response.end() - 2);
                std::vector<uint8_t> ecm_init(ecm.begin() + 0x04, ecm.begin() + 0x04 + 0x17);
                std::vector<uint8_t> plain_data(kcl.begin(), kcl.end());
                plain_data.insert(plain_data.end(), ecm_init.begin(), ecm_init.end());
                sha256_t hash = SHA256::hash(plain_data);
                if (ecm_response.size() < hash.size()) {
                    return false;
                }
                for (size_t i = 0; i < hash.size(); ++i) {
                    hash[i] ^= ecm_response[i];
                }
                std::copy(hash.begin(), hash.begin() + 0x10, odd_key.begin());
                std::copy(hash.begin() + 0x10, hash.begin() + 0x20, even_key.begin());
                has_key = true;
                return true;
            } catch (const std::runtime_error& e) {
                std::cerr << e.what() << std::endl;
                return false;
            }
        }
        return false;
    }

    bool get_a0_auth_kcl(sha256_t& output)
    {
        std::default_random_engine engine(std::random_device{}());
        std::uniform_int_distribution<int> distrib(0, 255);

        std::vector<uint8_t> data = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x8A, 0xF7 };
        std::vector<uint8_t> a0init(8);
        for (uint8_t& b : a0init) {
            b = static_cast<uint8_t>(distrib(engine));
        }
        data.insert(data.end(), a0init.begin(), a0init.end());

        int32_t status = SCARD_S_SUCCESS;
        auto response = card.transmit(apdu_case4(0x90, 0xA0, 0x00, 0x01, data, 0x00), &status);
        if (status != SCARD_S_SUCCESS || response.size() < 0x0e + 32 + 2 ||
            response[response.size() - 2] != 0x90 || response[response.size() - 1] != 0x00) {
            return false;
        }

        std::vector<uint8_t> a0data(response.begin(), response.end() - 2);
        if (a0data.size() < 0x0e + 32) {
            return false;
        }
        std::vector<uint8_t> a0response(a0data.begin() + 0x06, a0data.begin() + 0x0e);
        std::vector<uint8_t> a0hash(a0data.begin() + 0x0e, a0data.end());

        std::vector<uint8_t> plain_kcl(std::begin(kMasterKey), std::end(kMasterKey));
        plain_kcl.insert(plain_kcl.end(), a0init.begin(), a0init.end());
        plain_kcl.insert(plain_kcl.end(), a0response.begin(), a0response.end());
        sha256_t kcl = SHA256::hash(plain_kcl);

        std::vector<uint8_t> plain_data(kcl.begin(), kcl.end());
        plain_data.insert(plain_data.end(), a0init.begin(), a0init.end());
        sha256_t hash = SHA256::hash(plain_data);
        if (a0hash.size() < hash.size() || !std::equal(hash.begin(), hash.end(), a0hash.begin())) {
            return false;
        }

        output = kcl;
        return true;
    }

    PcscCard card;
    std::vector<uint8_t> last_ecm;
    std::array<uint8_t, 16> odd_key{};
    std::array<uint8_t, 16> even_key{};
    bool has_key = false;
};

struct MmtpInfo {
    uint16_t packet_id = 0;
    uint32_t sequence = 0;
    uint8_t payload_type = 0;
    uint8_t scramble_flag = 0;
    size_t payload_offset = 0;
    size_t payload_size = 0;
};

bool parse_mmtp(uint8_t *data, size_t size, MmtpInfo& out)
{
    if (size < 12) {
        return false;
    }

    const bool packet_counter = (data[0] & 0x20) != 0;
    const bool extension_header = (data[0] & 0x02) != 0;
    out.payload_type = data[1] & 0x3f;
    out.packet_id = be16(data + 2);
    out.sequence = be32(data + 8);

    size_t pos = 12;
    if (packet_counter) {
        if (size < pos + 4) {
            return false;
        }
        pos += 4;
    }

    if (extension_header) {
        if (size < pos + 4) {
            return false;
        }
        const uint16_t extension_length = be16(data + pos + 2);
        pos += 4;
        if (size < pos + extension_length) {
            return false;
        }

        if (extension_length >= 5 && (be16(data + pos) & 0x7fff) == 0x0001) {
            out.scramble_flag = (data[pos + 4] & 0x18) >> 3;
        }
        pos += extension_length;
    }

    out.payload_offset = pos;
    out.payload_size = size - pos;
    return true;
}

bool parse_ecm_table(const uint8_t *data, size_t size, std::vector<uint8_t>& ecm)
{
    if (size < 10 || (data[0] != kMmtTableEcm0 && data[0] != kMmtTableEcm1)) {
        return false;
    }
    const uint16_t section_length = be16(data + 1) & 0x0fff;
    (void)section_length;
    if (size < 10) {
        return false;
    }
    const size_t ecm_offset = 8;
    if (size < ecm_offset + 4) {
        return false;
    }
    ecm.assign(data + ecm_offset, data + size - 4);
    return !ecm.empty();
}

void process_signaling_message(const uint8_t *data, size_t size, Acas& acas)
{
    if (size < 5) {
        return;
    }
    const uint16_t message_id = be16(data);
    if (message_id != 0x8000 && message_id != 0x8001 && message_id != 0x8002 && message_id != 0x8003) {
        return;
    }

    std::vector<uint8_t> ecm;
    if (parse_ecm_table(data + 5, size - 5, ecm)) {
        acas.on_ecm(ecm);
    }
}

void process_signaling_payload(uint16_t packet_id, const uint8_t *payload, size_t size,
                               std::map<uint16_t, std::vector<uint8_t>>& fragments,
                               Acas& acas)
{
    if (size < 2) {
        return;
    }
    const uint8_t fragmentation = (payload[0] & 0xc0) >> 6;
    const bool length_extension = (payload[0] & 0x02) != 0;
    const bool aggregation = (payload[0] & 0x01) != 0;
    const uint8_t *message_payload = payload + 2;
    size_t message_size = size - 2;

    auto handle_message = [&](const uint8_t *msg, size_t msg_size) {
        if (fragmentation == 0) {
            process_signaling_message(msg, msg_size, acas);
        } else if (fragmentation == 1) {
            fragments[packet_id].assign(msg, msg + msg_size);
        } else if (fragmentation == 2) {
            auto& fragment = fragments[packet_id];
            fragment.insert(fragment.end(), msg, msg + msg_size);
        } else if (fragmentation == 3) {
            auto& fragment = fragments[packet_id];
            fragment.insert(fragment.end(), msg, msg + msg_size);
            process_signaling_message(fragment.data(), fragment.size(), acas);
            fragments.erase(packet_id);
        }
    };

    if (!aggregation) {
        handle_message(message_payload, message_size);
        return;
    }

    if (fragmentation != 0) {
        return;
    }
    size_t pos = 0;
    while (pos < message_size) {
        size_t length_size = length_extension ? 4 : 2;
        if (message_size - pos < length_size) {
            break;
        }
        uint32_t length = length_extension ? be32(message_payload + pos) : be16(message_payload + pos);
        pos += length_size;
        if (message_size - pos < length) {
            break;
        }
        process_signaling_message(message_payload + pos, length, acas);
        pos += length;
    }
}

class Passthrough {
public:
    int process(const uint8_t *data, size_t size, acas_output_callback output, void *opaque)
    {
        buffer.insert(buffer.end(), data, data + size);
        return drain(output, opaque, false);
    }

    int flush(acas_output_callback output, void *opaque)
    {
        return drain(output, opaque, true);
    }

private:
    int drain(acas_output_callback output, void *opaque, bool flush)
    {
        while (!buffer.empty()) {
            if (buffer[0] != kTlvSync) {
                auto it = std::find(buffer.begin() + 1, buffer.end(), kTlvSync);
                size_t raw_size = it == buffer.end() ? buffer.size() : static_cast<size_t>(it - buffer.begin());
                if (output(opaque, buffer.data(), raw_size) < 0) {
                    return -1;
                }
                buffer.erase(buffer.begin(), buffer.begin() + raw_size);
                continue;
            }

            if (buffer.size() < 4) {
                break;
            }
            if (!is_valid_tlv_packet_type(buffer[1])) {
                if (output(opaque, buffer.data(), 1) < 0) {
                    return -1;
                }
                buffer.erase(buffer.begin());
                continue;
            }

            const size_t packet_size = 4 + be16(buffer.data() + 2);
            if (packet_size > kMaxTlvPacketSize) {
                if (output(opaque, buffer.data(), 1) < 0) {
                    return -1;
                }
                buffer.erase(buffer.begin());
                continue;
            }
            if (buffer.size() < packet_size) {
                break;
            }

            std::vector<uint8_t> packet(buffer.begin(), buffer.begin() + packet_size);
            process_tlv(packet.data(), packet.size());
            if (output(opaque, packet.data(), packet.size()) < 0) {
                return -1;
            }
            buffer.erase(buffer.begin(), buffer.begin() + packet_size);
        }

        if (flush && !buffer.empty()) {
            if (output(opaque, buffer.data(), buffer.size()) < 0) {
                return -1;
            }
            buffer.clear();
        }
        return 0;
    }

    void process_tlv(uint8_t *packet, size_t packet_size)
    {
        if (packet_size < 7 || packet[1] != kTlvHeaderCompressedIpPacket) {
            return;
        }

        uint8_t *tlv_data = packet + 4;
        size_t tlv_size = packet_size - 4;
        if (tlv_size < 3) {
            return;
        }

        size_t pos = 3;
        const uint8_t header_type = tlv_data[2];
        if (header_type == 0x60) {
            pos += 42;
        }
        if (tlv_size <= pos) {
            return;
        }

        uint8_t *mmtp = tlv_data + pos;
        size_t mmtp_size = tlv_size - pos;
        MmtpInfo info;
        if (!parse_mmtp(mmtp, mmtp_size, info)) {
            return;
        }

        uint8_t *payload = mmtp + info.payload_offset;
        if (info.payload_type == kMmtpPayloadControlMessage) {
            process_signaling_payload(info.packet_id, payload, info.payload_size, fragments, acas);
        }

        if (info.scramble_flag == kScrambleEven || info.scramble_flag == kScrambleOdd) {
            if (!acas.decrypt_payload(info.scramble_flag, info.packet_id, info.sequence,
                                      payload, info.payload_size)) {
                return;
            }
        }
    }

    Acas acas;
    std::vector<uint8_t> buffer;
    std::map<uint16_t, std::vector<uint8_t>> fragments;
};

} // namespace

struct acas_passthrough {
    Passthrough impl;
};

extern "C" acas_passthrough_t *acas_passthrough_create(void)
{
    try {
        return new acas_passthrough();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return nullptr;
    }
}

extern "C" void acas_passthrough_destroy(acas_passthrough_t *ctx)
{
    delete ctx;
}

extern "C" int acas_passthrough_process(acas_passthrough_t *ctx,
                                         const uint8_t *data,
                                         size_t size,
                                         acas_output_callback output,
                                         void *opaque)
{
    if (!ctx || !output) {
        return -1;
    }
    return ctx->impl.process(data, size, output, opaque);
}

extern "C" int acas_passthrough_flush(acas_passthrough_t *ctx,
                                       acas_output_callback output,
                                       void *opaque)
{
    if (!ctx || !output) {
        return -1;
    }
    return ctx->impl.flush(output, opaque);
}
