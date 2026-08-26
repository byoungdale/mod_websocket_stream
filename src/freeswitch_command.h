#ifndef MWS_FREESWITCH_COMMAND_H
#define MWS_FREESWITCH_COMMAND_H

#include <stdbool.h>

typedef struct {
	char *name;
	char *arguments;
} mws_freeswitch_command_t;

bool mws_freeswitch_command_parse(
		char *text,
		mws_freeswitch_command_t *command);

bool mws_freeswitch_command_would_stop_module_or_server(
		const mws_freeswitch_command_t *command);

#endif
