#ifndef ESDB_ORM_USER_WIRE_HPP_INCLUDED
#define ESDB_ORM_USER_WIRE_HPP_INCLUDED

/*
 * ESDB ORM "user" example: strict U1 wire v1 helpers (C++11).
 *
 * The wire contract is generated in
 * generated/bridge/user_orm_bridge.h. This header implements only that
 * contract; it never interprets caller text as SQL and never passes caller
 * bytes through a printf-style format.
 *
 * Reply shapes:
 *   ok        "U1:O"
 *   none      "U1:N"
 *   error     "U1:E:<status_decimal>:<message_hex_upper>"
 *   handle    "U1:H:<handle_decimal>"
 *   changes   "U1:C:<changes_decimal>"
 *   row       "U1:R:<fieldCount>"  then per field "|<tag>|<valueHex>"
 *   rows      "U1:L:<rowCount>"    then per row "|<fieldCount>" + fields
 *
 * Parameter lanes (ES3 -> native), one string per parameter:
 *   "i:<canonical decimal>" | "t:<hex>" | "r:<decimal>" | "b:<hex>" | "n"
 * Integer parameters are canonical signed 64-bit decimals. Text/blob
 * parameters are uppercase or lowercase even-length hex of the payload bytes
 * (text payloads must be well-formed UTF-8). "n" is the null lane and is
 * rejected by every operation whose generated parameter is non-nullable.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace esdb_orm_user {
namespace wire {

inline bool hex_nibble(char code, int *out) {
    if (code >= '0' && code <= '9') {
        *out = code - '0';
        return true;
    }
    if (code >= 'A' && code <= 'F') {
        *out = 10 + (code - 'A');
        return true;
    }
    if (code >= 'a' && code <= 'f') {
        *out = 10 + (code - 'a');
        return true;
    }
    return false;
}

inline bool hex_decode(const char *text, std::vector<unsigned char> *out) {
    if (text == NULL || out == NULL) {
        return false;
    }
    const std::size_t length = std::strlen(text);
    if ((length & 1u) != 0u) {
        return false;
    }
    out->clear();
    out->reserve(length / 2u);
    for (std::size_t i = 0; i < length; i += 2u) {
        int high = 0;
        int low = 0;
        if (!hex_nibble(text[i], &high) || !hex_nibble(text[i + 1u], &low)) {
            out->clear();
            return false;
        }
        out->push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

inline std::string hex_encode(const unsigned char *data, std::size_t size) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.resize(size * 2u);
    for (std::size_t i = 0; i < size; i += 1u) {
        out[i * 2u] = kDigits[(data[i] >> 4) & 0x0fu];
        out[i * 2u + 1u] = kDigits[data[i] & 0x0fu];
    }
    return out;
}

inline std::string hex_encode(const std::string &bytes) {
    return hex_encode(
        reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
}

/*
 * Canonical signed 64-bit decimal: optional '-', no leading zeros (except the
 * single digit 0), no '+', no whitespace, no "-0".
 */
inline bool canonical_int64(const char *text, std::int64_t *out) {
    if (text == NULL || text[0] == '\0' || out == NULL) {
        return false;
    }
    const char *cursor = text;
    if (*cursor == '-') {
        cursor += 1;
        if (*cursor == '\0') {
            return false;
        }
    }
    if (*cursor == '0') {
        if (cursor[1] != '\0') {
            return false;
        }
        if (cursor != text) {
            return false; /* "-0" is not canonical */
        }
        *out = 0;
        return true;
    }
    if (*cursor < '1' || *cursor > '9') {
        return false;
    }
    cursor += 1;
    while (*cursor != '\0') {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        cursor += 1;
    }
    errno = 0;
    char *end = NULL;
    const long long value = std::strtoll(text, &end, 10);
    if (errno == ERANGE || end == NULL || *end != '\0') {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

inline std::string int64_to_decimal(std::int64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    return std::string(buffer);
}

/* Well-formed UTF-8 required: rejects overlongs, surrogates, and > U+10FFFF. */
inline bool utf8_valid(const unsigned char *bytes, std::size_t size) {
    std::size_t i = 0;
    while (i < size) {
        const unsigned char lead = bytes[i];
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (lead < 0x80u) {
            i += 1u;
            continue;
        }
        if (lead >= 0xc2u && lead <= 0xdfu) {
            extra = 1u;
            code = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            extra = 2u;
            code = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            extra = 3u;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= size) {
            return false;
        }
        for (std::size_t j = 1u; j <= extra; j += 1u) {
            const unsigned char continuation = bytes[i + j];
            if (continuation < 0x80u || continuation > 0xbfu) {
                return false;
            }
            code = (code << 6) | (continuation & 0x3fu);
        }
        if (extra == 2u && (code < 0x800u || (code >= 0xd800u && code <= 0xdfffu))) {
            return false;
        }
        if (extra == 3u && (code < 0x10000u || code > 0x10ffffu)) {
            return false;
        }
        i += extra + 1u;
    }
    return true;
}

inline bool utf8_valid(const std::string &bytes) {
    return utf8_valid(
        reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
}

inline void append_field(std::string *out, char tag, const unsigned char *data, std::size_t size) {
    out->push_back('|');
    out->push_back(tag);
    out->push_back('|');
    if (size > 0u) {
        out->append(hex_encode(data, size));
    }
}

inline void append_field(std::string *out, char tag, const std::string &bytes) {
    append_field(out, tag, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
}

inline void append_integer_field(std::string *out, std::int64_t value) {
    append_field(out, 'i', int64_to_decimal(value));
}

inline void append_null_field(std::string *out) {
    append_field(out, 'n', std::string());
}

inline std::string reply_ok() {
    return std::string("U1:O");
}

inline std::string reply_none() {
    return std::string("U1:N");
}

inline std::string reply_handle(std::int32_t handle) {
    return "U1:H:" + int64_to_decimal(static_cast<std::int64_t>(handle));
}

inline std::string reply_changes(std::int64_t changes) {
    return "U1:C:" + int64_to_decimal(changes);
}

/* status is a decimal ESABI status; message is UTF-8 text that is hex-encoded. */
inline std::string reply_error(int status, const std::string &message) {
    return "U1:E:" + int64_to_decimal(static_cast<std::int64_t>(status)) + ":" + hex_encode(message);
}

} /* namespace wire */
} /* namespace esdb_orm_user */

#endif /* ESDB_ORM_USER_WIRE_HPP_INCLUDED */
