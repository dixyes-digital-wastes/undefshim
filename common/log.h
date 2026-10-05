/*
 * How much a message is worth saying, and what it is drawn in
 *
 * The boot writes to two places that do not know about each other - a serial
 * port that may not be there and a screen that may not be there either - and
 * neither of them can be asked to filter. So the level is decided where the
 * message is written, and a message above the configured level is dropped
 * whole rather than byte by byte
 *
 * Its order is what the configuration means by "off", "error" and the rest:
 * UsLogOff writes nothing, UsLogError only failures, and UsLogDebug everything
 *
 * Each level has a colour, and the colours are the levels rather than the
 * subjects: an error is red wherever it is said from, and the tag it is said
 * under says which part was talking
 */

#ifndef US_LOG_H
#define US_LOG_H

typedef enum UsLogLevel_e {
    UsLogOff,
    UsLogError,   /* red */
    UsLogWarn,    /* yellow */
    UsLogInfo,    /* black, which is what a line with no colour is drawn in */
    UsLogVerbose, /* grey */
    UsLogDebug,   /* light cyan */
} UsLogLevel;

#endif
