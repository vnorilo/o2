//
//  o2_send.c
//  O2
//
//  Created by 弛张 on 2/4/16.
//  Copyright © 2016 弛张. All rights reserved.
//

#include "ctype.h"
#include "o2_internal.h"
#include "o2_send.h"
#include "o2_message.h"
#include "o2_interoperation.h"
#include "o2_discovery.h"


#include <errno.h>


// to prevent deep recursion, messages go into a queue if we are already
// delivering a message via o2_msg_data_deliver:
int o2_in_find_and_call_handlers = 0; // counter to allow nesting
static o2_message_ptr pending_head = NULL;
static o2_message_ptr pending_tail = NULL;


void o2_deliver_pending()
{
    while (pending_head) {
        o2_message_ptr msg = pending_head;
        if (pending_head == pending_tail) {
            pending_head = pending_tail = NULL;
        } else {
            pending_head = pending_head->next;
        }
        o2_message_send_sched(msg, TRUE);
    }
}


/*o2string o2_key_pad(char *padded, const char *key)
{
    int i;
    for (i = 0; i < MAX_SERVICE_LEN; i++) {
        char c = (padded[i] = key[i]);
        if (c == '/') {
            padded[i] = 0;
            break;
        } else if (c == 0) {
            break;
        }
    }
    // make sure final word is padded to word boundary (or else hash
    // computation might be corrupted by stray bytes
    padded[i++] = 0;
    padded[i++] = 0;
    padded[i++] = 0;
    padded[i++] = 0;
    return padded;
}
*/

/* prereq: service_name does not contain '/'
 */
services_entry_ptr *o2_services_find(const char *service_name)
{
    // all callers are passing in (possibly) unaligned strings, so we
    // need to copy the service_name to aligned storage and pad it
    char key[NAME_BUF_LEN];
    o2_string_pad(key, service_name);
    return (services_entry_ptr *) o2_lookup(&o2_path_tree, key);
}    


o2_info_ptr o2_msg_service(o2_msg_data_ptr msg, services_entry_ptr *services)
{
    char *service_name = msg->address + 1;
    char *slash = strchr(service_name, '/');
    if (slash) *slash = 0;
    o2_info_ptr rslt = o2_service_find(service_name, services);
    if (slash) *slash = '/';
    return rslt;
}


/* prereq: service_name does not contain '/'
 */
o2_info_ptr o2_service_find(const char *service_name, services_entry_ptr *services)
{
    *services = *o2_services_find(service_name);
    if (!*services)
        return NULL;
    assert((*services)->services.length > 0);
    return GET_SERVICE((*services)->services, 0);
}


// This function is invoked by macros o2_send and o2_send_cmd.
// It expects arguments to end with O2_MARKER_A and O2_MARKER_B
int o2_send_marker(const char *path, double time, int tcp_flag, const char *typestring, ...)
{
    va_list ap;
    va_start(ap, typestring);

    o2_message_ptr msg;
    int rslt = o2_message_build(&msg, time, NULL, path, typestring, tcp_flag,
                                ap);
#ifndef O2_NO_DEBUGGING
    if (o2_debug & // either non-system (s) or system (S) mask
        (msg->data.address[1] != '_' && !isdigit(msg->data.address[1]) ?
         O2_DBs_FLAG : O2_DBS_FLAG)) {
        printf("O2: sending%s ", (tcp_flag ? " cmd" : ""));
        o2_msg_data_print(&(msg->data));
        printf("\n");
    }
#endif
    if (rslt != O2_SUCCESS) {
        return rslt; // could not allocate a message!
    }
    return o2_message_send_sched(msg, TRUE);
}

// This is the externally visible message send function.
//
int o2_message_send(o2_message_ptr msg)
{
    return o2_message_send_sched(msg, TRUE);
}

// Internal message send function.
// schedulable is normally TRUE meaning we can schedule messages
// according to their timestamps. If this message was dispatched
// by o2_ltsched, schedulable will be FALSE and we should ignore
// the timestamp, which has already been observed by o2_ltsched.
//
// msg is freed by this function
//
int o2_message_send_sched(o2_message_ptr msg, int schedulable)
{
    // Find the remote service, note that we skip over the leading '/':
    services_entry_ptr services;
    o2_info_ptr service = o2_msg_service(&msg->data, &services);
    if (!service) {
        o2_message_free(msg);
        return O2_FAIL;
    } else if (service->tag == TCP_SOCKET) { // remote delivery?
        o2_send_remote(&msg->data, msg->tcp_flag, (process_info_ptr) service);
        o2_message_free(msg);
    } else if (service->tag == OSC_REMOTE_SERVICE) {
        // this is a bit complicated: send immediately if it is a bundle
        // or is not scheduled in the future. Otherwise use O2 scheduling.
        if (!schedulable || IS_BUNDLE(&msg->data) ||
             msg->data.timestamp == 0.0 ||
             msg->data.timestamp <= o2_gtsched.last_time) {
            o2_send_osc((osc_info_ptr) service, &msg->data, services);
            o2_message_free(msg);
        } else {
            return o2_schedule(&o2_gtsched, msg); // delivery on time
        }
    } else if (schedulable && msg->data.timestamp > 0.0 &&
               msg->data.timestamp > o2_gtsched.last_time) { // local delivery
        return o2_schedule(&o2_gtsched, msg); // local delivery later
    } else if (o2_in_find_and_call_handlers) {
        if (pending_tail) {
            pending_tail->next = msg;
            pending_tail = msg;
        } else {
            pending_head = pending_tail = msg;
        }
    } else {
        o2_in_find_and_call_handlers++;
        o2_msg_data_deliver(&msg->data, msg->tcp_flag, service, services);
        o2_message_free(msg);
        o2_in_find_and_call_handlers--;
    }
    return O2_SUCCESS;
}


// deliver msg_data; similar to o2_message_send but local future
//     delivery requires the creation of an o2_message
int o2_msg_data_send(o2_msg_data_ptr msg, int tcp_flag)
{
    services_entry_ptr services;
    o2_info_ptr service = o2_msg_service(msg, &services);
    if (!service) return O2_FAIL;
    if (service->tag == TCP_SOCKET) {
        return o2_send_remote(msg, tcp_flag, (process_info_ptr) service);
    } else if (service->tag == OSC_REMOTE_SERVICE) {
        if (IS_BUNDLE(msg) || (msg->timestamp == 0.0 ||
                               msg->timestamp <= o2_gtsched.last_time)) {
            return o2_send_osc((osc_info_ptr) service, msg, services);
        }
    } else if (msg->timestamp == 0.0 ||
               msg->timestamp <= o2_gtsched.last_time) {
        o2_msg_data_deliver(msg, tcp_flag, service, services);
        return O2_SUCCESS;
    }
    // need to schedule o2_msg_data, so we need to copy to an o2_message
    int len = MSG_DATA_LENGTH(msg);
    o2_message_ptr message = o2_alloc_size_message(len);
    memcpy((char *) &(message->data), msg, len);
    message->length = len;
    return o2_schedule(&o2_gtsched, message);
}


int o2_send_remote(o2_msg_data_ptr msg, int tcp_flag, process_info_ptr info)
{
    // send the message to remote process
    if (tcp_flag) {
        return send_by_tcp_to_process(info, msg);
    } else { // send via UDP
        O2_DBs(if (msg->address[1] != '_' && !isdigit(msg->address[1]))
                   o2_dbg_msg("sent UDP", msg, "to", info->proc.name));
        O2_DBS(if (msg->address[1] == '_' || isdigit(msg->address[1]))
                   o2_dbg_msg("sent UDP", msg, "to", info->proc.name));
#if IS_LITTLE_ENDIAN
        o2_msg_swap_endian(msg, TRUE);
#endif
        if (sendto(local_send_sock, (char *) msg, MSG_DATA_LENGTH(msg),
                   0, (struct sockaddr *) &(info->proc.udp_sa),
                   sizeof(info->proc.udp_sa)) < 0) {
            perror("o2_send_remote");
            return O2_FAIL;
        }
    }
    return O2_SUCCESS;
}


// Note: the message is converted to network byte order. Free the
// message after calling this.
int send_by_tcp_to_process(process_info_ptr info, o2_msg_data_ptr msg)
{
	O2_DBs(if (msg->address[1] != '_' && !isdigit(msg->address[1]))
           o2_dbg_msg("sending TCP", msg, "to", info->proc.name));
    O2_DBS(if (msg->address[1] == '_' || isdigit(msg->address[1]))
           o2_dbg_msg("sending TCP", msg, "to", info->proc.name));
#if IS_LITTLE_ENDIAN
    o2_msg_swap_endian(msg, TRUE);
#endif
    // Send the length of the message followed by the message.
    // We want to do this in one send; otherwise, we'll send 2 
    // network packets due to the NODELAY socket option.
    int32_t len = MSG_DATA_LENGTH(msg);
    MSG_DATA_LENGTH(msg) = htonl(len);
    SOCKET fd = DA_GET(o2_fds, struct pollfd, info->fds_index)->fd;

	fd_set writable_sockets;
	FD_ZERO(&writable_sockets);
	FD_SET(fd, &writable_sockets);

retry:
    if (SOCKET_ERROR == send(fd, (char *) &MSG_DATA_LENGTH(msg), len + sizeof(int32_t),
             MSG_NOSIGNAL)) {
		int err = errno;
		if (!err || err == ETIMEDOUT) {
			o2_recv();
			goto retry;
		}
        if (err != EAGAIN && err != EINTR) {
            O2_DBo(printf("%s removing remote process after send error to socket %ld", o2_debug_prefix, (long) fd));
            o2_remove_remote_process(info);
        } else {
            perror("send_by_tcp_to_process");
        }
        return O2_FAIL;
    }
    // restore len just in case caller needs it to skip over the
    // message, which has now been byte-swapped and should not be read
    MSG_DATA_LENGTH(msg) = len;
    return O2_SUCCESS;
}    
