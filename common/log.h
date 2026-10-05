/*
 * How much a message is worth saying
 *
 * The boot writes to two places that do not know about each other - a serial
 * port that may not be there and a screen that may not be there either - and
 * neither of them can be asked to filter. So the level is decided where the
 * message is written, and a message above the configured level is dropped
 * whole rather than byte by byte
 *
 * Its order is what the configuration means by "off", "error" and the rest:
 * UsLogOff writes nothing, UsLogError only failures, and UsLogDebug everything
 */

#ifndef US_LOG_H
#define US_LOG_H

typedef enum UsLogLevel_e {
    UsLogOff,
    UsLogError,
    UsLogWarn,
    UsLogInfo,
    UsLogVerbose,
    UsLogDebug,
} UsLogLevel;

#endif
