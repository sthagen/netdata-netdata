// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_RRDR_OPTIONS_H
#define NETDATA_RRDR_OPTIONS_H

#include "libnetdata/libnetdata.h"

typedef enum rrdr_options {
    RRDR_OPTION_NONZERO         = (1ULL << 0), // don't output dimensions with just zero values
    RRDR_OPTION_REVERSED        = (1ULL << 1), // output the rows in reverse order (oldest to newest)
    RRDR_OPTION_ABSOLUTE        = (1ULL << 2), // values positive, for DATASOURCE_SSV before summing
    RRDR_OPTION_DIMS_MIN2MAX    = (1ULL << 3), // when adding dimensions, use max - min, instead of sum
    RRDR_OPTION_DIMS_AVERAGE    = (1ULL << 4), // when adding dimensions, use average, instead of sum
    RRDR_OPTION_DIMS_MIN        = (1ULL << 5), // when adding dimensions, use minimum, instead of sum
    RRDR_OPTION_DIMS_MAX        = (1ULL << 6), // when adding dimensions, use maximum, instead of sum
    RRDR_OPTION_SECONDS         = (1ULL << 7), // output seconds, instead of dates
    RRDR_OPTION_MILLISECONDS    = (1ULL << 8), // output milliseconds, instead of dates
    RRDR_OPTION_NULL2ZERO       = (1ULL << 9), // do not show nulls, convert them to zeros
    RRDR_OPTION_OBJECTSROWS     = (1ULL << 10), // each row of values should be an object, not an array
    RRDR_OPTION_GOOGLE_JSON     = (1ULL << 11), // comply with google JSON/JSONP specs
    RRDR_OPTION_JSON_WRAP       = (1ULL << 12), // wrap the response in a JSON header with info about the result
    RRDR_OPTION_LABEL_QUOTES    = (1ULL << 13), // in CSV output, wrap header labels in double quotes
    RRDR_OPTION_PERCENTAGE      = (1ULL << 14), // give values as percentage of total
    RRDR_OPTION_NOT_ALIGNED     = (1ULL << 15), // do not align charts for persistent timeframes
    RRDR_OPTION_DISPLAY_ABS     = (1ULL << 16), // for badges, display the absolute value, but calculate colors with sign
    RRDR_OPTION_MATCH_IDS       = (1ULL << 17), // when filtering dimensions, match only IDs
    RRDR_OPTION_MATCH_NAMES     = (1ULL << 18), // when filtering dimensions, match only names
    RRDR_OPTION_NATURAL_POINTS  = (1ULL << 19), // return the natural points of the database
    RRDR_OPTION_VIRTUAL_POINTS  = (1ULL << 20), // return virtual points
    RRDR_OPTION_ANOMALY_BIT     = (1ULL << 21), // Return the anomaly bit stored in each collected_number
    RRDR_OPTION_RETURN_RAW      = (1ULL << 22), // Return raw data for aggregating across multiple nodes
    RRDR_OPTION_RETURN_JWAR     = (1ULL << 23), // Return anomaly rates in jsonwrap
    RRDR_OPTION_SELECTED_TIER   = (1ULL << 24), // Use the selected tier for the query
    RRDR_OPTION_ALL_DIMENSIONS  = (1ULL << 25), // Return the full dimensions list
    RRDR_OPTION_SHOW_DETAILS    = (1ULL << 26), // v2 returns detailed object tree
    RRDR_OPTION_DEBUG           = (1ULL << 27), // v2 returns request description
    RRDR_OPTION_MINIFY          = (1ULL << 28), // remove JSON spaces and newlines from JSON output
    RRDR_OPTION_GROUP_BY_LABELS = (1ULL << 29), // v2 returns flattened labels per dimension of the chart
    RRDR_OPTION_MINIMAL_STATS   = (1ULL << 30), // Remove "totals" and statistics fields (qr, sl, ex) from response
    RRDR_OPTION_LONG_JSON_KEYS  = (1ULL << 31), // Use descriptive long JSON keys instead of cryptic short ones
    RRDR_OPTION_MCP_INFO        = (1ULL << 32), // Include "info" nodes in JSON response sections
    RRDR_OPTION_RFC3339         = (1ULL << 33), // Return timestamps in RFC3339 format instead of epoch seconds

    // internal ones - not to be exposed to the API
    RRDR_OPTION_INTERNAL_AR     = (1ULL << 63), // internal use only, to let the formatters know we want to render the anomaly rate
} RRDR_OPTIONS;

static_assert(sizeof(RRDR_OPTIONS) == 8, "RRDR_OPTIONS must be 8 bytes");

void rrdr_options_to_buffer(BUFFER *wb, RRDR_OPTIONS options);
void rrdr_options_to_buffer_json_array(BUFFER *wb, const char *key, RRDR_OPTIONS options);
void web_client_api_request_data_vX_options_to_string(char *buf, size_t size, RRDR_OPTIONS options);
void rrdr_options_init(void);

RRDR_OPTIONS rrdr_options_parse(const char *options_str);
RRDR_OPTIONS rrdr_options_parse_one(const char *o);

#endif //NETDATA_RRDR_OPTIONS_H
