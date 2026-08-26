#include "websocket_protocol.h"

bool mws_websocket_close_code_is_valid(uint16_t close_code)
{
	if (close_code >= 3000 && close_code <= 4999) {
		return true;
	}

	switch (close_code) {
	case 1000:
	case 1001:
	case 1002:
	case 1003:
	case 1007:
	case 1008:
	case 1009:
	case 1011:
	case 1012:
	case 1013:
	case 1014:
		return true;
	default:
		return false;
	}
}

bool mws_websocket_text_is_valid_utf8(
		const uint8_t *data,
		size_t data_length)
{
	size_t offset = 0;

	while (offset < data_length) {
		uint8_t first = data[offset++];

		if (first <= 0x7f) {
			continue;
		}
		if (first >= 0xc2 && first <= 0xdf) {
			if (offset >= data_length ||
					(data[offset++] & 0xc0) != 0x80) {
				return false;
			}
			continue;
		}
		if (first >= 0xe0 && first <= 0xef) {
			uint8_t second;

			if (data_length - offset < 2) {
				return false;
			}
			second = data[offset++];
			if ((second & 0xc0) != 0x80 ||
					(first == 0xe0 && second < 0xa0) ||
					(first == 0xed && second >= 0xa0) ||
					(data[offset++] & 0xc0) != 0x80) {
				return false;
			}
			continue;
		}
		if (first >= 0xf0 && first <= 0xf4) {
			uint8_t second;

			if (data_length - offset < 3) {
				return false;
			}
			second = data[offset++];
			if ((second & 0xc0) != 0x80 ||
					(first == 0xf0 && second < 0x90) ||
					(first == 0xf4 && second >= 0x90) ||
					(data[offset++] & 0xc0) != 0x80 ||
					(data[offset++] & 0xc0) != 0x80) {
				return false;
			}
			continue;
		}

		return false;
	}

	return true;
}

bool mws_websocket_close_payload_is_valid(
		const uint8_t *payload,
		size_t payload_length)
{
	uint16_t close_code;

	if (payload_length > MWS_WEBSOCKET_MAXIMUM_CLOSE_PAYLOAD_BYTES) {
		return false;
	}
	if (!payload_length) {
		return true;
	}
	if (payload_length == 1) {
		return false;
	}

	close_code = (uint16_t) (((uint16_t) payload[0] << 8) | payload[1]);
	if (!mws_websocket_close_code_is_valid(close_code)) {
		return false;
	}

	return mws_websocket_text_is_valid_utf8(
			payload + 2,
			payload_length - 2);
}
