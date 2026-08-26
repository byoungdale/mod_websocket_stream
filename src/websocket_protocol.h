#ifndef MWS_WEBSOCKET_PROTOCOL_H
#define MWS_WEBSOCKET_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MWS_WEBSOCKET_NORMAL_CLOSE_CODE 1000
#define MWS_WEBSOCKET_MAXIMUM_CLOSE_PAYLOAD_BYTES 125

bool mws_websocket_close_code_is_valid(uint16_t close_code);

bool mws_websocket_text_is_valid_utf8(
		const uint8_t *data,
		size_t data_length);

bool mws_websocket_close_payload_is_valid(
		const uint8_t *payload,
		size_t payload_length);

#endif
