#ifndef _SWAY_LOCK_H
#define _SWAY_LOCK_H
#include <stdbool.h>

void arrange_locks(void);

/**
 * Locks the session with lock_command, started as a privileged client. Does
 * nothing while a live locker holds the lock or is starting up. Returns false
 * if the locker couldn't be started.
 */
bool sway_session_lock_request(void);

#endif
