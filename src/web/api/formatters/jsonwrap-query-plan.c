// SPDX-License-Identifier: GPL-3.0-or-later

#include "jsonwrap.h"
#include "jsonwrap-internal.h"

void jsonwrap_query_metric_plan(BUFFER *wb, QUERY_METRIC *qm, RRDR_OPTIONS options) {
    buffer_json_member_add_array(wb, "plans");
    for (size_t p = 0; p < qm->plan.used; p++) {
        QUERY_PLAN_ENTRY *qp = &qm->plan.array[p];

        buffer_json_add_array_item_object(wb);
        buffer_json_member_add_uint64(wb, JSKEY(tier), qp->tier);
        buffer_json_member_add_time_t_formatted(wb, JSKEY(after), qp->after, options & RRDR_OPTION_RFC3339);
        buffer_json_member_add_time_t_formatted(wb, JSKEY(before), qp->before, options & RRDR_OPTION_RFC3339);
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);

    buffer_json_member_add_array(wb, "tiers");
    for (size_t tier = 0; tier < nd_profile.storage_tiers; tier++) {
        buffer_json_add_array_item_object(wb);
        buffer_json_member_add_uint64(wb, JSKEY(tier), tier);
        buffer_json_member_add_time_t_formatted(wb, JSKEY(first_entry), qm->tiers[tier].db_first_time_s, options & RRDR_OPTION_RFC3339);
        buffer_json_member_add_time_t_formatted(wb, JSKEY(last_entry), qm->tiers[tier].db_last_time_s, options & RRDR_OPTION_RFC3339);
        buffer_json_member_add_int64(wb, JSKEY(weight), qm->tiers[tier].weight);
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);
}

void jsonwrap_query_plan(RRDR *r, BUFFER *wb, RRDR_OPTIONS options) {
    QUERY_TARGET *qt = r->internal.qt;

    buffer_json_member_add_object(wb, "query_plan");
    for(size_t m = 0; m < qt->query.used; m++) {
        QUERY_METRIC *qm = query_metric(qt, m);
        buffer_json_member_add_object(wb, query_metric_id(qt, qm));
        jsonwrap_query_metric_plan(wb, qm, options);
        buffer_json_object_close(wb);
    }
    buffer_json_object_close(wb);
}

