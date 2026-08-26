#include "freeswitch_command.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#define MWS_MODULE_NAME "mod_websocket_stream"
#define MWS_MODULE_FILENAME MWS_MODULE_NAME ".so"

static unsigned char ascii_to_lower(unsigned char character)
{
	if (character >= 'A' && character <= 'Z') {
		return (unsigned char) (character + ('a' - 'A'));
	}

	return character;
}

static bool ascii_strings_are_equal(const char *left, const char *right)
{
	if (!left || !right) {
		return false;
	}

	while (*left && *right) {
		if (ascii_to_lower((unsigned char) *left) !=
				ascii_to_lower((unsigned char) *right)) {
			return false;
		}
		left++;
		right++;
	}

	return *left == *right;
}

static bool ascii_token_is_equal(
		const char *token,
		size_t token_length,
		const char *value)
{
	size_t index;

	if (!token || !value || strlen(value) != token_length) {
		return false;
	}
	for (index = 0; index < token_length; index++) {
		if (ascii_to_lower((unsigned char) token[index]) !=
				ascii_to_lower((unsigned char) value[index])) {
			return false;
		}
	}

	return true;
}

static bool text_starts_with_word(
		const char *text,
		const char *word)
{
	size_t word_length;
	size_t index;

	if (!text || !*text || !word || !*word) {
		return false;
	}
	while (*text && isspace((unsigned char) *text)) {
		text++;
	}
	word_length = strlen(word);
	for (index = 0; index < word_length; index++) {
		if (!text[index] ||
				ascii_to_lower((unsigned char) text[index]) !=
				ascii_to_lower((unsigned char) word[index])) {
			return false;
		}
	}

	return !text[word_length] ||
			isspace((unsigned char) text[word_length]);
}

static bool arguments_name_this_module(const char *arguments)
{
	const char *token = arguments;

	while (token && *token) {
		const char *token_end;
		size_t token_length;

		while (*token && isspace((unsigned char) *token)) {
			token++;
		}
		if (!*token) {
			break;
		}
		token_end = token;
		while (*token_end && !isspace((unsigned char) *token_end)) {
			token_end++;
		}
		token_length = (size_t) (token_end - token);
		if (ascii_token_is_equal(
					token,
					token_length,
					MWS_MODULE_NAME) ||
				ascii_token_is_equal(
					token,
					token_length,
					MWS_MODULE_FILENAME)) {
			return true;
		}
		token = token_end;
	}

	return false;
}

bool mws_freeswitch_command_parse(
		char *text,
		mws_freeswitch_command_t *command)
{
	char *arguments;

	if (!command) {
		return false;
	}
	command->name = NULL;
	command->arguments = NULL;
	if (!text) {
		return false;
	}

	while (*text && isspace((unsigned char) *text)) {
		text++;
	}
	if (!*text) {
		return false;
	}

	command->name = text;
	arguments = text;
	while (*arguments && !isspace((unsigned char) *arguments)) {
		arguments++;
	}
	if (!*arguments) {
		return true;
	}

	*arguments++ = '\0';
	while (*arguments && isspace((unsigned char) *arguments)) {
		arguments++;
	}
	if (*arguments) {
		command->arguments = arguments;
	}

	return true;
}

bool mws_freeswitch_command_would_stop_module_or_server(
		const mws_freeswitch_command_t *command)
{
	if (!command || !command->name) {
		return false;
	}
	if (ascii_strings_are_equal(command->name, "shutdown")) {
		return true;
	}
	if (ascii_strings_are_equal(command->name, "fsctl") &&
			text_starts_with_word(command->arguments, "shutdown")) {
		return true;
	}
	if ((ascii_strings_are_equal(command->name, "unload") ||
			ascii_strings_are_equal(command->name, "reload")) &&
			arguments_name_this_module(command->arguments)) {
		return true;
	}

	return false;
}
