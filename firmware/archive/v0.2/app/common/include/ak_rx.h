/**
 * @file ak_rx.h
 * @brief Line assembler + tolerant field extractors for inbound JSONL
 *        (PC -> board). Pure, host-testable; not a full JSON parser.
 */

#ifndef AK_RX_H
#define AK_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_RX_LINE_MAX 256U

typedef void (*ak_rx_line_cb)(const char *line);

typedef struct {
    char   buf[AK_RX_LINE_MAX];
    size_t len;
    bool   overflow; /* current line exceeded the buffer; dropped until newline */
} ak_rx_reader_t;

void ak_rx_reader_init(ak_rx_reader_t *reader);

/**
 * Feed raw received bytes. For each complete '\n'-terminated line, invokes
 * cb with a NUL-terminated string (newline stripped). Overlong lines are
 * dropped (never truncated-and-delivered).
 */
void ak_rx_reader_feed(ak_rx_reader_t *reader, const uint8_t *data, size_t n, ak_rx_line_cb cb);

/** Extract a string field `"key":"value"`. Returns false if absent. */
bool ak_json_str(const char *json, const char *key, char *out, size_t out_size);

/** Extract an unsigned integer field `"key":123`. Returns false if absent. */
bool ak_json_uint(const char *json, const char *key, uint32_t *out);

#endif /* AK_RX_H */
