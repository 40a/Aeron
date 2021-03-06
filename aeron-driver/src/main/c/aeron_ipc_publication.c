/*
 * Copyright 2014 - 2017 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <errno.h>
#include <concurrent/aeron_counters_manager.h>
#include "aeron_ipc_publication.h"
#include "util/aeron_fileutil.h"
#include "aeron_alloc.h"
#include "protocol/aeron_udp_protocol.h"

int aeron_ipc_publication_create(
    aeron_ipc_publication_t **publication,
    aeron_driver_context_t *context,
    int32_t session_id,
    int32_t stream_id,
    int64_t registration_id,
    aeron_position_t *pub_lmt_position,
    int32_t initial_term_id,
    size_t term_buffer_length,
    size_t mtu_length,
    bool is_exclusive)
{
    char path[AERON_MAX_PATH];
    int path_length =
        aeron_ipc_publication_location(path, sizeof(path), context->aeron_dir, session_id, stream_id, registration_id);
    aeron_ipc_publication_t *_pub = NULL;
    const uint64_t usable_fs_space = context->usable_fs_space_func(context->aeron_dir);
    const uint64_t log_length = AERON_LOGBUFFER_COMPUTE_LOG_LENGTH(term_buffer_length);

    *publication = NULL;

    if (usable_fs_space < log_length)
    {
        errno = ENOSPC;
        return -1;
    }

    if (aeron_alloc((void **)&_pub, sizeof(aeron_ipc_publication_t)) < 0)
    {
        return -1;
    }

    _pub->log_file_name = NULL;
    if (aeron_alloc((void **)(&_pub->log_file_name), (size_t)path_length) < 0)
    {
        aeron_free(_pub);
        return -1;
    }

    if (context->map_raw_log_func(&_pub->mapped_raw_log, path, context->term_buffer_sparse_file, term_buffer_length) < 0)
    {
        aeron_free(_pub->log_file_name);
        aeron_free(_pub);
        return -1;
    }
    _pub->map_raw_log_close_func = context->map_raw_log_close_func;

    strncpy(_pub->log_file_name, path, path_length);
    _pub->log_file_name_length = (size_t)path_length;
    _pub->log_meta_data = (aeron_logbuffer_metadata_t *)(_pub->mapped_raw_log.log_meta_data.addr);

    _pub->log_meta_data->term_tail_counters[0] = (int64_t)initial_term_id << 32;
    _pub->log_meta_data->initialTerm_id = initial_term_id;
    _pub->log_meta_data->mtu_length = (int32_t)mtu_length;
    _pub->log_meta_data->correlation_id = registration_id;
    _pub->log_meta_data->time_of_last_status_message = 0;
    _pub->log_meta_data->end_of_stream_position = INT64_MAX;
    aeron_logbuffer_fill_default_header(
        _pub->mapped_raw_log.log_meta_data.addr, session_id, stream_id, initial_term_id);

    _pub->conductor_fields.subscribeable.array = NULL;
    _pub->conductor_fields.subscribeable.length = 0;
    _pub->conductor_fields.subscribeable.capacity = 0;
    _pub->conductor_fields.managed_resource.registration_id = registration_id;
    _pub->conductor_fields.managed_resource.clientd = _pub;
    _pub->conductor_fields.managed_resource.incref = aeron_ipc_publication_incref;
    _pub->conductor_fields.managed_resource.decref = aeron_ipc_publication_decref;
    _pub->conductor_fields.has_reached_end_of_life = false;
    _pub->conductor_fields.cleaning_position = 0;
    _pub->conductor_fields.trip_limit = 0;
    _pub->conductor_fields.consumer_position = 0;
    _pub->conductor_fields.status = AERON_IPC_PUBLICATION_STATUS_ACTIVE;
    _pub->conductor_fields.refcnt = 1;
    _pub->session_id = session_id;
    _pub->stream_id = stream_id;
    _pub->pub_lmt_position.counter_id = pub_lmt_position->counter_id;
    _pub->pub_lmt_position.value_addr = pub_lmt_position->value_addr;
    _pub->initial_term_id = initial_term_id;
    _pub->position_bits_to_shift = (size_t)aeron_number_of_trailing_zeroes((int32_t)term_buffer_length);
    _pub->term_window_length = (int64_t)aeron_ipc_publication_term_window_length(context, term_buffer_length);
    _pub->trip_gain = _pub->term_window_length / 8;
    _pub->linger_timeout_ns = (int64_t)context->publication_linger_timeout_ns;
    _pub->is_exclusive = is_exclusive;

    _pub->conductor_fields.consumer_position = aeron_ipc_publication_producer_position(_pub);

    *publication = _pub;
    return 0;
}

void aeron_ipc_publication_close(aeron_counters_manager_t *counters_manager, aeron_ipc_publication_t *publication)
{
    aeron_subscribeable_t *subscribeable = &publication->conductor_fields.subscribeable;

    aeron_counters_manager_free(counters_manager, (int32_t)publication->pub_lmt_position.counter_id);

    for (size_t i = 0, length = subscribeable->length; i < length; i++)
    {
        aeron_counters_manager_free(counters_manager, (int32_t)subscribeable->array[i].counter_id);
    }

    if (NULL != publication)
    {
        publication->map_raw_log_close_func(&publication->mapped_raw_log);
        aeron_free(publication->log_file_name);
    }

    aeron_free(publication);
}

int aeron_ipc_publication_update_pub_lmt(aeron_ipc_publication_t *publication)
{
    int work_count = 0;
    int64_t min_sub_pos = INT64_MAX;
    int64_t max_sub_pos = publication->conductor_fields.consumer_position;

    for (size_t i = 0, length = publication->conductor_fields.subscribeable.length; i < length; i++)
    {
        int64_t position = aeron_counter_get_value(publication->conductor_fields.subscribeable.array[i].value_addr);

        min_sub_pos = (position < min_sub_pos) ? (position) : (min_sub_pos);
        max_sub_pos = (position > max_sub_pos) ? (position) : (max_sub_pos);
    }

    if (0 == publication->conductor_fields.subscribeable.length)
    {
        aeron_counter_set_value(publication->pub_lmt_position.value_addr, max_sub_pos);
        publication->conductor_fields.trip_limit = max_sub_pos;
    }
    else
    {
        int64_t proposed_limit = min_sub_pos + publication->term_window_length;
        if (proposed_limit > publication->conductor_fields.trip_limit)
        {
            aeron_counter_set_value(publication->pub_lmt_position.value_addr, proposed_limit);
            publication->conductor_fields.trip_limit = proposed_limit + publication->trip_gain;

            aeron_ipc_publication_clean_buffer(publication, min_sub_pos);
            work_count = 1;
        }

        publication->conductor_fields.consumer_position = max_sub_pos;
    }

    return work_count;
}

void aeron_ipc_publication_clean_buffer(aeron_ipc_publication_t *publication, int64_t min_sub_pos)
{
    int64_t cleaning_position = publication->conductor_fields.cleaning_position;
    size_t dirty_index = aeron_logbuffer_index_by_position(min_sub_pos, publication->position_bits_to_shift);
    size_t bytes_to_clean = (size_t)(min_sub_pos - cleaning_position);
    size_t term_length = publication->mapped_raw_log.term_length;
    int32_t term_offset = (int32_t)(cleaning_position & (term_length - 1));
    size_t bytes_left_in_term = term_length - term_offset;
    size_t length = bytes_to_clean < bytes_left_in_term ? bytes_to_clean : bytes_left_in_term;

    if (0 < length)
    {
        memset((uint8_t *)publication->mapped_raw_log.term_buffers[dirty_index].addr + term_offset, 0, length);
        publication->conductor_fields.cleaning_position = min_sub_pos;
    }
}

void aeron_ipc_publication_on_time_event(aeron_ipc_publication_t *publication, int64_t now_ns, int64_t now_ms)
{
    aeron_ipc_publication_check_for_blocked_publisher(publication, now_ns);

    if (0 < publication->conductor_fields.subscribeable.length)
    {
        AERON_PUT_ORDERED(publication->log_meta_data->time_of_last_status_message, now_ms);
    }

    switch (publication->conductor_fields.status)
    {
        case AERON_IPC_PUBLICATION_STATUS_INACTIVE:
            if (aeron_ipc_publication_is_drained(publication))
            {
                publication->conductor_fields.status = AERON_IPC_PUBLICATION_STATUS_LINGER;
                publication->conductor_fields.managed_resource.time_of_last_status_change = now_ns;
                /* TODO: transition to linger - send unavailable_image */
            }
            break;

        case AERON_IPC_PUBLICATION_STATUS_LINGER:
            if (now_ns >
                (publication->conductor_fields.managed_resource.time_of_last_status_change + publication->linger_timeout_ns))
            {
                publication->conductor_fields.has_reached_end_of_life = true;
                /* TODO: conductor cleanup */
            }
            break;

        default:
            break;
    }
}

void aeron_ipc_publication_incref(void *clientd)
{
    aeron_ipc_publication_t *publication = (aeron_ipc_publication_t *)clientd;

    publication->conductor_fields.refcnt++;
}

void aeron_ipc_publication_decref(void *clientd)
{
    aeron_ipc_publication_t *publication = (aeron_ipc_publication_t *)clientd;
    int32_t ref_count = --publication->conductor_fields.refcnt;

    if (0 == ref_count)
    {
        publication->conductor_fields.status = AERON_IPC_PUBLICATION_STATUS_INACTIVE;
        AERON_PUT_ORDERED(publication->log_meta_data->end_of_stream_position, aeron_ipc_publication_producer_position(publication));
    }
}

void aeron_ipc_publication_check_for_blocked_publisher(aeron_ipc_publication_t *publication, int64_t now_ns)
{
    /* TODO: finish */
}

extern int64_t aeron_ipc_publication_producer_position(aeron_ipc_publication_t *publication);
extern int64_t aeron_ipc_publication_joining_position(aeron_ipc_publication_t *publication);
extern bool aeron_ipc_publication_has_reached_end_of_life(aeron_ipc_publication_t *publication);
extern bool aeron_ipc_publication_is_drained(aeron_ipc_publication_t *publication);
