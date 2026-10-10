/*
 * mcme exit codes.
 *
 * STABLE INTERFACE. Scripts and agents branch on these numbers. Add new codes
 * at the end; never renumber. Documented in wiki/cli.md.
 */

#ifndef MCME_CLI_H
#define MCME_CLI_H

typedef enum {
    MCME_OK                  = 0,  /* success */
    MCME_EXIT_USAGE          = 1,  /* bad command line */
    MCME_EXIT_NOT_FOUND      = 2,  /* path, world, or key does not exist */
    MCME_EXIT_BAD_FORMAT     = 3,  /* exists but is not the expected format */
    MCME_EXIT_IO             = 4,  /* read/write failed */
    MCME_EXIT_NOT_IMPLEMENTED= 5,  /* recognised but not built yet */
    MCME_EXIT_INTERNAL       = 6,  /* should not happen; a bug */
} McmeExit;

#endif /* MCME_CLI_H */
