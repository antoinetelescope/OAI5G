#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include "utils.h"

/****************************************************************************
 **                                                                        **
 ** Name:        hex_char_to_hex_value()                                   **
 **                                                                        **
 ** Description: Converts an hexadecimal ASCII coded digit into its value. **
 **                                                                        **
 ** Inputs:      c:             A char holding the ASCII coded value       **
 **              Others:        None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **              Return:        Converted value (-1 on error)              **
 **              Others:        None                                       **
 **                                                                        **
 ***************************************************************************/
int hex_char_to_hex_value (char c)
{
  if (!((c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F') ||
        (c >= '0' && c <= '9')))
    return -1;

  if (c >= 'A') {
    /* Remove case bit */
    c &= ~('a' ^ 'A');

    return (c - 'A' + 10);
  } else {
    return (c - '0');
  }
}

/****************************************************************************
 **                                                                        **
 ** Name:        hex_string_to_hex_value()                                 **
 **                                                                        **
 ** Description: Converts an hexadecimal ASCII coded string into its value.**
 **                                                                        **
 ** Inputs:      hex_value:     A pointer to the location to store the     **
 **                             conversion result                          **
 **              size:          The size of hex_value in bytes             **
 **              Others:        None                                       **
 **                                                                        **
 ** Outputs:     hex_value:     Converted value                            **
 **              Return:        0 on success, -1 on error                  **
 **              Others:        None                                       **
 **                                                                        **
 ***************************************************************************/
int hex_string_to_hex_value (uint8_t *hex_value, const char *hex_string, int size)
{
  int i;

  if (strlen(hex_string) != size*2) {
    fprintf(stderr, "the string '%s' should be of length %d\n", hex_string, size*2);
    return -1;
  }

  for (i=0; i < size; i++) {
    int a = hex_char_to_hex_value(hex_string[2 * i]);
    int b = hex_char_to_hex_value(hex_string[2 * i + 1]);
    if (a == -1 || b == -1) goto error;
    hex_value[i] = (a << 4) | b;
  }
  return 0;

error:
  fprintf(stderr, "the string '%s' is not a valid hexadecimal string\n", hex_string);
  for (i=0; i < size; i++)
    hex_value[i] = 0;
  return -1;
}

char *itoa(int i) {
  char buffer[64];
  int ret;

  ret = snprintf(buffer, sizeof(buffer), "%d",i);
  if ( ret <= 0 ) {
    return NULL;
  }

  return strdup(buffer);
}

/**
 * @brief Convert a version string x.y.z into numbers.
 *
 * The function takes a version string of format "x.y.z" where x is the major
 * version number, y minor, z patch. It tries to match version, and outputs the
 * numbers in the correspondingly named variables.
 *
 * @return The number of version parts matched (should be three on x.y.z).
 */
int read_version(const char *version, uint8_t *major, uint8_t *minor, uint8_t *patch)
{
  int ret = sscanf(version, "%hhu.%hhu.%hhu", major, minor, patch);
  // EOF means "end of input reached or matching failure"
  if (ret == EOF)
    return 3;
  return ret; // ret has number of items matched
}
