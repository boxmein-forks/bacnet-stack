/**
 * @file
 * @brief Provides BSD-specific DataLink functions for MS/TP.
 * @author Steve Karg <skarg@users.sourceforge.net>
 * @date 2008
 * @copyright SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <errno.h>
/* BSD includes */
#include <IOKit/serial/ioss.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/bacdef.h"
#include "bacnet/bacaddr.h"
#include "bacnet/npdu.h"
#include "bacnet/datalink/mstp.h"
#include "bacnet/basic/sys/ringbuf.h"
#include "bacnet/basic/sys/debug.h"
/* port specific */
#include "dlmstp_port.h"
#include "rs485.h"
/* OS Specific include */
#include "bacport.h"

#define BACNET_PDU_CONTROL_BYTE_OFFSET 1
#define BACNET_DATA_EXPECTING_REPLY_BIT 2
#define BACNET_DATA_EXPECTING_REPLY(control) \
    ((control & (1 << BACNET_DATA_EXPECTING_REPLY_BIT)) > 0)

#define INCREMENT_AND_LIMIT_UINT16(x) \
    {                                 \
        if (x < 0xFFFF)               \
            x++;                      \
    }

/**
 * Calculate the time difference between two timespec values.
 *
 * @param l - The minued (time from which we subtract).
 * @param r - The subtrahend (time that is being subtracted).
 *
 * @returns True if the difference is negative, otherwise 0.
 */
static int timespec_subtract(
    struct timespec *result, const struct timespec *l, const struct timespec *r)
{
#define NS_PER_S 1000000000 /* nano-seconds per second */
    struct timespec right = *r;
    int secs;

    /* Perform the carry for the later subtraction by updating y. */
    if (l->tv_nsec < right.tv_nsec) {
        secs = (right.tv_nsec - l->tv_nsec) / NS_PER_S + 1;
        right.tv_nsec -= NS_PER_S * secs;
        right.tv_sec += secs;
    }
    if (l->tv_nsec - right.tv_nsec > NS_PER_S) {
        secs = (l->tv_nsec - right.tv_nsec) / NS_PER_S;
        right.tv_nsec += NS_PER_S * secs;
        right.tv_sec -= secs;
    }

    /* Compute the time remaining. tv_nsec is certainly positive. */
    result->tv_sec = l->tv_sec - right.tv_sec;
    result->tv_nsec = l->tv_nsec - right.tv_nsec;

    return l->tv_sec < right.tv_sec;
}

/**
 * Add a certain number of nanoseconds to the specified time.
 *
 * @param ts - The time to which to add to.
 * @param ns - The number of nanoseconds to add.  Allowed range
 *      is -NS_PER_S..NS_PER_S (i.e., plus minus one second).
 */
static void timespec_add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    if (ts->tv_nsec > NS_PER_S) {
        ts->tv_nsec -= NS_PER_S;
        ts->tv_sec += 1;
    } else if (ts->tv_nsec < 0) {
        ts->tv_nsec += NS_PER_S;
        ts->tv_sec -= 1;
    }
}

static uint32_t Timer_Silence(void *poPort)
{
    int32_t res;
    struct timespec now, tmp_diff;
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return -1;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    timespec_subtract(&tmp_diff, &now, &poSharedData->start);
    res = ((tmp_diff.tv_sec) * 1000 + (tmp_diff.tv_nsec) / 1000000);

    return (res >= 0 ? res : -res);
}

static void Timer_Silence_Reset(void *poPort)
{
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &poSharedData->start);
}

static void get_abstime(struct timespec *abstime, unsigned long milliseconds)
{
    clock_gettime(CLOCK_MONOTONIC, abstime);
    if (milliseconds > 1000) {
        fprintf(
            stderr, "DLMSTP: limited timeout of %lums to 1000ms\n",
            milliseconds);
        milliseconds = 1000;
    }
    timespec_add_ns(abstime, 1000000 * milliseconds);
}

void dlmstp_cleanup(void *poPort)
{
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return;
    }

    /* restore the old port settings */
    tcsetattr(poSharedData->RS485_Handle, TCSANOW, &poSharedData->RS485_oldtio);
    close(poSharedData->RS485_Handle);

    pthread_cond_destroy(&poSharedData->Received_Frame_Flag);
    pthread_cond_destroy(&poSharedData->Master_Done_Flag);
    pthread_mutex_destroy(&poSharedData->Received_Frame_Mutex);
    pthread_mutex_destroy(&poSharedData->Master_Done_Mutex);
}

/* returns number of bytes sent on success, zero on failure */
int dlmstp_send_pdu(
    void *poPort,
    BACNET_ADDRESS *dest, /* destination address */
    uint8_t *pdu, /* any data to be sent - may be null */
    unsigned pdu_len)
{ /* number of bytes of data */
    int bytes_sent = 0;
    struct mstp_pdu_packet *pkt;
    unsigned i = 0;
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return 0;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return 0;
    }

    pkt = (struct mstp_pdu_packet *)Ringbuf_Data_Peek(&poSharedData->PDU_Queue);
    if (pkt) {
        pkt->data_expecting_reply =
            BACNET_DATA_EXPECTING_REPLY(pdu[BACNET_PDU_CONTROL_BYTE_OFFSET]);
        for (i = 0; i < pdu_len; i++) {
            pkt->buffer[i] = pdu[i];
        }
        pkt->length = pdu_len;
        pkt->destination_mac = dest->mac[0];
        if (Ringbuf_Data_Put(&poSharedData->PDU_Queue, (uint8_t *)pkt)) {
            bytes_sent = pdu_len;
        }
    }

    return bytes_sent;
}

uint16_t dlmstp_receive(
    void *poPort,
    BACNET_ADDRESS *src, /* source address */
    uint8_t *pdu, /* PDU data */
    uint16_t max_pdu, /* amount of space available in the PDU  */
    unsigned timeout)
{ /* milliseconds to wait for a packet */
    uint16_t pdu_len = 0;
    struct timespec abstime;
    int rv = 0;
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return 0;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return 0;
    }
    (void)max_pdu;
    /* see if there is a packet available, and a place
       to put the reply (if necessary) and process it */
    get_abstime(&abstime, timeout);
    rv = dispatch_semaphore_wait(poSharedData->Receive_Packet_Flag, &abstime);
    if (rv == 0) {
        if (poSharedData->Receive_Packet.ready) {
            if (poSharedData->Receive_Packet.pdu_len) {
                poSharedData->MSTP_Packets++;
                if (src) {
                    memmove(
                        src, &poSharedData->Receive_Packet.address,
                        sizeof(poSharedData->Receive_Packet.address));
                }
                if (pdu) {
                    memmove(
                        pdu, &poSharedData->Receive_Packet.pdu,
                        sizeof(poSharedData->Receive_Packet.pdu));
                }
                pdu_len = poSharedData->Receive_Packet.pdu_len;
            }
            poSharedData->Receive_Packet.ready = false;
        }
    }

    return pdu_len;
}

static void *dlmstp_receive_fsm_task(void *pArg)
{
    bool received_frame;
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)pArg;
    if (!mstp_port) {
        return NULL;
    }

    poSharedData =
        (SHARED_MSTP_DATA *)((struct mstp_port_struct_t *)pArg)->UserData;
    if (!poSharedData) {
        return NULL;
    }

    for (;;) {
        /* only do receive state machine while we don't have a frame */
        if ((mstp_port->ReceivedValidFrame == false) &&
            (mstp_port->ReceivedValidFrameNotForUs == false) &&
            (mstp_port->ReceivedInvalidFrame == false)) {
            do {
                RS485_Check_UART_Data(mstp_port);
                MSTP_Receive_Frame_FSM((struct mstp_port_struct_t *)pArg);
                received_frame = mstp_port->ReceivedValidFrame ||
                    mstp_port->ReceivedValidFrameNotForUs ||
                    mstp_port->ReceivedInvalidFrame;
                if (received_frame) {
                    pthread_cond_signal(&poSharedData->Received_Frame_Flag);
                    break;
                }
            } while (mstp_port->DataAvailable);
        }
    }

    return NULL;
}

static void *dlmstp_master_fsm_task(void *pArg)
{
    uint32_t silence = 0;
    bool run_master = false;
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)pArg;
    if (!mstp_port) {
        return NULL;
    }

    poSharedData =
        (SHARED_MSTP_DATA *)((struct mstp_port_struct_t *)pArg)->UserData;
    if (!poSharedData) {
        return NULL;
    }

    for (;;) {
        if (mstp_port->ReceivedValidFrame == false &&
            mstp_port->ReceivedValidFrameNotForUs == false &&
            mstp_port->ReceivedInvalidFrame == false) {
            RS485_Check_UART_Data(mstp_port);
            MSTP_Receive_Frame_FSM(mstp_port);
        }
        if (mstp_port->ReceivedValidFrame || mstp_port->ReceivedInvalidFrame ||
            mstp_port->ReceivedValidFrameNotForUs) {
            run_master = true;
        } else {
            silence = mstp_port->SilenceTimer(NULL);
            switch (mstp_port->master_state) {
                case MSTP_MASTER_STATE_IDLE:
                    if (silence >= Tno_token) {
                        run_master = true;
                    }
                    break;
                case MSTP_MASTER_STATE_WAIT_FOR_REPLY:
                    if (silence >= mstp_port->Treply_timeout) {
                        run_master = true;
                    }
                    break;
                case MSTP_MASTER_STATE_POLL_FOR_MASTER:
                    if (silence >= mstp_port->Tusage_timeout) {
                        run_master = true;
                    }
                    break;
                default:
                    run_master = true;
                    break;
            }
        }
        if (run_master) {
            if (mstp_port->This_Station <= DEFAULT_MAX_MASTER) {
                while (MSTP_Master_Node_FSM(mstp_port)) {
                    /* do nothing while immediate transitioning */
                }
            } else if (mstp_port->This_Station < 255) {
                MSTP_Slave_Node_FSM(mstp_port);
            }
        }
    }

    return NULL;
}

void dlmstp_fill_bacnet_address(BACNET_ADDRESS *src, uint8_t mstp_address)
{
    int i = 0;

    if (mstp_address == MSTP_BROADCAST_ADDRESS) {
        /* mac_len = 0 if broadcast address */
        src->mac_len = 0;
        src->mac[0] = 0;
    } else {
        src->mac_len = 1;
        src->mac[0] = mstp_address;
    }
    /* fill with 0's starting with index 1; index 0 filled above */
    for (i = 1; i < MAX_MAC_LEN; i++) {
        src->mac[i] = 0;
    }
    src->net = 0;
    src->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        src->adr[i] = 0;
    }
}

/* for the MS/TP state machine to use for putting received data */
uint16_t MSTP_Put_Receive(struct mstp_port_struct_t *mstp_port)
{
    uint16_t pdu_len = 0;
    SHARED_MSTP_DATA *poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;

    if (!poSharedData) {
        return 0;
    }

    if (!poSharedData->Receive_Packet.ready) {
        /* bounds check - maybe this should send an abort? */
        pdu_len = mstp_port->DataLength;
        if (pdu_len > sizeof(poSharedData->Receive_Packet.pdu)) {
            pdu_len = sizeof(poSharedData->Receive_Packet.pdu);
        }
        memmove(
            (void *)&poSharedData->Receive_Packet.pdu[0],
            (void *)&mstp_port->InputBuffer[0], pdu_len);
        dlmstp_fill_bacnet_address(
            &poSharedData->Receive_Packet.address, mstp_port->SourceAddress);
        poSharedData->Receive_Packet.pdu_len = mstp_port->DataLength;
        poSharedData->Receive_Packet.ready = true;
        dispatch_semaphore_signal(poSharedData->Receive_Packet_Flag);
    }

    return pdu_len;
}

/* for the MS/TP state machine to use for getting data to send */
/* Return: amount of PDU data */
uint16_t MSTP_Get_Send(struct mstp_port_struct_t *mstp_port, unsigned timeout)
{ /* milliseconds to wait for a packet */
    uint16_t pdu_len = 0;
    uint8_t frame_type = 0;
    struct mstp_pdu_packet *pkt;
    SHARED_MSTP_DATA *poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;

    if (!poSharedData) {
        return 0;
    }

    (void)timeout;
    if (Ringbuf_Empty(&poSharedData->PDU_Queue)) {
        return 0;
    }
    pkt = (struct mstp_pdu_packet *)Ringbuf_Peek(&poSharedData->PDU_Queue);
    if (pkt->data_expecting_reply) {
        frame_type = FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY;
    } else {
        frame_type = FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY;
    }
    /* convert the PDU into the MSTP Frame */
    pdu_len = MSTP_Create_Frame(
        &mstp_port->OutputBuffer[0], /* <-- loading this */
        mstp_port->OutputBufferSize, frame_type, pkt->destination_mac,
        mstp_port->This_Station, (uint8_t *)&pkt->buffer[0], pkt->length);
    (void)Ringbuf_Pop(&poSharedData->PDU_Queue, NULL);

    return pdu_len;
}

/**
 * @brief Send an MSTP frame
 * @param mstp_port - port specific data
 * @param buffer - data to send
 * @param nbytes - number of bytes of data to send
 */
void MSTP_Send_Frame(
    struct mstp_port_struct_t *mstp_port,
    const uint8_t *buffer,
    uint16_t nbytes)
{
    RS485_Send_Frame(mstp_port, buffer, nbytes);
}

static bool dlmstp_compare_data_expecting_reply(
    const uint8_t *request_pdu,
    uint16_t request_pdu_len,
    uint8_t src_address,
    const uint8_t *reply_pdu,
    uint16_t reply_pdu_len,
    uint8_t dest_address)
{
    uint16_t offset;
    /* One way to check the message is to compare NPDU
       src, dest, along with the APDU type, invoke id.
       Seems a bit overkill */
    struct DER_compare_t {
        BACNET_NPDU_DATA npdu_data;
        BACNET_ADDRESS address;
        uint8_t pdu_type;
        uint8_t invoke_id;
        uint8_t service_choice;
    };
    struct DER_compare_t request;
    struct DER_compare_t reply;

    /* unused parameters */
    (void)request_pdu_len;
    (void)reply_pdu_len;

    /* decode the request data */
    request.address.mac[0] = src_address;
    request.address.mac_len = 1;
    offset = bacnet_npdu_decode(
        request_pdu, request_pdu_len, NULL, &request.address,
        &request.npdu_data);
    if (request.npdu_data.network_layer_message) {
        debug_printf("DLMSTP: DER Compare failed: "
                     "Request is Network message.\n");
        return false;
    }
    request.pdu_type = request_pdu[offset] & 0xF0;
    if (request.pdu_type != PDU_TYPE_CONFIRMED_SERVICE_REQUEST) {
        debug_printf("DLMSTP: DER Compare failed: "
                     "Not Confirmed Request.\n");
        return false;
    }
    request.invoke_id = request_pdu[offset + 2];
    /* segmented message? */
    if (request_pdu[offset] & BIT(3)) {
        request.service_choice = request_pdu[offset + 5];
    } else {
        request.service_choice = request_pdu[offset + 3];
    }
    /* decode the reply data */
    reply.address.mac[0] = dest_address;
    reply.address.mac_len = 1;
    offset = bacnet_npdu_decode(
        reply_pdu, reply_pdu_len, &reply.address, NULL, &reply.npdu_data);
    if (reply.npdu_data.network_layer_message) {
        debug_printf("DLMSTP: DER Compare failed: "
                     "Reply is Network message.\n");
        return false;
    }
    /* reply could be a lot of things:
       confirmed, simple ack, abort, reject, error */
    reply.pdu_type = reply_pdu[offset] & 0xF0;
    switch (reply.pdu_type) {
        case PDU_TYPE_SIMPLE_ACK:
            reply.invoke_id = reply_pdu[offset + 1];
            reply.service_choice = reply_pdu[offset + 2];
            break;
        case PDU_TYPE_COMPLEX_ACK:
            reply.invoke_id = reply_pdu[offset + 1];
            /* segmented message? */
            if (reply_pdu[offset] & BIT(3)) {
                reply.service_choice = reply_pdu[offset + 4];
            } else {
                reply.service_choice = reply_pdu[offset + 2];
            }
            break;
        case PDU_TYPE_ERROR:
            reply.invoke_id = reply_pdu[offset + 1];
            reply.service_choice = reply_pdu[offset + 2];
            break;
        case PDU_TYPE_REJECT:
        case PDU_TYPE_ABORT:
        case PDU_TYPE_SEGMENT_ACK:
            reply.invoke_id = reply_pdu[offset + 1];
            break;
        default:
            return false;
    }
    /* these don't have service choice included */
    if ((reply.pdu_type == PDU_TYPE_REJECT) ||
        (reply.pdu_type == PDU_TYPE_ABORT) ||
        (reply.pdu_type == PDU_TYPE_SEGMENT_ACK)) {
        if (request.invoke_id != reply.invoke_id) {
            debug_printf("DLMSTP: DER Compare failed: "
                         "Invoke ID mismatch.\n");
            return false;
        }
    } else {
        if (request.invoke_id != reply.invoke_id) {
            debug_printf("DLMSTP: DER Compare failed: "
                         "Invoke ID mismatch.\n");
            return false;
        }
        if (request.service_choice != reply.service_choice) {
            debug_printf("DLMSTP: DER Compare failed: "
                         "Service choice mismatch.\n");
            return false;
        }
    }
    if (request.npdu_data.protocol_version !=
        reply.npdu_data.protocol_version) {
        debug_printf("DLMSTP: DER Compare failed: "
                     "NPDU Protocol Version mismatch.\n");
        return false;
    }
#if 0
    /* the NDPU priority doesn't get passed through the stack, and
       all outgoing messages have NORMAL priority */
    if (request.npdu_data.priority != reply.npdu_data.priority) {
        debug_printf(
            "DLMSTP: DER Compare failed: " "NPDU Priority mismatch.\n");
        return false;
    }
#endif
    if (!bacnet_address_same(&request.address, &reply.address)) {
        debug_printf("DLMSTP: DER Compare failed: "
                     "BACnet Address mismatch.\n");
        return false;
    }

    return true;
}

/* Get the reply to a DATA_EXPECTING_REPLY frame, or nothing */
uint16_t MSTP_Get_Reply(struct mstp_port_struct_t *mstp_port, unsigned timeout)
{ /* milliseconds to wait for a packet */
    uint16_t pdu_len = 0; /* return value */
    bool matched = false;
    uint8_t frame_type = 0;
    struct mstp_pdu_packet *pkt;
    SHARED_MSTP_DATA *poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;

    (void)timeout;
    if (!poSharedData) {
        return 0;
    }
    if (Ringbuf_Empty(&poSharedData->PDU_Queue)) {
        return 0;
    }
    pkt = (struct mstp_pdu_packet *)Ringbuf_Peek(&poSharedData->PDU_Queue);
    /* is this the reply to the DER? */
    matched = dlmstp_compare_data_expecting_reply(
        &mstp_port->InputBuffer[0], mstp_port->DataLength,
        mstp_port->SourceAddress, (uint8_t *)&pkt->buffer[0], pkt->length,
        pkt->destination_mac);
    if (!matched) {
        /* Walk the rest of the ring buffer to see if we can find a match */
        while (!matched &&
               (pkt = (struct mstp_pdu_packet *)Ringbuf_Peek_Next(
                    &poSharedData->PDU_Queue, (uint8_t *)pkt)) != NULL) {
            matched = dlmstp_compare_data_expecting_reply(
                &mstp_port->InputBuffer[0], mstp_port->DataLength,
                mstp_port->SourceAddress, (uint8_t *)&pkt->buffer[0],
                pkt->length, pkt->destination_mac);
        }
        if (!matched) {
            /* Still didn't find a match so just bail out */
            return 0;
        }
    }
    if (pkt->data_expecting_reply) {
        frame_type = FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY;
    } else {
        frame_type = FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY;
    }
    /* convert the PDU into the MSTP Frame */
    pdu_len = MSTP_Create_Frame(
        &mstp_port->OutputBuffer[0], /* <-- loading this */
        mstp_port->OutputBufferSize, frame_type, pkt->destination_mac,
        mstp_port->This_Station, (uint8_t *)&pkt->buffer[0], pkt->length);
    /* This will pop the element no matter where we found it */
    (void)Ringbuf_Pop_Element(&poSharedData->PDU_Queue, (uint8_t *)pkt, NULL);

    return pdu_len;
}

void dlmstp_set_mac_address(void *poPort, uint8_t mac_address)
{
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return;
    }
    /* Master Nodes can only have address 0-127 */
    if (mac_address <= 127) {
        mstp_port->This_Station = mac_address;
        if (mac_address > mstp_port->Nmax_master) {
            dlmstp_set_max_master(mstp_port, mac_address);
        }
    }

    return;
}

uint8_t dlmstp_mac_address(void *poPort)
{
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return 0;
    }

    return mstp_port->This_Station;
}

/* This parameter represents the value of the Max_Info_Frames property of */
/* the node's Device object. The value of Max_Info_Frames specifies the */
/* maximum number of information frames the node may send before it must */
/* pass the token. Max_Info_Frames may have different values on different */
/* nodes. This may be used to allocate more or less of the available link */
/* bandwidth to particular nodes. If Max_Info_Frames is not writable in a */
/* node, its value shall be 1. */
void dlmstp_set_max_info_frames(void *poPort, uint8_t max_info_frames)
{
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;

    if (!mstp_port) {
        return;
    }
    if (max_info_frames >= 1) {
        mstp_port->Nmax_info_frames = max_info_frames;
    }

    return;
}

uint8_t dlmstp_max_info_frames(void *poPort)
{
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return 0;
    }

    return mstp_port->Nmax_info_frames;
}

/* This parameter represents the value of the Max_Master property of the */
/* node's Device object. The value of Max_Master specifies the highest */
/* allowable address for master nodes. The value of Max_Master shall be */
/* less than or equal to 127. If Max_Master is not writable in a node, */
/* its value shall be 127. */
void dlmstp_set_max_master(void *poPort, uint8_t max_master)
{
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return;
    }
    if (max_master <= 127) {
        if (mstp_port->This_Station <= max_master) {
            mstp_port->Nmax_master = max_master;
        }
    }

    return;
}

uint8_t dlmstp_max_master(void *poPort)
{
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;

    if (!mstp_port) {
        return 0;
    }

    return mstp_port->Nmax_master;
}

/* RS485 Baud Rate 9600, 19200, 38400, 57600, 115200 */
void dlmstp_set_baud_rate(void *poPort, uint32_t baud)
{
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return;
    }

    switch (baud) {
        case 9600:
            poSharedData->RS485_Baud = B9600;
            break;
        case 19200:
            poSharedData->RS485_Baud = B19200;
            break;
        case 38400:
            poSharedData->RS485_Baud = B38400;
            break;
        case 57600:
            poSharedData->RS485_Baud = B57600;
            break;
        case 115200:
            poSharedData->RS485_Baud = B115200;
            break;
        default:
            break;
    }
}

uint32_t dlmstp_baud_rate(void *poPort)
{
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return false;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return false;
    }

    switch (poSharedData->RS485_Baud) {
        case B19200:
            return 19200;
        case B38400:
            return 38400;
        case B57600:
            return 57600;
        case B115200:
            return 115200;
        default:
        case B9600:
            return 9600;
    }
}

void dlmstp_get_my_address(void *poPort, BACNET_ADDRESS *my_address)
{
    int i = 0; /* counter */
    SHARED_MSTP_DATA *poSharedData;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    if (!mstp_port) {
        return;
    }
    poSharedData = (SHARED_MSTP_DATA *)mstp_port->UserData;
    if (!poSharedData) {
        return;
    }
    my_address->mac_len = 1;
    my_address->mac[0] = mstp_port->This_Station;
    my_address->net = 0; /* local only, no routing */
    my_address->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        my_address->adr[i] = 0;
    }

    return;
}

void dlmstp_get_broadcast_address(BACNET_ADDRESS *dest)
{ /* destination address */
    int i = 0; /* counter */

    if (dest) {
        dest->mac_len = 1;
        dest->mac[0] = MSTP_BROADCAST_ADDRESS;
        dest->net = BACNET_BROADCAST_NETWORK;
        dest->len = 0; /* always zero when DNET is broadcast */
        for (i = 0; i < MAX_MAC_LEN; i++) {
            dest->adr[i] = 0;
        }
    }

    return;
}

bool dlmstp_init(void *poPort, char *ifname)
{
    pthread_t hThread = 0;
    int rv = 0;
    SHARED_MSTP_DATA *poSharedData;
    struct termios newtio;
    struct mstp_port_struct_t *mstp_port = (struct mstp_port_struct_t *)poPort;
    dispatch_semaphore_t *sem = NULL;
    int handshake;
    unsigned long mics = 1UL;
    if (!mstp_port) {
        return false;
    }

    poSharedData =
        (SHARED_MSTP_DATA *)((struct mstp_port_struct_t *)mstp_port)->UserData;
    if (!poSharedData) {
        return false;
    }

    poSharedData->RS485_Port_Name = ifname;
    /* initialize PDU queue */
    Ringbuf_Init(
        &poSharedData->PDU_Queue, (uint8_t *)&poSharedData->PDU_Buffer,
        sizeof(struct mstp_pdu_packet), MSTP_PDU_PACKET_COUNT);
    /* initialize packet queue */
    poSharedData->Receive_Packet.ready = false;
    poSharedData->Receive_Packet.pdu_len = 0;
    sem = &poSharedData->Receive_Packet_Flag;
    *sem = dispatch_semaphore_create(0);

    printf("RS485 Port: Initializing %s\n", poSharedData->RS485_Port_Name);
    /*
       Open device for reading and writing.
       Blocking mode - more CPU effecient
     */
    poSharedData->RS485_Handle = open(
        poSharedData->RS485_Port_Name,
        O_RDWR | O_NOCTTY | O_NONBLOCK /*| O_NDELAY */);
    if (poSharedData->RS485_Handle < 0) {
        perror(poSharedData->RS485_Port_Name);
        exit(-1);
    }
    if (ioctl(poSharedData->RS485_Handle, TIOCEXCL) == -1) {
        printf(
            "Error setting TIOCEXCL on %s - %s(%d).\n",
            poSharedData->RS485_Port_Name, strerror(errno), errno);
        exit(-1);
    }
#if 0
    /* non blocking for the read */
    fcntl(poSharedData->RS485_Handle, F_SETFL, FNDELAY);
#else
    /* efficient blocking for the read */
    fcntl(poSharedData->RS485_Handle, F_SETFL, 0);
#endif
    /* save current serial port settings */
    tcgetattr(poSharedData->RS485_Handle, &poSharedData->RS485_oldtio);
    /* clear struct for new port settings */
    bzero(&newtio, sizeof(newtio));
    /*
       BAUDRATE: Set bps rate. You could also use cfsetispeed and cfsetospeed.
       CRTSCTS : output hardware flow control (only used if the cable has
       all necessary lines. See sect. 7 of Serial-HOWTO)
       CLOCAL  : local connection, no modem contol
       CREAD   : enable receiving characters
     */
    printf(
        "Default/current input baud rate is %d\n",
        (int)cfgetispeed(&poSharedData->RS485_oldtio));
    printf(
        "Default/current output baud rate is %d\n",
        (int)cfgetospeed(&poSharedData->RS485_oldtio));
    newtio.c_cc[VMIN] = 0;
    newtio.c_cc[VTIME] = 10;
    // newtio.c_cflag =
    //     poSharedData->RS485_Baud | poSharedData->RS485MOD | CLOCAL | CREAD;
    cfsetspeed(&newtio, poSharedData->RS485_Baud);
    newtio.c_cflag &= ~PARENB; /* No Parity */
    newtio.c_cflag &= ~CSTOPB; /* 1 Stop Bit */
    newtio.c_cflag &= ~CSIZE;
    newtio.c_cflag |= CS8; /* Use 8 bit words */
    /* Raw input */
    newtio.c_iflag = 0;
    /* Raw output */
    newtio.c_oflag = 0;
    /* no processing */
    newtio.c_lflag = 0;
    if (ioctl(
            poSharedData->RS485_Handle, IOSSIOSPEED,
            &poSharedData->RS485_Baud) == -1) {
        printf(
            "Error calling ioctl(..., IOSSIOSPEED, ...) %s - %s(%d).\n",
            poSharedData->RS485_Port_Name, strerror(errno), errno);
    }
    printf("Input baud rate changed to %d\n", (int)cfgetispeed(&newtio));
    printf("Output baud rate changed to %d\n", (int)cfgetospeed(&newtio));

    /* activate the settings for the port after flushing I/O */
    tcsetattr(poSharedData->RS485_Handle, TCSANOW, &newtio);

    /* To set the modem handshake lines, use the following ioctls.
     See tty(4) <x-man-page//4/tty> and ioctl(2) <x-man-page//2/ioctl> for
     details.*/

    /* Assert Data Terminal Ready (DTR) */
    if (ioctl(poSharedData->RS485_Handle, TIOCSDTR) == -1) {
        printf(
            "Error asserting DTR %s - %s(%d).\n", poSharedData->RS485_Port_Name,
            strerror(errno), errno);
    }

    /* Clear Data Terminal Ready (DTR) */
    if (ioctl(poSharedData->RS485_Handle, TIOCCDTR) == -1) {
        printf(
            "Error clearing DTR %s - %s(%d).\n", poSharedData->RS485_Port_Name,
            strerror(errno), errno);
    }

    /* Set the modem lines depending on the bits set in handshake */
    handshake = TIOCM_DTR | TIOCM_RTS | TIOCM_CTS | TIOCM_DSR;
    if (ioctl(poSharedData->RS485_Handle, TIOCMSET, &handshake) == -1) {
        printf(
            "Error setting handshake lines %s - %s(%d).\n",
            poSharedData->RS485_Port_Name, strerror(errno), errno);
    }

    /* To read the state of the modem lines, use the following ioctl.
     See tty(4) <x-man-page//4/tty> and ioctl(2) <x-man-page//2/ioctl> for
     details. */

    /* Store the state of the modem lines in handshake */
    if (ioctl(poSharedData->RS485_Handle, TIOCMGET, &handshake) == -1) {
        printf(
            "Error getting handshake lines %s - %s(%d).\n",
            poSharedData->RS485_Port_Name, strerror(errno), errno);
    }

    printf("Handshake lines currently set to %d\n", handshake);

    if (ioctl(poSharedData->RS485_Handle, IOSSDATALAT, &mics) == -1) {
        /* set latency to 1 microsecond */
        printf(
            "Error setting read latency %s - %s(%d).\n",
            poSharedData->RS485_Port_Name, strerror(errno), errno);
        exit(-1);
    }

    /* flush any data waiting */
    usleep(200000);
    tcflush(poSharedData->RS485_Handle, TCIOFLUSH);
    /* ringbuffer */
    FIFO_Init(
        &poSharedData->Rx_FIFO, poSharedData->Rx_Buffer,
        sizeof(poSharedData->Rx_Buffer));
    printf("success!\n");
    mstp_port->InputBuffer = &poSharedData->RxBuffer[0];
    mstp_port->InputBufferSize = sizeof(poSharedData->RxBuffer);
    mstp_port->OutputBuffer = &poSharedData->TxBuffer[0];
    mstp_port->OutputBufferSize = sizeof(poSharedData->TxBuffer);
    clock_gettime(CLOCK_MONOTONIC, &poSharedData->start);
    mstp_port->SilenceTimer = Timer_Silence;
    mstp_port->SilenceTimerReset = Timer_Silence_Reset;
    MSTP_Init(mstp_port);
    debug_fprintf(stderr, "MS/TP MAC: %02X\n", mstp_port->This_Station);
    debug_fprintf(stderr, "MS/TP Max_Master: %02X\n", mstp_port->Nmax_master);
    debug_fprintf(
        stderr, "MS/TP Max_Info_Frames: %u\n", mstp_port->Nmax_info_frames);
    rv = pthread_create(&hThread, NULL, dlmstp_master_fsm_task, mstp_port);
    if (rv != 0) {
        fprintf(stderr, "Failed to start Master Node FSM task\n");
    }

    /* You can try also this for thread. This here so we ignore
     * -Wunused-function compiler warning
     */
    dlmstp_receive_fsm_task(NULL);

    return true;
}
